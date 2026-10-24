#pragma once
#include <QVariantMap>
#include <QString>

namespace ExternalSessionDescriptor {
// Identifies a process incarnation, never just a reusable PID.
QString processStamp(qint64 pid, const QString& executable);
QString validate(const QVariantMap& descriptor);
QVariantMap read(const QString& path, QString& error);
QString sessionId(const QVariantMap& descriptor);
}
