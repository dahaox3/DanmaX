#include "renderer/DanmakuItem.h"
#include "renderer/TextTextureCache.h"
#include <QQuickWindow>
#include <QScreen>
#include <algorithm>
#include <cmath>
#include <QTest>
#include "infrastructure/DanmakuStyle.h"
#include "infrastructure/SettingsStore.h"
#include <QTemporaryDir>

class RendererTests : public QObject {
    Q_OBJECT
  private slots:
    void retainedDesktopScroll() {
        if (QGuiApplication::platformName() != "windows") QSKIP("Requires a native Windows composition target");
        QQuickWindow window;
        window.setFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowTransparentForInput | Qt::WindowDoesNotAcceptFocus);
        window.setColor(Qt::transparent);
        window.resize(430, 230);
        DanmakuItem item(window.contentItem());
        item.setSize(QSizeF(window.size()));
        item.configure({{"fontFamily", "Microsoft YaHei"}, {"fontSize", 22}, {"strokeWidth", 1},
                        {"lineSpacing", .2}, {"opacity", .45}});
        window.show();
        QTRY_VERIFY(window.isExposed());
        item.enableDesktopScroll(true);
        danmaku::Engine engine;
        danmaku::Options options;
        options.overlap = true; options.trackHeight = item.trackHeight();
        engine.configure(options, window.width(), window.height());
        engine.load({{0, danmaku::Mode::Scroll, "共享试验", 0xff80ff}, {0, danmaku::Mode::Scroll, "共享试验", 0xff80ff},
                     {0, danmaku::Mode::Bottom, "固定", 0xffffff}});
        auto measure = [&](const auto& line) { return item.measure(line); };
        engine.tick(0, 0, true, measure);
        item.present(engine, 180);
        QVERIFY(item.desktopActive());
        const auto& desktop = item.desktopLayer();
        QCOMPARE(desktop.count(), 2);
        QCOMPARE(desktop.individualCount(), 2);
        QCOMPARE(desktop.sharedCount(), 0);
        QVERIFY(desktop.individualKeyframes() > 0);
        QCOMPARE(desktop.uploads(), 1u);
        QVERIFY(desktop.bytes() > 0);
        QTRY_COMPARE(item.sceneEntries(), 1);
        const auto start = desktop.commits();
        for (int n = 1; n <= 30; ++n) {
            engine.tick(n / 60., 1. / 60, true, measure);
            item.present(engine, 180);
        }
        QCOMPARE(desktop.commits(), start);
        item.present(engine, 0);
        QCOMPARE(desktop.commits(), start + 1);
        QCOMPARE(desktop.individualKeyframes(), 0);
        item.present(engine, 0);
        QCOMPARE(desktop.commits(), start + 1);
        item.present(engine, 360);
        QCOMPARE(desktop.commits(), start + 2);
        QCOMPARE(desktop.individualCount(), 2);
        QCOMPARE(desktop.uploads(), 1u);
        item.enableDesktopScroll(false);
        item.present(engine, 360);
        QTRY_COMPARE(item.sceneEntries(), 3);
        QCOMPARE(desktop.bytes(), 0);
        item.enableDesktopScroll(true);
        item.present(engine, 360);
        engine.seek(20);
        item.present(engine);
        QCOMPARE(desktop.count(), 0);
        QCOMPARE(desktop.bytes(), 0);
        engine.load({{0, danmaku::Mode::Scroll, std::string(2400, 'W'), 0xffffff}});
        engine.tick(0, 0, true, measure);
        item.present(engine, 180);
        QVERIFY(!item.desktopActive());
        QVERIFY(desktop.lastError() < 0);
        QTRY_COMPARE(item.sceneEntries(), 1);
        QCOMPARE(item.snapshotCount(), 1);
        item.clearContent();
        QVERIFY(!item.desktopActive());
    }
    void desktopClockSwitchRetainsSurfaces() {
        if (QGuiApplication::platformName() != "windows") QSKIP("Requires a native Windows composition target");
        QQuickWindow window;
        window.setFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowTransparentForInput |
                        Qt::WindowDoesNotAcceptFocus);
        window.setColor(Qt::transparent);
        window.resize(400, 180);
        window.show();
        QTRY_VERIFY(window.isExposed());
        if (window.screen() != QGuiApplication::primaryScreen()) QSKIP("Requires the primary output");
        const auto primary = reinterpret_cast<quintptr>(window.screen());
        const double hz = std::max(1.0, window.screen()->refreshRate());
        const double cadence = hz / std::max(1, (qRound(hz) + 79) / 80);
        auto text = std::make_shared<QTextLayout>(QStringLiteral("phase"));
        text->beginLayout();
        text->createLine().setLineWidth(100);
        text->endLayout();
        const std::vector<DanmakuVisual> lines{{1, 300, 10, qRgb(255, 255, 255), text, true}};
        DesktopScrollLayer layer;
        const auto submit = [&](quintptr screen) {
            return layer.synchronize(window.winId(), lines, 1, 0, 1, 160, cadence, 1, 8 * 1024 * 1024, screen);
        };
        QVERIFY(submit(0));
        QVERIFY(!layer.refreshAligned());
        QCOMPARE(layer.animationCadence(), cadence);
        const auto initial = layer.commits();
        QVERIFY(submit(primary));
        QCOMPARE(layer.commits(), initial + 1);
        QVERIFY(std::abs(layer.animationCadence() / cadence - 1) < .012);
        QVERIFY(submit(primary));
        QCOMPARE(layer.commits(), initial + 1);
        QVERIFY(submit(0));
        QCOMPARE(layer.commits(), initial + 2);
        QVERIFY(!layer.refreshAligned());
        QCOMPARE(layer.animationCadence(), cadence);
        QCOMPARE(layer.individualCount(), 1);
        QCOMPARE(layer.uploads(), 1u);
    }
    void boundedPerCommentMotion() {
        if (QGuiApplication::platformName() != "windows") QSKIP("Requires a native Windows composition target");
        QQuickWindow window;
        window.setFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowTransparentForInput | Qt::WindowDoesNotAcceptFocus);
        window.setColor(Qt::transparent);
        window.resize(640, 320);
        window.show();
        QTRY_VERIFY(window.isExposed());
        DesktopScrollLayer layer;
        QFont font("Microsoft YaHei");
        font.setPixelSize(16);
        auto layout = std::make_shared<QTextLayout>("flight", font);
        layout->beginLayout();
        layout->createLine().setLineWidth(10000);
        layout->endLayout();
        std::vector<DanmakuVisual> lines;
        for (quint64 i = 0; i < 120; ++i)
            lines.push_back({i + 1, 520, double(i), qRgb(255, 255, 255), layout, true});
        const auto submit = [&](double velocity, double cadence = 80, qreal scale = 1) {
            return layer.synchronize(window.winId(), lines, scale, 0, 1, velocity, cadence, 1, 8 * 1024 * 1024);
        };

        QVERIFY(submit(100));
        QCOMPARE(layer.uploads(), 1u);
        QVERIFY(layer.individualCount() > 0);
        QVERIFY(layer.sharedCount() > 0);
        QCOMPARE(layer.individualCount() + layer.sharedCount(), int(lines.size()));
        QVERIFY(layer.individualKeyframes() <= DesktopScrollLayer::maxNewKeyframes);
        for (int i = 0; i < 12; ++i) {
            const int previous = layer.individualKeyframes();
            QVERIFY(submit(100));
            QVERIFY(layer.individualKeyframes() >= previous);
            QVERIFY(layer.individualKeyframes() - previous <= DesktopScrollLayer::maxNewKeyframes);
            QVERIFY(layer.individualKeyframes() <= DesktopScrollLayer::maxIndividualKeyframes);
        }
        QVERIFY(layer.individualKeyframes() > DesktopScrollLayer::maxNewKeyframes);
        QVERIFY(layer.sharedCount() > 0);

        // Releasing existing private curves makes room for an overflow line.
        lines.erase(lines.begin(), lines.end() - 1);
        QVERIFY(submit(100));
        QCOMPARE(layer.individualCount(), 1);
        QCOMPARE(layer.sharedCount(), 0);
        QCOMPARE(layer.uploads(), 1u);

        // A very slow flight cannot allocate one segment for every future step.
        QVERIFY(submit(0.00000001));
        QCOMPARE(layer.sharedCount(), 1);
        QCOMPARE(layer.individualKeyframes(), 0);
        const auto slowCommit = layer.commits();
        QVERIFY(submit(0.00000001));
        QCOMPARE(layer.commits(), slowCommit);
        QVERIFY(submit(0));
        QCOMPARE(layer.sharedCount(), 0);
        QCOMPARE(layer.individualKeyframes(), 0);
        QVERIFY(submit(200, 60, 1.5));
        QCOMPARE(layer.individualCount(), 1);
        QCOMPARE(layer.sharedCount(), 0);
        QCOMPARE(layer.uploads(), 2u);
        lines.clear();
        QVERIFY(submit(200, 60, 1.5));
        QCOMPARE(layer.individualKeyframes(), 0);
        QCOMPARE(layer.bytes(), 0);

        auto longLayout = std::make_shared<QTextLayout>(QString(180, 'W'), font);
        longLayout->beginLayout();
        longLayout->createLine().setLineWidth(10000);
        longLayout->endLayout();
        lines.push_back({1, 100, 0, qRgb(255, 255, 255), layout, true});
        lines.push_back({2, 100, 20, qRgb(255, 255, 255), longLayout, true});
        QVERIFY(submit(25));
        QCOMPARE(layer.individualCount(), 1);
        QCOMPARE(layer.sharedCount(), 1);
        const auto mixedUploads = layer.uploads();
        // The long line fits an individual curve near the end of its flight.
        lines.back().x = -longLayout->lineAt(0).naturalTextWidth() + 30;
        QVERIFY(submit(25));
        QCOMPARE(layer.individualCount(), 2);
        QCOMPARE(layer.sharedCount(), 0);
        QCOMPARE(layer.uploads(), mixedUploads);
    }
    void sourceSpecificStyles() {
        QTemporaryDir dir;
        {
            SettingsStore saved(dir.path());
            QVERIFY(saved.setValue("localXmlFontSize", 42)); QVERIFY(saved.setValue("localXmlBold", false));
            QVERIFY(saved.setValue("localXmlColor", "#12ABEF")); QVERIFY(saved.setValue("localXmlUseSourceColor", false));
            QVERIFY(!saved.setValue("localXmlColor", "invalid")); QVERIFY(!saved.setValue("localXmlFontSize", 2));
            saved.flushPending();
        }
        SettingsStore saved(dir.path()); auto settings = saved.values();
        DanmakuItem renderer;
        renderer.configure(danmakuStyle(settings, true, true));
        const danmaku::Item small{0, danmaku::Mode::Scroll, "same text", 0xff0000, 15};
        auto large = small; large.fontSize = 50;
        QCOMPARE(renderer.measure(small).height, renderer.measure(large).height);
        danmaku::Engine engine; engine.configure({}, 800, 400); engine.load({small});
        engine.tick(0, 0, true, [&](const auto& item) { return renderer.measure(item); }); renderer.present(engine);
        QCOMPARE(renderer.firstSnapshotFontSize(), 42.0);
        QCOMPARE(renderer.firstSnapshotColor(), QRgb(0xff12abef));
        renderer.configure(danmakuStyle(settings, true, false));
        QVERIFY(renderer.measure(large).height > renderer.measure(small).height);
        renderer.present(engine); QCOMPARE(renderer.firstSnapshotFontSize(), 15.0);
        QCOMPARE(renderer.firstSnapshotColor(), QRgb(0xffff0000));
        const auto server = danmakuStyle(settings, true, false);
        QVERIFY(!server.value("uniformFontSize").toBool()); QVERIFY(server.value("textColor").toString().isEmpty());
    }
    void delayedSynchronization_data() {
        QTest::addColumn<int>("mode");
        QTest::addColumn<double>("velocity");
        QTest::newRow("scroll") << 1 << 100.0;
        QTest::newRow("double-speed") << 1 << 200.0;
        QTest::newRow("paused") << 1 << 0.0;
        QTest::newRow("fixed") << 5 << 100.0;
    }
    void delayedSynchronization() {
        QFETCH(int, mode);
        QFETCH(double, velocity);
        QQuickWindow window;
        window.resize(800, 400);
        DanmakuItem renderer(window.contentItem());
        renderer.setSize(QSizeF(window.size()));
        renderer.configure({{"fontFamily", "Microsoft YaHei"}, {"fontSize", 25},
                            {"strokeWidth", 1}, {"lineSpacing", .2}, {"opacity", 1.0}});
        danmaku::Engine engine;
        engine.configure({}, 800, 400);
        engine.load({{0, static_cast<danmaku::Mode>(mode), "delayed synchronization"}});
        engine.tick(0, 0, true, [&](const auto& item) { return renderer.measure(item); });
        renderer.present(engine, velocity);
        const double x = renderer.firstSnapshotX();
        // Simulate backpressure between GUI snapshot preparation and scene sync.
        QTest::qSleep(40);
        window.show();
        QTRY_COMPARE(renderer.sceneEntries(), 1);
        QVERIFY(renderer.snapshotAgeMs() >= 30);
        const auto correction = mode == 1 ? velocity * std::min(.1, renderer.snapshotAgeMs() / 1000) : 0;
        QVERIFY(std::abs(renderer.firstRenderedX() - (x - correction)) < .01);
        QCOMPARE(renderer.firstSnapshotX(), x); // Domain state was not extrapolated.
        renderer.present(engine); // Pause or seek publishes an exact, frozen snapshot.
        QTRY_VERIFY(std::abs(renderer.firstRenderedX() - x) < .01);
    }
    void budgetGrowthAndHysteresis() {
        constexpr auto MiB = TextureBudget::MiB;
        TextureBudget policy;
        QCOMPARE(policy.update(10 * MiB, true, 512 * MiB, 0), 64 * MiB);
        QCOMPARE(policy.update(65 * MiB, true, 512 * MiB, 1), 128 * MiB);
        QCOMPARE(policy.update(147 * MiB, true, 512 * MiB, 2), 256 * MiB);
        QCOMPARE(policy.update(260 * MiB, true, 512 * MiB, 3), 512 * MiB);
        QCOMPARE(policy.update(10 * MiB, true, 512 * MiB, 100), 512 * MiB);
        QCOMPARE(policy.update(10 * MiB, true, 512 * MiB, 10099), 512 * MiB);
        QCOMPARE(policy.update(10 * MiB, true, 512 * MiB, 10100), 64 * MiB);
        QCOMPARE(policy.update(1000 * MiB, true, 96 * MiB, 10101), 96 * MiB);
        QCOMPARE(policy.update(0, false, 32 * MiB, 10102), 32 * MiB);
        QCOMPARE(policy.update(0, false, 160 * MiB, 10103), 160 * MiB);
        // A new burst cancels a pending automatic shrink, including timestamp zero.
        TextureBudget burst;
        QCOMPARE(burst.update(100 * MiB, true, 512 * MiB, 0), 128 * MiB);
        QCOMPARE(burst.update(1 * MiB, true, 512 * MiB, 0), 128 * MiB);
        QCOMPARE(burst.update(100 * MiB, true, 512 * MiB, 9999), 128 * MiB);
        QCOMPARE(burst.update(1 * MiB, true, 512 * MiB, 10000), 128 * MiB);
        QCOMPARE(burst.update(1 * MiB, true, 512 * MiB, 20000), 64 * MiB);
    }

    void partialScrollLaneIsClipped() {
        QQuickWindow window;
        window.setColor(Qt::transparent);
        window.resize(400, 180);
        DanmakuItem renderer(window.contentItem());
        renderer.configure({{"fontFamily", "Microsoft YaHei"}, {"fontSize", 28},
                            {"strokeWidth", 1}, {"lineSpacing", .2}, {"opacity", 1.0}});
        const double step = renderer.trackHeight();
        const int viewportHeight = static_cast<int>(std::ceil(step * 2 + step * .6));
        renderer.setSize(QSizeF(400, viewportHeight));
        QVERIFY(renderer.clip());
        danmaku::Options options;
        options.trackHeight = step; options.maxTracks = 60; options.speed = 100;
        danmaku::Engine engine;
        engine.configure(options, 100, viewportHeight);
        engine.load({{0, danmaku::Mode::Scroll, "ABC"}, {0, danmaku::Mode::Scroll, "ABC"},
                     {0, danmaku::Mode::Scroll, "ABC"}});
        engine.tick(0, 0, true, [&](const auto& item) { return renderer.measure(item); });
        QCOMPARE(engine.activeCount(), std::size_t(3));
        const auto& last = engine.activeSlots()[engine.activeIndices().back()];
        QVERIFY(last.y < viewportHeight && last.y + last.height > viewportHeight);
        engine.tick(1, 1, true, [&](const auto& item) { return renderer.measure(item); });
        renderer.present(engine);
        window.show();
        QTRY_COMPARE(renderer.sceneEntries(), 3);
        QTRY_COMPARE(renderer.imageNodeCount(), 3);
        const auto image = window.grabWindow().convertToFormat(QImage::Format_ARGB32);
        QVERIFY(!image.isNull());
        const double dpr = window.effectiveDevicePixelRatio();
        int visible = 0, outside = 0;
        for (int y = static_cast<int>(std::ceil(last.y * dpr)); y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x) {
                if (!qAlpha(image.pixel(x, y))) continue;
                if (y < static_cast<int>(std::ceil(viewportHeight * dpr))) ++visible;
                else ++outside;
            }
        QVERIFY(visible > 0);
        QCOMPARE(outside, 0);
        const auto artifacts = qEnvironmentVariable("DANMAKU_RENDERER_TEST_ARTIFACTS");
        if (!artifacts.isEmpty()) QVERIFY(image.save(artifacts + "-partial-scroll.png"));
    }

    void incrementalMotionMatchesFullRepaint() {
        QQuickWindow window;
        window.setColor(Qt::transparent);
        window.resize(403, 193);
        DanmakuItem item(window.contentItem());
        item.setSize(QSizeF(window.size()));
        item.configure({{"fontFamily", "Microsoft YaHei"}, {"fontSize", 23}, {"strokeWidth", 2},
                        {"lineSpacing", .3}, {"opacity", .65}});
        danmaku::Engine engine;
        danmaku::Options options;
        options.speed = 145; options.trackHeight = item.trackHeight(); options.overlap = true;
        engine.configure(options, window.width(), window.height());
        engine.load({{0, danmaku::Mode::Scroll, "滚动 Abc", 0xff2000},
                     {0, danmaku::Mode::Top, "顶 Top", 0x00ff00},
                     {0, danmaku::Mode::Bottom, "底 Bottom", 0x1000ff}});
        const auto measure = [&](const auto& line) { return item.measure(line); };
        engine.tick(0, 0, true, measure);
        item.present(engine);
        window.show();
        // A cold glyph upload may be deferred by the per-frame budget. Supply
        // the further update requests that normal playback would generate.
        QTRY_VERIFY(([&] { item.update(); return item.imageNodeCount() == 3; })());
        double position = 0;
        for (const double delta : {.31, .013, .029, .37, .61, .023, .52, .017, .83, .45, 1.4, 1.1}) {
            position += delta;
            engine.tick(position, delta, true, measure);
            item.present(engine);
            // The software adaptation grabs its retained backing store. A change
            // of the window clear color also gives us a full repaint to compare.
            const QImage incremental = window.grabWindow();
            QVERIFY(!incremental.isNull());
            window.setColor(Qt::white);
            window.grabWindow();
            window.setColor(Qt::transparent);
            const QImage repainted = window.grabWindow();
            QCOMPARE(incremental, repainted);
            if (position < 1) {
                const QImage pixels = incremental.convertToFormat(QImage::Format_ARGB32);
                int red = 0, green = 0, blue = 0;
                for (int y = 0; y < pixels.height(); ++y)
                    for (int x = 0; x < pixels.width(); ++x) {
                        const auto pixel = pixels.pixel(x, y);
                        if (qAlpha(pixel) < 24) continue;
                        red += qRed(pixel) > 150 && qBlue(pixel) < 64;
                        green += qGreen(pixel) > 150 && qRed(pixel) < 64;
                        blue += qBlue(pixel) > 150 && y > pixels.height() / 2;
                    }
                QVERIFY(red && green && blue);
            }
        }
        engine.clear(); item.present(engine);
        const auto cleared = window.grabWindow().convertToFormat(QImage::Format_ARGB32);
        for (int y = 0; y < cleared.height(); ++y)
            for (int x = 0; x < cleared.width(); ++x)
                QVERIFY(qAlpha(cleared.pixel(x, y)) == 0);
    }
    void colorsOutlineAndTransparency_data() {
        QTest::addColumn<int>("stroke");
        QTest::newRow("no-outline") << 0;
        QTest::newRow("default-outline") << 1;
        QTest::newRow("wide-outline") << 3;
    }
    void colorsOutlineAndTransparency() {
        QFETCH(int, stroke);
        QQuickWindow window;
        window.setColor(Qt::transparent);
        window.resize(800, 400);
        DanmakuItem renderer(window.contentItem());
        renderer.setSize(QSizeF(window.size()));
        renderer.configure({{"fontFamily", "Microsoft YaHei"}, {"fontSize", 28},
                            {"strokeWidth", stroke}, {"lineSpacing", .2}, {"opacity", 1.0}});
        danmaku::Engine engine;
        engine.configure({}, 800, 400);
        engine.load({{0, danmaku::Mode::Top, "RED ABC", 0xff0000},
                     {0, danmaku::Mode::Top, "GREEN xyz", 0x00ff00},
                     {0, danmaku::Mode::Bottom, QStringLiteral("中文 😀 e\u0301").toUtf8().toStdString(), 0xffffff}});
        engine.tick(0, 0, true, [&](const auto& item) { return renderer.measure(item); });
        renderer.present(engine);
        window.show();
        QTRY_COMPARE(renderer.sceneEntries(), 3);
        QElapsedTimer preparation;
        preparation.start();
        while (renderer.imageNodeCount() < 3 && preparation.elapsed() < 3000) {
            renderer.present(engine);
            QTest::qWait(20);
        }
        QCOMPARE(renderer.imageNodeCount(), 3);
        const auto image = window.grabWindow().convertToFormat(QImage::Format_ARGB32);
        QVERIFY(!image.isNull());
        int red = 0, green = 0, black = 0, transparent = 0;
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x) {
                const auto pixel = image.pixel(x, y);
                if (qAlpha(pixel) == 0) ++transparent;
                else {
                    if (qRed(pixel) > 100 && qGreen(pixel) < 30) ++red;
                    if (qGreen(pixel) > 100 && qRed(pixel) < 30) ++green;
                    if (qRed(pixel) < 10 && qGreen(pixel) < 10 && qBlue(pixel) < 10) ++black;
                }
            }
        QVERIFY(red > 0);
        QVERIFY(green > 0);
        QVERIFY(transparent > 0);
        if (stroke) QVERIFY(black > 0);
        const auto artifacts = qEnvironmentVariable("DANMAKU_RENDERER_TEST_ARTIFACTS");
        if (!artifacts.isEmpty())
            QVERIFY(image.save(artifacts + QString("-stroke%1.png").arg(stroke)));
    }

    void progressivePreparationAndRetirement() {
        QQuickWindow window;
        window.resize(2560, 1440);
        DanmakuItem renderer(window.contentItem());
        renderer.setSize(QSizeF(window.size()));
        QVariantMap settings{{"fontFamily", "Microsoft YaHei"}, {"fontSize", 24},
                             {"strokeWidth", 1}, {"lineSpacing", .2}, {"opacity", 1.0},
                             {"textureBudgetAuto", false}, {"textureBudgetMiB", 64}};
        renderer.configure(settings);
        QVERIFY(renderer.imageBackend());
        danmaku::Options options;
        options.maxActive = 100;
        options.maxTracks = 100;
        options.trackHeight = renderer.trackHeight();
        options.overlap = true;
        options.fixedSeconds = 30;
        danmaku::Engine engine;
        // A virtual tall viewport supplies collision-free text for cache pressure.
        // The actual window clips the extra rows; production overlap is cross-class only.
        engine.configure(options, window.width(), 100 * options.trackHeight);
        std::vector<danmaku::Item> items;
        const qreal dpr = window.effectiveDevicePixelRatio();
        // Keep raster area comparable across DPI settings and below the per-texture cap.
        const int warmupGlyphs = std::max(1, qRound(20 / (dpr * dpr)));
        for (int i = 0; i < 100; ++i)
            items.push_back({0, danmaku::Mode::Top,
                (QString(warmupGlyphs, QChar(0x5f39)) + QString::number(i)).toUtf8().toStdString(), 0xffffff});
        engine.load(std::move(items));
        engine.tick(0, 0, true, [&](const auto& item) { return renderer.measure(item); });
        renderer.present(engine);
        window.show();
        QTRY_COMPARE(renderer.sceneEntries(), 100);
        // The cold frame cannot upload the whole burst. Deferred text remains visible.
        QVERIFY(renderer.imageNodeCount() > 0);
        QVERIFY(renderer.imageNodeCount() < 100);
        const auto coldCount = renderer.imageNodeCount();
        const bool native = QGuiApplication::platformName() == "windows";
        const auto id = renderer.firstSnapshotId();
        const auto x = renderer.firstSnapshotX();
        QElapsedTimer warmup;
        warmup.start();
        while (((native && renderer.imageNodeCount() < 100) || renderer.imageNodeCount() <= coldCount ||
                renderer.textureBytes() <= TextTextureCache::maxUploadBytes) && warmup.elapsed() < 10000) {
            renderer.present(engine);
            QTest::qWait(20);
        }
        QVERIFY(renderer.imageNodeCount() > coldCount);
        if (native) QCOMPARE(renderer.imageNodeCount(), 100);
        QCOMPARE(renderer.firstSnapshotId(), id);
        QCOMPARE(renderer.firstSnapshotX(), x);
        QVERIFY(renderer.textureBytes() > TextTextureCache::maxUploadBytes);
        QVERIFY(renderer.textureBytes() <= renderer.textureBudgetBytes());

        // New appearances replace the old resource budget rather than keeping stale textures.
        settings["strokeWidth"] = 3;
        settings["fontSize"] = 64;
        renderer.configure(settings);
        options.trackHeight = renderer.trackHeight();
        engine.reconfigure(options, window.width(), 100 * options.trackHeight,
                           [&](const auto& item) { return renderer.measure(item); });
        renderer.present(engine);
        QTest::qWait(100);
        QVERIFY(renderer.textureBytes() <= renderer.textureBudgetBytes());
        QCOMPARE(renderer.firstSnapshotId(), id);

        // Saturate the real cache cap with unique live textures; excess entries stay as text.
        settings["strokeWidth"] = 0;
        renderer.configure(settings);
        // Change membership so QTRY_COMPARE waits for this snapshot, rather than
        // accepting the preceding workload's still-published scene count.
        constexpr int pressureComments = 96;
        options.maxActive = pressureComments;
        options.trackHeight = renderer.trackHeight();
        engine.configure(options, window.width(), 100 * options.trackHeight);
        items.clear();
        const int pressureGlyphs = std::max(1, qRound(45 / (dpr * dpr)));
        for (int i = 0; i < pressureComments; ++i)
            items.push_back({0, danmaku::Mode::Top,
                (QString(pressureGlyphs, QChar(0x5f39)) + QString::number(i)).toUtf8().toStdString(), 0xffffff});
        engine.load(std::move(items));
        engine.tick(0, 0, true, [&](const auto& item) { return renderer.measure(item); });
        renderer.present(engine);
        QTRY_COMPARE(renderer.sceneEntries(), pressureComments);
        warmup.restart();
        const auto initialBytes = renderer.textureBytes();
        while (((native && (renderer.textureBytes() < 60 * 1024 * 1024 ||
                            renderer.textureFallbackReasons()["capacity"].toInt() == 0)) ||
                renderer.textureBytes() <= initialBytes ||
                renderer.textureBytes() <= TextTextureCache::maxUploadBytes) && warmup.elapsed() < 10000) {
            renderer.present(engine);
            QTest::qWait(20);
        }
        QVERIFY(renderer.textureBytes() > initialBytes);
        if (native) QVERIFY(renderer.textureBytes() >= 60 * 1024 * 1024);
        QVERIFY(renderer.textureBytes() <= renderer.textureBudgetBytes());
        QVERIFY(renderer.imageNodeCount() < renderer.snapshotCount());
        if (native) QVERIFY(renderer.textureFallbackReasons()["capacity"].toInt() > 0);
        const auto currentId = renderer.firstSnapshotId();
        const auto currentX = renderer.firstSnapshotX();
        const auto currentLayouts = renderer.cacheMisses();
        settings["textureBudgetMiB"] = 32;
        renderer.configure(settings);
        renderer.present(engine);
        QTRY_COMPARE(renderer.textureBudgetBytes(), qint64(32 * TextureBudget::MiB));
        QVERIFY(renderer.textureBytes() <= renderer.textureBudgetBytes());
        QCOMPARE(renderer.firstSnapshotId(), currentId);
        QCOMPARE(renderer.firstSnapshotX(), currentX);
        QCOMPARE(renderer.cacheMisses(), currentLayouts);
        QCOMPARE(renderer.sceneEntries(), pressureComments);
        settings["textureBudgetAuto"] = true;
        settings["textureBudgetMiB"] = 128;
        renderer.configure(settings);
        renderer.present(engine);
        QTRY_COMPARE(renderer.textureBudgetBytes(), qint64(128 * TextureBudget::MiB));
        const auto reducedBytes = renderer.textureBytes();
        warmup.restart();
        while (renderer.textureBytes() <= reducedBytes && warmup.elapsed() < 10000) {
            renderer.present(engine);
            QTest::qWait(20);
        }
        QVERIFY(renderer.textureBytes() > reducedBytes);
        QVERIFY(renderer.textureBytes() <= renderer.textureBudgetBytes());
        QCOMPARE(renderer.firstSnapshotId(), currentId);
        QCOMPARE(renderer.firstSnapshotX(), currentX);
        engine.unload();
        renderer.clearContent();
        QTRY_COMPARE(renderer.sceneEntries(), 0);
        QCOMPARE(renderer.textureBytes(), qint64(0));
        QCOMPARE(renderer.textureBudgetBytes(), qint64(0));
        QCOMPARE(renderer.textureDemandBytes(), qint64(0));
    }

    void sharedTexturesAndOversizeFallback() {
        QQuickWindow window;
        window.resize(800, 600);
        DanmakuItem renderer(window.contentItem());
        renderer.setSize(QSizeF(window.size()));
        renderer.configure({{"fontFamily", "Microsoft YaHei"}, {"fontSize", 24},
                            {"strokeWidth", 1}, {"lineSpacing", .2}, {"opacity", 1.0}});
        danmaku::Options options;
        options.overlap = true;
        danmaku::Engine engine;
        engine.configure(options, 800, 600);
        const auto duplicate = QStringLiteral("中文 A 😀 e\u0301").toUtf8().toStdString();
        engine.load({{0, danmaku::Mode::Top, duplicate, 0xff4040}});
        engine.tick(0, 0, true, [&](const auto& item) { return renderer.measure(item); });
        renderer.present(engine);
        window.show();
        QTRY_COMPARE(renderer.imageNodeCount(), 1);
        const auto bytes = renderer.textureBytes();
        engine.load({{0, danmaku::Mode::Top, duplicate, 0xff4040},
                     {0, danmaku::Mode::Top, duplicate, 0xff4040},
                     {0, danmaku::Mode::Top, QString(1000, QChar(0x5f39)).toUtf8().toStdString(), 0xffffff}});
        engine.tick(0, 0, true, [&](const auto& item) { return renderer.measure(item); });
        renderer.present(engine);
        QTRY_COMPARE(renderer.sceneEntries(), 3);
        QTRY_COMPARE(renderer.imageNodeCount(), 2);
        QCOMPARE(renderer.textureBytes(), bytes);
        QCOMPARE(renderer.textureDemandBytes(), bytes);
        QCOMPARE(renderer.textureFallbackReasons()["size"].toInt(), 1);
        const auto image = window.grabWindow();
        QVERIFY(!image.isNull());
        const auto artifacts = qEnvironmentVariable("DANMAKU_RENDERER_TEST_ARTIFACTS");
        if (!artifacts.isEmpty()) QVERIFY(image.save(artifacts + "-fallback.png"));
        engine.unload();
        renderer.clearContent();
        QTRY_COMPARE(renderer.textureBytes(), qint64(0));
    }
};

QTEST_MAIN(RendererTests)
#include "RendererTests.moc"
