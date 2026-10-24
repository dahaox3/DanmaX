#include "infrastructure/ExternalSessionDescriptor.h"
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <windows.h>

QString ExternalSessionDescriptor::processStamp(qint64 pid, const QString& executable) {
    if (pid <= 0 || quint64(pid) > MAXDWORD || executable.isEmpty()) return {};
    const auto process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, DWORD(pid));
    if (!process) return {};
    FILETIME created{}, exit{}, kernel{}, user{};
    wchar_t path[32768]; DWORD length = 32768;
    QString stamp;
    if (WaitForSingleObject(process, 0) == WAIT_TIMEOUT && GetProcessTimes(process, &created, &exit, &kernel, &user)
        && QueryFullProcessImageNameW(process, 0, path, &length)
        && QFileInfo(QString::fromWCharArray(path, length)).canonicalFilePath().compare(
            QFileInfo(executable).canonicalFilePath(), Qt::CaseInsensitive) == 0)
        stamp = QString::number((quint64(created.dwHighDateTime) << 32) | created.dwLowDateTime);
    CloseHandle(process);
    return stamp;
}
QString ExternalSessionDescriptor::sessionId(const QVariantMap& d) {
    const auto p = d.value("process").toMap();
    return "danmax-mpv:" + p.value("pid").toString() + ':' + p.value("created").toString();
}
QString ExternalSessionDescriptor::validate(const QVariantMap& d) {
    const auto p = d.value("process").toMap();
    const auto stamp = processStamp(p.value("pid").toLongLong(), p.value("executable").toString());
    const auto pipe = d.value("ipcPipe").toString();
    if (d.value("version").toInt() != 1 || d.value("sessionId").toString().isEmpty()
        || d.value("sessionId").toString().size() > 128 || d.value("revision").toLongLong() < 1
        || stamp.isEmpty() || stamp != p.value("created").toString()
        || !pipe.startsWith(QStringLiteral("\\\\.\\pipe\\mpv-url-handler-"))
        || !QRegularExpression(QStringLiteral("^[A-Za-z0-9-]{1,100}$")).match(pipe.mid(QStringLiteral("\\\\.\\pipe\\mpv-url-handler-").size())).hasMatch())
        return QStringLiteral("外部播放会话无效或目标播放器已经退出");
    const auto e = d.value("emby").toMap();
    for (const auto& key : {"serverId", "itemId", "seriesId", "seasonId", "title", "seriesTitle", "fileName", "type"})
        if (e.value(key).toString().size() > 1024) return QStringLiteral("媒体信息过长");
    return {};
}
QVariantMap ExternalSessionDescriptor::read(const QString& path, QString& error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 64 * 1024) {
        error = QStringLiteral("无法读取外部播放会话文件"); return {};
    }
    const auto doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject()) { error = QStringLiteral("外部播放会话文件格式错误"); return {}; }
    const auto result = doc.object().toVariantMap();
    error = validate(result);
    return error.isEmpty() ? result : QVariantMap{};
}
