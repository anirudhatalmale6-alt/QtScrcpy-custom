#include "webserver.h"
#include "config.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QBuffer>
#include <QMouseEvent>
#include <QHostAddress>

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
    if (m_throttleTimer.elapsed() < 500) {
        QMutexLocker lock(&m_mutex);
        m_frameSize = QSize(width, height);
        return;
    }
    m_throttleTimer.restart();

    int sw = width / 4;
    int sh = height / 4;
    if (sw < 1 || sh < 1) return;

    QImage img(sw, sh, QImage::Format_RGB888);
    for (int y = 0; y < sh; y++) {
        uint8_t *line = img.scanLine(y);
        int srcY = y * 4;
        for (int x = 0; x < sw; x++) {
            int srcX = x * 4;
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
}

bool FrameCapture::hasFrame() const
{
    QMutexLocker lock(&m_mutex);
    return !m_image.isNull();
}

QByteArray FrameCapture::getJpeg()
{
    QMutexLocker lock(&m_mutex);
    if (m_image.isNull()) return QByteArray();
    QByteArray jpeg;
    QBuffer buf(&jpeg);
    buf.open(QIODevice::WriteOnly);
    m_image.save(&buf, "JPEG", 50);
    return jpeg;
}


// ---- WebServer ----

WebServer::WebServer(QObject *parent)
    : QTcpServer(parent)
{
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
        return true;
    }

    for (quint16 p = 8080; p < 8100; p++) {
        if (listen(QHostAddress::Any, p)) {
            m_port = serverPort();
            return true;
        }
    }
    return false;
}

void WebServer::stopServer()
{
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
}

void WebServer::incomingConnection(qintptr socketDescriptor)
{
    auto *socket = new QTcpSocket(this);
    socket->setSocketDescriptor(socketDescriptor);
    connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
        handleRequest(socket);
    });
    connect(socket, &QTcpSocket::disconnected, socket, &QTcpSocket::deleteLater);
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

    if (method == "GET" && path == "/") {
        sendHtmlPage(socket);
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
                sendResponse(socket, 200, "application/json", "{\"ok\":true}");
            } else {
                sendResponse(socket, 404, "application/json", "{\"error\":\"device not found\"}");
            }
        }
    } else {
        sendResponse(socket, 404, "text/plain", "Not Found");
    }
}

void WebServer::sendResponse(QTcpSocket *socket, int statusCode, const QString &contentType,
                              const QByteArray &body)
{
    QString statusText = (statusCode == 200) ? "OK" : "Not Found";
    QByteArray response;
    response.append(QString("HTTP/1.1 %1 %2\r\n").arg(statusCode).arg(statusText).toUtf8());
    response.append(QString("Content-Type: %1\r\n").arg(contentType).toUtf8());
    response.append(QString("Content-Length: %1\r\n").arg(body.size()).toUtf8());
    response.append("Access-Control-Allow-Origin: *\r\n");
    response.append("Connection: close\r\n");
    response.append("\r\n");
    response.append(body);
    socket->write(response);
    socket->flush();
    socket->disconnectFromHost();
}

void WebServer::sendDeviceList(QTcpSocket *socket)
{
    QJsonArray devices;
    for (auto it = m_captures.begin(); it != m_captures.end(); ++it) {
        QJsonObject dev;
        dev["serial"] = it.key();
        dev["name"] = Config::getInstance().getNickName(it.key());
        QSize fs = it.value()->frameSize();
        dev["width"] = fs.width();
        dev["height"] = fs.height();
        dev["hasFrame"] = it.value()->hasFrame();
        devices.append(dev);
    }
    QJsonDocument doc(devices);
    sendResponse(socket, 200, "application/json", doc.toJson(QJsonDocument::Compact));
}

void WebServer::sendSnapshot(QTcpSocket *socket, const QString &serial)
{
    if (!m_captures.contains(serial)) {
        sendResponse(socket, 404, "text/plain", "Device not found");
        return;
    }
    QByteArray jpeg = m_captures[serial]->getJpeg();
    if (jpeg.isEmpty()) {
        sendResponse(socket, 503, "text/plain", "No frame available");
        return;
    }
    sendResponse(socket, 200, "image/jpeg", jpeg);
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

void WebServer::sendHtmlPage(QTcpSocket *socket)
{
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
.grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(220px, 1fr)); gap: 8px; padding: 12px; }
.tile { background: #222; border: 1px solid #444; border-radius: 4px; overflow: hidden; cursor: pointer; transition: border-color 0.2s; }
.tile:hover { border-color: #0078d7; }
.tile img { width: 100%; display: block; background: #111; min-height: 300px; object-fit: contain; }
.tile .info { padding: 6px 8px; font-size: 11px; color: #aaa; text-align: center; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.tile .info .name { color: #ddd; font-weight: 500; }
.tile .actions { display: flex; gap: 4px; padding: 4px 8px 8px; justify-content: center; }
.tile .actions button { background: #333; border: 1px solid #555; color: #ccc; padding: 4px 10px; border-radius: 3px; cursor: pointer; font-size: 11px; }
.tile .actions button:hover { background: #444; color: #fff; }
.no-devices { text-align: center; padding: 80px 20px; color: #666; font-size: 16px; }
</style>
</head>
<body>
<div class="header">
  <h1>AniFelix Remote</h1>
  <input class="search" type="text" id="search" placeholder="Search devices..." oninput="filterDevices()">
  <span class="status" id="status">Loading...</span>
</div>
<div class="grid" id="grid"></div>
<div class="no-devices" id="noDevices" style="display:none">No devices connected</div>

<script>
let devices = [];
let refreshInterval = 1000;

async function fetchDevices() {
  try {
    const res = await fetch('/api/devices');
    devices = await res.json();
    document.getElementById('status').textContent = devices.length + ' device(s)';
    renderGrid();
  } catch(e) {
    document.getElementById('status').textContent = 'Connection error';
  }
}

function filterDevices() {
  renderGrid();
}

function renderGrid() {
  const grid = document.getElementById('grid');
  const noDevices = document.getElementById('noDevices');
  const filter = document.getElementById('search').value.toLowerCase();

  const filtered = devices.filter(d =>
    d.serial.toLowerCase().includes(filter) ||
    (d.name && d.name.toLowerCase().includes(filter))
  );

  if (filtered.length === 0) {
    grid.innerHTML = '';
    noDevices.style.display = 'block';
    return;
  }
  noDevices.style.display = 'none';

  grid.innerHTML = filtered.map(d => `
    <div class="tile" data-serial="${d.serial}">
      <img src="/api/snapshot/${d.serial}?t=${Date.now()}"
           onerror="this.src='data:image/svg+xml,<svg xmlns=%22http://www.w3.org/2000/svg%22 viewBox=%220 0 200 350%22><rect fill=%22%23111%22 width=%22200%22 height=%22350%22/><text x=%2250%25%22 y=%2250%25%22 fill=%22%23444%22 text-anchor=%22middle%22 font-size=%2214%22>No Signal</text></svg>'"
           onclick="handleClick(event, '${d.serial}')" />
      <div class="info">
        <span class="name">${d.name || 'Phone'}</span><br>${d.serial}
      </div>
      <div class="actions">
        <button onclick="sendAction('${d.serial}','home')">Home</button>
        <button onclick="sendAction('${d.serial}','back')">Back</button>
        <button onclick="sendAction('${d.serial}','menu')">Menu</button>
      </div>
    </div>
  `).join('');
}

function handleClick(event, serial) {
  const img = event.target;
  const rect = img.getBoundingClientRect();
  const x = (event.clientX - rect.left) / rect.width;
  const y = (event.clientY - rect.top) / rect.height;
  fetch('/api/click/' + serial, {
    method: 'POST',
    headers: {'Content-Type': 'application/json'},
    body: JSON.stringify({x, y})
  });
}

async function sendAction(serial, action) {
  await fetch('/api/action/' + serial + '/' + action, {method: 'POST'});
}

function refreshImages() {
  document.querySelectorAll('.tile img').forEach(img => {
    const serial = img.closest('.tile').dataset.serial;
    img.src = '/api/snapshot/' + serial + '?t=' + Date.now();
  });
}

fetchDevices();
setInterval(fetchDevices, 5000);
setInterval(refreshImages, refreshInterval);
</script>
</body>
</html>)HTML";

    sendResponse(socket, 200, "text/html; charset=utf-8", html);
}
