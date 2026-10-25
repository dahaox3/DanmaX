#include "platform/windows/PrivateJson.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QUuid>
#include <windows.h>
#include <sddl.h>
#include <vector>

QString PrivateJson::userSid() {
    HANDLE token{};
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return {};
    DWORD size{}; GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<char> bytes(size);
    QString result;
    if (size && GetTokenInformation(token, TokenUser, bytes.data(), size, &size)) {
        LPWSTR sid{};
        if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid, &sid)) {
            result = QString::fromWCharArray(sid); LocalFree(sid);
        }
    }
    CloseHandle(token); return result;
}
bool PrivateJson::write(const QString& path, const QJsonObject& value, QString& error) {
    const auto bytes = QJsonDocument(value).toJson();
    const auto sid = userSid();
    if (sid.isEmpty() || bytes.size() > 4 * 1024 * 1024 || !QDir().mkpath(QFileInfo(path).absolutePath())) {
        error = QStringLiteral("无法准备当前用户的私密配置"); return false;
    }
    const auto sddl = QStringLiteral("D:P(A;;FA;;;SY)(A;;FA;;;%1)").arg(sid);
    PSECURITY_DESCRIPTOR descriptor{};
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(reinterpret_cast<LPCWSTR>(sddl.utf16()),
            SDDL_REVISION_1, &descriptor, nullptr)) {
        error = QStringLiteral("无法设置配置访问权限"); return false;
    }
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE};
    const auto temporary = QDir::toNativeSeparators(path + '.' + QUuid::createUuid().toString(QUuid::Id128) + ".tmp");
    const auto file = CreateFileW(reinterpret_cast<LPCWSTR>(temporary.utf16()), GENERIC_WRITE, 0, &attributes,
                                 CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    LocalFree(descriptor);
    if (file == INVALID_HANDLE_VALUE) { error = QStringLiteral("无法创建受限配置文件"); return false; }
    DWORD written{};
    bool ok = WriteFile(file, bytes.constData(), DWORD(bytes.size()), &written, nullptr) && written == DWORD(bytes.size())
        && FlushFileBuffers(file);
    CloseHandle(file);
    const auto destination = QDir::toNativeSeparators(path);
    if (ok) ok = MoveFileExW(reinterpret_cast<LPCWSTR>(temporary.utf16()), reinterpret_cast<LPCWSTR>(destination.utf16()),
                             MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!ok) { QFile::remove(temporary); error = QStringLiteral("无法原子保存配置，原文件已保留"); }
    return ok;
}
