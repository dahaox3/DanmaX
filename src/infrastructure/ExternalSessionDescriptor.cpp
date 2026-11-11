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
        || !pipe.startsWith(QStringLiteral("\\\\.\\pipe\\"))
        || !QRegularExpression(QStringLiteral("^[A-Za-z0-9_.-]{1,200}$")).match(pipe.mid(9)).hasMatch()
        || (!d.contains("localXml") && !pipe.startsWith(QStringLiteral("\\\\.\\pipe\\mpv-url-handler-"))))
        return QStringLiteral("外部播放会话无效或目标播放器已经退出");
    if (d.contains("localXml")) {
        const QFileInfo file(d.value("localXml").toString());
        bool valid{}; const auto entry = d.value("playlistEntryId").toString().toLongLong(&valid);
        if (!file.isAbsolute() || !file.isFile() || file.suffix().compare("xml", Qt::CaseInsensitive) != 0
            || !valid || entry < 0) return QStringLiteral("本地 XML 或 mpv 播放条目无效");
    }
    const auto e = d.value("emby").toMap();
    for (const auto& key : {"serverId", "itemId", "seriesId", "seasonId", "title", "seriesTitle", "fileName", "type"})
        if (e.value(key).toString().size() > 1024) return QStringLiteral("媒体信息过长");
    return {};
}
QVariantMap ExternalSessionDescriptor::localXml(qint64 pid, const QString& ipc, const QString& file,
                                               const QString& entry, QString& error) {
    QString exe;
    if (pid > 0 && quint64(pid) <= MAXDWORD) {
        const auto process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(pid));
        wchar_t path[32768]; DWORD length = 32768;
        if (process) {
            if (QueryFullProcessImageNameW(process, 0, path, &length)) exe = QString::fromWCharArray(path, length);
            CloseHandle(process);
        }
    }
    if (QFileInfo(exe).fileName().compare("mpv.exe", Qt::CaseInsensitive) != 0) {
        error = QStringLiteral("目标进程不是 mpv.exe"); return {};
    }
    const auto pipe = ipc.startsWith(QStringLiteral("\\\\.\\pipe\\")) ? ipc : QStringLiteral("\\\\.\\pipe\\") + ipc;
    QVariantMap d{{"version", 1}, {"sessionId", "xml-drop"}, {"revision", 1},
        {"process", QVariantMap{{"pid", pid}, {"executable", exe}, {"created", processStamp(pid, exe)}}},
        {"ipcPipe", pipe}, {"localXml", QFileInfo(file).absoluteFilePath()}, {"playlistEntryId", entry}};
    error = validate(d); return error.isEmpty() ? d : QVariantMap{};
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
