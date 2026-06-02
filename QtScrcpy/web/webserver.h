#ifndef WEBSERVER_H
#define WEBSERVER_H

#include <QTcpServer>
#include <QTcpSocket>
#include <QMutex>
#include <QImage>
#include <QByteArray>
#include <QMap>
#include <QElapsedTimer>

#include "../QtScrcpyCore/include/QtScrcpyCore.h"

class FrameCapture : public QObject, public qsc::DeviceObserver
{
    Q_OBJECT
public:
    explicit FrameCapture(const QString &serial, QObject *parent = nullptr);
    ~FrameCapture();

    QByteArray getJpeg();
    bool hasFrame() const;
    QSize frameSize() const { return m_frameSize; }

private:
    void onFrame(int width, int height, uint8_t *dataY, uint8_t *dataU, uint8_t *dataV,
                 int linesizeY, int linesizeU, int linesizeV) override;

    QString m_serial;
    mutable QMutex m_mutex;
    QImage m_image;
    QSize m_frameSize;
    QElapsedTimer m_throttleTimer;
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
    void handleRequest(QTcpSocket *socket);
    void sendResponse(QTcpSocket *socket, int statusCode, const QString &contentType,
                      const QByteArray &body);
    void sendHtmlPage(QTcpSocket *socket);
    void sendDeviceList(QTcpSocket *socket);
    void sendSnapshot(QTcpSocket *socket, const QString &serial);
    void sendClick(QTcpSocket *socket, const QString &serial, const QByteArray &body);

    quint16 m_port = 0;
    QMap<QString, FrameCapture*> m_captures;
};

#endif // WEBSERVER_H
