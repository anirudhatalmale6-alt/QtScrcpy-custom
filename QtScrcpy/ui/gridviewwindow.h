#ifndef GRIDVIEWWINDOW_H
#define GRIDVIEWWINDOW_H

#include <QWidget>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QPointer>

#include "../QtScrcpyCore/include/QtScrcpyCore.h"

class QYUVOpenGLWidget;

class GridTile : public QWidget, public qsc::DeviceObserver
{
    Q_OBJECT
public:
    explicit GridTile(const QString &serial, QWidget *parent = nullptr);
    ~GridTile();

    const QString &serial() const { return m_serial; }
    QString displayName() const { return m_displayName; }
    void setDisplayName(const QString &name) { m_displayName = name; }

signals:
    void tileDoubleClicked(const QString &serial);
    void frameReady(int width, int height, uint8_t* dataY, uint8_t* dataU, uint8_t* dataV,
                    int linesizeY, int linesizeU, int linesizeV);

private slots:
    void onFrameReady(int width, int height, uint8_t* dataY, uint8_t* dataU, uint8_t* dataV,
                      int linesizeY, int linesizeU, int linesizeV);

protected:
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;

private:
    void onFrame(int width, int height, uint8_t* dataY, uint8_t* dataU, uint8_t* dataV,
                 int linesizeY, int linesizeU, int linesizeV) override;

    QString m_serial;
    QString m_displayName;
    QPointer<QYUVOpenGLWidget> m_videoWidget;
    QPointer<QLabel> m_nameLabel;
    bool m_firstFrame = true;
};


class GridViewWindow : public QWidget
{
    Q_OBJECT
public:
    explicit GridViewWindow(QWidget *parent = nullptr);
    ~GridViewWindow();

    void addDevice(const QString &serial, const QString &displayName);
    void removeDevice(const QString &serial);
    void clear();

signals:
    void windowClosed();

private slots:
    void onTileDoubleClicked(const QString &serial);
    void onSearchTextChanged(const QString &text);

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    void rearrangeGrid();
    int optimalColumns(int count = 0) const;

    QLineEdit *m_searchEdit;
    QGridLayout *m_gridLayout;
    QMap<QString, GridTile*> m_tiles;
    QString m_searchFilter;

#endif // GRIDVIEWWINDOW_H
