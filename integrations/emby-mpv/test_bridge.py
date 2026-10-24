import base64
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
import sys
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parent))
from danmax_bridge import DanmaxBridge, emby_address, process_stamp, safe_metadata


class BridgeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        spec = importlib.util.spec_from_file_location("handler", Path(__file__).with_name("mpv-url-handler.py"))
        cls.handler = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.handler)

    def test_existing_protocols_and_subtitles(self):
        url = "http://emby.test/videos/24065/stream?api_key=secret&DeviceId=browser"
        sub = "http://emby.test/subtitles/1?api_key=secret"
        encoded = base64.urlsafe_b64encode(url.encode()).decode().rstrip("=")
        encoded_sub = base64.urlsafe_b64encode(sub.encode()).decode().rstrip("=")
        for scheme in ("mpv", "mpv-handler", "mpv-handler-debug"):
            self.assertEqual(self.handler.decode_uri(scheme + "://play/" + encoded + "?subfile=" + encoded_sub), (url, sub))
        self.assertEqual(self.handler.decode_uri("mpv://" + url), (url, None))

    def test_no_secrets_in_descriptor(self):
        item = {"Id": "24065", "Name": "8月31日", "SeriesName": "魔法禁书目录", "Type": "Episode",
                "ParentIndexNumber": 2, "IndexNumber": 1, "Path": "http://stream.test/title.mkv?api_key=secret",
                "MediaSources": [{"Path": "http://stream.test/secret"}], "ApiKey": "secret"}
        metadata = safe_metadata(item, "server")
        self.assertEqual(metadata["fileName"], "title.mkv")
        self.assertNotIn("secret", json.dumps(metadata))
        self.assertEqual((metadata["season"], metadata["episode"]), (2, 1))

    def test_optional_launch_and_config_switch(self):
        with tempfile.TemporaryDirectory() as root:
            bridge = DanmaxBridge(root, "mpv.exe", Mock(), Mock())
            self.assertEqual(bridge.player_options(), [])
            Path(root, "danmax-bridge.json").write_text('{"enabled":true}', "utf-8")
            bridge = DanmaxBridge(root, "mpv.exe", Mock(), Mock())
            self.assertIn("autoload_for_url=no", bridge.player_options()[0])

    def test_real_process_identity(self):
        import os
        self.assertGreater(int(process_stamp(os.getpid())), 0)

    def test_reports_keep_existing_completion_threshold(self):
        # The bridge must not introduce any Emby write operation of its own.
        source = Path(__file__).with_name("danmax_bridge.py").read_text("utf-8")
        self.assertNotIn('"POST"', source)
        self.assertIn("0.90", Path(__file__).with_name("mpv-url-handler.py").read_text("utf-8"))

    def test_metadata_failure_does_not_stop_video(self):
        with tempfile.TemporaryDirectory() as root:
            bridge = DanmaxBridge(root, "mpv.exe", Mock(side_effect=OSError("offline")), Mock())
            bridge.config = {"enabled": True}
            child = Mock(); child.pid = 123
            with patch.object(bridge, "_notify") as notice:
                bridge._watch(child, "pipe", "http://emby.test/videos/1/stream?api_key=private", None, None, None, None)
            notice.assert_called_once()
            child.kill.assert_not_called()
            child.terminate.assert_not_called()


if __name__ == "__main__":
    unittest.main()
