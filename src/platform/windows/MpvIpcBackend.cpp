#include "platform/windows/MpvIpcBackend.h"
#include "infrastructure/ExternalSessionDescriptor.h"
#include "platform/windows/MediaMonitor.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QCryptographicHash>
#include <windows.h>
#include <cmath>

namespace {
const QStringList properties{"time-pos", "duration", "pause", "speed", "path", "idle-active"};
}
MpvIpcBackend::MpvIpcBackend(QObject* parent) : MediaPlaybackBackend(parent) {
    timer_.setInterval(100);
    connect(&timer_, &QTimer::timeout, this, &MpvIpcBackend::maintain);
    connect(&socket_, &QLocalSocket::readyRead, this, &MpvIpcBackend::receive);
    connect(&socket_, &QLocalSocket::connected, this, [this] {
        ULONG pid{};
        if (!GetNamedPipeServerProcessId(reinterpret_cast<HANDLE>(socket_.socketDescriptor()), &pid)
            || pid != descriptor_.value("process").toMap().value("pid").toUInt()) {
            socket_.abort(); emit errorOccurred(QStringLiteral("mpv IPC 管道不属于指定播放器")); return;
        }
        for (int i = 0; i < properties.size(); ++i) {
            const auto message = QJsonObject{{"command", QJsonArray{"observe_property", i + 1, properties[i]}}};
            socket_.write(QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n');
        }
        lastQuery_ = 0;
    });
}
MpvIpcBackend::~MpvIpcBackend() { detach(); }
QString MpvIpcBackend::bind(const QVariantMap& d) {
    const auto error = ExternalSessionDescriptor::validate(d);
    if (!error.isEmpty()) return error;
    if (descriptor_.value("process") != d.value("process") || descriptor_.value("ipcPipe") != d.value("ipcPipe")) {
        detach(); descriptor_ = d; id_ = ExternalSessionDescriptor::sessionId(d); exited_ = false;
        const auto p = d.value("process").toMap();
        processHandle_ = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, p.value("pid").toUInt());
        FILETIME created{}, exit{}, kernel{}, user{};
        if (!processHandle_ || !GetProcessTimes(processHandle_, &created, &exit, &kernel, &user)
            || QString::number((quint64(created.dwHighDateTime) << 32) | created.dwLowDateTime) != p.value("created").toString()) {
            detach(); return QStringLiteral("目标播放器已经退出");
        }
    } else descriptor_ = d;
    timer_.start(); maintain();
    return {};
}
void MpvIpcBackend::detach() {
    timer_.stop(); socket_.abort(); descriptor_.clear(); id_.clear(); selected_.clear();
    properties_.clear(); buffer_.clear(); path_.clear(); latest_ = {};
    observed_ = lastQuery_ = 0; validPosition_ = false; window_ = 0; rect_ = {}; windowVisible_ = false;
    if (processHandle_) { CloseHandle(processHandle_); processHandle_ = nullptr; }
}
void MpvIpcBackend::select(const QString& id, quint64 generation) {
    selected_ = accepts(id) ? id : QString(); generation_ = generation;
    if (!selected_.isEmpty()) publish();
}
void MpvIpcBackend::requestPlayback(MediaSample, bool, quint64 request) {
    emit playbackControlFinished(request, false, QStringLiteral("请在 mpv 中控制视频播放"));
}
void MpvIpcBackend::requestSeek(MediaSample, double, quint64 request) {
    emit seekFinished(request, false, QStringLiteral("请在 mpv 中跳转视频"));
}
void MpvIpcBackend::command(const QString& property, int request) {
    socket_.write(QJsonDocument(QJsonObject{{"command", QJsonArray{"get_property", property}}, {"request_id", request}})
                      .toJson(QJsonDocument::Compact) + '\n');
}
void MpvIpcBackend::maintain() {
    if (!processHandle_ || WaitForSingleObject(processHandle_, 0) != WAIT_TIMEOUT) {
        if (!exited_) {
            exited_ = true; socket_.abort(); timer_.stop(); windowVisible_ = false; observed_ = 0;
            publish(); emit geometryChanged(); emit processExited();
        }
        return;
    }
    locateWindow();
    if (socket_.state() == QLocalSocket::UnconnectedState) {
        socket_.connectToServer(descriptor_.value("ipcPipe").toString());
    }
    const double now = MediaMonitor::monotonicSeconds();
    if (socket_.state() == QLocalSocket::ConnectedState && now - lastQuery_ >= .25) {
        lastQuery_ = now;
        for (int i = 0; i < properties.size(); ++i) command(properties[i], 100 + i);
    }
    if (observed_ > 0 && now - observed_ > .75) { observed_ = 0; publish(); }
}
void MpvIpcBackend::receive() {
    buffer_ += socket_.readAll();
    if (buffer_.size() > 1024 * 1024) { buffer_.clear(); socket_.abort(); return; }
    bool updated = false;
    while (buffer_.contains('\n')) {
        const int end = static_cast<int>(buffer_.indexOf('\n'));
        const auto object = QJsonDocument::fromJson(buffer_.left(end)).object(); buffer_.remove(0, end + 1);
        QString property;
        if (object.value("event") == "property-change") property = object.value("name").toString();
        else {
            const int index = object.value("request_id").toInt() - 100;
            if (index >= 0 && index < properties.size() && object.value("error") == "success") property = properties[index];
        }
        if (property.isEmpty()) continue;
        const auto value = object.value("data");
        properties_[property] = value.toVariant();
        if (property == "time-pos") {
            validPosition_ = value.isDouble() && std::isfinite(value.toDouble()) && value.toDouble() >= 0;
            observed_ = MediaMonitor::monotonicSeconds(); updated = true;
        }
        if (property == "path" && value.isString() && !value.toString().isEmpty() && path_ != value.toString()) {
            path_ = value.toString(); ++contentToken_; emit contentChanged(path_); updated = true;
        }
        if (property == "pause" || property == "idle-active") updated = true;
    }
    if (updated) publish();
}
void MpvIpcBackend::publish() {
    if (selected_.isEmpty()) return;
    MediaSample s;
    s.id = id_; s.generation = generation_; s.sessionToken = contentToken_;
    s.title = descriptor_.value("emby").toMap().value("title").toString();
    s.identity = QString::number(contentToken_);
    // A URL can contain Emby/115 credentials. Only expose its identity digest.
    s.mediaUri = QString::fromLatin1(QCryptographicHash::hash(path_.toUtf8(), QCryptographicHash::Sha256).toHex());
    s.observedAt = MediaMonitor::monotonicSeconds();
    bool complete = true;
    for (const auto& property : properties) complete = complete && properties_.contains(property);
    s.found = complete && !exited_ && observed_ > 0 && s.observedAt - observed_ < .75;
    s.position = properties_.value("time-pos").toDouble(); s.duration = properties_.value("duration").toDouble();
    s.rate = properties_.value("speed", 1).toDouble();
    const bool idle = properties_.value("idle-active", true).toBool() || !validPosition_;
    s.playing = s.found && !idle && !properties_.value("pause", true).toBool();
    s.playback = exited_ ? MediaSample::Playback::Closed : !s.found ? MediaSample::Playback::Unknown
        : idle ? MediaSample::Playback::Stopped : s.playing ? MediaSample::Playback::Playing : MediaSample::Playback::Paused;
    latest_ = s; emit sampleReceived(s);
}
void MpvIpcBackend::locateWindow() {
    struct Scan { DWORD pid; HWND window{}; } scan{descriptor_.value("process").toMap().value("pid").toUInt()};
    EnumWindows([](HWND window, LPARAM context) -> BOOL {
        auto& scan = *reinterpret_cast<Scan*>(context); DWORD pid{}; GetWindowThreadProcessId(window, &pid);
        if (pid == scan.pid && IsWindowVisible(window) && !GetWindow(window, GW_OWNER)) {
            scan.window = window; return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&scan));
    QRect rect; const bool visible = scan.window && !IsIconic(scan.window);
    if (visible) {
        RECT client{}; POINT origin{};
        if (GetClientRect(scan.window, &client) && ClientToScreen(scan.window, &origin))
            rect = QRect(origin.x, origin.y, client.right, client.bottom);
    }
    const auto window = reinterpret_cast<quintptr>(scan.window);
    if (rect != rect_ || window != window_ || visible != windowVisible_) {
        rect_ = rect; window_ = window; windowVisible_ = visible; emit geometryChanged();
    }
}
bool MpvIpcBackend::foreground() const {
    const auto foreground = GetAncestor(GetForegroundWindow(), GA_ROOT);
    return windowVisible_ && foreground == reinterpret_cast<HWND>(window_);
}
QString MpvIpcBackend::screenName() const {
    MONITORINFOEXW info{}; info.cbSize = sizeof(info);
    return GetMonitorInfoW(MonitorFromWindow(reinterpret_cast<HWND>(window_), MONITOR_DEFAULTTONEAREST), &info)
        ? QString::fromWCharArray(info.szDevice) : QString();
}
QRect MpvIpcBackend::nativeScreenRect() const {
    MONITORINFO info{}; info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(MonitorFromWindow(reinterpret_cast<HWND>(window_), MONITOR_DEFAULTTONEAREST), &info)) return {};
    return QRect(info.rcMonitor.left, info.rcMonitor.top, info.rcMonitor.right - info.rcMonitor.left, info.rcMonitor.bottom - info.rcMonitor.top);
}
