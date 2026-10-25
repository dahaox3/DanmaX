#include "infrastructure/MpvIntegration.h"
#include "infrastructure/OnlineDanmakuData.h"
#include "platform/windows/PrivateJson.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QUrl>
#include <QDesktopServices>
#include <windows.h>

MpvIntegration::MpvIntegration(QString data, QString executable, QObject* parent, QString root)
    : QObject(parent), data_(std::move(data)), protocols_(data_, std::move(executable), std::move(root)) { refresh(); }
void MpvIntegration::refresh() {
    error_.clear(); notice_.clear(); config_ = {}; readable_ = true;
    QFile file(QDir(data_).filePath("external-player.json"));
    if (file.exists()) {
        if (!file.open(QIODevice::ReadOnly) || file.size() > 65536) readable_ = false;
        else {
            const auto doc = QJsonDocument::fromJson(file.readAll());
            readable_ = doc.isObject(); if (readable_) config_ = doc.object();
        }
        if (!readable_) error_ = QStringLiteral("自动外挂配置无法读取，原文件已保留");
    }
    emit changed();
}
QString MpvIntegration::mpvPath() const {
    const auto stored = config_.value("mpv").toObject().value("executable").toString();
    return stored.isEmpty() ? QString() : QDir::cleanPath(QDir(data_).absoluteFilePath(stored));
}
QString MpvIntegration::serverUrl() const { return config_.value("misaka").toObject().value("url").toString(); }
bool MpvIntegration::tokenStored() const { return !config_.value("misaka").toObject().value("token").toString().isEmpty(); }
QString MpvIntegration::localPath(const QUrl& url) const { return url.isLocalFile() ? url.toLocalFile() : url.toString(); }
bool MpvIntegration::hasBackup() const { return QFileInfo::exists(QDir(data_).filePath("mpv-protocol-backup.json")); }
QString MpvIntegration::status() const {
    if (!notice_.isEmpty()) return notice_;
    if (registered()) return QStringLiteral("已绑定 mpv / mpv-handler 网页协议");
    return QStringLiteral("选择 mpv 并绑定网页播放协议；移动程序后可重新绑定");
}
QString MpvIntegration::validateExecutable(const QString& path) {
    const QFileInfo file(path);
    DWORD type{};
    if (!file.isAbsolute() || !file.isFile() || file.fileName().compare("mpv.exe", Qt::CaseInsensitive) != 0
        || !GetBinaryTypeW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(file.absoluteFilePath()).utf16()), &type)
        || (type != SCS_32BIT_BINARY && type != SCS_64BIT_BINARY))
        return QStringLiteral("请选择有效的 mpv.exe 程序");
    return {};
}
bool MpvIntegration::reject(const QString& issue) { error_ = issue; emit changed(); return false; }
bool MpvIntegration::save(const QString& mpv, const QString& server, const QString& inputToken, bool enabled) {
    if (!readable_) return reject(QStringLiteral("配置读取失败，未覆盖原文件"));
    const auto url = QUrl(mpv); const QString path = url.isLocalFile() ? url.toLocalFile() : mpv.trimmed();
    const auto issue = validateExecutable(path); if (!issue.isEmpty()) return reject(issue);
    const auto base = server.trimmed().isEmpty() ? QString() : normalizeDanmakuServer(server);
    if ((!server.trimmed().isEmpty() && base.isEmpty()) || inputToken.size() > 512 || inputToken.contains(QChar::Null)
        || inputToken.contains('\r') || inputToken.contains('\n'))
        return reject(QStringLiteral("弹幕服务器或播放器 Token 格式无效"));
    QString token = inputToken.trimmed();
    if (token.isEmpty() && base == normalizeDanmakuServer(serverUrl())) token = config_.value("misaka").toObject().value("token").toString();
    if (enabled && (base.isEmpty() || token.isEmpty())) return reject(QStringLiteral("自动弹幕需要服务器地址和播放器 Token"));
    auto next = config_; next["version"] = 1; next["enabled"] = enabled;
    const auto canonical = QFileInfo(path).canonicalFilePath();
    const auto relative = QDir(data_).relativeFilePath(canonical);
    next["mpv"] = QJsonObject{{"executable", relative.startsWith("../") || QFileInfo(relative).isAbsolute() ? canonical : relative}};
    next["misaka"] = QJsonObject{{"url", base}, {"token", token}};
    QString error;
    if (!PrivateJson::write(QDir(data_).filePath("external-player.json"), next, error)) return reject(error);
    config_ = next; error_.clear(); notice_ = QStringLiteral("播放配置已保存"); emit changed(); return true;
}
bool MpvIntegration::bindProtocols() {
    const auto issue = validateExecutable(mpvPath()); if (!issue.isEmpty()) return reject(issue);
    const auto error = protocols_.bind(); if (!error.isEmpty()) return reject(error);
    error_.clear(); notice_ = QStringLiteral("网页协议已绑定，点击网页 MPV 即可播放并加载外挂弹幕"); emit changed(); return true;
}
bool MpvIntegration::restoreProtocols() {
    const auto error = protocols_.restore(); if (!error.isEmpty()) return reject(error);
    error_.clear(); notice_ = QStringLiteral("已还原本机原有绑定；其他程序后来接管的协议保持现状"); emit changed(); return true;
}
void MpvIntegration::openDefaultApps() { QDesktopServices::openUrl(QUrl("ms-settings:defaultapps")); }
