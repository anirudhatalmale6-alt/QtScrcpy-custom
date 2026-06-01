#include "gridviewwindow.h"
#include "qyuvopenglwidget.h"
#include "config.h"

#include <QCloseEvent>
#include <QMouseEvent>
#include <QVBoxLayout>
#include <QScrollArea>
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

    m_videoWidget = new QYUVOpenGLWidget();
    m_videoWidget->setMinimumSize(120, 200);
    m_videoWidget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    layout->addWidget(m_videoWidget);

    QString name = Config::getInstance().getNickName(serial);
    if (name.isEmpty()) name = "Phone";
    m_nameLabel = new QLabel(name + "\n" + serial);
    m_nameLabel->setAlignment(Qt::AlignCenter);
    m_nameLabel->setStyleSheet("QLabel { color: #cccccc; font-size: 10px; }");
    m_nameLabel->setMaximumHeight(30);
    layout->addWidget(m_nameLabel);

    setStyleSheet("GridTile { border: 1px solid #444; background: #222; }");
    setMinimumSize(140, 240);

    qRegisterMetaType<uint8_t*>("uint8_t*");
    connect(this, &GridTile::frameReady, this, &GridTile::onFrameReady, Qt::QueuedConnection);
}

GridTile::~GridTile()
{
}

void GridTile::onFrame(int width, int height, uint8_t *dataY, uint8_t *dataU, uint8_t *dataV,
                       int linesizeY, int linesizeU, int linesizeV)
{
    emit frameReady(width, height, dataY, dataU, dataV, linesizeY, linesizeU, linesizeV);
}

void GridTile::onFrameReady(int width, int height, uint8_t *dataY, uint8_t *dataU, uint8_t *dataV,
                            int linesizeY, int linesizeU, int linesizeV)
{
    if (!m_videoWidget) return;

    if (m_firstFrame) {
        m_videoWidget->show();
        m_firstFrame = false;
    }

    m_videoWidget->setFrameSize(QSize(width, height));
    m_videoWidget->updateTextures(dataY, dataU, dataV, linesizeY, linesizeU, linesizeV);
}

void GridTile::mouseDoubleClickEvent(QMouseEvent *event)
{
    Q_UNUSED(event);
    emit tileDoubleClicked(m_serial);
}

void GridTile::mousePressEvent(QMouseEvent *event)
{
    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    if (device) {
        QSize frameSize = m_videoWidget->frameSize();
        QSize showSize = m_videoWidget->size();
        device->mouseEvent(event, frameSize, showSize);
    }
}

void GridTile::mouseReleaseEvent(QMouseEvent *event)
{
    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    if (device) {
        QSize frameSize = m_videoWidget->frameSize();
        QSize showSize = m_videoWidget->size();
        device->mouseEvent(event, frameSize, showSize);
    }
}

void GridTile::mouseMoveEvent(QMouseEvent *event)
{
    auto device = qsc::IDeviceManage::getInstance().getDevice(m_serial);
    if (device) {
        QSize frameSize = m_videoWidget->frameSize();
        QSize showSize = m_videoWidget->size();
        device->mouseEvent(event, frameSize, showSize);
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
    return 5;
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
