#ifndef WEBSERVER_H
#define WEBSERVER_H

#include <QTcpServer>
#include <QTcpSocket>
#include <QMutex>
#include <QImage>
#include <QByteArray>
#include <QMap>
#include <QList>
#include <QTimer>
#include <QElapsedTimer>
#include <QProcess>

#include "../QtScrcpyCore/include/QtScrcpyCore.h"

class FrameCapture : public QObject, public qsc::DeviceObserver
{
    Q_OBJECT
public:
    explicit FrameCapture(const QString &serial, QObject *parent = nullptr);
    ~FrameCapture();

    QByteArray getImageData(QString &format);
    bool hasFrame() const;
    QSize frameSize() const { return m_frameSize; }
    quint64 version() const { return m_version; }

private:
    void onFrame(int width, int height, uint8_t *dataY, uint8_t *dataU, uint8_t *dataV,
                 int linesizeY, int linesizeU, int linesizeV) override;

    QString m_serial;
    mutable QMutex m_mutex;
    QImage m_image;
    QSize m_frameSize;
    QElapsedTimer m_throttleTimer;
    quint64 m_version = 0;
};


class WebServer : public QTcpServer
{
    Q_OBJECT
public:
    explicit WebServer(QObject *parent = nullptr);
    ~WebServer();

    bool startServer(quint16 port = 8080);
    void stopServer();
    quint16 serverPort() const { return m_port; }

    void addDevice(const QString &serial);
    void removeDevice(const QString &serial);
    void clearDevices();

protected:
    void incomingConnection(qintptr socketDescriptor) override;

private:
    bool checkAuth(QTcpSocket *socket, const QString &request, QString &username);
    void sendLoginPage(QTcpSocket *socket);
    void handleRequest(QTcpSocket *socket);
    void sendResponse(QTcpSocket *socket, int statusCode, const QString &contentType,
                      const QByteArray &body, const QByteArray &extraHeaders = QByteArray());
    void sendHtmlPage(QTcpSocket *socket, const QString &username);
    void sendDeviceList(QTcpSocket *socket);
    void sendSnapshot(QTcpSocket *socket, const QString &serial);
    void sendClick(QTcpSocket *socket, const QString &serial, const QByteArray &body);
    void sendSwipe(QTcpSocket *socket, const QString &serial, const QByteArray &body);
    void sendCustomButtons(QTcpSocket *socket);
    void sendShellCmd(QTcpSocket *socket, const QString &serial, const QByteArray &body);

    bool handleWebSocketUpgrade(QTcpSocket *socket, const QString &request, const QString &username);
    void sendWsFrame(QTcpSocket *socket, const QByteArray &data, bool binary = false);
    void onWsData(QTcpSocket *socket);
    void processWsMessage(QTcpSocket *socket, const QByteArray &message, bool binary);
    void removeWsClient(QTcpSocket *socket);
    void pushFramesToClients();
    void sendWsDeviceList();

    QJsonArray buildDeviceArray();
    void loadAuthConfig();
    QString configDir();

    quint16 m_port = 0;
    QMap<QString, FrameCapture*> m_captures;
    QStringList m_deviceOrder;
    QList<QTcpSocket*> m_wsClients;
    QMap<QTcpSocket*, QString> m_wsUsernames;
    QMap<QString, quint64> m_lastPushedVersion;
    QTimer *m_pushTimer = nullptr;
    QMap<QString, QString> m_users;
    bool m_authEnabled = false;
    QString m_iframeUrl;
};

#endif // WEBSERVER_H
