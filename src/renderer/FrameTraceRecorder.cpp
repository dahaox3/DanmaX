#include "renderer/FrameTraceRecorder.h"
#include "renderer/DanmakuItem.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QQuickWindow>
#include <QSaveFile>
#include <QTextStream>
#include <QTimeZone>
#include <QUuid>
#include <chrono>
#include <mutex>
#include <vector>

namespace {
qint64 nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
constexpr size_t maxFrames = 65536;
struct Frame {
    FrameTraceRecorder::Context context;
    DanmakuItem::TracePoint point;
    qint64 begin{}, sync{}, render{}, rendered{}, swap{};
    int width{}, height{};
    double dpr{};
};
QString save(const QString& path, const QString& reason, qint64 unixMs, qint64 steadyNs, const std::vector<Frame>& rows) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return file.errorString();
    QTextStream out(&file);
    out.setLocale(QLocale::c());
    out.setRealNumberPrecision(14);
    out << "# schema=2\n# unix_ms=" << unixMs << "\n# steady_ns=" << steadyNs
        << "\n# pid=" << QCoreApplication::applicationPid() << "\n# end=" << reason << '\n';
    out << "frame,tick,media_generation,resets,playing,visible,max_fps,rate,position_s,width,height,dpr,"
           "active,id,snapshot,engine_x,draw_x,tick_ns,snapshot_ns,scene_ns,begin_ns,sync_ns,render_ns,render_end_ns,swap_ns,effective_max_fps\n";
    quint64 index = 0;
    for (const auto& f : rows) {
        const auto& c = f.context;
        const auto& p = f.point;
        out << ++index << ',' << c.tick << ',' << c.mediaGeneration << ',' << c.resets << ',' << int(c.playing)
            << ',' << int(c.visible) << ',' << c.maxFps << ',' << c.rate << ',' << c.position << ',' << f.width
            << ',' << f.height << ',' << f.dpr << ',' << p.active << ',' << p.id << ',' << p.snapshot
            << ',' << p.engineX << ',' << p.drawnX << ',' << c.tickNs << ',' << p.snapshotNs << ',' << p.sceneNs
            << ',' << f.begin << ',' << f.sync << ',' << f.render << ',' << f.rendered << ',' << f.swap
            << ',' << c.effectiveMaxFps << '\n';
    }
    out.flush();
    if (out.status() != QTextStream::Ok || !file.commit()) return file.errorString();
    return {};
}
}

struct FrameTraceRecorder::Capture {
    Frame current; // Only the window's direct scene-graph callbacks access this frame.
    std::vector<Frame> frames;
    std::mutex mutex; // Serializes the final swap callback with stopping from the GUI.
    bool closed{}, firstPlayingFrame{}, full{};
    qint64 unixMs = QDateTime::currentMSecsSinceEpoch(), steadyNs = nowNs();
};

FrameTraceRecorder::FrameTraceRecorder(const QString& dataDirectory)
    : directory_(QDir(dataDirectory).filePath("diagnostics")), marker_(QDir(directory_).filePath("next-frame-trace")),
      armed_(QFile::exists(marker_)) {
    deadline_.setSingleShot(true);
    deadline_.setInterval(60000);
    deadline_.setTimerType(Qt::PreciseTimer);
    connect(&deadline_, &QTimer::timeout, this, [this] { finish("duration"); });
    writer_.setMaxThreadCount(1);
}
FrameTraceRecorder::~FrameTraceRecorder() {
    finish("exit");
    writer_.waitForDone();
}
void FrameTraceRecorder::setStatus(const QString& status) {
    status_ = status;
    emit changed();
}
void FrameTraceRecorder::attach(QQuickWindow* window, DanmakuItem* item, std::function<Context()> context) {
    if (window_ != window || item_ != item) finish("window");
    window_ = window;
    item_ = item;
    context_ = std::move(context);
    if (armed_ && running_) start();
}
void FrameTraceRecorder::setEnabled(bool enabled) {
    if (enabled == this->enabled()) return;
    if (recording()) { finish("user"); return; }
    if (enabled) {
        QSaveFile marker(marker_);
        if (!QDir().mkpath(directory_) || !marker.open(QIODevice::WriteOnly) || !marker.commit()) {
            setStatus(QStringLiteral("无法创建诊断标记，请检查数据目录权限。")); return;
        }
    } else if (QFile::exists(marker_) && !QFile::remove(marker_)) {
        setStatus(QStringLiteral("无法取消诊断标记，请检查数据目录权限。")); return;
    }
    ++serial_;
    armed_ = enabled;
    setStatus(enabled ? QStringLiteral("等待弹幕开始播放。") : QString());
    if (enabled && running_) start();
}
void FrameTraceRecorder::setPlaybackRunning(bool running) {
    running_ = running;
    if (!running) { finish("stop"); return; }
    if (!recording() && QFile::exists(marker_)) { armed_ = true; start(); }
}
void FrameTraceRecorder::start() {
    if (!armed_ || recording() || !window_ || !item_) return;
    if (!QFile::remove(marker_)) {
        armed_ = QFile::exists(marker_);
        setStatus(QStringLiteral("无法领取本次逐帧诊断。")); return;
    }
    armed_ = false;
    const auto s = std::make_shared<Capture>();
    s->frames.reserve(maxFrames);
    capture_ = s;
    ++serial_;
    item_->enableFrameTrace(true);
    // Install extra signal handlers only for the recording. A new capture owns
    // a distinct state, including frames still in flight when playback stops.
    connections_ << connect(window_, &QQuickWindow::beforeFrameBegin, item_, [s] {
        s->current.begin = nowNs();
    }, Qt::DirectConnection);
    connections_ << connect(window_, &QQuickWindow::beforeSynchronizing, item_, [this,s] {
        auto& f = s->current;
        f.sync = nowNs();
        f.context = context_();
        f.width = window_->width(); f.height = window_->height();
        f.dpr = window_->effectiveDevicePixelRatio();
    }, Qt::DirectConnection);
    connections_ << connect(window_, &QQuickWindow::afterSynchronizing, item_, [this,s] {
        s->current.point = s->current.context.visible ? item_->synchronizedTracePoint() : DanmakuItem::TracePoint{};
    }, Qt::DirectConnection);
    connections_ << connect(window_, &QQuickWindow::beforeRendering, item_, [s] {
        s->current.render = nowNs();
    }, Qt::DirectConnection);
    connections_ << connect(window_, &QQuickWindow::afterRendering, item_, [s] {
        s->current.rendered = nowNs();
    }, Qt::DirectConnection);
    connections_ << connect(window_, &QQuickWindow::frameSwapped, item_, [this,s] {
        auto& f = s->current;
        f.swap = nowNs();
        std::lock_guard guard(s->mutex);
        if (s->closed || s->full) return;
        s->frames.push_back(f);
        if (!s->firstPlayingFrame && f.context.playing && f.context.visible) {
            s->firstPlayingFrame = true;
            QMetaObject::invokeMethod(this, [this,s] { if (capture_ == s) deadline_.start(); }, Qt::QueuedConnection);
        }
        if (s->frames.size() == maxFrames) {
            s->full = true;
            QMetaObject::invokeMethod(this, [this,s] { if (capture_ == s) finish("capacity"); }, Qt::QueuedConnection);
        }
        // A render-only submission retains the last synchronized coordinates.
        f.begin = f.sync = f.render = f.rendered = 0;
    }, Qt::DirectConnection);
    setStatus(QStringLiteral("正在采集，播放满 60 秒、停止播放或关闭开关时保存。"));
}
void FrameTraceRecorder::finish(const QString& reason) {
    if (!capture_) return;
    deadline_.stop();
    for (const auto& connection : connections_) disconnect(connection);
    connections_.clear();
    if (item_) item_->enableFrameTrace(false);
    const auto s = std::move(capture_);
    std::vector<Frame> frames;
    {
        std::lock_guard guard(s->mutex);
        s->closed = true;
        frames.swap(s->frames);
    }
    const auto name = "frames-" + QDateTime::fromMSecsSinceEpoch(s->unixMs, QTimeZone::UTC).toString("yyyyMMdd-HHmmss-zzz")
                    + '-' + QUuid::createUuid().toString(QUuid::WithoutBraces) + ".csv";
    const auto path = QDir(directory_).filePath(name);
    const auto serial = serial_;
    writer_.start([this, path, reason, serial, unixMs=s->unixMs, steadyNs=s->steadyNs, rows=std::move(frames)] {
        const auto error = save(path, reason, unixMs, steadyNs, rows);
        QMetaObject::invokeMethod(this, [this, path, error, serial] {
            if (serial == serial_) setStatus(error.isEmpty()
                ? QStringLiteral("已保存到 %1").arg(QDir::toNativeSeparators(path))
                : QStringLiteral("逐帧记录保存失败：%1").arg(error));
        }, Qt::QueuedConnection);
    });
    setStatus(QStringLiteral("正在保存逐帧记录…"));
}
