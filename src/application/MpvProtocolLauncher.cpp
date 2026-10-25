#include "application/MpvProtocolLauncher.h"
#include "infrastructure/MpvIntegration.h"
#include "infrastructure/ExternalSessionDescriptor.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSaveFile>
#include <QLockFile>
#include <QUrlQuery>
#include <QUuid>
#include <cmath>
#include <windows.h>

namespace {
QUrl origin(const QUrl& url) { return url.adjusted(QUrl::RemovePath | QUrl::RemoveQuery | QUrl::RemoveFragment | QUrl::RemoveUserInfo); }
QString itemId(const QUrl& url) {
    return QRegularExpression("/videos/([A-Za-z0-9_-]+)/", QRegularExpression::CaseInsensitiveOption).match(url.path()).captured(1);
}
QByteArray decode64(QString text) {
    text.replace('-', '+'); text.replace('_', '/');
    while (text.size() % 4) text += '=';
    return QByteArray::fromBase64(text.toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
}
QVariantMap metadata(const QJsonObject& item, const QString& server) {
    QString path = item.value("Path").toString();
    if (path.contains("://")) path = QUrl(path).path();
    return {{"serverId", server}, {"itemId", item.value("Id").toString()}, {"seriesId", item.value("SeriesId").toString()},
        {"seasonId", item.value("SeasonId").toString()}, {"type", item.value("Type").toString()},
        {"title", item.value("Name").toString().left(1024)}, {"seriesTitle", item.value("SeriesName").toString().left(1024)},
        {"season", item.value("ParentIndexNumber").toInt()}, {"episode", item.value("IndexNumber").toInt()},
        {"year", item.value("ProductionYear").toInt()}, {"fileName", path.replace('\\', '/').section('/', -1).left(1024)}};
}
}
MpvLaunchRequest MpvLaunchRequest::decode(const QString& original) {
    MpvLaunchRequest result;
    if (original.size() > 65536 || original.contains(QChar::Null) || original.contains('\r') || original.contains('\n')) {
        result.error = QStringLiteral("网页播放请求格式无效"); return result;
    }
    QString uri = original;
    for (const auto& scheme : {"mpv-handler-debug://", "mpv-handler://", "mpv://"})
        if (uri.startsWith(QLatin1String(scheme), Qt::CaseInsensitive)) { uri = QUrl::fromPercentEncoding(uri.mid(qstrlen(scheme)).toUtf8()); break; }
    if (uri.startsWith("play/", Qt::CaseInsensitive)) {
        const auto rest = uri.mid(5); const auto split = rest.indexOf('?');
        auto payload = split < 0 ? rest : rest.left(split);
        while (payload.endsWith('/')) payload.chop(1);
        const auto bytes = decode64(payload);
        if (bytes.isEmpty() || QString::fromUtf8(bytes).toUtf8() != bytes) { result.error = QStringLiteral("网页播放地址编码无效"); return result; }
        result.video = QUrl::fromEncoded(bytes, QUrl::StrictMode);
        if (split >= 0) {
            const auto sub = QUrlQuery(rest.mid(split + 1)).queryItemValue("subfile", QUrl::FullyDecoded);
            if (!sub.isEmpty()) {
                const auto decoded = decode64(sub);
                result.subtitle = QUrl::fromEncoded(decoded, QUrl::StrictMode);
                if (decoded.isEmpty() || !result.subtitle.isValid() || (result.subtitle.scheme() != "http" && result.subtitle.scheme() != "https"))
                    result.error = QStringLiteral("字幕地址无效");
            }
        }
    } else result.video = QUrl(uri, QUrl::StrictMode);
    if (!result.video.isValid() || result.video.host().isEmpty() || (result.video.scheme() != "http" && result.video.scheme() != "https"))
        result.error = QStringLiteral("网页播放仅接受 HTTP 或 HTTPS 视频地址");
    return result;
}
struct MpvProtocolLauncher::Context {
    QUrl url, base;
    QString token, user, item, source, playSession{QUuid::createUuid().toString(QUuid::Id128)};
    QJsonObject metadata;
    qint64 resume{}, position{}, duration{};
    bool ready{}, gotSample{}, paused{}, reported{}, finalizing{};
    QJsonObject body() const {
        return {{"ItemId", item}, {"MediaSourceId", source}, {"PositionTicks", position}, {"IsPaused", paused},
            {"CanSeek", true}, {"PlayMethod", "DirectStream"}, {"PlaySessionId", playSession},
            {"DeviceId", "mpv-handler-windows"}, {"Client", "mpv-handler"}, {"DeviceName", "mpv Windows"},
            {"IsDirectStream", true}, {"EventName", "TimeUpdate"}};
    }
};
MpvProtocolLauncher::MpvProtocolLauncher(QString data, QString cache, Options options, QObject* parent)
    : QObject(parent), data_(std::move(data)), cache_(std::move(cache)), options_(options) {
    process_.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) { args->flags |= CREATE_NO_WINDOW; });
    connect(&process_, &QProcess::started, this, [this] {
        claimPresentation();
        const auto d = descriptor();
        const auto issue = player_.bind(d);
        if (!issue.isEmpty()) log(QStringLiteral("IPC 身份校验失败，视频继续播放"));
        else player_.select(ExternalSessionDescriptor::sessionId(d), 1);
        progress_.start(options_.progressInterval); publish(); emit playbackStarted(process_.processId());
    });
    connect(&process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, [this] { processEnded(); });
    connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            ended_ = true; player_.detach(); progress_.stop();
            emit this->error(QStringLiteral("mpv 未能启动，请重新选择程序位置")); emit finished(3);
        }
    });
    connect(&player_, &MediaPlaybackBackend::sampleReceived, this, &MpvProtocolLauncher::observe);
    connect(&player_, &MpvIpcBackend::contentChanged, this, &MpvProtocolLauncher::changedFile);
    connect(&progress_, &QTimer::timeout, this, [this] {
        const auto c = context_;
        if (c && c->gotSample && c->reported && !c->finalizing)
            request(c, "/Sessions/Playing/Progress", "POST", c->body(), [](bool, QJsonObject) {});
    });
}
void MpvProtocolLauncher::log(const QString& text) const {
    QFile file(QDir(data_).filePath("mpv-launcher.log"));
    if (file.size() > 1024 * 1024) { file.remove(); file.setFileName(QDir(data_).filePath("mpv-launcher.log")); }
    if (file.open(QIODevice::Append)) file.write((QDateTime::currentDateTime().toString(Qt::ISODate) + ' ' + text + '\n').toUtf8());
}
void MpvProtocolLauncher::request(const std::shared_ptr<Context>& c, const QString& endpoint, const QString& method, QJsonObject body, Reply callback) {
    QUrl url(c->base.toString(QUrl::FullyEncoded) + "/emby" + endpoint);
    QNetworkRequest request(url); request.setTransferTimeout(options_.networkTimeout);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setRawHeader("X-Emby-Token", c->token.toUtf8()); request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("X-Emby-Client", "mpv-handler"); request.setRawHeader("X-Emby-Device-Id", "mpv-handler-windows");
    request.setRawHeader("X-Emby-Device-Name", "mpv Windows"); request.setRawHeader("X-Emby-Client-Version", "1.0");
    auto* reply = method == "GET" ? network_.get(request) : network_.post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    QTimer::singleShot(options_.networkTimeout, reply, [reply] { if (!reply->isFinished()) reply->abort(); });
    auto bytes = std::make_shared<QByteArray>(); reply->setReadBufferSize(65536);
    connect(reply, &QIODevice::readyRead, this, [reply, bytes] { *bytes += reply->readAll(); if (bytes->size() > 4 * 1024 * 1024) reply->abort(); });
    connect(reply, &QNetworkReply::finished, this, [this, reply, bytes, callback = std::move(callback)] {
        *bytes += reply->readAll(); const auto code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool ok = reply->error() == QNetworkReply::NoError && code >= 200 && code < 300 && bytes->size() <= 4 * 1024 * 1024;
        if (!ok) log(QStringLiteral("Emby 接口失败（HTTP %1）；未记录地址或凭据").arg(code));
        const auto doc = QJsonDocument::fromJson(*bytes);
        const auto object = doc.isArray() ? QJsonObject{{"_array", doc.array()}} : doc.object();
        reply->deleteLater(); callback(ok, object);
    });
}
void MpvProtocolLauncher::start(const QString& uri) {
    if (started_) return; started_ = true;
    input_ = MpvLaunchRequest::decode(uri);
    MpvIntegration settings(data_, QCoreApplication::applicationFilePath());
    mpv_ = settings.mpvPath(); automatic_ = settings.enabled();
    const auto issue = input_.error.isEmpty() ? MpvIntegration::validateExecutable(mpv_) : input_.error;
    if (!issue.isEmpty() || !settings.error().isEmpty()) {
        emit error(issue.isEmpty() ? settings.error() : issue); emit finished(2); return;
    }
    session_ = QUuid::createUuid().toString(QUuid::Id128);
    pipe_ = QStringLiteral("\\\\.\\pipe\\mpv-url-handler-") + session_;
    auto c = std::make_shared<Context>(); c->url = input_.video; c->base = origin(c->url); c->item = itemId(c->url);
    c->token = QUrlQuery(c->url).queryItemValue("api_key", QUrl::FullyDecoded);
    if (c->token.size() > 2048 || c->token.contains('\r') || c->token.contains('\n') || c->token.contains(QChar::Null)) c->token.clear();
    c->source = QUrlQuery(c->url).queryItemValue("MediaSourceId", QUrl::FullyDecoded); context_ = c;
    if (c->item.isEmpty() || c->token.isEmpty()) { c->ready = true; launch(0); return; }
    serverInfoReady_ = false;
    request(c, "/System/Info/Public", "GET", {}, [this, c](bool ok, QJsonObject info) {
        serverInfoReady_ = true;
        if (!ended_ && context_) { if (ok && context_->base == c->base) serverId_ = info.value("Id").toString(); publish(); }
    });
    prepare(c, true);
}
void MpvProtocolLauncher::prepare(const std::shared_ptr<Context>& c, bool first) {
    const auto load = [this, c, first] {
        const auto prefix = c->user.isEmpty() ? QString() : "/Users/" + QString::fromLatin1(QUrl::toPercentEncoding(c->user));
        request(c, prefix + "/Items?Ids=" + c->item + "&Fields=Path,ProviderIds", "GET", {}, [this, c, first](bool ok, QJsonObject root) {
            if (ended_ || context_ != c) return;
            const auto items = root.value("Items").toArray();
            if (ok && !items.isEmpty()) c->metadata = items.first().toObject();
            c->resume = qint64(c->metadata.value("UserData").toObject().value("PlaybackPositionTicks").toDouble());
            c->duration = qint64(c->metadata.value("RunTimeTicks").toDouble()); c->ready = true;
            if (first) launch(c->resume); else publish();
        });
    };
    if (!first && !c->user.isEmpty()) { load(); return; }
    request(c, "/Sessions", "GET", {}, [this, c, load](bool ok, QJsonObject root) {
        if (ended_ || context_ != c) return;
        if (ok) {
            const auto browser = QUrlQuery(c->url).queryItemValue("DeviceId", QUrl::FullyDecoded);
            QJsonObject chosen;
            for (const auto& entry : root.value("_array").toArray()) {
                const auto session = entry.toObject();
                if (session.value("UserId").toString().isEmpty()) continue;
                if (chosen.isEmpty()) chosen = session;
                if (!browser.isEmpty() && session.value("DeviceId").toString() == browser) { chosen = session; break; }
            }
            const auto user = chosen.value("UserId").toString();
            if (QRegularExpression("^[A-Za-z0-9_-]{1,128}$").match(user).hasMatch()) c->user = user;
        }
        load();
    });
}
void MpvProtocolLauncher::launch(qint64 resume) {
    if (launched_ || ended_) return; launched_ = true;
    QStringList args{"--force-window=immediate", "--input-ipc-server=" + pipe_};
    if (resume > 0) args << "--start=" + QString::number(resume / 10000000);
    if (!input_.subtitle.isEmpty()) args << "--sub-file=" + input_.subtitle.toString(QUrl::FullyEncoded);
    if (automatic_) args << "--script-opts-append=uosc_danmaku-auto_load=no,uosc_danmaku-autoload_for_url=no,uosc_danmaku-autoload_local_danmaku=no";
    args << "--" << input_.video.toString(QUrl::FullyEncoded);
    process_.setWorkingDirectory(QFileInfo(mpv_).absolutePath());
    process_.setStandardOutputFile(QProcess::nullDevice()); process_.setStandardErrorFile(QProcess::nullDevice());
    process_.start(mpv_, args); log(QStringLiteral("启动本次独立 mpv 会话"));
}
QVariantMap MpvProtocolLauncher::descriptor() const {
    return {{"version", 1}, {"sessionId", session_}, {"revision", revision_ + 1}, {"ipcPipe", pipe_},
        {"process", QVariantMap{{"pid", process_.processId()}, {"executable", mpv_},
            {"created", ExternalSessionDescriptor::processStamp(process_.processId(), mpv_)}}},
        {"emby", metadata(context_ ? context_->metadata : QJsonObject{}, serverId_)}};
}
void MpvProtocolLauncher::publish() {
    if (!automatic_ || ended_ || !serverInfoReady_ || process_.state() != QProcess::Running || !context_ || !context_->ready) return;
    QFile lease(QDir(cache_).filePath("external-sessions/latest.json"));
    if (!lease.open(QIODevice::ReadOnly) || QJsonDocument::fromJson(lease.readAll()).object().value("sessionId") != session_) return;
    auto d = descriptor(); ++revision_;
    const auto directory = QDir(cache_).filePath("external-sessions");
    if (!QDir().mkpath(directory)) { log(QStringLiteral("会话目录不可写，视频继续播放")); return; }
    const auto path = QDir(directory).filePath(session_ + '-' + QString::number(revision_) + ".json");
    QSaveFile file(path); const auto bytes = QJsonDocument(QJsonObject::fromVariantMap(d)).toJson();
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) { log(QStringLiteral("会话文件写入失败，视频继续播放")); return; }
    descriptorFiles_ << path; emit descriptorPublished(path);
    if (options_.spawnCompanion && !QProcess::startDetached(QCoreApplication::applicationFilePath(),
        {"--external-session", path, "--data-dir", data_, "--cache-dir", cache_})) log(QStringLiteral("外挂未能启动，视频继续播放"));
}
void MpvProtocolLauncher::claimPresentation() {
    if (ended_ || !automatic_) return;
    const auto dir = QDir(cache_).filePath("external-sessions");
    if (!QDir().mkpath(dir)) return;
    QLockFile lock(QDir(dir).filePath("latest.lock"));
    if (!lock.tryLock(0)) { QTimer::singleShot(20, this, [this] { claimPresentation(); if (revision_ == 0) publish(); }); return; }
    const auto created = ExternalSessionDescriptor::processStamp(process_.processId(), mpv_);
    QFile previous(QDir(dir).filePath("latest.json"));
    if (previous.open(QIODevice::ReadOnly)) {
        const auto old = QJsonDocument::fromJson(previous.readAll()).object(); previous.close();
        if (old.value("created").toString().toULongLong() > created.toULongLong()) return;
    }
    QSaveFile file(QDir(dir).filePath("latest.json"));
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(QJsonObject{{"sessionId", session_}, {"created", created}}).toJson()); file.commit();
    }
}
void MpvProtocolLauncher::observe(const MediaSample& s) {
    const auto c = context_;
    if (!c || !s.found || (s.playback != MediaSample::Playback::Playing && s.playback != MediaSample::Playback::Paused)
        || !std::isfinite(s.position) || s.position < 0 || c->finalizing) return;
    c->gotSample = true; c->position = qint64(s.position * 10000000); c->paused = !s.playing;
    if (std::isfinite(s.duration) && s.duration > 0) c->duration = std::max(c->duration, qint64(s.duration * 10000000));
    if (!c->reported && !c->user.isEmpty()) {
        c->reported = true; request(c, "/Sessions/Playing", "POST", c->body(), [](bool, QJsonObject) {});
    }
}
void MpvProtocolLauncher::changedFile(const QString& path) {
    if (ended_ || !context_) return;
    const QUrl url(path); const auto id = itemId(url);
    if (url == context_->url) return;
    if (id == context_->item && origin(url) == context_->base && QUrlQuery(url).queryItemValue("MediaSourceId") == context_->source) return;
    const auto old = context_; finalize(old);
    auto c = std::make_shared<Context>(); c->url = url; c->base = origin(url); c->item = id;
    c->source = QUrlQuery(url).queryItemValue("MediaSourceId", QUrl::FullyDecoded); context_ = c;
    // Never reuse credentials on a redirected or unrelated origin.
    if (c->base == old->base && !id.isEmpty()) { c->token = old->token; c->user = old->user; prepare(c, false); }
    else { c->ready = true; publish(); }
}
void MpvProtocolLauncher::finalize(const std::shared_ptr<Context>& c) {
    if (!c || c->finalizing || c->user.isEmpty() || c->item.isEmpty()) return;
    c->finalizing = true; ++finalizations_; c->paused = true;
    const auto done = [this] { --finalizations_; maybeFinished(); };
    const auto cleanup = [this, c, done] {
        QTimer::singleShot(options_.cleanupDelay, this, [this, c, done] {
            const auto endpoint = "/Users/" + c->user + "/PlayedItems/" + c->item + "/Delete";
            request(c, endpoint, "POST", {}, [this, c, done](bool, QJsonObject) {
                if (!c->gotSample) { done(); return; }
                request(c, "/Sessions/Playing", "POST", c->body(), [this, c, done](bool, QJsonObject) {
                    request(c, "/Sessions/Playing/Stopped", "POST", c->body(), [done](bool, QJsonObject) { done(); });
                });
            });
        });
    };
    if (!c->gotSample) { cleanup(); return; }
    request(c, "/Sessions/Playing/Stopped", "POST", c->body(), [this, c, done, cleanup](bool, QJsonObject) {
        if (c->duration > 0 && c->position >= qint64(c->duration * .9)) {
            request(c, "/Users/" + c->user + "/PlayedItems/" + c->item, "POST", {}, [done](bool, QJsonObject) { done(); });
        } else cleanup();
    });
}
void MpvProtocolLauncher::processEnded() {
    if (ended_) return; ended_ = true; progress_.stop(); player_.detach();
    for (const auto& path : descriptorFiles_) QFile::remove(path);
    finalize(context_); maybeFinished();
}
void MpvProtocolLauncher::maybeFinished() { if (ended_ && finalizations_ == 0) emit finished(0); }
