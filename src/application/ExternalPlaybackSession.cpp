#include "application/ExternalPlaybackSession.h"
#include "application/AppController.h"
#include "infrastructure/ExternalSessionDescriptor.h"
#include "infrastructure/MediaTitle.h"
#include "platform/windows/MpvIpcBackend.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QScopedValueRollback>
#include <QUrl>
#include <QRegularExpression>
#include <cmath>

ExternalPlaybackSession::ExternalPlaybackSession(const QString& data, const QString& cache, AppController* app)
    : QObject(app), app_(app), library_(cache, this), data_(data), cache_(cache) {
    library_.setFreshDownloads(true); library_.setAutomaticSelection(false); library_.setExtendedIdentifiers(true);
    QFile file(QDir(data_).filePath("external-associations.json"));
    if (file.open(QIODevice::ReadOnly) && file.size() < 4 * 1024 * 1024)
        associations_ = QJsonDocument::fromJson(file.readAll()).object().toVariantMap();
    connect(&library_, &DanmakuLibrary::changed, this, [this] {
        const auto epoch = epoch_;
        QTimer::singleShot(0, this, [this, epoch] { if (epoch == epoch_) advance(); });
        emit changed();
    });
    connect(&library_, &DanmakuLibrary::sourceReady, this, &ExternalPlaybackSession::accept);
    connect(app_->monitor_.mpv(), &MpvIpcBackend::processExited, this, [this] {
        if (!active_) return;
        deactivate();
        if (app_->coordinator_.owner() == PlaybackCoordinator::External) app_->handoff(PlaybackCoordinator::None);
        emit ended();
    });
    connect(app_->monitor_.mpv(), &MpvIpcBackend::geometryChanged, app_, [this] {
        if (!active_) return;
        app_->updateWindow(); app_->updatePresentationVisibility(true);
    });
    connect(app_->monitor_.mpv(), &MpvIpcBackend::contentChanged, this, &ExternalPlaybackSession::contentChanged);
}
QString ExternalPlaybackSession::title() const {
    if (emby_.value("seriesTitle").toString().isEmpty()) return emby_.value("title").toString();
    return emby_.value("seriesTitle").toString() + QStringLiteral(" S%1E%2 · ")
        .arg(emby_.value("season").toInt(), 2, 10, QLatin1Char('0'))
        .arg(emby_.value("episode").toInt(), 2, 10, QLatin1Char('0')) + emby_.value("title").toString();
}
QString ExternalPlaybackSession::exactKey() const {
    if (emby_.value("serverId").toString().isEmpty() || emby_.value("itemId").toString().isEmpty()) return {};
    return "item:" + emby_.value("serverId").toString() + ':' + emby_.value("itemId").toString();
}
QString ExternalPlaybackSession::seasonKey() const {
    if (emby_.value("serverId").toString().isEmpty() || emby_.value("seriesId").toString().isEmpty()) return {};
    return "season:" + emby_.value("serverId").toString() + ':' + emby_.value("seriesId").toString()
        + ':' + emby_.value("season").toString();
}
QString ExternalPlaybackSession::searchTitle() const {
    auto title = emby_.value("seriesTitle").toString();
    if (title.isEmpty()) return emby_.value("title").toString();
    const int season = emby_.value("season").toInt();
    if (season > 0) title += QStringLiteral(" 第%1季").arg(season);
    return title;
}
bool ExternalPlaybackSession::loadProfile() {
    QFile file(QDir(data_).filePath("external-player.json"));
    if (!file.open(QIODevice::ReadOnly) || file.size() > 64 * 1024) return false;
    const auto config = QJsonDocument::fromJson(file.readAll()).object();
    if (!config.value("enabled").toBool()) return false;
    const auto misaka = config.value("misaka").toObject();
    const auto base = normalizeDanmakuServer(misaka.value("url").toString());
    const auto token = misaka.value("token").toString();
    if (base.isEmpty() || token.isEmpty() || token.size() > 512) return false;
    alias_ = base + "/profiles/emby";
    library_.setPrivateEndpoint(alias_, base + "/api/v1/" + QString::fromLatin1(QUrl::toPercentEncoding(token)));
    library_.setServers({alias_});
    return true;
}
QString ExternalPlaybackSession::open(const QString& file) {
    QString error; const auto d = ExternalSessionDescriptor::read(file, error);
    if (!error.isEmpty()) return error;
    const auto created = d.value("process").toMap().value("created").toULongLong();
    const auto revision = d.value("revision").toLongLong();
    if (created < newestProcess_ || (created == newestProcess_ && revision <= revision_)) return {};
    // Reject obsolete launches even after the newest player has closed.
    newestProcess_ = created; revision_ = revision;
    const bool sameProcess = descriptor_.value("process") == d.value("process");
    const auto oldPath = contentPath_;
    app_->handoff(PlaybackCoordinator::External);
    descriptor_ = d; emby_ = d.value("emby").toMap();
    bound_ = ExternalSessionDescriptor::sessionId(d);
    active_ = true; stopped_ = cancelled_ = false; sample_ = {}; cacheTried_ = false; metadata_.clear();
    contentPath_ = sameProcess ? oldPath : QString(); contentReady_ = true;
    error = app_->monitor_.mpv()->bind(d);
    if (!error.isEmpty()) { deactivate(); return error; }
    app_->bindMedia(bound_);
    if (!loadProfile()) { offer(QStringLiteral("自动弹幕未配置，请检查 external-player.json 后重试")); return {}; }
    resolve(); emit changed();
    return {};
}
void ExternalPlaybackSession::deactivate() {
    ++epoch_; active_ = contentReady_ = false; phase_ = Idle;
    library_.cancel(); metadata_.clear(); sample_ = {};
    app_->monitor_.mpv()->detach(); emit changed();
}
void ExternalPlaybackSession::resolve() {
    if (!active_ || !contentReady_) return;
    error_.clear(); loadError_.clear(); cacheTried_ = false;
    const auto exact = associations_.value(exactKey()).toMap();
    const auto series = associations_.value(seasonKey()).toMap();
    if (!exact.isEmpty() && exact.value("server") == alias_) {
        metadata_ = exact; phase_ = Loading; library_.loadKnownEpisode(exact); return;
    }
    if (!series.isEmpty() && series.value("server") == alias_ && emby_.value("episode").toInt() > 0) {
        const auto knownEpisodes = series.value("episodes").toMap();
        const auto number = QString::number(emby_.value("episode").toInt());
        if (knownEpisodes.contains(number)) {
            auto episode = series; episode.remove("episodes");
            const auto selected = knownEpisodes.value(number).toMap();
            episode["episodeId"] = selected.value("id"); episode["episodeTitle"] = selected.value("title");
            episode["episodeNumber"] = emby_.value("episode");
            metadata_ = episode; phase_ = Loading; library_.loadKnownEpisode(episode); return;
        }
        metadata_ = series; phase_ = Episodes;
        library_.loadKnownAnime(series, emby_.value("episode").toInt()); return;
    }
    phase_ = Search; status_ = QStringLiteral("正在匹配 ") + title();
    library_.searchAnime(searchTitle());
}
void ExternalPlaybackSession::offer(const QString& reason) {
    if (!active_) return;
    phase_ = Choice; status_ = reason; error_ = loadError_.isEmpty() ? library_.error() : loadError_;
    emit changed(); emit showChoice();
}
void ExternalPlaybackSession::advance() {
    if (!active_ || advancing_ || library_.busy() || phase_ == Idle || phase_ == Ready) return;
    QScopedValueRollback guard(advancing_, true);
    if (!library_.error().isEmpty()) {
        if (loadError_.isEmpty()) loadError_ = library_.error();
        if (!cacheTried_ && !metadata_.value("episodeId").toString().isEmpty()) {
            cacheTried_ = true; phase_ = Cached;
            library_.loadCached(danmakuCacheKey(alias_, metadata_.value("episodeId").toLongLong())); return;
        }
        if (phase_ != Choice) offer(QStringLiteral("弹幕加载失败，视频继续播放。可以重试或手动选源。"));
        return;
    }
    if (phase_ == Search) {
        QString match; int count{};
        const auto requested = canonicalAnimeTitle(searchTitle());
        const bool movie = emby_.value("type").toString() == "Movie";
        if (!movie && emby_.value("season").toInt() <= 0) {
            offer(QStringLiteral("特别篇或缺少季度信息，请确认对应作品和分集。")); return;
        }
        for (const auto& entry : library_.animes()) {
            const auto item = entry.toMap();
            const auto candidate = canonicalAnimeTitle(item.value("matchTitle", item.value("title")).toString());
            bool same = candidate == requested;
            // Season 1 is frequently implicit. Never apply this shortcut to S00/S02+.
            if (!movie && emby_.value("season").toInt() == 1)
                same = same || candidate == canonicalAnimeTitle(emby_.value("seriesTitle").toString());
            if (same) { match = item.value("id").toString(); ++count; }
        }
        if (count != 1) { offer(QStringLiteral("请确认作品、季度和弹幕来源；确认后会记住关联。")); return; }
        phase_ = Episodes; library_.selectAnime(match); return;
    }
    if (phase_ == Episodes) {
        QString match; int count{};
        const bool movie = emby_.value("type").toString() == "Movie";
        const auto number = emby_.value("episode").toDouble();
        for (const auto& entry : library_.episodes()) {
            const auto item = entry.toMap();
            const auto ep = danmakuEpisodeNumber(item.value("title").toString(), item.value("number").toDouble());
            if ((movie && library_.episodes().size() == 1) || (number > 0 && std::abs(ep - number) < .0001)) {
                match = item.value("id").toString(); ++count;
            }
        }
        if (count != 1) { offer(QStringLiteral("未能唯一确认集数，请手动选择当前分集。")); return; }
        if (metadata_.contains("sourcePrefix") && !match.startsWith(metadata_.value("sourcePrefix").toString())) {
            offer(QStringLiteral("服务器返回了不同的弹幕来源，请重新确认。")); return;
        }
        phase_ = Loading; library_.selectEpisode(match); library_.downloadEpisode(true);
    }
}
void ExternalPlaybackSession::accept() {
    const auto result = library_.takeReadyResult();
    if (!active_ || !contentReady_ || stopped_ || !result) return;
    const bool fallback = phase_ == Cached;
    metadata_ = result->metadata;
    phase_ = Ready;
    status_ = fallback ? QStringLiteral("网络获取失败，使用本集上次成功的缓存；可重试刷新。")
        : QStringLiteral("已载入 %1 条服务器弹幕").arg(result->items.size());
    error_.clear();
    if (!fallback) saveAssociation();
    app_->presentMedia(std::move(result->items), title(), sample_);
    emit changed(); emit sourceAccepted();
}
void ExternalPlaybackSession::saveAssociation() {
    const auto exact = exactKey();
    if (exact.isEmpty()) return;
    associations_[exact] = metadata_;
    if (!seasonKey().isEmpty() && emby_.value("episode").toDouble() > 0
        && std::abs(metadata_.value("episodeNumber").toDouble() - emby_.value("episode").toDouble()) < .0001) {
        auto season = metadata_; season.remove("episodeId"); season.remove("episodeTitle"); season.remove("episodeNumber");
        auto episodes = associations_.value(seasonKey()).toMap().value("episodes").toMap();
        for (const auto& value : library_.episodes()) {
            const auto entry = value.toMap();
            const auto number = danmakuEpisodeNumber(entry.value("title").toString(), entry.value("number").toDouble());
            if (number > 0 && std::floor(number) == number) episodes[QString::number(int(number))] = entry;
        }
        season["episodes"] = episodes;
        const auto id = metadata_.value("episodeId").toString();
        if (QRegularExpression("^25[0-9]{12}$").match(id).hasMatch()) season["sourcePrefix"] = id.left(10);
        associations_[seasonKey()] = season;
    }
    QSaveFile file(QDir(data_).filePath("external-associations.json"));
    const auto bytes = QJsonDocument(QJsonObject::fromVariantMap(associations_)).toJson();
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        error_ = QStringLiteral("弹幕已载入，但关联保存失败");
    }
}
void ExternalPlaybackSession::observe(const MediaSample& sample) {
    if (!active_ || sample.id != bound_) return;
    sample_ = sample;
    if (sample.playback == MediaSample::Playback::Stopped && !stopped_ && !cancelled_) {
        stopped_ = true; library_.cancel(); app_->clearMediaItems();
        status_ = QStringLiteral("视频已停止，弹幕已清除"); emit changed();
    } else if (stopped_ && !cancelled_ && contentReady_ && (sample.playback == MediaSample::Playback::Playing
        || sample.playback == MediaSample::Playback::Paused)) {
        stopped_ = false; resolve(); emit changed();
    }
}
void ExternalPlaybackSession::contentChanged(const QString& path) {
    if (!active_) return;
    if (contentPath_.isEmpty()) { contentPath_ = path; return; }
    if (path == contentPath_) return;
    contentPath_ = path;
    ++epoch_; contentReady_ = false; phase_ = Choice; library_.clearSelection(); metadata_.clear();
    emby_.clear(); emby_["title"] = QFileInfo(QUrl(path).path()).fileName().left(512);
    app_->clearMediaItems();
    status_ = QStringLiteral("mpv 已换片，正在等待新条目信息；也可手动选源。"); emit changed();
    // The launcher sends a new revision for identifiable Emby URLs. Other files
    // can still use the manual source picker, but must never inherit old XML.
    const auto epoch = epoch_;
    QTimer::singleShot(3000, this, [this, epoch] { if (active_ && epoch == epoch_ && !contentReady_) emit showChoice(); });
}
void ExternalPlaybackSession::cancelChoice() {
    library_.cancel(); phase_ = Idle; stopped_ = cancelled_ = true; app_->clearMediaItems();
    status_ = QStringLiteral("本次弹幕已取消，视频继续播放"); emit changed();
    emit choiceCancelled();
}
void ExternalPlaybackSession::retry() {
    if (!active_) return;
    ++epoch_; stopped_ = cancelled_ = false; library_.clearSelection();
    if (!loadProfile()) { offer(QStringLiteral("请检查自动弹幕服务配置后重试")); return; }
    contentReady_ = true; resolve();
}
void ExternalPlaybackSession::chooseSource() {
    if (!active_) return;
    ++epoch_; stopped_ = cancelled_ = false; contentReady_ = true; library_.clearSelection(); metadata_.clear();
    loadError_.clear();
    phase_ = Choice; library_.searchAnime(searchTitle()); emit changed(); emit showChoice();
}
void ExternalPlaybackSession::stop() { cancelChoice(); }
