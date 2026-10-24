#include "application/AppController.h"
#include "application/ExternalPlaybackSession.h"
#include "infrastructure/ExternalSessionDescriptor.h"
#include "infrastructure/OnlineDanmakuData.h"
#include "platform/windows/MpvIpcBackend.h"
#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUuid>
#include <QtTest>

namespace {
void save(const QString& path, const QJsonObject& value) {
    QFile file(path); if (file.open(QIODevice::WriteOnly)) file.write(QJsonDocument(value).toJson());
}
class FakeMpv {
  public:
    QLocalServer server;
    QJsonObject values{{"time-pos", 42.5}, {"duration", 1400.0}, {"pause", false}, {"speed", 1.5},
                       {"path", "http://example.test/videos/24065/stream?api_key=private"}, {"idle-active", false}};
    QList<QLocalSocket*> clients;
    FakeMpv() {
        server.setSocketOptions(QLocalServer::UserAccessOption);
        server.listen("mpv-url-handler-test-" + QUuid::createUuid().toString(QUuid::WithoutBraces));
        QObject::connect(&server, &QLocalServer::newConnection, &server, [this] {
            while (server.hasPendingConnections()) {
                auto* socket = server.nextPendingConnection(); clients.append(socket);
                auto buffer = std::make_shared<QByteArray>();
                QObject::connect(socket, &QLocalSocket::readyRead, &server, [this, socket, buffer] {
                    *buffer += socket->readAll();
                    while (buffer->contains('\n')) {
                        const auto end = buffer->indexOf('\n');
                        const auto message = QJsonDocument::fromJson(buffer->left(end)).object(); buffer->remove(0, end + 1);
                        const auto command = message.value("command").toArray();
                        if (command.size() < 2 || command.at(0).toString() != "get_property") continue;
                        const auto name = command.at(1).toString();
                        const auto reply = QJsonObject{{"request_id", message.value("request_id")}, {"error", "success"}, {"data", values.value(name)}};
                        socket->write(QJsonDocument(reply).toJson(QJsonDocument::Compact) + '\n');
                    }
                });
            }
        });
    }
    QVariantMap descriptor(int revision = 1, int episode = 1) const {
        const auto exe = QCoreApplication::applicationFilePath();
        return {{"version", 1}, {"sessionId", "test-session"}, {"revision", revision},
                {"process", QVariantMap{{"pid", QCoreApplication::applicationPid()}, {"executable", exe},
                    {"created", ExternalSessionDescriptor::processStamp(QCoreApplication::applicationPid(), exe)}}},
                {"ipcPipe", server.fullServerName()},
                {"emby", QVariantMap{{"serverId", "emby-test"}, {"itemId", QString::number(24064 + episode)},
                    {"seriesId", "24052"}, {"seasonId", "24053"}, {"type", "Episode"},
                    {"seriesTitle", QStringLiteral("魔法禁书目录")}, {"title", episode == 1 ? QStringLiteral("8月31日") : QStringLiteral("法之书")},
                    {"season", 2}, {"episode", episode}}}};
    }
};
class FakeMisaka : public QTcpServer {
  public:
    int downloads{}, searches{}, polls{};
    bool ambiguous{}, fail{}, pending{}, transient{};
    FakeMisaka() {
        listen(QHostAddress::LocalHost);
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (hasPendingConnections()) {
                auto* socket = nextPendingConnection(); auto buffer = std::make_shared<QByteArray>();
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, this, [this, socket, buffer] {
                    *buffer += socket->readAll(); if (!buffer->contains("\r\n\r\n")) return;
                    disconnect(socket, &QTcpSocket::readyRead, this, nullptr);
                    const auto target = buffer->split(' ').value(1); QByteArray bytes;
                    if (target.contains("/search/anime")) {
                        ++searches;
                        QJsonArray animes{QJsonObject{{"animeId", 18}, {"bangumiId", "A18"}, {"animeTitle", QStringLiteral("魔法禁书目录 第二季")}}};
                        if (transient) animes = {QJsonObject{{"animeId", 900000}, {"bangumiId", "A900000"},
                            {"animeTitle", QStringLiteral("魔法禁书目录 第二季 （搜索：1-24）（来源：bilibili 年份：2010）")}}};
                        // An exact same-title source is ambiguous, even with one best-looking result.
                        if (ambiguous) animes.append(QJsonObject{{"animeId", 19}, {"bangumiId", "A19"}, {"animeTitle", QStringLiteral("魔法禁书目录 第二季")}});
                        animes.append(QJsonObject{{"animeId", 41}, {"bangumiId", "A41"}, {"animeTitle", QStringLiteral("魔法禁书目录 第一季")}});
                        bytes = QJsonDocument(QJsonObject{{"animes", animes}}).toJson();
                    } else if (target.contains("/bangumi/")) {
                        bytes = R"({"bangumi":{"episodes":[{"episodeId":18001,"episodeTitle":"8月31日","episodeNumber":"1"},{"episodeId":18002,"episodeTitle":"法之书","episodeNumber":"2"}]}})";
                        if (transient) bytes = R"({"bangumi":{"animeId":900000,"episodes":[{"episodeId":25000018010001,"episodeTitle":"8月31日","episodeNumber":"1"},{"episodeId":25000018010002,"episodeTitle":"法之书","episodeNumber":"2"}]}})";
                    } else if (target.contains("/taskcomment/")) {
                        ++polls; bytes = R"({"status":"completed","episodeId":18001})";
                    } else {
                        ++downloads;
                        if (pending && polls == 0) bytes = R"({"status":"pending","taskId":"test-task","comments":[]})";
                        else bytes = R"({"comments":[{"p":"42,1,16777215,user","m":"测试合并 ₁₂","fontSize":35.5}]})";
                    }
                    socket->write("HTTP/1.1 " + QByteArray(fail ? "503 Unavailable" : "200 OK") + "\r\nContent-Type: application/json\r\nContent-Length: "
                        + QByteArray::number(bytes.size()) + "\r\nConnection: close\r\n\r\n" + bytes);
                    socket->disconnectFromHost();
                });
            }
        });
    }
    void profile(const QTemporaryDir& dir) {
        save(dir.filePath("external-player.json"), {{"enabled", true},
             {"misaka", QJsonObject{{"url", QString("http://127.0.0.1:%1").arg(serverPort())}, {"token", "test-secret-token"}}}});
    }
};
}
class ExternalPlaybackTests final : public QObject {
    Q_OBJECT
  private slots:
    void fractionalJsonFonts() {
        const auto result = parseOnlineDanmaku(R"({"comments":[{"p":"1,1,255,u","m":"a","fontSize":24.4},{"p":"2,5,255,u","m":"b","fontSize":24.5},{"p":"3,1,255,u","m":"c","fontSize":0},{"p":"4,1,255,u","m":"d"}]})");
        QCOMPARE(result.items.size(), size_t(3)); QCOMPARE(result.invalid, 1);
        QCOMPARE(result.items[0].fontSize, 24); QCOMPARE(result.items[1].fontSize, 25); QCOMPARE(result.items[2].fontSize, 25);
    }
    void identityAndIpc() {
        FakeMpv player; auto d = player.descriptor();
        QVERIFY2(ExternalSessionDescriptor::validate(d).isEmpty(), qPrintable(ExternalSessionDescriptor::validate(d)));
        MpvIpcBackend backend; QSignalSpy samples(&backend, &MediaPlaybackBackend::sampleReceived);
        QVERIFY(backend.bind(d).isEmpty()); backend.select(ExternalSessionDescriptor::sessionId(d), 7);
        QTRY_VERIFY_WITH_TIMEOUT(backend.latest().found, 3000);
        QCOMPARE(backend.latest().generation, quint64(7)); QCOMPARE(backend.latest().position, 42.5);
        QTRY_COMPARE(backend.latest().rate, 1.5); QVERIFY(!backend.latest().mediaUri.contains("private"));
        player.values["pause"] = true; QTRY_VERIFY(!backend.latest().playing);
        player.values["time-pos"] = 5.0; QTRY_COMPARE(backend.latest().position, 5.0);
        auto process = d.value("process").toMap(); process["created"] = "1"; d["process"] = process;
        QVERIFY(!ExternalSessionDescriptor::validate(d).isEmpty());
    }
    void uniqueSeasonRefreshAndRemember() {
        QTemporaryDir dir; FakeMisaka server; server.profile(dir); FakeMpv player;
        AppController app(dir.path()); DanmakuItem renderer; QQuickWindow overlay;
        app.attach(&renderer, &overlay);
        const auto path = dir.filePath("session.json"); save(path, QJsonObject::fromVariantMap(player.descriptor()));
        QVERIFY(app.openExternalSession(path).isEmpty());
        auto* session = qobject_cast<ExternalPlaybackSession*>(app.externalPlayback());
        QTRY_COMPARE_WITH_TIMEOUT(session->metadata().value("episodeId").toString(), QString("18001"), 5000);
        QCOMPARE(app.total(), 1); QCOMPARE(server.downloads, 1); QCOMPARE(server.searches, 1);
        QFile mapping(dir.filePath("external-associations.json")); QVERIFY(mapping.open(QIODevice::ReadOnly));
        const auto saved = mapping.readAll(); QVERIFY(!saved.contains("test-secret-token")); QVERIFY(saved.contains("season:emby-test:24052:2"));
        save(path, QJsonObject::fromVariantMap(player.descriptor(2, 2))); QVERIFY(app.openExternalSession(path).isEmpty());
        QTRY_COMPARE(session->metadata().value("episodeId").toString(), QString("18002"));
        QCOMPARE(server.searches, 1); QTRY_COMPARE(server.downloads, 2);
        save(path, QJsonObject::fromVariantMap(player.descriptor(1))); QVERIFY(app.openExternalSession(path).isEmpty());
        QCOMPARE(session->metadata().value("episodeId").toString(), QString("18002"));
        // An existing cache must not conceal a changed Pakku configuration.
        save(path, QJsonObject::fromVariantMap(player.descriptor(3, 2))); QVERIFY(app.openExternalSession(path).isEmpty());
        QTRY_COMPARE(server.downloads, 3);
    }
    void ambiguityCancellationAndCacheFallback() {
        QTemporaryDir dir; FakeMisaka server; server.profile(dir); server.ambiguous = true; FakeMpv player;
        AppController app(dir.path()); DanmakuItem renderer; QQuickWindow overlay; app.attach(&renderer, &overlay);
        const auto path = dir.filePath("session.json"); save(path, QJsonObject::fromVariantMap(player.descriptor()));
        auto* session = qobject_cast<ExternalPlaybackSession*>(app.externalPlayback());
        QVERIFY(app.openExternalSession(path).isEmpty()); QTRY_VERIFY(session->needsChoice()); QCOMPARE(server.downloads, 0);
        session->cancelChoice(); QTest::qWait(100); QCOMPARE(app.total(), 0);
        server.ambiguous = false; session->retry(); QTRY_COMPARE(app.total(), 1);
        server.fail = true;
        save(path, QJsonObject::fromVariantMap(player.descriptor(2))); QVERIFY(app.openExternalSession(path).isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(session->status().contains(QStringLiteral("缓存")), 5000); QCOMPARE(app.total(), 1);
    }
    void asynchronousDownloadAndStop() {
        QTemporaryDir dir; FakeMisaka server; server.profile(dir); server.pending = true; FakeMpv player;
        AppController app(dir.path()); DanmakuItem renderer; QQuickWindow overlay; app.attach(&renderer, &overlay);
        const auto path = dir.filePath("session.json"); save(path, QJsonObject::fromVariantMap(player.descriptor()));
        QVERIFY(app.openExternalSession(path).isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(app.total(), 1, 6000); QCOMPARE(server.polls, 1); QCOMPARE(server.downloads, 2);
        player.values["idle-active"] = true;
        QTRY_COMPARE(app.total(), 0);
        player.values["idle-active"] = false; player.values["time-pos"] = 5;
        QTRY_COMPARE(app.total(), 1);
        qobject_cast<ExternalPlaybackSession*>(app.externalPlayback())->cancelChoice();
        player.values["pause"] = true; QTest::qWait(300); player.values["pause"] = false; QTest::qWait(300);
        QCOMPARE(app.total(), 0);
        QCOMPARE(qobject_cast<SettingsStore*>(app.settings())->values().value("targetSession").toString(), QString());
    }
    void temporaryIdsAndPendingCancellation() {
        QTemporaryDir dir; FakeMisaka server; server.profile(dir); server.transient = true; FakeMpv player;
        AppController app(dir.path()); DanmakuItem renderer; QQuickWindow overlay; app.attach(&renderer, &overlay);
        const auto path = dir.filePath("session.json"); save(path, QJsonObject::fromVariantMap(player.descriptor()));
        QVERIFY(app.openExternalSession(path).isEmpty()); QTRY_COMPARE(app.total(), 1);
        auto* session = qobject_cast<ExternalPlaybackSession*>(app.externalPlayback());
        QCOMPARE(session->metadata().value("animeId").toString(), QString("A18"));
        session->stop(); server.pending = true;
        save(path, QJsonObject::fromVariantMap(player.descriptor(2))); QVERIFY(app.openExternalSession(path).isEmpty());
        QTRY_COMPARE(server.downloads, 2); session->cancelChoice(); QTest::qWait(2300);
        QCOMPARE(server.polls, 0); QCOMPARE(app.total(), 0);
    }
};
QTEST_MAIN(ExternalPlaybackTests)
#include "ExternalPlaybackTests.moc"
