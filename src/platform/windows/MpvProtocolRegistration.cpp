#include "platform/windows/MpvProtocolRegistration.h"
#include "platform/windows/PrivateJson.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <windows.h>
#include <shlobj.h>
#include <vector>

namespace {
const QStringList schemes{"mpv", "mpv-handler", "mpv-handler-debug"};
const QStringList registrations{"mpv", "mpv-handler", "mpv-handler-debug", "DanmaX.Mpv"};
LPCWSTR wide(const QString& s) { return reinterpret_cast<LPCWSTR>(s.utf16()); }
QString identity() { return PrivateJson::userSid() + ':' + qEnvironmentVariable("COMPUTERNAME"); }
QString quoted(const QString& argument) {
    QString result = "\""; int slashes = 0;
    for (const auto ch : argument) {
        if (ch == '\\') { ++slashes; continue; }
        result += QString(slashes * (ch == '"' ? 2 : 1) + (ch == '"' ? 1 : 0), '\\');
        result += ch; slashes = 0;
    }
    return result + QString(slashes * 2, '\\') + '"';
}
QString value(const QString& root, const QString& scheme, const QString& name) {
    QSettings settings("HKEY_CURRENT_USER\\" + root + '\\' + scheme, QSettings::NativeFormat);
    return settings.value(name).toString();
}
bool validSnapshot(const QJsonObject& saved) {
    if (!saved.value("present").isBool()) return false;
    if (!saved.value("present").toBool()) return true;
    const auto keys = saved.value("keys").toArray();
    if (keys.isEmpty() || keys.size() > 512) return false;
    for (const auto& item : keys) {
        const auto key = item.toObject(); const auto path = key.value("path").toString();
        if (path.startsWith('\\') || path.contains(QChar::Null) || path.split('\\').contains("..") || !key.value("values").isArray()) return false;
        for (const auto& v : key.value("values").toArray()) {
            const auto entry = v.toObject(); const auto text = entry.value("data").toString().toLatin1();
            const auto data = QByteArray::fromBase64(text); const int type = entry.value("type").toInt(-1);
            if (type < 0 || type > REG_QWORD || data.size() > 65536 || data.toBase64() != text || entry.value("name").toString().contains(QChar::Null)) return false;
        }
    }
    return true;
}
bool capture(HKEY key, const QString& path, QJsonArray& keys, int depth = 0) {
    if (depth > 32 || keys.size() > 512) return false;
    QJsonArray values;
    for (DWORD i = 0;; ++i) {
        wchar_t name[16384]; DWORD length = 16384, type{}, size{};
        auto code = RegEnumValueW(key, i, name, &length, nullptr, &type, nullptr, &size);
        if (code == ERROR_NO_MORE_ITEMS) break;
        if (code != ERROR_SUCCESS || size > 65536 || i > 512) return false;
        QByteArray data(qsizetype(size), Qt::Uninitialized);
        if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<LPBYTE>(data.data()), &size) != ERROR_SUCCESS) return false;
        data.resize(size);
        values.append(QJsonObject{{"name", QString::fromWCharArray(name, length)}, {"type", int(type)},
                                  {"data", QString::fromLatin1(data.toBase64())}});
    }
    keys.append(QJsonObject{{"path", path}, {"values", values}});
    for (DWORD i = 0;; ++i) {
        wchar_t name[256]; DWORD length = 256;
        const auto code = RegEnumKeyExW(key, i, name, &length, nullptr, nullptr, nullptr, nullptr);
        if (code == ERROR_NO_MORE_ITEMS) return true;
        if (code != ERROR_SUCCESS) return false;
        HKEY child{};
        if (RegOpenKeyExW(key, name, 0, KEY_READ, &child) != ERROR_SUCCESS) return false;
        const bool ok = capture(child, path.isEmpty() ? QString::fromWCharArray(name) : path + '\\' + QString::fromWCharArray(name), keys, depth + 1);
        RegCloseKey(child); if (!ok) return false;
    }
}
bool snapshot(const QString& path, QJsonObject& result) {
    HKEY key{}; const auto code = RegOpenKeyExW(HKEY_CURRENT_USER, wide(path), 0, KEY_READ, &key);
    if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) { result = {{"present", false}}; return true; }
    if (code != ERROR_SUCCESS) return false;
    QJsonArray keys; const bool ok = capture(key, {}, keys); RegCloseKey(key);
    result = {{"present", true}, {"keys", keys}}; return ok;
}
bool apply(const QString& path, const QJsonObject& saved) {
    // The caller supplies a fixed registration root, never a backup path.
    auto code = RegDeleteTreeW(HKEY_CURRENT_USER, wide(path));
    if (code != ERROR_SUCCESS && code != ERROR_FILE_NOT_FOUND && code != ERROR_PATH_NOT_FOUND) return false;
    if (!saved.value("present").toBool()) return true;
    for (const auto& entry : saved.value("keys").toArray()) {
        const auto object = entry.toObject(); const auto suffix = object.value("path").toString();
        if (suffix.startsWith('\\') || suffix.contains(QChar::Null) || suffix.split('\\').contains("..")) return false;
        HKEY key{}; const auto name = path + (suffix.isEmpty() ? QString() : '\\' + suffix);
        if (RegCreateKeyExW(HKEY_CURRENT_USER, wide(name), 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS) return false;
        bool ok = true;
        for (const auto& item : object.value("values").toArray()) {
            const auto v = item.toObject(); const auto data = QByteArray::fromBase64(v.value("data").toString().toLatin1());
            const auto n = v.value("name").toString();
            if (n.contains(QChar::Null) || RegSetValueExW(key, wide(n), 0, DWORD(v.value("type").toInt()),
                    reinterpret_cast<const BYTE*>(data.constData()), DWORD(data.size())) != ERROR_SUCCESS) { ok = false; break; }
        }
        RegCloseKey(key); if (!ok) return false;
    }
    return true;
}
}
MpvProtocolRegistration::MpvProtocolRegistration(QString data, QString executable, QString root)
    : data_(std::move(data)), executable_(std::move(executable)), root_(std::move(root)) {}
QString MpvProtocolRegistration::command() const {
    return quoted(QDir::toNativeSeparators(executable_)) + " --data-dir " + quoted(QDir::toNativeSeparators(data_)) + " --mpv-url \"%1\"";
}
bool MpvProtocolRegistration::registered() const {
    for (const auto& scheme : schemes)
        if (value(root_, scheme, "shell/open/command/.") != command()) return false;
    if (root_ == "Software\\Classes") {
        for (const auto& scheme : schemes) {
            QSettings choice("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\Shell\\Associations\\UrlAssociations\\" + scheme + "\\UserChoice", QSettings::NativeFormat);
            const auto id = choice.value("ProgId").toString();
            if (id.isEmpty()) continue;
            QSettings effective("HKEY_CLASSES_ROOT\\" + id, QSettings::NativeFormat);
            if (effective.value("shell/open/command/.").toString() != command()) return false;
        }
    }
    return true;
}
QString MpvProtocolRegistration::bind() {
    const auto backupPath = QDir(data_).filePath("mpv-protocol-backup.json");
    QJsonObject previous;
    QFile file(backupPath);
    if (file.exists()) {
        if (!file.open(QIODevice::ReadOnly) || file.size() > 4 * 1024 * 1024) return QStringLiteral("无法读取原协议备份，未修改绑定");
        const auto doc = QJsonDocument::fromJson(file.readAll());
        if (!doc.isObject()) return QStringLiteral("原协议备份损坏，未修改绑定");
        previous = doc.object();
        file.close();
        if (previous.value("version").toInt() != 1 || !previous.value("protocols").isObject()) return QStringLiteral("原协议备份格式无效，未修改绑定");
    }
    const auto applicationRoot = root_ == "Software\\Classes" ? QString("Software\\RegisteredApplications") : root_ + "\\RegisteredApplications";
    QSettings application("HKEY_CURRENT_USER\\" + applicationRoot, QSettings::NativeFormat);
    const auto capabilities = root_ + "\\DanmaX.Mpv\\Capabilities";
    const QJsonObject oldApplication{{"present", application.contains("DanmaX.Mpv")}, {"value", application.value("DanmaX.Mpv").toString()}};
    QJsonObject current, originals;
    for (const auto& scheme : registrations) {
        QJsonObject saved;
        if (!snapshot(root_ + '\\' + scheme, saved)) return QStringLiteral("无法备份当前用户的协议绑定");
        current[scheme] = saved;
        const bool ours = value(root_, scheme, "DanmaXOwner") == "DanmaX"
            && value(root_, scheme, "shell/open/command/.") == previous.value("command").toString();
        originals[scheme] = ours && previous.value("identity") == identity() && previous.value("root") == root_
            && previous.value("protocols").toObject().contains(scheme) ? previous.value("protocols").toObject().value(scheme) : QJsonValue(saved);
        if (!validSnapshot(originals.value(scheme).toObject())) return QStringLiteral("原协议备份内容无效，未修改绑定");
    }
    const auto originalApplication = application.value("DanmaX.Mpv").toString() == capabilities && previous.value("identity") == identity() && previous.value("root") == root_
        && previous.value("registeredApplication").isObject() ? previous.value("registeredApplication").toObject() : oldApplication;
    QString error;
    if (!PrivateJson::write(backupPath, {{"version", 1}, {"identity", identity()}, {"root", root_}, {"command", command()}, {"protocols", originals}, {"registeredApplication", originalApplication}}, error)) return error;
    for (const auto& scheme : registrations) {
        QSettings settings("HKEY_CURRENT_USER\\" + root_ + '\\' + scheme, QSettings::NativeFormat);
        settings.clear(); settings.setValue(".", "URL:DanmaX mpv"); settings.setValue("URL Protocol", "");
        settings.setValue("DanmaXOwner", "DanmaX"); settings.setValue("DefaultIcon/.", '"' + QDir::toNativeSeparators(executable_) + "\",0");
        settings.setValue("shell/open/command/.", command()); settings.sync();
        if (scheme == "DanmaX.Mpv") {
            settings.setValue("Capabilities/ApplicationName", "DanmaX MPV");
            settings.setValue("Capabilities/ApplicationDescription", QStringLiteral("MPV 网页播放与外挂弹幕"));
            settings.setValue("Capabilities/ApplicationIcon", '"' + QDir::toNativeSeparators(executable_) + "\",0");
            // Keep the per-protocol ProgID so restoring its original command
            // remains useful even after Windows records a UserChoice.
            for (const auto& s : schemes) settings.setValue("Capabilities/URLAssociations/" + s, s);
            settings.sync();
        }
        if (settings.status() != QSettings::NoError) {
            for (const auto& s : registrations) apply(root_ + '\\' + s, current.value(s).toObject());
            return QStringLiteral("协议绑定失败，已尝试还原原绑定");
        }
    }
    application.setValue("DanmaX.Mpv", capabilities); application.sync();
    if (application.status() != QSettings::NoError) {
        for (const auto& s : registrations) apply(root_ + '\\' + s, current.value(s).toObject());
        return QStringLiteral("无法注册默认应用入口，已尝试回滚，协议备份已保留");
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return registered() ? QString() : QStringLiteral("协议已注册，但 Windows 仍选择了其他默认应用。请在 Windows 默认应用中选择 DanmaX MPV。");
}
QString MpvProtocolRegistration::restore() {
    QFile file(QDir(data_).filePath("mpv-protocol-backup.json"));
    if (!file.open(QIODevice::ReadOnly) || file.size() > 4 * 1024 * 1024) return QStringLiteral("没有可用的本机协议备份");
    const auto saved = QJsonDocument::fromJson(file.readAll()).object();
    if (saved.value("identity") != identity() || saved.value("root") != root_)
        return QStringLiteral("备份来自其他电脑或用户，未修改本机协议");
    for (const auto& scheme : registrations)
        if (!validSnapshot(saved.value("protocols").toObject().value(scheme).toObject())) return QStringLiteral("协议备份内容无效，未进行还原");
    for (const auto& scheme : registrations) {
        if (value(root_, scheme, "shell/open/command/.") != command()) continue; // A new owner must be preserved.
        if (!saved.value("protocols").toObject().contains(scheme) || !apply(root_ + '\\' + scheme, saved.value("protocols").toObject().value(scheme).toObject()))
            return QStringLiteral("协议还原失败，备份已保留");
    }
    const auto applicationRoot = root_ == "Software\\Classes" ? QString("Software\\RegisteredApplications") : root_ + "\\RegisteredApplications";
    QSettings application("HKEY_CURRENT_USER\\" + applicationRoot, QSettings::NativeFormat);
    if (application.value("DanmaX.Mpv").toString() == root_ + "\\DanmaX.Mpv\\Capabilities") {
        const auto old = saved.value("registeredApplication").toObject();
        if (old.value("present").toBool()) application.setValue("DanmaX.Mpv", old.value("value").toString());
        else application.remove("DanmaX.Mpv");
        application.sync();
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr); return {};
}
