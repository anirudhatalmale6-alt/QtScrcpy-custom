#ifndef LICENSEDIALOG_H
#define LICENSEDIALOG_H

#include <QDialog>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <QSettings>

class LicenseDialog : public QDialog
{
    Q_OBJECT

public:
    explicit LicenseDialog(QWidget *parent = nullptr);
    static bool isActivated();
    static bool validateKey(const QString &key, const QString &machineId);
    static QString getMachineId();

private slots:
    void onActivateClicked();
    void onKeyTextChanged(const QString &text);

private:
    QLineEdit *m_keyEdit;
    QPushButton *m_activateBtn;
    QLabel *m_statusLabel;

    static QString settingsPath();
    static quint16 computeChecksum(const QString &prefix, const QString &machineId);
};

#endif // LICENSEDIALOG_H
