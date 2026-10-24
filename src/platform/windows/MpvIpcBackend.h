#pragma once
#include "infrastructure/MediaPlaybackBackend.h"
#include <QLocalSocket>
#include <QTimer>
#include <QElapsedTimer>
#include <QRect>

class MpvIpcBackend final : public MediaPlaybackBackend {
    Q_OBJECT
  public:
    explicit MpvIpcBackend(QObject* parent = nullptr);
    ~MpvIpcBackend() override;
    QString bind(const QVariantMap& descriptor);
    void detach();
    bool accepts(const QString& id) const override { return !id_.isEmpty() && id == id_; }
    void select(const QString& id, quint64 generation) override;
    void requestPlayback(MediaSample, bool, quint64 request) override;
    void requestSeek(MediaSample, double, quint64 request) override;
    QRect clientRect() const { return rect_; }
    quintptr window() const { return window_; }
    bool windowVisible() const { return windowVisible_; }
    bool foreground() const;
    QString screenName() const;
    QRect nativeScreenRect() const;
    MediaSample latest() const { return latest_; }
  signals:
    void geometryChanged();
    void processExited();
    void contentChanged(QString path);
  private:
    void maintain();
    void receive();
    void command(const QString& property, int request);
    void publish();
    void locateWindow();
    QLocalSocket socket_;
    QTimer timer_;
    QVariantMap descriptor_;
    QVariantMap properties_;
    QByteArray buffer_;
    QString id_, selected_, path_;
    quint64 generation_{}, contentToken_{1};
    double observed_{}, lastQuery_{};
    bool validPosition_{}, exited_{}, windowVisible_{};
    quintptr window_{};
    QRect rect_;
    MediaSample latest_;
    void* processHandle_{};
};
