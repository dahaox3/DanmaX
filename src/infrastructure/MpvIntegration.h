#pragma once
#include <QObject>
#include <QJsonObject>
#include <QUrl>
#include "platform/windows/MpvProtocolRegistration.h"

class MpvIntegration final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString mpvPath READ mpvPath NOTIFY changed)
    Q_PROPERTY(QString serverUrl READ serverUrl NOTIFY changed)
    Q_PROPERTY(bool tokenStored READ tokenStored NOTIFY changed)
    Q_PROPERTY(bool enabled READ enabled NOTIFY changed)
    Q_PROPERTY(bool registered READ registered NOTIFY changed)
    Q_PROPERTY(bool hasBackup READ hasBackup NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
  public:
    MpvIntegration(QString data, QString executable, QObject* parent = nullptr, QString registryRoot = "Software\\Classes");
    QString mpvPath() const;
    QString serverUrl() const;
    bool tokenStored() const;
    bool enabled() const { return config_.value("enabled").toBool(true); }
    bool registered() const { return protocols_.registered(); }
    bool hasBackup() const;
    QString status() const;
    QString error() const { return error_; }
    static QString validateExecutable(const QString& path);
    Q_INVOKABLE void refresh();
    Q_INVOKABLE QString localPath(const QUrl& url) const;
    Q_INVOKABLE bool save(const QString& mpv, const QString& server, const QString& token, bool enabled);
    Q_INVOKABLE bool bindProtocols();
    Q_INVOKABLE bool restoreProtocols();
    Q_INVOKABLE void openDefaultApps();
  signals:
    void changed();
  private:
    bool reject(const QString& error);
    QString data_, error_, notice_;
    QJsonObject config_;
    bool readable_{true};
    MpvProtocolRegistration protocols_;
};
