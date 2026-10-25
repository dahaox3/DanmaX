#include "application/MpvProtocolLauncher.h"
#include "infrastructure/MpvIntegration.h"
#include "platform/windows/PrivateJson.h"
#include <QCoreApplication>
#include <QFile>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QSettings>
#include <QSignalSpy>
#include <QScopeGuard>
#include <QTest>
#include <QUrlQuery>
#include <QUuid>
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <shellapi.h>

namespace {
QString makeMpv(const QString& directory) {
    const auto dir = QDir(directory).filePath(QStringLiteral("mpv 程序 with spaces"));
    if (!QDir().mkpath(dir)) return {};
    const auto path = QDir(dir).filePath("mpv.exe");
    return QFile::copy(QCoreApplication::applicationFilePath(), path) ? path : QString();
}
QString encoded(const QString& video, const QString& subtitle = {}) {
    auto result = "mpv-handler://play/" + QString::fromLatin1(video.toUtf8().toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
    if (!subtitle.isEmpty()) result += "?subfile=" + QString::fromLatin1(subtitle.toUtf8().toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
    return result;
}
int fakeMpv(const QStringList& args, QCoreApplication& app) {
    QString pipe;
    for (const auto& a : args) if (a.startsWith("--input-ipc-server=")) pipe = a.mid(19);
    const auto video = args.last(); const QUrlQuery query{QUrl(video)};
    QFile record("arguments.json"); if (record.open(QIODevice::WriteOnly)) record.write(QJsonDocument(QJsonArray::fromStringList(args)).toJson());
    QLocalServer server;
    if (query.queryItemValue("testIpc") != "no" && !server.listen(pipe)) return 3;
    const QJsonObject values{{"time-pos", query.queryItemValue("testPosition").toDouble()}, {"duration", 100},
        {"pause", false}, {"speed", 1.0}, {"path", video}, {"idle-active", false}};
    QObject::connect(&server, &QLocalServer::newConnection, &server, [&] {
        while (server.hasPendingConnections()) {
            auto* socket = server.nextPendingConnection(); auto buffer = std::make_shared<QByteArray>();
            QObject::connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
            QObject::connect(socket, &QLocalSocket::readyRead, socket, [socket, buffer, values] {
                *buffer += socket->readAll();
                while (buffer->contains('\n')) {
                    const auto end = buffer->indexOf('\n'); const auto o = QJsonDocument::fromJson(buffer->left(end)).object(); buffer->remove(0, end + 1);
                    const auto command = o.value("command").toArray(); if (command.size() < 2) continue;
                    if (command[0] == "get_property") socket->write(QJsonDocument(QJsonObject{{"error", "success"},
                        {"request_id", o.value("request_id")}, {"data", values.value(command[1].toString())}}).toJson(QJsonDocument::Compact) + '\n');
                    else if (command[0] == "observe_property" && command.size() == 3) socket->write(QJsonDocument(QJsonObject{
                        {"event", "property-change"}, {"name", command[2]}, {"data", values.value(command[2].toString())}}).toJson(QJsonDocument::Compact) + '\n');
                }
            });
        }
    });
    const auto lifetime = query.queryItemValue("testMs").toInt();
    QTimer::singleShot(lifetime > 0 ? lifetime : 650, &app, &QCoreApplication::quit); return app.exec();
}
class FakeEmby : public QTcpServer {
  public:
    struct Call { QString target, method, token; QJsonObject body; };
    QList<Call> calls;
    bool fail{};
    int infoDelay{};
    FakeEmby() {
        listen(QHostAddress::LocalHost);
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (hasPendingConnections()) {
                auto* s = nextPendingConnection(); auto bytes = std::make_shared<QByteArray>();
                connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
                connect(s, &QTcpSocket::readyRead, s, [this, s, bytes] {
                    *bytes += s->readAll(); const auto end = bytes->indexOf("\r\n\r\n"); if (end < 0) return;
                    int size = 0; QString token;
                    for (const auto& line : bytes->left(end).split('\n')) {
                        if (line.toLower().startsWith("content-length:")) size = line.mid(15).trimmed().toInt();
                        if (line.toLower().startsWith("x-emby-token:")) token = QString::fromUtf8(line.mid(13).trimmed());
                    }
                    if (bytes->size() < end + 4 + size) return;
                    disconnect(s, &QTcpSocket::readyRead, s, nullptr);
                    const auto parts = bytes->split(' '); const auto target = QString::fromLatin1(parts.value(1));
                    const auto method = QString::fromLatin1(parts.value(0));
                    calls.append({target, method, token, QJsonDocument::fromJson(bytes->mid(end + 4, size)).object()});
                    QByteArray body = "{}";
                    if (target == "/emby/Sessions") body = R"([{"DeviceId":"other","UserId":"wrong-user"},{"DeviceId":"browser","UserId":"right-user"}])";
                    else if (target == "/emby/System/Info/Public") body = R"({"Id":"server-test"})";
                    else if (target.contains("/Items?")) body = R"({"Items":[{"Id":"24080","SeriesId":"24052","SeasonId":"24053","Type":"Episode","Name":"Episode","SeriesName":"Series","IndexNumber":2,"ParentIndexNumber":2,"RunTimeTicks":1000000000,"UserData":{"PlaybackPositionTicks":100000000},"Path":"http://storage.invalid/stream.mkv?secret=must-not-leak"}]})";
                    if (fail) body = "{}";
                    const auto response = (fail ? "HTTP/1.1 401 Unauthorized\r\n" : "HTTP/1.1 200 OK\r\n")
                        + QByteArray("Content-Type: application/json\r\nContent-Length: ") + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
                    QTimer::singleShot(target == "/emby/System/Info/Public" ? infoDelay : 0, s, [s, response] {
                        s->write(response); s->disconnectFromHost();
                    });
                });
            }
        });
    }
    QString video(double position = 25, bool ipc = true) const {
        return QString("http://127.0.0.1:%1/videos/24080/stream.mkv?api_key=emby-test-token&DeviceId=browser&MediaSourceId=source-2&testPosition=%2&testIpc=%3")
            .arg(serverPort()).arg(position).arg(ipc ? "yes" : "no");
    }
};
}
class MpvIntegrationTests : public QObject {
    Q_OBJECT
  private slots:
    void protocolDecoding() {
        const auto video = QStringLiteral("http://example.test/videos/123/stream.mkv?api_key=fixture&note=x%26y");
        const auto sub = QStringLiteral("https://example.test/subtitle.ass?x=1&y=2");
        const auto r = MpvLaunchRequest::decode(encoded(video, sub)); QVERIFY(r.error.isEmpty());
        QCOMPARE(r.video.toString(QUrl::FullyEncoded), video); QCOMPARE(r.subtitle.toString(QUrl::FullyEncoded), sub);
        for (const auto& prefix : {"mpv://", "mpv-handler://", "mpv-handler-debug://"}) {
            const auto direct = MpvLaunchRequest::decode(prefix + video); QVERIFY(direct.error.isEmpty());
        }
        for (const auto& bad : {"mpv-handler://play/%%%%", "mpv://--script=evil", "mpv://file:///C:/file", "http://x.test/\nunsafe"})
            QVERIFY(!MpvLaunchRequest::decode(bad).error.isEmpty());
    }
    void settingsAndPrivatePermissions() {
        QTemporaryDir dir; const auto mpv = makeMpv(dir.path()); QVERIFY(!mpv.isEmpty());
        const auto root = "Software\\DanmaX.Tests\\" + QUuid::createUuid().toString(QUuid::Id128);
        MpvIntegration settings(dir.path(), QCoreApplication::applicationFilePath(), nullptr, root);
        QVERIFY(settings.save(mpv, "http://server.test:7768", "private-fixture-token", true));
        QVERIFY(settings.tokenStored()); QVERIFY(settings.save(mpv, "http://server.test:7768", {}, true));
        QVERIFY(!settings.save(mpv, "http://different.test:7768", {}, true));
        QCOMPARE(settings.serverUrl(), QString("http://server.test:7768"));
        QVERIFY(!settings.save("relative.exe", {}, {}, false));
        MpvIntegration restored(dir.path(), QCoreApplication::applicationFilePath(), nullptr, root);
        QCOMPARE(restored.mpvPath(), QDir::cleanPath(mpv)); QVERIFY(restored.tokenStored());
        const auto path = QDir(dir.path()).filePath("external-player.json");
        PACL acl{}; PSECURITY_DESCRIPTOR descriptor{};
        const auto native = QDir::toNativeSeparators(path);
        QCOMPARE(GetNamedSecurityInfoW(const_cast<LPWSTR>(reinterpret_cast<LPCWSTR>(native.utf16())), SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION, nullptr, nullptr, &acl, nullptr, &descriptor), DWORD(ERROR_SUCCESS));
        const auto release = qScopeGuard([&] { LocalFree(descriptor); });
        SECURITY_DESCRIPTOR_CONTROL control{}; DWORD version{}; QVERIFY(GetSecurityDescriptorControl(descriptor, &control, &version));
        QVERIFY(control & SE_DACL_PROTECTED); QCOMPARE(acl->AceCount, WORD(2));
    }
    void protocolBindingAndRestoration() {
        QTemporaryDir dir;
        const auto root = "Software\\DanmaX.Tests\\" + QUuid::createUuid().toString(QUuid::Id128) + "\\Classes";
        const auto clean = qScopeGuard([&] { RegDeleteTreeW(HKEY_CURRENT_USER, reinterpret_cast<LPCWSTR>(root.utf16())); });
        QSettings original("HKEY_CURRENT_USER\\" + root + "\\mpv-handler", QSettings::NativeFormat);
        original.setValue("shell/open/command/.", "old-handler.exe \"%1\"");
        original.setValue("CustomBinary", QByteArray("abc\0def", 7)); original.setValue("Number", 17u); original.sync();
        MpvProtocolRegistration protocols(dir.path(), QCoreApplication::applicationFilePath(), root);
        QVERIFY2(protocols.bind().isEmpty(), "Can back up and bind isolated test protocol roots"); QVERIFY(protocols.registered());
        QVERIFY(protocols.bind().isEmpty()); // Repair must retain the original backup.
        QSettings foreign("HKEY_CURRENT_USER\\" + root + "\\mpv", QSettings::NativeFormat);
        foreign.setValue("shell/open/command/.", "new-owner.exe \"%1\""); foreign.sync();
        QVERIFY(protocols.restore().isEmpty());
        QSettings restored("HKEY_CURRENT_USER\\" + root + "\\mpv-handler", QSettings::NativeFormat);
        QCOMPARE(restored.value("shell/open/command/.").toString(), QString("old-handler.exe \"%1\""));
        QCOMPARE(restored.value("CustomBinary").toByteArray(), QByteArray("abc\0def", 7)); QCOMPARE(restored.value("Number").toUInt(), 17u);
        QCOMPARE(foreign.value("shell/open/command/.").toString(), QString("new-owner.exe \"%1\""));
    }
    void protocolCommandQuotesDriveRoot() {
        MpvProtocolRegistration p("C:/", "C:/app with spaces/DanmaX.exe");
        int count = 0; auto* args = CommandLineToArgvW(reinterpret_cast<LPCWSTR>(p.command().utf16()), &count);
        QVERIFY(args); const auto release = qScopeGuard([&] { LocalFree(args); });
        QCOMPARE(count, 5); QCOMPARE(QString::fromWCharArray(args[2]), QString("C:\\"));
        QCOMPARE(QString::fromWCharArray(args[4]), QString("%1"));
    }
    void rebindAfterMoveRetainsOriginalAndCapturesNewOwner() {
        QTemporaryDir dir;
        const auto root = "Software\\DanmaX.Tests\\" + QUuid::createUuid().toString(QUuid::Id128) + "\\Classes";
        const auto clean = qScopeGuard([&] { RegDeleteTreeW(HKEY_CURRENT_USER, reinterpret_cast<LPCWSTR>(root.utf16())); });
        QSettings original("HKEY_CURRENT_USER\\" + root + "\\mpv-handler", QSettings::NativeFormat);
        original.setValue("shell/open/command/.", "original-handler"); original.sync();
        MpvProtocolRegistration first(dir.path(), "C:/old/DanmaX.exe", root);
        QVERIFY(first.bind().isEmpty());
        QSettings foreign("HKEY_CURRENT_USER\\" + root + "\\mpv", QSettings::NativeFormat);
        foreign.setValue("shell/open/command/.", "later-handler"); foreign.sync();
        MpvProtocolRegistration moved(dir.path(), "C:/moved/DanmaX.exe", root);
        QVERIFY(moved.bind().isEmpty()); QVERIFY(moved.registered()); QVERIFY(moved.restore().isEmpty());
        // Restoration recreates the keys; read with fresh registry handles.
        QSettings restored("HKEY_CURRENT_USER\\" + root + "\\mpv-handler", QSettings::NativeFormat);
        QSettings restoredForeign("HKEY_CURRENT_USER\\" + root + "\\mpv", QSettings::NativeFormat);
        QCOMPARE(restored.value("shell/open/command/.").toString(), QString("original-handler"));
        QCOMPARE(restoredForeign.value("shell/open/command/.").toString(), QString("later-handler"));
    }
    void reportingFlow_data() {
        QTest::addColumn<double>("position"); QTest::addColumn<bool>("ipc"); QTest::addColumn<bool>("enabled"); QTest::addColumn<bool>("failure");
        QTest::newRow("partial") << 25.0 << true << true << false;
        QTest::newRow("completed") << 99.0 << true << true << false;
        QTest::newRow("no-real-position") << 25.0 << false << true << false;
        QTest::newRow("video-only") << 25.0 << true << false << false;
        QTest::newRow("emby-unavailable") << 25.0 << true << true << true;
    }
    void reportingFlow() {
        QFETCH(double, position); QFETCH(bool, ipc); QFETCH(bool, enabled); QFETCH(bool, failure);
        QTemporaryDir dir; const auto mpv = makeMpv(dir.path()); QVERIFY(!mpv.isEmpty());
        FakeEmby server; server.fail = failure;
        MpvIntegration settings(dir.path(), QCoreApplication::applicationFilePath());
        QVERIFY(settings.save(mpv, "http://127.0.0.1:1", "fixture-danmaku-token", enabled));
        MpvProtocolLauncher launcher(dir.path(), dir.filePath("cache"), {false, 1000, 30, 1});
        QSignalSpy ended(&launcher, &MpvProtocolLauncher::finished), errors(&launcher, &MpvProtocolLauncher::error), started(&launcher, &MpvProtocolLauncher::playbackStarted);
        QList<QByteArray> descriptors;
        connect(&launcher, &MpvProtocolLauncher::descriptorPublished, this, [&](const QString& path) {
            QFile file(path); if (file.open(QIODevice::ReadOnly)) descriptors.append(file.readAll());
        });
        const auto video = server.video(position, ipc); const auto subtitle = "https://example.test/sub.ass?lang=zh";
        launcher.start(encoded(video, subtitle)); QTRY_COMPARE_WITH_TIMEOUT(ended.size(), 1, 8000);
        QCOMPARE(errors.size(), 0); QCOMPARE(started.size(), 1);
        QFile argsFile(QDir(QFileInfo(mpv).absolutePath()).filePath("arguments.json")); QVERIFY(argsFile.open(QIODevice::ReadOnly));
        const auto args = QJsonDocument::fromJson(argsFile.readAll()).array().toVariantList();
        QVERIFY(args.contains(video)); QVERIFY(args.contains(QString("--sub-file=") + subtitle));
        QCOMPARE(args.contains(QString("--start=10")), !failure);
        QCOMPARE(descriptors.isEmpty(), !enabled);
        for (const auto& descriptor : descriptors) {
            QVERIFY(!descriptor.contains("emby-test-token")); QVERIFY(!descriptor.contains("must-not-leak"));
            const auto data = QJsonDocument::fromJson(descriptor).object().value("emby").toObject();
            if (!failure) { QCOMPARE(data.value("season").toInt(), 2); QCOMPARE(data.value("episode").toInt(), 2); }
        }
        int stops = 0, played = 0, deleted = 0;
        for (const auto& call : server.calls) {
            QCOMPARE(call.token, QString("emby-test-token"));
            if (call.method == "POST" && call.target.endsWith("/Stopped")) { ++stops; QCOMPARE(call.body.value("PositionTicks").toDouble(), position * 10000000); }
            if (call.target == "/emby/Users/right-user/PlayedItems/24080") ++played;
            if (call.target.endsWith("/Delete")) ++deleted;
            QVERIFY(!call.target.contains("wrong-user"));
        }
        if (failure) { QCOMPARE(stops, 0); QCOMPARE(played, 0); QCOMPARE(deleted, 0); }
        else if (!ipc) { QCOMPARE(stops, 0); QCOMPARE(played, 0); QCOMPARE(deleted, 1); }
        else if (position >= 90) { QCOMPARE(stops, 1); QCOMPARE(played, 1); QCOMPARE(deleted, 0); }
        else { QCOMPARE(stops, 2); QCOMPARE(played, 0); QCOMPARE(deleted, 1); }
        auto files = QDir(dir.filePath("cache/external-sessions")).entryList({"*.json"}, QDir::Files); files.removeAll("latest.json");
        QCOMPARE(files.size(), 0);
    }
    void failedLaunchDoesNotAlterViewingHistory() {
        QTemporaryDir dir; FakeEmby server; const auto mpv = makeMpv(dir.path()); QVERIFY(!mpv.isEmpty());
        MpvIntegration settings(dir.path(), QCoreApplication::applicationFilePath()); QVERIFY(settings.save(mpv, {}, {}, false));
        MpvProtocolLauncher launcher(dir.path(), dir.filePath("cache"), {false, 1000, 30, 1});
        QSignalSpy ended(&launcher, &MpvProtocolLauncher::finished), errors(&launcher, &MpvProtocolLauncher::error);
        launcher.start(encoded(server.video())); QVERIFY(QFile::remove(mpv));
        QTRY_COMPARE_WITH_TIMEOUT(ended.size(), 1, 4000); QCOMPARE(errors.size(), 1);
        for (const auto& call : server.calls) QVERIFY(call.method != "POST");
    }
    void olderMetadataCannotReopenOverlayAfterLatestExit() {
        QTemporaryDir dir; FakeEmby older, newer; older.infoDelay = 900;
        const auto mpv = makeMpv(dir.path()); QVERIFY(!mpv.isEmpty());
        MpvIntegration settings(dir.path(), QCoreApplication::applicationFilePath());
        QVERIFY(settings.save(mpv, "http://127.0.0.1:1", "fixture-token", true));
        MpvProtocolLauncher a(dir.path(), dir.filePath("cache"), {false, 2000, 50, 1});
        MpvProtocolLauncher b(dir.path(), dir.filePath("cache"), {false, 2000, 50, 1});
        QSignalSpy aStarted(&a, &MpvProtocolLauncher::playbackStarted), aEnded(&a, &MpvProtocolLauncher::finished), bEnded(&b, &MpvProtocolLauncher::finished);
        QSignalSpy aPublished(&a, &MpvProtocolLauncher::descriptorPublished), bPublished(&b, &MpvProtocolLauncher::descriptorPublished);
        a.start(encoded(older.video() + "&testMs=1500")); QTRY_COMPARE(aStarted.size(), 1);
        b.start(encoded(newer.video() + "&testMs=400"));
        QTRY_COMPARE_WITH_TIMEOUT(bEnded.size(), 1, 3000); QTRY_COMPARE_WITH_TIMEOUT(aEnded.size(), 1, 3000);
        QVERIFY(!bPublished.isEmpty()); QCOMPARE(aPublished.size(), 0);
    }
    void nativeExecutableProtocolEntry() {
        QTemporaryDir dir; FakeEmby server; const auto mpv = makeMpv(dir.path()); QVERIFY(!mpv.isEmpty());
        MpvIntegration settings(dir.path(), QCoreApplication::applicationFilePath()); QVERIFY(settings.save(mpv, {}, {}, false));
        const auto exe = QDir(QCoreApplication::applicationDirPath()).filePath("DanmaX.exe"); QVERIFY(QFileInfo::exists(exe));
        QProcess host; host.setStandardOutputFile(QProcess::nullDevice()); host.setStandardErrorFile(QProcess::nullDevice());
        QSignalSpy exited(&host, qOverload<int, QProcess::ExitStatus>(&QProcess::finished));
        host.start(exe, {"--data-dir", dir.path(), "--mpv-url", encoded(server.video(99))});
        QTRY_COMPARE_WITH_TIMEOUT(exited.size(), 1, 6000); QCOMPARE(host.exitCode(), 0);
        bool completed = false;
        for (const auto& call : server.calls) if (call.target == "/emby/Users/right-user/PlayedItems/24080") completed = true;
        QVERIFY(completed);
        const auto before = server.calls.size(); exited.clear();
        host.start(exe, {"--data-dir", dir.path(), "--mpv-url", encoded(server.video()), "--file", "unexpected.xml"});
        QTRY_COMPARE_WITH_TIMEOUT(exited.size(), 1, 3000); QCOMPARE(host.exitCode(), 2); QCOMPARE(server.calls.size(), before);
        exited.clear(); const auto unexpected = dir.filePath("unexpected-data");
        host.start(exe, {"--data-dir", dir.path(), "--mpv-url", encoded(server.video()), "--data-dir", unexpected});
        QTRY_COMPARE_WITH_TIMEOUT(exited.size(), 1, 3000); QCOMPARE(host.exitCode(), 2);
        QVERIFY(!QFileInfo::exists(unexpected)); QCOMPARE(server.calls.size(), before);
    }
};
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    for (const auto& argument : app.arguments()) if (argument.startsWith("--input-ipc-server=")) return fakeMpv(app.arguments(), app);
    MpvIntegrationTests test; return QTest::qExec(&test, argc, argv);
}
#include "MpvIntegrationTests.moc"
