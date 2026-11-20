#pragma once
#include <QtGui/qrgb.h>
#include <QtGlobal>
#include <memory>
#include <span>

class QTextLayout;

struct DanmakuVisual {
    quint64 id{};
    double x{}, y{};
    QRgb color{};
    std::shared_ptr<QTextLayout> layout;
    bool scrolling{};
};

// GUI-thread owner of a retained visual tree on the existing input-transparent HWND.
class DesktopScrollLayer {
public:
    // Bounds on the number of DirectComposition segments, independent of bitmap memory.
    static constexpr int maxNewKeyframes = 4096;
    static constexpr int maxIndividualKeyframes = 32768;
    DesktopScrollLayer();
    ~DesktopScrollLayer();
    bool synchronize(quintptr window, std::span<const DanmakuVisual> lines, qreal dpr, int stroke,
                     qreal opacity, double speed, double cadence, quint64 generation, qint64 byteLimit, quintptr primaryScreen = 0);
    void reset();
    quint64 commits() const { return commits_; }
    quint64 uploads() const { return uploads_; }
    qint64 bytes() const;
    int count() const;
    int individualCount() const;
    int sharedCount() const;
    int individualKeyframes() const;
    bool refreshAligned() const;
    double animationCadence() const;
    long lastError() const { return lastError_; }
private:
    struct State;
    std::unique_ptr<State> state_;
    quint64 commits_{}, uploads_{};
    long lastError_{};
};
