#pragma once
#include <QLocalServer>
#include <QVariantMap>
#include <functional>

class InstanceBroker final : public QObject {
    Q_OBJECT
  public:
    explicit InstanceBroker(const QString& data, QObject* parent = nullptr);
    // Returns true in the owning instance, false after successful forwarding.
    bool claim(const QVariantMap& message, QString& error);
    std::function<QString(const QVariantMap&)> receive;
  private:
    QLocalServer server_;
    QString name_;
};
