"""Optional DanmaX companion for the existing Emby mpv URL handler.

No playback reporting or mpv control is performed here. Credentials stay in
memory/local configuration; descriptors contain metadata and process identity.
"""
import ctypes
from ctypes import wintypes
import json
import logging
import os
from pathlib import Path
import re
import subprocess
import threading
import time
from urllib.parse import parse_qs, urlparse, unquote
import uuid


def emby_address(url):
    parsed = urlparse(url)
    match = re.search(r"/videos/([^/]+)/", parsed.path, re.I)
    if parsed.scheme not in ("http", "https") or not match:
        return None
    return parsed.scheme + "://" + parsed.netloc, match.group(1), parse_qs(parsed.query)


def process_stamp(pid):
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.GetProcessTimes.argtypes = [wintypes.HANDLE] + [ctypes.POINTER(wintypes.FILETIME)] * 4
    kernel.GetProcessTimes.restype = wintypes.BOOL
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    handle = kernel.OpenProcess(0x1000, False, pid)
    if not handle:
        raise OSError("Cannot identify player process")
    try:
        values = [wintypes.FILETIME() for _ in range(4)]
        if not kernel.GetProcessTimes(handle, *[ctypes.byref(x) for x in values]):
            raise OSError("Cannot read player creation time")
        return str((values[0].dwHighDateTime << 32) | values[0].dwLowDateTime)
    finally:
        kernel.CloseHandle(handle)


def safe_metadata(item, server_id):
    """Allowlist prevents stream URLs and tokens from crossing into descriptors."""
    source = item.get("Path", "")
    if "://" in source:
        source = unquote(urlparse(source).path)
    return {
        "serverId": str(server_id), "itemId": str(item.get("Id", "")),
        "seriesId": str(item.get("SeriesId", "")), "seasonId": str(item.get("SeasonId", "")),
        "type": item.get("Type", ""), "title": item.get("Name", "")[:1024],
        "seriesTitle": item.get("SeriesName", "")[:1024],
        "season": item.get("ParentIndexNumber", 0) or 0,
        "episode": item.get("IndexNumber", 0) or 0,
        "year": item.get("ProductionYear", 0) or 0,
        "fileName": source.replace("\\", "/").rsplit("/", 1)[-1][:1024],
    }


class DanmaxBridge:
    def __init__(self, root, mpv, request, property_reader):
        self.root, self.mpv = Path(root), mpv
        self.request, self.property_reader = request, property_reader
        self.config = {}
        try:
            path = self.root / "danmax-bridge.json"
            if path.stat().st_size <= 65536:
                self.config = json.loads(path.read_text("utf-8-sig"))
        except (OSError, ValueError):
            pass
        self.enabled = self.config.get("enabled") is True
        self.thread = None

    def player_options(self):
        if not self.enabled:
            return []
        return ["--script-opts-append=uosc_danmaku-auto_load=no,"
                "uosc_danmaku-autoload_for_url=no,uosc_danmaku-autoload_local_danmaku=no"]

    def start(self, child, pipe, video_url, item=None, host=None, token=None, user_id=None):
        if not self.enabled:
            return
        self.thread = threading.Thread(target=self._watch,
            args=(child, pipe, video_url, item, host, token, user_id), daemon=True,
            name="danmax-metadata")
        self.thread.start()

    def _get(self, host, path, token):
        return json.loads(self.request(host + "/emby" + path, token))

    def _watch(self, child, pipe, video_url, item, host, token, user_id):
        files = []
        try:
            address = emby_address(video_url)
            if address:
                host = address[0]
                token = token or address[2].get("api_key", [""])[0]
            server_id = ""
            if host and token:
                server_id = self._get(host, "/System/Info/Public", token).get("Id", "")
            created = process_stamp(child.pid)
            session_id = uuid.uuid4().hex
            previous_item = ""
            previous_path = video_url
            revision = 0

            def publish(current_item):
                nonlocal revision, previous_item
                revision += 1
                current_item = current_item or {}
                previous_item = str(current_item.get("Id", ""))
                descriptor = {"version": 1, "sessionId": session_id, "revision": revision,
                    "process": {"pid": child.pid, "created": created, "executable": self.mpv},
                    "ipcPipe": pipe, "emby": safe_metadata(current_item, server_id)}
                directory = Path(self.config["sessionDirectory"])
                directory.mkdir(parents=True, exist_ok=True)
                path = directory / (session_id + "-" + str(revision) + ".json")
                temporary = path.with_suffix(".tmp")
                temporary.write_text(json.dumps(descriptor, ensure_ascii=False), "utf-8")
                os.replace(temporary, path)
                files.append(path)
                args = [self.config["executable"], "--external-session", str(path)]
                for setting, option in (("dataDirectory", "--data-dir"), ("cacheDirectory", "--cache-dir")):
                    if self.config.get(setting):
                        args += [option, self.config[setting]]
                if self.config.get("diagnosticReport"):
                    args += ["--report", self.config["diagnosticReport"]]
                # First launch stays alive as the overlay. Later launches only
                # forward to it. Never wait for the first process to finish.
                companion = subprocess.Popen(args, cwd=str(Path(self.config["executable"]).parent),
                    close_fds=True, creationflags=subprocess.CREATE_NO_WINDOW)
                logging.info("DanmaX session submitted mpv_pid=%s revision=%s", child.pid, revision)
                return companion

            if item is None and address and host and token:
                prefix = "/Users/%s" % user_id if user_id else ""
                result = self._get(host, prefix + "/Items?Ids=" + address[1] + "&Fields=Path,ProviderIds", token)
                item = next(iter(result.get("Items", [])), None)
            companion = publish(item)
            while child.poll() is None:
                time.sleep(.5)
                if child.poll() is not None:
                    break
                if companion.poll() not in (None, 0):
                    logging.warning("DanmaX unavailable; video playback continues")
                    # Only show a generic local notification; never include URLs.
                    self._notify()
                    companion = _SuccessfulProcess()
                try:
                    path = self.property_reader(pipe, "path", attempts=1, quick=True)
                    current = emby_address(path) if isinstance(path, str) else None
                    # Never send an existing Emby credential to a new host.
                    if current and current[0] == host and token and (current[1] != previous_item or path != previous_path):
                        prefix = "/Users/%s" % user_id if user_id else ""
                        result = self._get(host, prefix + "/Items?Ids=" + current[1] + "&Fields=Path,ProviderIds", token)
                        value = next(iter(result.get("Items", [])), None)
                        if value:
                            companion = publish(value)
                    previous_path = path
                except Exception as exc:
                    logging.debug("DanmaX metadata update unavailable: %s", type(exc).__name__)
        except Exception as exc:
            logging.warning("DanmaX setup unavailable: %s; video playback continues", type(exc).__name__)
            self._notify()
        finally:
            for path in files:
                try:
                    path.unlink(missing_ok=True)
                except OSError:
                    pass

    @staticmethod
    def _notify():
        # The video is already playing; notification runs only on companion failure.
        ctypes.windll.user32.MessageBoxW(None,
            "外挂弹幕未能启动，视频继续播放。请检查 DanmaX 配置后重新打开。",
            "DanmaX", 0x40 | 0x00040000)


class _SuccessfulProcess:
    def poll(self):
        return 0
