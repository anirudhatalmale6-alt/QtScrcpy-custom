#include "licensedialog.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFont>
#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QCoreApplication>

LicenseDialog::LicenseDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle("Activation");
    setFixedSize(420, 200);
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(12);
    mainLayout->setContentsMargins(24, 24, 24, 24);

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

    if (validateKey(key)) {
        QSettings settings(settingsPath(), QSettings::IniFormat);
        settings.setValue("license/key", key);
        settings.sync();
        m_statusLabel->setStyleSheet("color: green;");
        m_statusLabel->setText("Activated successfully!");
        accept();
    } else {
        m_statusLabel->setStyleSheet("color: red;");
        m_statusLabel->setText("Invalid license key. Please try again.");
    }
}

bool LicenseDialog::isActivated()
{
    QSettings settings(settingsPath(), QSettings::IniFormat);
    QString storedKey = settings.value("license/key", "").toString();
    return !storedKey.isEmpty() && validateKey(storedKey);
}

bool LicenseDialog::validateKey(const QString &key)
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
    quint16 expected = computeChecksum(prefix);
    QString expectedStr = QString("%1").arg(expected, 4, 16, QChar('0')).toUpper();

    return parts[3] == expectedStr;
}

quint16 LicenseDialog::computeChecksum(const QString &prefix)
{
    QByteArray data = prefix.toUtf8();
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
