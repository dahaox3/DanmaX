#pragma once
#include <QJsonObject>
#include <QString>
namespace PrivateJson {
QString userSid();
bool write(const QString& path, const QJsonObject& value, QString& error);
}
