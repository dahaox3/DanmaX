#pragma once
#include "application/DanmakuLibrary.h"
#include "infrastructure/MediaPlaybackBackend.h"
#include <QObject>
#include "infrastructure/XmlLoader.h"
#include <memory>
#include <thread>

class AppController;
class ExternalPlaybackSession final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QObject* library READ library CONSTANT)
    Q_PROPERTY(bool active READ active NOTIFY changed)
    Q_PROPERTY(bool localXml READ localXml NOTIFY changed)
    Q_PROPERTY(bool needsChoice READ needsChoice NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QString title READ title NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
  public:
    ExternalPlaybackSession(const QString& data, const QString& cache, AppController* app);
    ~ExternalPlaybackSession() override;
    QObject* library() { return &library_; }
    bool active() const { return active_; }
    bool needsChoice() const { return phase_ == Choice; }
    QString status() const { return status_; }
    QString error() const { return error_; }
    QString title() const;
    QString boundSession() const { return bound_; }
    QVariantMap metadata() const { return metadata_; }
    QString open(const QString& file);
    QString openLocalXml(const QVariantMap& descriptor);
    bool localXml() const { return !localFile_.isEmpty(); }
    void deactivate();
    void observe(const MediaSample& sample);
    Q_INVOKABLE void cancelChoice();
    Q_INVOKABLE void retry();
    Q_INVOKABLE void chooseSource();
    Q_INVOKABLE void stop();
  signals:
    void changed();
    void showChoice();
    void ended();
    void sourceAccepted();
    void choiceCancelled();
  private:
    enum Phase { Idle, Search, Episodes, Choice, Loading, Ready, Cached };
    void resolve();
    void advance();
    void offer(const QString& reason);
    void accept();
    void saveAssociation();
    QString exactKey() const;
    QString seasonKey() const;
    QString searchTitle() const;
    bool loadProfile();
    void contentChanged(const QString& path);
    void loadLocalXml();
    void presentLocalXml();
    AppController* app_;
    DanmakuLibrary library_;
    QString data_, cache_, bound_, status_, error_, alias_, loadError_;
    QVariantMap descriptor_, emby_, associations_, metadata_;
    MediaSample sample_;
    Phase phase_{Idle};
    bool active_{}, advancing_{}, contentReady_{}, cacheTried_{}, stopped_{}, cancelled_{};
    QString contentPath_;
    quint64 epoch_{};
    quint64 newestProcess_{};
    qint64 revision_{};
    QString localFile_, localEntry_;
    std::shared_ptr<XmlResult> localResult_;
    std::jthread localLoader_;
};
