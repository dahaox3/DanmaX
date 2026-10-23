#include "application/AppController.h"
#include <QFile>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QTest>
#include <cmath>
#include <limits>

class FrameRateTests final : public QObject {
    Q_OBJECT
    struct Fixture {
        QTemporaryDir directory;
        AppController app{directory.filePath("data"), nullptr, {}, directory.filePath("cache")};
        QQuickWindow window;
        DanmakuItem renderer{window.contentItem()};
        SettingsStore* settings = qobject_cast<SettingsStore*>(app.settings());
        QString xml = directory.filePath("comments.xml");
        Fixture() {
            settings->setDanmakuServers({});
            window.resize(800, 400);
            renderer.setSize(QSizeF(800, 400));
            app.attach(&renderer, &window);
            QObject::connect(&app, &AppController::stateChanged, &window, [this] {
                window.setVisible(app.overlayVisible());
            });
            QFile file(xml);
            if (file.open(QIODevice::WriteOnly))
                file.write("<i><d p=\"0,1,25,16777215\">scrolling frame rate test</d>"
                           "<d p=\"0,5,25,16777215\">fixed</d>"
                           "<d p=\"100,1,25,16777215\">future</d></i>");
        }
        void start(int fps) {
            settings->setValue("maxFps", fps);
            qobject_cast<NowPlayingModel*>(app.nowPlaying())->openStandalone(xml);
        }
    };
  private slots:
    void settingsPersistenceAndValidation() {
        QTemporaryDir directory;
        SettingsStore settings(directory.path());
        QCOMPARE(settings.values().value("maxFps").toInt(), 0);
        QVERIFY(settings.setValue("maxFps", 144));
        settings.flushPending();
        QVERIFY(settings.error().isEmpty());
        SettingsStore restored(directory.path());
        QCOMPARE(restored.values().value("maxFps").toInt(), 144);
        for (const double value : {-1.0, 1001.0, 59.5, std::numeric_limits<double>::quiet_NaN(),
                                  std::numeric_limits<double>::infinity()}) {
            QVERIFY(!settings.setValue("maxFps", value));
            QCOMPARE(settings.values().value("maxFps").toInt(), 144);
        }
        for (const int fps : {1, 1000, 0}) QVERIFY(settings.setValue("maxFps", fps));
    }
    void continuousFrames_data() {
        QTest::addColumn<int>("fps");
        QTest::newRow("1 FPS") << 1;
        QTest::newRow("10 FPS") << 10;
        QTest::newRow("30 FPS") << 30;
    }
    void continuousFrames() {
        QFETCH(int, fps);
        Fixture f;
        f.start(fps);
        QTRY_VERIFY(f.app.playing() && f.renderer.firstSnapshotId() != 0);
        QTest::qWait(100);
        const auto id = f.renderer.firstSnapshotId();
        const auto startPosition = f.app.position();
        const auto startX = f.renderer.firstSnapshotX();
        double previousX = startX;
        QElapsedTimer elapsed;
        elapsed.start();
        QList<double> updates;
        int swaps = 0;
        const auto swapsConnection = connect(&f.window, &QQuickWindow::frameSwapped, this,
                                             [&] { ++swaps; }, Qt::QueuedConnection);
        const auto connection = connect(&f.window, &QQuickWindow::afterAnimating, this, [&] {
            const auto x = f.renderer.firstSnapshotX();
            if (x != previousX) { updates.append(f.app.position()); previousX = x; }
        });
        QTest::qWait(fps == 1 ? 1500 : 1100);
        disconnect(connection);
        disconnect(swapsConnection);
        QVERIFY(!updates.isEmpty());
        const double minimum = 1.0 / fps;
        for (qsizetype i = 1; i < updates.size(); ++i)
            QVERIFY2(updates[i] - updates[i - 1] >= minimum - 0.001,
                     "Continuous animation must respect the configured frame interval");
        QVERIFY(updates.size() <= static_cast<int>(std::ceil(elapsed.nsecsElapsed() * 1e-9 * fps)) + 1);
        qInfo("Cap %d FPS: %lld animation updates, %d presented frames over %.3f seconds", fps,
              static_cast<long long>(updates.size()), swaps, elapsed.nsecsElapsed() * 1e-9);
        QVERIFY(swaps <= static_cast<int>(std::ceil(elapsed.nsecsElapsed() * 1e-9 * fps)) + 2);
        QCOMPARE(f.renderer.firstSnapshotId(), id);
        const double distance = (f.app.position() - startPosition) * f.settings->values().value("speed").toInt();
        QVERIFY(std::abs(startX - f.renderer.firstSnapshotX() - distance) < 0.01);
        QVERIFY(f.app.position() - startPosition > 0.5);
    }
    void liveEditsPauseAndHiddenWindow() {
        Fixture f;
        f.start(1);
        QTRY_VERIFY(f.app.playing() && f.renderer.firstSnapshotId() != 0);
        const auto id = f.renderer.firstSnapshotId();
        const auto initialX = f.renderer.firstSnapshotX();
        QVERIFY(f.settings->setValue("maxFps", 20));
        QTest::qWait(350);
        QVERIFY(f.renderer.firstSnapshotX() < initialX - 10);
        QCOMPARE(f.renderer.firstSnapshotId(), id);
        f.app.togglePause();
        const auto position = f.app.position();
        const auto x = f.renderer.firstSnapshotX();
        QTest::qWait(250);
        QCOMPARE(f.app.position(), position);
        QCOMPARE(f.renderer.firstSnapshotX(), x);
        QVERIFY(f.settings->setValue("maxFps", 0));
        QTest::qWait(100);
        QCOMPARE(f.app.position(), position);
        QCOMPARE(f.renderer.firstSnapshotId(), id);
        f.app.togglePause();
        QTRY_VERIFY(f.renderer.firstSnapshotX() < x - 5);
        QVERIFY(f.settings->setValue("maxFps", 10));
        f.window.hide();
        const auto hiddenPosition = f.app.position();
        QTest::qWait(250);
        QVERIFY(f.app.position() > hiddenPosition + 0.1);
        f.window.show();
        QTRY_VERIFY(f.renderer.firstSnapshotX() < x - 25);
        QCOMPARE(f.renderer.firstSnapshotId(), id);
        f.app.stop();
        QTest::qWait(150);
        QVERIFY(!f.app.running());
        QCOMPARE(f.renderer.snapshotCount(), 0);
    }
    void mediaSamplesCannotBypassCap() {
        Fixture f;
        QVERIFY(f.settings->setValue("maxFps", 10));
        f.app.setRenderTestSession("FrameRate.Player");
        auto* monitor = f.app.findChild<MediaMonitor*>(); QVERIFY(monitor);
        QObject::disconnect(monitor, &MediaMonitor::sampleReceived, &f.app, nullptr);
        f.app.loadFile(f.xml);
        QTRY_VERIFY(!f.app.loading() && f.app.total() == 3);
        f.app.start(false);
        QElapsedTimer clock;
        clock.start();
        QTimer heartbeat;
        bool initialSample = true;
        const auto sample = [&] {
            MediaSample value;
            value.id = "FrameRate.Player"; value.identity = "same video";
            value.found = value.playing = true;
            value.playback = MediaSample::Playback::Playing;
            value.position = initialSample ? 0 : clock.nsecsElapsed() * 1e-9;
            initialSample = false;
            value.duration = 120; value.rate = 1;
            value.generation = monitor->generation();
            value.observedAt = MediaMonitor::monotonicSeconds();
            QMetaObject::invokeMethod(&f.app, "onSample", Qt::DirectConnection, Q_ARG(MediaSample, value));
        };
        connect(&heartbeat, &QTimer::timeout, &f.app, sample);
        heartbeat.start(10);
        sample();
        QTRY_VERIFY(f.renderer.firstSnapshotId() != 0);
        double previousX = f.renderer.firstSnapshotX();
        QList<double> updates;
        const auto connection = connect(&f.window, &QQuickWindow::afterAnimating, this, [&] {
            if (f.renderer.firstSnapshotX() != previousX) {
                previousX = f.renderer.firstSnapshotX();
                updates.append(previousX);
            }
        });
        QTest::qWait(1100);
        disconnect(connection);
        QVERIFY(updates.size() >= 3 && updates.size() <= 12);
        // Motion records when a snapshot was published; vsync may present it
        // later. Measuring GUI callback arrival times would include that jitter.
        const double minimumDistance = f.settings->values().value("speed").toInt() * 0.099;
        for (qsizetype i = 1; i < updates.size(); ++i)
            QVERIFY(updates[i - 1] - updates[i] >= minimumDistance);
        heartbeat.stop();
    }
};

QTEST_MAIN(FrameRateTests)
#include "FrameRateTests.moc"
