#pragma once
#include "platform/windows/MpvIpcBackend.h"
#include <QObject>
#include <QNetworkAccessManager>
#include <QProcess>
#include <QTimer>
#include <QUrl>
#include <QJsonObject>
#include <functional>
#include <memory>

struct MpvLaunchRequest {
    QUrl video, subtitle;
    QString error;
    static MpvLaunchRequest decode(const QString& uri);
};
class MpvProtocolLauncher final : public QObject {
    Q_OBJECT
  public:
    struct Options { bool spawnCompanion{true}; int networkTimeout{4000}, progressInterval{10000}, cleanupDelay{5000}; };
    MpvProtocolLauncher(QString data, QString cache, Options options, QObject* parent = nullptr);
    void start(const QString& uri);
  signals:
    void finished(int code);
    void error(QString message);
    void playbackStarted(qint64 pid);
    void descriptorPublished(QString path);
  private:
    struct Context;
    using Reply = std::function<void(bool, QJsonObject)>;
    void request(const std::shared_ptr<Context>& context, const QString& endpoint, const QString& method, QJsonObject body, Reply reply);
    void prepare(const std::shared_ptr<Context>& context, bool first);
    void launch(qint64 resumeTicks);
    void publish();
    void claimPresentation();
    QVariantMap descriptor() const;
    void observe(const MediaSample& sample);
    void changedFile(const QString& path);
    void finalize(const std::shared_ptr<Context>& context);
    void processEnded();
    void maybeFinished();
    void log(const QString& message) const;
    QString data_, cache_, mpv_, session_, pipe_, serverId_;
    QStringList descriptorFiles_;
    MpvLaunchRequest input_;
    Options options_;
    QNetworkAccessManager network_;
    QProcess process_;
    MpvIpcBackend player_;
    QTimer progress_;
    std::shared_ptr<Context> context_;
    qint64 revision_{};
    int finalizations_{};
    bool automatic_{true}, launched_{}, ended_{}, started_{}, serverInfoReady_{true};
};
