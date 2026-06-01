#include "licensedialog.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFont>
#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QCoreApplication>
#include <QSysInfo>
#include <QNetworkInterface>
#include <QClipboard>

LicenseDialog::LicenseDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle("AniFelix - Activation");
    setFixedSize(480, 280);
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(10);
    mainLayout->setContentsMargins(24, 24, 24, 24);

    QString machineId = getMachineId();

    auto *machineIdLayout = new QHBoxLayout();
    auto *machineLabel = new QLabel("Machine ID:");
    machineLabel->setStyleSheet("color: #888;");
    machineIdLayout->addWidget(machineLabel);

    auto *machineIdEdit = new QLineEdit(machineId);
    machineIdEdit->setReadOnly(true);
    machineIdEdit->setFont(QFont("Consolas", 11));
    machineIdEdit->setAlignment(Qt::AlignCenter);
    machineIdEdit->setStyleSheet("background: #f0f0f0; border: 1px solid #ccc; padding: 4px;");
    machineIdLayout->addWidget(machineIdEdit);

    auto *copyBtn = new QPushButton("Copy");
    copyBtn->setFixedWidth(60);
    connect(copyBtn, &QPushButton::clicked, this, [machineId]() {
        QApplication::clipboard()->setText(machineId);
    });
    machineIdLayout->addWidget(copyBtn);
    mainLayout->addLayout(machineIdLayout);

    auto *infoLabel = new QLabel("Send your Machine ID to get a license key.");
    infoLabel->setStyleSheet("color: #666; font-size: 9px;");
    infoLabel->setAlignment(Qt::AlignCenter);
    mainLayout->addWidget(infoLabel);

    auto *titleLabel = new QLabel("Enter your license key to activate:");
    QFont titleFont = titleLabel->font();
    titleFont.setPointSize(10);
    titleLabel->setFont(titleFont);
    mainLayout->addWidget(titleLabel);

    m_keyEdit = new QLineEdit();
    m_keyEdit->setPlaceholderText("XXXX-XXXX-XXXX-XXXX");
    m_keyEdit->setMaxLength(19);
    QFont monoFont("Consolas", 14);
    monoFont.setStyleHint(QFont::Monospace);
    m_keyEdit->setFont(monoFont);
    m_keyEdit->setAlignment(Qt::AlignCenter);
    mainLayout->addWidget(m_keyEdit);

    m_statusLabel = new QLabel("");
    m_statusLabel->setAlignment(Qt::AlignCenter);
    mainLayout->addWidget(m_statusLabel);

    auto *btnLayout = new QHBoxLayout();
    btnLayout->addStretch();
    m_activateBtn = new QPushButton("Activate");
    m_activateBtn->setFixedWidth(120);
    m_activateBtn->setEnabled(false);
    btnLayout->addWidget(m_activateBtn);
    btnLayout->addStretch();
    mainLayout->addLayout(btnLayout);

    connect(m_activateBtn, &QPushButton::clicked, this, &LicenseDialog::onActivateClicked);
    connect(m_keyEdit, &QLineEdit::textChanged, this, &LicenseDialog::onKeyTextChanged);
    connect(m_keyEdit, &QLineEdit::returnPressed, this, &LicenseDialog::onActivateClicked);
}

void LicenseDialog::onKeyTextChanged(const QString &text)
{
    m_activateBtn->setEnabled(text.trimmed().length() >= 19);
    m_statusLabel->setText("");
}

void LicenseDialog::onActivateClicked()
{
    QString key = m_keyEdit->text().trimmed().toUpper();
    QString machineId = getMachineId();

    if (validateKey(key, machineId)) {
        QSettings settings(settingsPath(), QSettings::IniFormat);
        settings.setValue("license/key", key);
        settings.setValue("license/machineId", machineId);
        settings.sync();
        m_statusLabel->setStyleSheet("color: green;");
        m_statusLabel->setText("Activated successfully!");
        accept();
    } else {
        m_statusLabel->setStyleSheet("color: red;");
        m_statusLabel->setText("Invalid license key for this machine.");
    }
}

bool LicenseDialog::isActivated()
{
    QSettings settings(settingsPath(), QSettings::IniFormat);
    QString storedKey = settings.value("license/key", "").toString();
    QString storedMachineId = settings.value("license/machineId", "").toString();
    QString currentMachineId = getMachineId();

    if (storedMachineId != currentMachineId) return false;
    return !storedKey.isEmpty() && validateKey(storedKey, currentMachineId);
}

bool LicenseDialog::validateKey(const QString &key, const QString &machineId)
{
    QString normalized = key.toUpper().trimmed();

    QStringList parts = normalized.split('-');
    if (parts.size() != 4) return false;

    for (const auto &part : parts) {
        if (part.length() != 4) return false;
        for (const auto &ch : part) {
            if (!ch.isLetterOrNumber()) return false;
            if (ch.isLetter() && (ch < 'A' || ch > 'F')) return false;
        }
    }

    QString prefix = parts[0] + parts[1] + parts[2];
    quint16 expected = computeChecksum(prefix, machineId);
    QString expectedStr = QString("%1").arg(expected, 4, 16, QChar('0')).toUpper();

    return parts[3] == expectedStr;
}

QString LicenseDialog::getMachineId()
{
    QString raw;

    raw += QSysInfo::machineHostName();

    QList<QNetworkInterface> interfaces = QNetworkInterface::allInterfaces();
    for (const auto &iface : interfaces) {
        if (iface.flags().testFlag(QNetworkInterface::IsLoopBack)) continue;
        if (iface.flags().testFlag(QNetworkInterface::IsUp) && !iface.hardwareAddress().isEmpty()) {
            raw += iface.hardwareAddress();
            break;
        }
    }

    QByteArray hash = QCryptographicHash::hash(raw.toUtf8(), QCryptographicHash::Sha256);
    return hash.toHex().left(12).toUpper();
}

quint16 LicenseDialog::computeChecksum(const QString &prefix, const QString &machineId)
{
    QByteArray data = prefix.toUtf8();
    data.append(machineId.toUtf8());
    data.append("QtScrcpy_Salt_2024");
    QByteArray hash = QCryptographicHash::hash(data, QCryptographicHash::Sha256);

    quint16 result = 0;
    for (int i = 0; i < hash.size(); i += 2) {
        quint16 val = static_cast<quint8>(hash[i]) << 8;
        if (i + 1 < hash.size()) {
            val |= static_cast<quint8>(hash[i + 1]);
        }
        result ^= val;
    }
    return result;
}

QString LicenseDialog::settingsPath()
{
    QString configPath = QString::fromLocal8Bit(qgetenv("QTSCRCPY_CONFIG_PATH"));
    if (configPath.isEmpty()) {
        configPath = QCoreApplication::applicationDirPath() + "/config";
    }
    QDir dir(configPath);
    if (!dir.exists()) {
        dir.mkpath(".");
    }
    return configPath + "/license.ini";
}
