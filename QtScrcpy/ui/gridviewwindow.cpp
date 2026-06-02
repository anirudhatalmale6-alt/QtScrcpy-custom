#include "gridviewwindow.h"
#include "config.h"

#include <QCloseEvent>
#include <QMouseEvent>
#include <QVBoxLayout>
#include <QScrollArea>
#include <QImage>
#include <QPixmap>
#include <cmath>

#ifdef Q_OS_WIN32
#include "../util/winutils.h"
#endif

// ---- GridTile ----

GridTile::GridTile(const QString &serial, QWidget *parent)
    : QWidget(parent), m_serial(serial)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(2, 2, 2, 2);
    layout->setSpacing(2);

    m_imageLabel = new QLabel();
    m_imageLabel->setMinimumSize(80, 140);
    m_imageLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_imageLabel->setAlignment(Qt::AlignCenter);
    m_imageLabel->setStyleSheet("background: #111;");
    m_imageLabel->setScaledContents(true);
    layout->addWidget(m_imageLabel);

    QString name = Config::getInstance().getNickName(serial);
    if (name.isEmpty()) name = "Phone";
    m_nameLabel = new QLabel(name + "\n" + serial);
    m_nameLabel->setAlignment(Qt::AlignCenter);
    m_nameLabel->setStyleSheet("QLabel { color: #cccccc; font-size: 10px; }");
    m_nameLabel->setMaximumHeight(30);
    layout->addWidget(m_nameLabel);

    setStyleSheet("GridTile { border: 1px solid #444; background: #222; }");
    setMinimumSize(100, 160);

    qRegisterMetaType<uint8_t*>("uint8_t*");
    connect(this, &GridTile::frameReady, this, &GridTile::onFrameReady, Qt::QueuedConnection);

    m_throttle.start();
}

GridTile::~GridTile()
{
}

void GridTile::onFrame(int width, int height, uint8_t *dataY, uint8_t *dataU, uint8_t *dataV,
                       int linesizeY, int linesizeU, int linesizeV)
{
    if (m_throttle.elapsed() < 500) return;
    m_throttle.restart();
    emit frameReady(width, height, dataY, dataU, dataV, linesizeY, linesizeU, linesizeV);
}

void GridTile::onFrameReady(int width, int height, uint8_t *dataY, uint8_t *dataU, uint8_t *dataV,
                            int linesizeY, int linesizeU, int linesizeV)
{
    if (!m_imageLabel) return;

    m_frameSize = QSize(width, height);

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

    m_imageLabel->setPixmap(QPixmap::fromImage(img));
}

void GridTile::mouseDoubleClickEvent(QMouseEvent *event)
{
    Q_UNUSED(event);
    emit tileDoubleClicked(m_serial);
}

void GridTile::mousePressEvent(QMouseEvent *event)
{
    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    if (device && !m_frameSize.isEmpty()) {
        QSize showSize = m_imageLabel->size();
        device->mouseEvent(event, m_frameSize, showSize);
    }
}

void GridTile::mouseReleaseEvent(QMouseEvent *event)
{
    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    if (device && !m_frameSize.isEmpty()) {
        QSize showSize = m_imageLabel->size();
        device->mouseEvent(event, m_frameSize, showSize);
    }
}

void GridTile::mouseMoveEvent(QMouseEvent *event)
{
    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    if (device && !m_frameSize.isEmpty()) {
        QSize showSize = m_imageLabel->size();
        device->mouseEvent(event, m_frameSize, showSize);
    }
}


// ---- GridViewWindow ----

GridViewWindow::GridViewWindow(QWidget *parent)
    : QWidget(parent)
{
    setWindowTitle("AniFelix - Grid View");
    resize(1200, 800);

#ifdef Q_OS_WIN32
    WinUtils::setDarkBorderToWindow((HWND)this->winId(), true);
#endif

    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(4, 4, 4, 4);

    m_searchEdit = new QLineEdit();
    m_searchEdit->setPlaceholderText("Search devices...");
    m_searchEdit->setStyleSheet(
        "QLineEdit { background: #333; color: #eee; border: 1px solid #555; "
        "border-radius: 4px; padding: 6px 10px; font-size: 13px; }"
        "QLineEdit:focus { border: 1px solid #0078d7; }");
    m_searchEdit->setMaximumHeight(32);
    mainLayout->addWidget(m_searchEdit);
    connect(m_searchEdit, &QLineEdit::textChanged, this, &GridViewWindow::onSearchTextChanged);

    auto *scrollArea = new QScrollArea();
    scrollArea->setWidgetResizable(true);
    scrollArea->setStyleSheet("QScrollArea { border: none; background: #1a1a1a; }");

    auto *gridWidget = new QWidget();
    m_gridLayout = new QGridLayout(gridWidget);
    m_gridLayout->setSpacing(4);
    m_gridLayout->setContentsMargins(4, 4, 4, 4);

    scrollArea->setWidget(gridWidget);
    mainLayout->addWidget(scrollArea);
}

GridViewWindow::~GridViewWindow()
{
    clear();
}

void GridViewWindow::addDevice(const QString &serial, const QString &displayName)
{
    if (m_tiles.contains(serial)) return;

    auto *tile = new GridTile(serial);
    tile->setDisplayName(displayName);
    connect(tile, &GridTile::tileDoubleClicked, this, &GridViewWindow::onTileDoubleClicked);

    auto device = qsc::IDeviceManage::getInstance().getDevice(serial);
    if (device) {
        device->registerDeviceObserver(tile);
    }

    m_tiles[serial] = tile;
    rearrangeGrid();
}

void GridViewWindow::removeDevice(const QString &serial)
{
    if (!m_tiles.contains(serial)) return;

    auto *tile = m_tiles.take(serial);
    auto device = qsc::IDeviceManage::getInstance().getDevice(serial);
    if (device) {
        device->deRegisterDeviceObserver(tile);
    }

    m_gridLayout->removeWidget(tile);
    tile->deleteLater();
    rearrangeGrid();
}

void GridViewWindow::clear()
{
    for (auto it = m_tiles.begin(); it != m_tiles.end(); ++it) {
        auto device = qsc::IDeviceManage::getInstance().getDevice(it.key());
        if (device) {
            device->deRegisterDeviceObserver(it.value());
        }
        m_gridLayout->removeWidget(it.value());
        it.value()->deleteLater();
    }
    m_tiles.clear();
}

void GridViewWindow::onTileDoubleClicked(const QString &serial)
{
    auto device = qsc::IDeviceManage::getInstance().getDevice(serial);
    if (!device) return;

    auto *data = device->getUserData();
    if (data) {
        auto *vf = static_cast<QWidget*>(data);
        vf->show();
        vf->raise();
        vf->activateWindow();
    }
}

void GridViewWindow::rearrangeGrid()
{
    while (m_gridLayout->count() > 0) {
        auto *item = m_gridLayout->takeAt(0);
        delete item;
    }

    QString filter = m_searchFilter.trimmed().toLower();
    int visibleCount = 0;

    for (auto *tile : m_tiles) {
        bool match = filter.isEmpty() ||
                     tile->serial().toLower().contains(filter) ||
                     tile->displayName().toLower().contains(filter);
        tile->setVisible(match);
        if (match) visibleCount++;
    }

    int cols = optimalColumns(visibleCount);
    int row = 0, col = 0;

    for (auto *tile : m_tiles) {
        if (!tile->isVisible()) continue;
        m_gridLayout->addWidget(tile, row, col);
        col++;
        if (col >= cols) {
            col = 0;
            row++;
        }
    }
}

int GridViewWindow::optimalColumns(int count) const
{
    if (count <= 0) count = m_tiles.size();
    if (count <= 1) return 1;
    if (count <= 4) return 2;
    if (count <= 9) return 3;
    if (count <= 16) return 4;
    if (count <= 36) return 6;
    if (count <= 64) return 8;
    return 10;
}

void GridViewWindow::onSearchTextChanged(const QString &text)
{
    m_searchFilter = text;
    rearrangeGrid();
}

void GridViewWindow::closeEvent(QCloseEvent *event)
{
    emit windowClosed();
    QWidget::closeEvent(event);
}
