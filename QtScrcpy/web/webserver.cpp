#include "webserver.h"
#include "config.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QBuffer>
#include <QMouseEvent>
#include <QHostAddress>
#include <QCryptographicHash>
#include <QCoreApplication>
#include <QFile>
#include <QProcess>

// ---- FrameCapture ----

FrameCapture::FrameCapture(const QString &serial, QObject *parent)
    : QObject(parent), m_serial(serial)
{
    m_throttleTimer.start();
}

FrameCapture::~FrameCapture()
{
}

void FrameCapture::onFrame(int width, int height, uint8_t *dataY, uint8_t *dataU, uint8_t *dataV,
                           int linesizeY, int linesizeU, int linesizeV)
{
    if (m_throttleTimer.elapsed() < 300) {
        QMutexLocker lock(&m_mutex);
        m_frameSize = QSize(width, height);
        return;
    }
    m_throttleTimer.restart();

    int sw = width / 2;
    int sh = height / 2;
    if (sw < 1 || sh < 1) return;

    QImage img(sw, sh, QImage::Format_RGB888);
    for (int y = 0; y < sh; y++) {
        uint8_t *line = img.scanLine(y);
        int srcY = y * 2;
        for (int x = 0; x < sw; x++) {
            int srcX = x * 2;
            int yIdx = srcY * linesizeY + srcX;
            int uIdx = (srcY / 2) * linesizeU + (srcX / 2);
            int vIdx = (srcY / 2) * linesizeV + (srcX / 2);

            int Y = dataY[yIdx];
            int U = dataU[uIdx] - 128;
            int V = dataV[vIdx] - 128;

            line[x * 3]     = static_cast<uint8_t>(qBound(0, Y + ((V * 1436) >> 10), 255));
            line[x * 3 + 1] = static_cast<uint8_t>(qBound(0, Y - ((U * 352 + V * 731) >> 10), 255));
            line[x * 3 + 2] = static_cast<uint8_t>(qBound(0, Y + ((U * 1815) >> 10), 255));
        }
    }

    QMutexLocker lock(&m_mutex);
    m_image = img.copy();
    m_frameSize = QSize(width, height);
    m_version++;
}

bool FrameCapture::hasFrame() const
{
    QMutexLocker lock(&m_mutex);
    return !m_image.isNull();
}

QByteArray FrameCapture::getImageData(QString &format)
{
    QMutexLocker lock(&m_mutex);
    if (m_image.isNull()) return QByteArray();

    QByteArray data;
    QBuffer buf(&data);
    buf.open(QIODevice::WriteOnly);

    if (m_image.save(&buf, "JPEG", 85)) {
        format = "image/jpeg";
        return data;
    }

    data.clear();
    buf.seek(0);
    if (m_image.save(&buf, "PNG")) {
        format = "image/png";
        return data;
    }

    data.clear();
    buf.seek(0);
    if (m_image.save(&buf, "BMP")) {
        format = "image/bmp";
        return data;
    }

    return QByteArray();
}


// ---- WebServer ----

WebServer::WebServer(QObject *parent)
    : QTcpServer(parent)
{
    loadAuthConfig();
}

QString WebServer::configDir()
{
    return QCoreApplication::applicationDirPath() + "/config";
}

void WebServer::loadAuthConfig()
{
    QFile authFile(configDir() + "/web_auth.json");
    if (authFile.open(QIODevice::ReadOnly)) {
        QJsonDocument doc = QJsonDocument::fromJson(authFile.readAll());
        authFile.close();
        QJsonObject obj = doc.object();
        QJsonArray users = obj["users"].toArray();
        for (const auto &u : users) {
            QJsonObject user = u.toObject();
            QString name = user["username"].toString();
            QString pass = user["password"].toString();
            if (!name.isEmpty() && !pass.isEmpty()) {
                m_users[name] = pass;
            }
        }
        m_authEnabled = !m_users.isEmpty();
        m_iframeUrl = obj["iframe_url"].toString();
        m_webhookUrl = obj["webhook_url"].toString();
    }
}

WebServer::~WebServer()
{
    stopServer();
}

bool WebServer::startServer(quint16 port)
{
    if (isListening()) return true;

    if (listen(QHostAddress::Any, port)) {
        m_port = serverPort();
        if (!m_pushTimer) {
            m_pushTimer = new QTimer(this);
            connect(m_pushTimer, &QTimer::timeout, this, &WebServer::pushFramesToClients);
            m_pushTimer->start(300);
        }
        return true;
    }

    for (quint16 p = 8080; p < 8100; p++) {
        if (listen(QHostAddress::Any, p)) {
            m_port = serverPort();
            if (!m_pushTimer) {
                m_pushTimer = new QTimer(this);
                connect(m_pushTimer, &QTimer::timeout, this, &WebServer::pushFramesToClients);
                m_pushTimer->start(300);
            }
            return true;
        }
    }
    return false;
}

void WebServer::stopServer()
{
    if (m_pushTimer) {
        m_pushTimer->stop();
        delete m_pushTimer;
        m_pushTimer = nullptr;
    }
    for (auto *ws : m_wsClients) {
        ws->disconnectFromHost();
    }
    m_wsClients.clear();
    m_lastPushedVersion.clear();
    close();
    clearDevices();
}

void WebServer::addDevice(const QString &serial)
{
    if (m_captures.contains(serial)) return;

    auto *capture = new FrameCapture(serial, this);
    auto device = qsc::IDeviceManage::getInstance().getDevice(serial);
    if (device) {
        device->registerDeviceObserver(capture);
    }
    m_captures[serial] = capture;
    m_deviceOrder.append(serial);
    sendWsDeviceList();
}

void WebServer::removeDevice(const QString &serial)
{
    if (!m_captures.contains(serial)) return;

    auto *capture = m_captures.take(serial);
    auto device = qsc::IDeviceManage::getInstance().getDevice(serial);
    if (device) {
        device->deRegisterDeviceObserver(capture);
    }
    capture->deleteLater();
    m_lastPushedVersion.remove(serial);
    m_deviceOrder.removeAll(serial);
    sendWsDeviceList();
}

void WebServer::clearDevices()
{
    for (auto it = m_captures.begin(); it != m_captures.end(); ++it) {
        auto device = qsc::IDeviceManage::getInstance().getDevice(it.key());
        if (device) {
            device->deRegisterDeviceObserver(it.value());
        }
        it.value()->deleteLater();
    }
    m_captures.clear();
    m_lastPushedVersion.clear();
    m_deviceOrder.clear();
}

void WebServer::incomingConnection(qintptr socketDescriptor)
{
    auto *socket = new QTcpSocket(this);
    socket->setSocketDescriptor(socketDescriptor);
    connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
        if (m_wsClients.contains(socket)) {
            onWsData(socket);
        } else {
            handleRequest(socket);
        }
    });
    connect(socket, &QTcpSocket::disconnected, this, [this, socket]() {
        removeWsClient(socket);
        socket->deleteLater();
    });
}

bool WebServer::checkAuth(QTcpSocket *socket, const QString &request, QString &username)
{
    if (!m_authEnabled) {
        username = "anonymous";
        return true;
    }

    QStringList lines = request.split("\r\n");
    for (const auto &line : lines) {
        if (line.startsWith("Authorization: Basic ", Qt::CaseInsensitive)) {
            QByteArray decoded = QByteArray::fromBase64(line.mid(21).trimmed().toUtf8());
            QString creds = QString::fromUtf8(decoded);
            int colon = creds.indexOf(':');
            if (colon > 0) {
                QString user = creds.left(colon);
                QString pass = creds.mid(colon + 1);
                if (m_users.contains(user) && m_users[user] == pass) {
                    username = user;
                    return true;
                }
            }
        }
    }

    QByteArray body = "Unauthorized";
    QByteArray authHeader = "WWW-Authenticate: Basic realm=\"AniFelix Remote\"\r\n";
    sendResponse(socket, 401, "text/plain", body, authHeader);
    return false;
}

void WebServer::handleRequest(QTcpSocket *socket)
{
    QByteArray data = socket->readAll();
    QString request = QString::fromUtf8(data);
    QStringList lines = request.split("\r\n");
    if (lines.isEmpty()) return;

    QStringList parts = lines[0].split(" ");
    if (parts.size() < 2) return;

    QString method = parts[0];
    QString path = parts[1];
    int qmark = path.indexOf('?');
    if (qmark >= 0) path = path.left(qmark);

    QString username;
    if (!checkAuth(socket, request, username)) return;

    if (method == "GET" && path == "/ws") {
        if (handleWebSocketUpgrade(socket, request, username)) return;
    }

    if (method == "GET" && path == "/") {
        sendHtmlPage(socket, username);
    } else if (method == "GET" && path == "/api/devices") {
        sendDeviceList(socket);
    } else if (method == "GET" && path.startsWith("/api/snapshot/")) {
        QString serial = path.mid(14);
        sendSnapshot(socket, serial);
    } else if (method == "POST" && path.startsWith("/api/click/")) {
        QString serial = path.mid(11);
        QByteArray body;
        int bodyStart = data.indexOf("\r\n\r\n");
        if (bodyStart >= 0) body = data.mid(bodyStart + 4);
        sendClick(socket, serial, body);
    } else if (method == "POST" && path.startsWith("/api/swipe/")) {
        QString serial = path.mid(11);
        QByteArray body;
        int bodyStart = data.indexOf("\r\n\r\n");
        if (bodyStart >= 0) body = data.mid(bodyStart + 4);
        sendSwipe(socket, serial, body);
    } else if (method == "POST" && path.startsWith("/api/action/")) {
        QString rest = path.mid(12);
        int slash = rest.indexOf('/');
        if (slash > 0) {
            QString serial = rest.left(slash);
            QString action = rest.mid(slash + 1);
            auto device = qsc::IDeviceManage::getInstance().getDevice(serial);
            if (device) {
                if (action == "home") device->postGoHome();
                else if (action == "back") device->postGoBack();
                else if (action == "menu") device->postGoMenu();
                else if (action == "lock") device->setDisplayPower(false);
                else if (action == "wake") device->setDisplayPower(true);
                else if (action == "volup") device->postVolumeUp();
                else if (action == "voldown") device->postVolumeDown();
                else if (action == "appswitch") device->postAppSwitch();
                sendResponse(socket, 200, "application/json", "{\"ok\":true}");
            } else {
                sendResponse(socket, 404, "application/json", "{\"error\":\"device not found\"}");
            }
        }
    } else if (method == "POST" && path.startsWith("/api/keyevent/")) {
        QString serial = path.mid(14);
        auto device = qsc::IDeviceManage::getInstance().getDevice(serial);
        if (device) {
            QByteArray body;
            int bodyStart = data.indexOf("\r\n\r\n");
            if (bodyStart >= 0) body = data.mid(bodyStart + 4);
            QJsonDocument jdoc = QJsonDocument::fromJson(body);
            int keycode = jdoc.object()["keycode"].toInt();
            if (keycode > 0) {
                QKeyEvent pressEvt(QEvent::KeyPress, keycode, Qt::NoModifier);
                auto *capture = m_captures.value(serial, nullptr);
                QSize fs = capture ? capture->frameSize() : QSize(1080, 1920);
                if (fs.isEmpty()) fs = QSize(1080, 1920);
                device->keyEvent(&pressEvt, fs, fs);
                QKeyEvent releaseEvt(QEvent::KeyRelease, keycode, Qt::NoModifier);
                device->keyEvent(&releaseEvt, fs, fs);
            }
            sendResponse(socket, 200, "application/json", "{\"ok\":true}");
        } else {
            sendResponse(socket, 404, "application/json", "{\"error\":\"device not found\"}");
        }
    } else if (method == "GET" && path == "/api/buttons") {
        sendCustomButtons(socket);
    } else if (method == "POST" && path.startsWith("/api/shell/")) {
        QString serial = path.mid(11);
        QByteArray body;
        int bodyStart = data.indexOf("\r\n\r\n");
        if (bodyStart >= 0) body = data.mid(bodyStart + 4);
        sendShellCmd(socket, serial, body);
    } else if (method == "GET" && path == "/api/debug") {
        QJsonObject dbg;
        dbg["captureCount"] = m_captures.size();
        dbg["wsClients"] = m_wsClients.size();
        QJsonArray arr;
        for (auto it = m_captures.begin(); it != m_captures.end(); ++it) {
            QJsonObject d;
            d["serial"] = it.key();
            d["hasFrame"] = it.value()->hasFrame();
            QSize fs = it.value()->frameSize();
            d["frameW"] = fs.width();
            d["frameH"] = fs.height();
            d["version"] = static_cast<qint64>(it.value()->version());
            QString fmt;
            QByteArray imgData = it.value()->getImageData(fmt);
            d["imageBytes"] = imgData.size();
            d["imageFormat"] = fmt;
            arr.append(d);
        }
        dbg["devices"] = arr;
        sendResponse(socket, 200, "application/json", QJsonDocument(dbg).toJson());
    } else {
        sendResponse(socket, 404, "text/plain", "Not Found");
    }
}

void WebServer::sendResponse(QTcpSocket *socket, int statusCode, const QString &contentType,
                              const QByteArray &body, const QByteArray &extraHeaders)
{
    QString statusText;
    if (statusCode == 200) statusText = "OK";
    else if (statusCode == 401) statusText = "Unauthorized";
    else statusText = "Not Found";
    QByteArray response;
    response.append(QString("HTTP/1.1 %1 %2\r\n").arg(statusCode).arg(statusText).toUtf8());
    response.append(QString("Content-Type: %1\r\n").arg(contentType).toUtf8());
    response.append(QString("Content-Length: %1\r\n").arg(body.size()).toUtf8());
    response.append("Access-Control-Allow-Origin: *\r\n");
    if (!extraHeaders.isEmpty()) response.append(extraHeaders);
    response.append("Connection: close\r\n");
    response.append("\r\n");
    response.append(body);
    socket->write(response);
    socket->flush();
    socket->disconnectFromHost();
}

QJsonArray WebServer::buildDeviceArray()
{
    QJsonArray devices;
    for (int i = m_deviceOrder.size() - 1; i >= 0; i--) {
        const QString &serial = m_deviceOrder[i];
        auto *capture = m_captures.value(serial, nullptr);
        if (!capture) continue;
        QJsonObject dev;
        dev["serial"] = serial;
        dev["name"] = Config::getInstance().getNickName(serial);
        QSize fs = capture->frameSize();
        dev["width"] = fs.width();
        dev["height"] = fs.height();
        dev["hasFrame"] = capture->hasFrame();
        devices.append(dev);
    }
    return devices;
}

void WebServer::sendDeviceList(QTcpSocket *socket)
{
    QJsonDocument doc(buildDeviceArray());
    sendResponse(socket, 200, "application/json", doc.toJson(QJsonDocument::Compact));
}

void WebServer::sendSnapshot(QTcpSocket *socket, const QString &serial)
{
    if (!m_captures.contains(serial)) {
        sendResponse(socket, 404, "text/plain", "Device not found");
        return;
    }
    QString contentType;
    QByteArray imageData = m_captures[serial]->getImageData(contentType);
    if (imageData.isEmpty()) {
        sendResponse(socket, 503, "text/plain", "No frame available");
        return;
    }
    sendResponse(socket, 200, contentType, imageData);
}

void WebServer::sendClick(QTcpSocket *socket, const QString &serial, const QByteArray &body)
{
    auto device = qsc::IDeviceManage::getInstance().getDevice(serial);
    if (!device) {
        sendResponse(socket, 404, "application/json", "{\"error\":\"device not found\"}");
        return;
    }

    QJsonDocument doc = QJsonDocument::fromJson(body);
    QJsonObject obj = doc.object();
    double xRatio = obj["x"].toDouble();
    double yRatio = obj["y"].toDouble();

    auto *capture = m_captures.value(serial, nullptr);
    QSize frameSize = capture ? capture->frameSize() : QSize(1080, 1920);
    if (frameSize.isEmpty()) frameSize = QSize(1080, 1920);

    int clickX = static_cast<int>(xRatio * frameSize.width());
    int clickY = static_cast<int>(yRatio * frameSize.height());

    QPoint pos(clickX, clickY);
    QMouseEvent pressEvent(QEvent::MouseButtonPress, pos, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    device->mouseEvent(&pressEvent, frameSize, frameSize);

    QMouseEvent releaseEvent(QEvent::MouseButtonRelease, pos, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    device->mouseEvent(&releaseEvent, frameSize, frameSize);

    sendResponse(socket, 200, "application/json", "{\"ok\":true}");
}

void WebServer::sendSwipe(QTcpSocket *socket, const QString &serial, const QByteArray &body)
{
    auto device = qsc::IDeviceManage::getInstance().getDevice(serial);
    if (!device) {
        sendResponse(socket, 404, "application/json", "{\"error\":\"device not found\"}");
        return;
    }

    QJsonDocument doc = QJsonDocument::fromJson(body);
    QJsonObject obj = doc.object();
    double sx = obj["sx"].toDouble();
    double sy = obj["sy"].toDouble();
    double ex = obj["ex"].toDouble();
    double ey = obj["ey"].toDouble();

    auto *capture = m_captures.value(serial, nullptr);
    QSize frameSize = capture ? capture->frameSize() : QSize(1080, 1920);
    if (frameSize.isEmpty()) frameSize = QSize(1080, 1920);

    int startX = static_cast<int>(sx * frameSize.width());
    int startY = static_cast<int>(sy * frameSize.height());
    int endX = static_cast<int>(ex * frameSize.width());
    int endY = static_cast<int>(ey * frameSize.height());

    QPoint startPos(startX, startY);
    QMouseEvent pressEvent(QEvent::MouseButtonPress, startPos, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    device->mouseEvent(&pressEvent, frameSize, frameSize);

    int steps = 5;
    for (int i = 1; i <= steps; i++) {
        double t = static_cast<double>(i) / steps;
        int mx = startX + static_cast<int>((endX - startX) * t);
        int my = startY + static_cast<int>((endY - startY) * t);
        QPoint movePos(mx, my);
        QMouseEvent moveEvent(QEvent::MouseMove, movePos, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        device->mouseEvent(&moveEvent, frameSize, frameSize);
    }

    QPoint endPos(endX, endY);
    QMouseEvent releaseEvent(QEvent::MouseButtonRelease, endPos, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    device->mouseEvent(&releaseEvent, frameSize, frameSize);

    sendResponse(socket, 200, "application/json", "{\"ok\":true}");
}

// ---- WebSocket ----

bool WebServer::handleWebSocketUpgrade(QTcpSocket *socket, const QString &request, const QString &username)
{
    Q_UNUSED(username);
    if (!request.contains("Upgrade: websocket", Qt::CaseInsensitive)) return false;

    QString wsKey;
    QStringList lines = request.split("\r\n");
    for (const auto &line : lines) {
        if (line.startsWith("Sec-WebSocket-Key:", Qt::CaseInsensitive)) {
            wsKey = line.mid(18).trimmed();
            break;
        }
    }
    if (wsKey.isEmpty()) return false;

    QByteArray acceptRaw = wsKey.toUtf8() + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    QByteArray acceptHash = QCryptographicHash::hash(acceptRaw, QCryptographicHash::Sha1).toBase64();

    QByteArray resp;
    resp.append("HTTP/1.1 101 Switching Protocols\r\n");
    resp.append("Upgrade: websocket\r\n");
    resp.append("Connection: Upgrade\r\n");
    resp.append("Sec-WebSocket-Accept: " + acceptHash + "\r\n");
    resp.append("\r\n");
    socket->write(resp);
    socket->flush();

    m_wsClients.append(socket);
    m_wsUsernames[socket] = username;

    QJsonObject msg;
    msg["type"] = QString("devices");
    msg["data"] = buildDeviceArray();
    sendWsFrame(socket, QJsonDocument(msg).toJson(QJsonDocument::Compact), false);

    return true;
}

void WebServer::sendWsFrame(QTcpSocket *socket, const QByteArray &data, bool binary)
{
    if (!socket || socket->state() != QAbstractSocket::ConnectedState) return;

    QByteArray frame;
    uint8_t opcode = binary ? 0x82 : 0x81;
    frame.append(static_cast<char>(opcode));

    quint64 len = static_cast<quint64>(data.size());
    if (len < 126) {
        frame.append(static_cast<char>(len));
    } else if (len <= 0xFFFF) {
        frame.append(static_cast<char>(126));
        frame.append(static_cast<char>((len >> 8) & 0xFF));
        frame.append(static_cast<char>(len & 0xFF));
    } else {
        frame.append(static_cast<char>(127));
        for (int i = 7; i >= 0; i--) {
            frame.append(static_cast<char>((len >> (8 * i)) & 0xFF));
        }
    }

    frame.append(data);
    socket->write(frame);
}

void WebServer::onWsData(QTcpSocket *socket)
{
    QByteArray raw = socket->readAll();
    if (raw.size() < 2) return;

    int pos = 0;
    while (pos < raw.size()) {
        if (pos + 2 > raw.size()) break;

        uint8_t byte0 = static_cast<uint8_t>(raw[pos]);
        uint8_t byte1 = static_cast<uint8_t>(raw[pos + 1]);
        int opcode = byte0 & 0x0F;
        bool masked = (byte1 & 0x80) != 0;
        quint64 payloadLen = byte1 & 0x7F;
        pos += 2;

        if (opcode == 0x08) {
            removeWsClient(socket);
            socket->disconnectFromHost();
            return;
        }

        if (payloadLen == 126) {
            if (pos + 2 > raw.size()) break;
            payloadLen = (static_cast<uint8_t>(raw[pos]) << 8) | static_cast<uint8_t>(raw[pos + 1]);
            pos += 2;
        } else if (payloadLen == 127) {
            if (pos + 8 > raw.size()) break;
            payloadLen = 0;
            for (int i = 0; i < 8; i++) {
                payloadLen = (payloadLen << 8) | static_cast<uint8_t>(raw[pos + i]);
            }
            pos += 8;
        }

        char mask[4] = {0, 0, 0, 0};
        if (masked) {
            if (pos + 4 > raw.size()) break;
            memcpy(mask, raw.constData() + pos, 4);
            pos += 4;
        }

        if (pos + static_cast<int>(payloadLen) > raw.size()) break;

        QByteArray payload = raw.mid(pos, static_cast<int>(payloadLen));
        if (masked) {
            for (int i = 0; i < payload.size(); i++) {
                payload[i] = payload[i] ^ mask[i % 4];
            }
        }
        pos += static_cast<int>(payloadLen);

        if (opcode == 0x09) {
            sendWsFrame(socket, payload, false);
            continue;
        }

        bool isBinary = (opcode == 0x02);
        processWsMessage(socket, payload, isBinary);
    }
}

void WebServer::processWsMessage(QTcpSocket *socket, const QByteArray &message, bool binary)
{
    Q_UNUSED(binary);
    QJsonDocument doc = QJsonDocument::fromJson(message);
    if (!doc.isObject()) return;

    QJsonObject obj = doc.object();
    QString type = obj["type"].toString();
    QString serial = obj["serial"].toString();

    if (type == "click") {
        auto device = qsc::IDeviceManage::getInstance().getDevice(serial);
        if (!device) return;

        auto *capture = m_captures.value(serial, nullptr);
        QSize frameSize = capture ? capture->frameSize() : QSize(1080, 1920);
        if (frameSize.isEmpty()) frameSize = QSize(1080, 1920);

        int clickX = static_cast<int>(obj["x"].toDouble() * frameSize.width());
        int clickY = static_cast<int>(obj["y"].toDouble() * frameSize.height());

        QPoint pos(clickX, clickY);
        QMouseEvent pressEvent(QEvent::MouseButtonPress, pos, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        device->mouseEvent(&pressEvent, frameSize, frameSize);
        QMouseEvent releaseEvent(QEvent::MouseButtonRelease, pos, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        device->mouseEvent(&releaseEvent, frameSize, frameSize);
    } else if (type == "swipe") {
        auto device = qsc::IDeviceManage::getInstance().getDevice(serial);
        if (!device) return;

        auto *capture = m_captures.value(serial, nullptr);
        QSize frameSize = capture ? capture->frameSize() : QSize(1080, 1920);
        if (frameSize.isEmpty()) frameSize = QSize(1080, 1920);

        int startX = static_cast<int>(obj["sx"].toDouble() * frameSize.width());
        int startY = static_cast<int>(obj["sy"].toDouble() * frameSize.height());
        int endX = static_cast<int>(obj["ex"].toDouble() * frameSize.width());
        int endY = static_cast<int>(obj["ey"].toDouble() * frameSize.height());

        QPoint startPos(startX, startY);
        QMouseEvent pressEvent(QEvent::MouseButtonPress, startPos, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        device->mouseEvent(&pressEvent, frameSize, frameSize);

        int steps = 5;
        for (int i = 1; i <= steps; i++) {
            double t = static_cast<double>(i) / steps;
            int mx = startX + static_cast<int>((endX - startX) * t);
            int my = startY + static_cast<int>((endY - startY) * t);
            QPoint movePos(mx, my);
            QMouseEvent moveEvent(QEvent::MouseMove, movePos, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            device->mouseEvent(&moveEvent, frameSize, frameSize);
        }

        QPoint endPos(endX, endY);
        QMouseEvent releaseEvent(QEvent::MouseButtonRelease, endPos, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        device->mouseEvent(&releaseEvent, frameSize, frameSize);
    } else if (type == "action") {
        QString action = obj["action"].toString();
        auto device = qsc::IDeviceManage::getInstance().getDevice(serial);
        if (!device) return;
        if (action == "home") device->postGoHome();
        else if (action == "back") device->postGoBack();
        else if (action == "menu") device->postGoMenu();
        else if (action == "lock") device->setDisplayPower(false);
        else if (action == "wake") device->setDisplayPower(true);
        else if (action == "volup") device->postVolumeUp();
        else if (action == "voldown") device->postVolumeDown();
        else if (action == "appswitch") device->postAppSwitch();
    } else if (type == "keyevent") {
        auto device = qsc::IDeviceManage::getInstance().getDevice(serial);
        if (!device) return;
        int keycode = obj["keycode"].toInt();
        if (keycode > 0) {
            QKeyEvent pressEvt(QEvent::KeyPress, keycode, Qt::NoModifier);
            auto *capture = m_captures.value(serial, nullptr);
            QSize fs = capture ? capture->frameSize() : QSize(1080, 1920);
            if (fs.isEmpty()) fs = QSize(1080, 1920);
            device->keyEvent(&pressEvt, fs, fs);
            QKeyEvent releaseEvt(QEvent::KeyRelease, keycode, Qt::NoModifier);
            device->keyEvent(&releaseEvt, fs, fs);
        }
    } else if (type == "shell") {
        QString cmd = obj["command"].toString();
        if (!cmd.isEmpty() && !serial.isEmpty()) {
            QProcess *proc = new QProcess();
            connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                    proc, &QProcess::deleteLater);
            proc->start("adb", QStringList() << "-s" << serial << "shell" << cmd);
        }
    }
}

void WebServer::removeWsClient(QTcpSocket *socket)
{
    m_wsClients.removeAll(socket);
    m_wsUsernames.remove(socket);
}

void WebServer::pushFramesToClients()
{
    if (m_wsClients.isEmpty()) return;

    QList<QTcpSocket*> dead;
    for (auto *ws : m_wsClients) {
        if (ws->state() != QAbstractSocket::ConnectedState) {
            dead.append(ws);
        }
    }
    for (auto *ws : dead) {
        removeWsClient(ws);
    }
    if (m_wsClients.isEmpty()) return;

    for (auto it = m_captures.begin(); it != m_captures.end(); ++it) {
        const QString &serial = it.key();
        FrameCapture *capture = it.value();
        if (!capture->hasFrame()) continue;

        quint64 ver = capture->version();
        if (m_lastPushedVersion.value(serial, 0) == ver) continue;
        m_lastPushedVersion[serial] = ver;

        QString fmt;
        QByteArray jpegData = capture->getImageData(fmt);
        if (jpegData.isEmpty()) continue;

        QByteArray serialUtf8 = serial.toUtf8();
        quint16 serialLen = static_cast<quint16>(serialUtf8.size());

        QByteArray binaryFrame;
        binaryFrame.reserve(2 + serialUtf8.size() + jpegData.size());
        binaryFrame.append(static_cast<char>((serialLen >> 8) & 0xFF));
        binaryFrame.append(static_cast<char>(serialLen & 0xFF));
        binaryFrame.append(serialUtf8);
        binaryFrame.append(jpegData);

        for (auto *ws : m_wsClients) {
            if (ws->state() == QAbstractSocket::ConnectedState) {
                sendWsFrame(ws, binaryFrame, true);
            }
        }
    }

    for (auto *ws : m_wsClients) {
        if (ws->state() == QAbstractSocket::ConnectedState) {
            ws->flush();
        }
    }
}

void WebServer::sendWsDeviceList()
{
    if (m_wsClients.isEmpty()) return;

    QJsonObject msg;
    msg["type"] = QString("devices");
    msg["data"] = buildDeviceArray();
    QByteArray json = QJsonDocument(msg).toJson(QJsonDocument::Compact);

    for (auto *ws : m_wsClients) {
        if (ws->state() == QAbstractSocket::ConnectedState) {
            sendWsFrame(ws, json, false);
        }
    }
}

void WebServer::sendCustomButtons(QTcpSocket *socket)
{
    QFile file(configDir() + "/web_buttons.json");
    QJsonArray buttons;
    if (file.open(QIODevice::ReadOnly)) {
        QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
        file.close();
        if (doc.isArray()) buttons = doc.array();
    }
    QJsonObject result;
    result["buttons"] = buttons;
    if (!m_iframeUrl.isEmpty()) result["iframe_url"] = m_iframeUrl;
    if (!m_webhookUrl.isEmpty()) result["webhook_url"] = m_webhookUrl;
    sendResponse(socket, 200, "application/json", QJsonDocument(result).toJson(QJsonDocument::Compact));
}

void WebServer::sendShellCmd(QTcpSocket *socket, const QString &serial, const QByteArray &body)
{
    QJsonDocument doc = QJsonDocument::fromJson(body);
    QString cmd = doc.object()["command"].toString();
    if (cmd.isEmpty() || serial.isEmpty()) {
        sendResponse(socket, 400, "application/json", "{\"error\":\"missing command or serial\"}");
        return;
    }

    QProcess proc;
    proc.start("adb", QStringList() << "-s" << serial << "shell" << cmd);
    proc.waitForFinished(5000);
    QString output = QString::fromUtf8(proc.readAllStandardOutput());
    QString error = QString::fromUtf8(proc.readAllStandardError());

    QJsonObject result;
    result["ok"] = (proc.exitCode() == 0);
    result["output"] = output.trimmed();
    if (!error.isEmpty()) result["error"] = error.trimmed();
    sendResponse(socket, 200, "application/json", QJsonDocument(result).toJson(QJsonDocument::Compact));
}

void WebServer::sendHtmlPage(QTcpSocket *socket, const QString &username)
{
    QByteArray userBytes = username.toUtf8();
    QByteArray html = R"HTML(<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<title>AniFelix - Remote Grid</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
* { margin: 0; padding: 0; box-sizing: border-box; }
body { background: #1a1a1a; color: #eee; font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif; }
.header { background: #222; padding: 12px 20px; display: flex; align-items: center; gap: 16px; border-bottom: 1px solid #333; }
.header h1 { font-size: 18px; font-weight: 600; color: #fff; }
.search { background: #333; border: 1px solid #555; border-radius: 4px; padding: 6px 12px; color: #eee; font-size: 14px; width: 250px; }
.search:focus { outline: none; border-color: #0078d7; }
.status { margin-left: auto; font-size: 13px; color: #888; }
.user-badge { font-size: 11px; color: #aaa; margin-left: 8px; }
.ws-status { font-size: 11px; padding: 2px 8px; border-radius: 10px; margin-left: 8px; }
.ws-on { background: #1b5e20; color: #a5d6a7; }
.ws-off { background: #b71c1c; color: #ef9a9a; }
.grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(220px, 1fr)); gap: 8px; padding: 12px; }
.tile { background: #222; border: 1px solid #444; border-radius: 4px; overflow: hidden; cursor: pointer; transition: border-color 0.2s; }
.tile:hover { border-color: #0078d7; }
.tile img { width: 100%; display: block; background: #111; min-height: 300px; object-fit: contain; user-select: none; -webkit-user-drag: none; }
.tile .info { padding: 6px 8px; font-size: 11px; color: #aaa; text-align: center; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; cursor: pointer; }
.tile .info:hover { color: #0078d7; }
.tile .info .name { color: #ddd; font-weight: 500; }
.tile .actions { display: flex; gap: 3px; padding: 4px 6px 6px; justify-content: center; flex-wrap: wrap; }
.tile .actions button { background: #333; border: 1px solid #555; color: #ccc; padding: 3px 8px; border-radius: 3px; cursor: pointer; font-size: 10px; }
.tile .actions button:hover { background: #444; color: #fff; }
.tile .actions button.lock { background: #4a1a1a; border-color: #833; }
.tile .actions button.lock:hover { background: #622; }
.tile .actions button.wake { background: #1a3a1a; border-color: #383; }
.tile .actions button.wake:hover { background: #264; }
.tile .actions button.custom { background: #1a2a3a; border-color: #358; }
.tile .actions button.custom:hover { background: #264; }
.tile .actions button.info-btn { background: #2a2a1a; border-color: #885; }
.tile .actions button.info-btn:hover { background: #442; }
.no-devices { text-align: center; padding: 80px 20px; color: #666; font-size: 16px; }
.webhook-bar { position: fixed; bottom: 0; left: 0; right: 0; height: 0; background: #222; border-top: 1px solid #444; z-index: 400; display: flex; flex-direction: column; transition: height 0.3s; }
.webhook-bar.open { height: 180px; }
.webhook-bar .bar-header { display: flex; align-items: center; padding: 4px 12px; background: #2a2a2a; cursor: pointer; min-height: 28px; }
.webhook-bar .bar-header span { flex: 1; font-size: 11px; color: #aaa; }
.webhook-bar .bar-header button { background: none; border: none; color: #888; cursor: pointer; font-size: 14px; padding: 0 6px; }
.webhook-bar iframe { flex: 1; border: none; background: #111; width: 100%; }
body.has-webhook { padding-bottom: 28px; }
body.has-webhook.webhook-expanded { padding-bottom: 180px; }
.toast { position: fixed; bottom: 20px; right: 20px; background: #333; color: #eee; padding: 10px 18px; border-radius: 6px; font-size: 13px; z-index: 1000; opacity: 0; transition: opacity 0.3s; pointer-events: none; max-width: 400px; border: 1px solid #555; }
.toast.show { opacity: 1; }
.toast.ok { border-color: #4a4; }
.toast.err { border-color: #a44; }
.modal-overlay { display: none; position: fixed; top: 0; left: 0; right: 0; bottom: 0; background: rgba(0,0,0,0.7); z-index: 500; }
.modal-overlay.show { display: flex; align-items: center; justify-content: center; }
.modal { background: #222; border: 1px solid #444; border-radius: 8px; width: 90%; max-width: 800px; height: 80vh; display: flex; flex-direction: column; }
.modal-header { display: flex; align-items: center; padding: 12px 16px; border-bottom: 1px solid #333; }
.modal-header h3 { flex: 1; font-size: 14px; color: #ddd; }
.modal-header button { background: #444; border: none; color: #ccc; padding: 4px 12px; border-radius: 4px; cursor: pointer; font-size: 12px; }
.modal-header button:hover { background: #555; }
.modal iframe { flex: 1; border: none; background: #111; }
</style>
</head>
<body>
<div class="header">
  <h1>AniFelix Remote</h1>
  <input class="search" type="text" id="search" placeholder="Search devices..." oninput="filterDevices()">
  <span class="status" id="status">Connecting...</span>
  <span class="user-badge" id="userBadge"></span>
  <span class="ws-status ws-off" id="wsStatus">WS</span>
</div>
<div class="grid" id="grid"></div>
<div class="no-devices" id="noDevices" style="display:none">No devices connected</div>
<div class="webhook-bar" id="webhookBar">
  <div class="bar-header" onclick="toggleWebhook()">
    <span>Webhook Messages</span>
    <button id="webhookToggle">^</button>
  </div>
  <iframe id="webhookIframe" src="about:blank"></iframe>
</div>
<div class="toast" id="toast"></div>
<div class="modal-overlay" id="modalOverlay" onclick="closeModal(event)">
  <div class="modal" onclick="event.stopPropagation()">
    <div class="modal-header">
      <h3 id="modalTitle">Device Info</h3>
      <button onclick="closeModal()">Close</button>
    </div>
    <iframe id="modalIframe" src="about:blank"></iframe>
  </div>
</div>

<script>
var devices = [];
var imageBlobs = {};
var customButtons = [];
var iframeUrl = '';
var webhookUrl = '';
var webhookOpen = false;
var currentUser = ')HTML" + userBytes + R"HTML(';
var ws = null;
var wsConnected = false;
var toastTimer = null;

document.getElementById('userBadge').textContent = currentUser !== 'anonymous' ? currentUser : '';

function showToast(msg, ok) {
  var t = document.getElementById('toast');
  t.textContent = msg;
  t.className = 'toast show ' + (ok ? 'ok' : 'err');
  clearTimeout(toastTimer);
  toastTimer = setTimeout(function() { t.className = 'toast'; }, 2500);
}

function openModal(serial) {
  if (!iframeUrl) return;
  var url = iframeUrl.replace(/{serial}/g, serial).replace(/{user}/g, currentUser);
  document.getElementById('modalTitle').textContent = serial;
  document.getElementById('modalIframe').src = url;
  document.getElementById('modalOverlay').className = 'modal-overlay show';
}

function closeModal(e) {
  if (e && e.target !== document.getElementById('modalOverlay')) return;
  document.getElementById('modalOverlay').className = 'modal-overlay';
  document.getElementById('modalIframe').src = 'about:blank';
}

function connectWs() {
  var proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
  ws = new WebSocket(proto + '//' + location.host + '/ws');
  ws.binaryType = 'arraybuffer';

  ws.onopen = function() {
    wsConnected = true;
    document.getElementById('wsStatus').className = 'ws-status ws-on';
    document.getElementById('wsStatus').textContent = 'Live';
  };

  ws.onclose = function() {
    wsConnected = false;
    document.getElementById('wsStatus').className = 'ws-status ws-off';
    document.getElementById('wsStatus').textContent = 'Offline';
    setTimeout(connectWs, 2000);
  };

  ws.onerror = function() { ws.close(); };

  ws.onmessage = function(evt) {
    if (typeof evt.data === 'string') {
      var msg = JSON.parse(evt.data);
      if (msg.type === 'devices') {
        devices = msg.data;
        var withFrames = devices.filter(function(d) { return d.hasFrame; }).length;
        document.getElementById('status').textContent = devices.length + ' device(s), ' + withFrames + ' streaming';
        renderGrid();
      }
    } else {
      var buf = new Uint8Array(evt.data);
      if (buf.length < 4) return;
      var serialLen = (buf[0] << 8) | buf[1];
      if (buf.length < 2 + serialLen) return;
      var serial = '';
      for (var i = 0; i < serialLen; i++) serial += String.fromCharCode(buf[2 + i]);
      var jpegData = evt.data.slice(2 + serialLen);
      var blob = new Blob([jpegData], {type: 'image/jpeg'});
      if (imageBlobs[serial]) URL.revokeObjectURL(imageBlobs[serial]);
      imageBlobs[serial] = URL.createObjectURL(blob);
      var imgs = document.querySelectorAll('.tile[data-serial="' + serial + '"] img');
      for (var j = 0; j < imgs.length; j++) imgs[j].src = imageBlobs[serial];
    }
  };
}

function filterDevices() { renderGrid(); }

function renderGrid() {
  var grid = document.getElementById('grid');
  var noDevices = document.getElementById('noDevices');
  var filter = document.getElementById('search').value.toLowerCase();

  var filtered = devices.filter(function(d) {
    return d.serial.toLowerCase().indexOf(filter) >= 0 ||
      (d.name && d.name.toLowerCase().indexOf(filter) >= 0);
  });

  if (filtered.length === 0) { grid.innerHTML = ''; noDevices.style.display = 'block'; return; }
  noDevices.style.display = 'none';

  var html = '';
  for (var i = 0; i < filtered.length; i++) {
    var d = filtered[i];
    var imgSrc = imageBlobs[d.serial] || '/api/snapshot/' + d.serial;
    html += '<div class="tile" data-serial="' + d.serial + '">';
    html += '<img src="' + imgSrc + '"';
    html += ' onerror="this.src=\'data:image/svg+xml,<svg xmlns=%22http://www.w3.org/2000/svg%22 viewBox=%220 0 200 350%22><rect fill=%22%23111%22 width=%22200%22 height=%22350%22/><text x=%2250%25%22 y=%2250%25%22 fill=%22%23444%22 text-anchor=%22middle%22 font-size=%2214%22>No Signal</text></svg>\'"';
    html += ' onmousedown="startTouch(event,\'' + d.serial + '\')"';
    html += ' onmouseup="endTouch(event,\'' + d.serial + '\')"';
    html += ' onmouseleave="endTouch(event,\'' + d.serial + '\')"';
    html += ' draggable="false" />';
    html += '<div class="info" onclick="openModal(\'' + d.serial + '\')"><span class="name">' + (d.name || 'Phone') + '</span><br>' + d.serial + '</div>';
    html += '<div class="actions">';
    html += '<button onclick="doAction(\'' + d.serial + '\',\'home\')">Home</button>';
    html += '<button onclick="doAction(\'' + d.serial + '\',\'back\')">Back</button>';
    html += '<button onclick="doAction(\'' + d.serial + '\',\'menu\')">Menu</button>';
    html += '<button class="lock" onclick="doAction(\'' + d.serial + '\',\'lock\')">Lock</button>';
    html += '<button class="wake" onclick="doAction(\'' + d.serial + '\',\'wake\')">Wake</button>';
    if (iframeUrl) html += '<button class="info-btn" onclick="openModal(\'' + d.serial + '\')">Info</button>';
    for (var b = 0; b < customButtons.length; b++) {
      html += '<button class="custom" onclick="runCustomBtn(\'' + d.serial + '\',' + b + ')">' + customButtons[b].label + '</button>';
    }
    html += '</div></div>';
  }
  grid.innerHTML = html;
}

var touchState = {};

function startTouch(event, serial) {
  event.preventDefault();
  var rect = event.target.getBoundingClientRect();
  touchState = { serial: serial, sx: (event.clientX - rect.left) / rect.width, sy: (event.clientY - rect.top) / rect.height, active: true };
}

function endTouch(event, serial) {
  if (!touchState.active || touchState.serial !== serial) return;
  touchState.active = false;
  var rect = event.target.getBoundingClientRect();
  var ex = (event.clientX - rect.left) / rect.width;
  var ey = (event.clientY - rect.top) / rect.height;
  var dist = Math.sqrt(Math.pow(ex - touchState.sx, 2) + Math.pow(ey - touchState.sy, 2));
  if (wsConnected && ws && ws.readyState === 1) {
    if (dist < 0.03) ws.send(JSON.stringify({type:'click', serial:serial, x:touchState.sx, y:touchState.sy}));
    else ws.send(JSON.stringify({type:'swipe', serial:serial, sx:touchState.sx, sy:touchState.sy, ex:ex, ey:ey}));
  } else {
    if (dist < 0.03) fetch('/api/click/' + serial, {method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify({x:touchState.sx, y:touchState.sy})});
    else fetch('/api/swipe/' + serial, {method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify({sx:touchState.sx, sy:touchState.sy, ex:ex, ey:ey})});
  }
}

function doAction(serial, action) {
  if (wsConnected && ws && ws.readyState === 1) {
    ws.send(JSON.stringify({type:'action', serial:serial, action:action}));
  } else {
    fetch('/api/action/' + serial + '/' + action, {method:'POST'});
  }
  showToast(action + ' sent to ' + serial.substr(0,12), true);
}

function runCustomBtn(serial, index) {
  var btn = customButtons[index];
  if (!btn) return;
  if (btn.action) {
    doAction(serial, btn.action);
  } else if (btn.keycode) {
    if (wsConnected && ws && ws.readyState === 1) ws.send(JSON.stringify({type:'keyevent', serial:serial, keycode:btn.keycode}));
    else fetch('/api/keyevent/' + serial, {method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify({keycode:btn.keycode})});
    showToast(btn.label + ' sent', true);
  } else if (btn.shell) {
    var cmd = btn.shell.replace(/{serial}/g, serial).replace(/{user}/g, currentUser);
    if (wsConnected && ws && ws.readyState === 1) ws.send(JSON.stringify({type:'shell', serial:serial, command:cmd}));
    else fetch('/api/shell/' + serial, {method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify({command:cmd})}).then(function(r){return r.json();}).then(function(d){showToast(d.output||d.error||'Done', d.ok);});
    showToast(btn.label + ' running...', true);
  } else if (btn.url) {
    var url = btn.url.replace(/{serial}/g, serial).replace(/{user}/g, currentUser);
    fetch(url, {method:btn.method||'GET'}).then(function(){showToast(btn.label+' OK',true);}).catch(function(){showToast(btn.label+' failed',false);});
  }
}

function toggleWebhook() {
  webhookOpen = !webhookOpen;
  var bar = document.getElementById('webhookBar');
  var btn = document.getElementById('webhookToggle');
  if (webhookOpen) {
    bar.className = 'webhook-bar open';
    btn.textContent = 'v';
    document.body.className = 'has-webhook webhook-expanded';
  } else {
    bar.className = 'webhook-bar';
    btn.textContent = '^';
    document.body.className = 'has-webhook';
  }
}

function loadConfig() {
  fetch('/api/buttons').then(function(r){return r.json();}).then(function(data) {
    if (data.buttons) customButtons = data.buttons;
    else if (Array.isArray(data)) customButtons = data;
    if (data.iframe_url) iframeUrl = data.iframe_url;
    if (data.webhook_url) {
      webhookUrl = data.webhook_url.replace(/{user}/g, currentUser);
      document.getElementById('webhookIframe').src = webhookUrl;
      document.getElementById('webhookBar').style.display = 'flex';
      document.body.className = 'has-webhook';
    }
    renderGrid();
  }).catch(function(){});
}

loadConfig();
connectWs();
</script>
</body>
</html>)HTML";

    sendResponse(socket, 200, "text/html; charset=utf-8", html);
}
