#include "infrastructure/InstanceBroker.h"
#include <QCryptographicHash>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QLockFile>
#include <QDir>
#include <QTimer>
#include <QElapsedTimer>

InstanceBroker::InstanceBroker(const QString& data, QObject* parent) : QObject(parent) {
    name_ = "DanmaX-" + QString::fromLatin1(QCryptographicHash::hash(
        (QFileInfo(data).canonicalFilePath().toCaseFolded() + '\n' + qEnvironmentVariable("USERNAME")).toUtf8(),
        QCryptographicHash::Sha256).toHex().left(32));
    server_.setSocketOptions(QLocalServer::UserAccessOption);
    connect(&server_, &QLocalServer::newConnection, this, [this] {
        while (server_.hasPendingConnections()) {
            auto* socket = server_.nextPendingConnection(); auto bytes = std::make_shared<QByteArray>();
            connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
            QTimer::singleShot(5000, socket, [socket] { socket->disconnectFromServer(); });
            connect(socket, &QLocalSocket::readyRead, this, [this, socket, bytes] {
                *bytes += socket->readAll();
                if (bytes->size() > 64 * 1024) { socket->abort(); return; }
                if (!bytes->contains('\n')) return;
                disconnect(socket, &QLocalSocket::readyRead, this, nullptr);
                const auto object = QJsonDocument::fromJson(bytes->left(bytes->indexOf('\n'))).object();
                const auto error = object.value("version").toInt() == 1 && receive
                    ? receive(object.toVariantMap()) : QStringLiteral("会话请求格式无效");
                socket->write(QJsonDocument(QJsonObject{{"error", error}}).toJson(QJsonDocument::Compact) + '\n');
                socket->disconnectFromServer();
            });
        }
    });
}
bool InstanceBroker::claim(const QVariantMap& message, QString& error) {
    // The lock serializes simultaneous browser launches, including first startup.
    QLockFile lock(QDir::temp().filePath(name_ + ".lock"));
    if (!lock.tryLock(5000)) { error = QStringLiteral("DanmaX 启动繁忙，请重试"); return false; }
    QLocalSocket socket;
    socket.connectToServer(name_);
    if (socket.waitForConnected(300)) {
        socket.write(QJsonDocument(QJsonObject::fromVariantMap(message)).toJson(QJsonDocument::Compact) + '\n');
        socket.waitForBytesWritten(1000);
        QByteArray reply; QElapsedTimer elapsed; elapsed.start();
        while (!reply.contains('\n') && elapsed.elapsed() < 10000) {
            if (socket.bytesAvailable() || socket.waitForReadyRead(500)) reply += socket.readAll();
            else if (socket.state() == QLocalSocket::UnconnectedState) break;
        }
        const auto document = QJsonDocument::fromJson(reply.trimmed());
        error = document.isObject() ? document.object().value("error").toString()
            : QStringLiteral("已有 DanmaX 实例没有响应，请关闭后重试");
        return false;
    }
    // Windows named pipes disappear with their server; never unlink a live peer.
    if (!server_.listen(name_)) { error = QStringLiteral("无法创建 DanmaX 本地通信管道"); return false; }
    return true;
}
