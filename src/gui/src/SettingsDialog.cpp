/*
 * InputLeap -- mouse and keyboard sharing utility
 * Copyright (C) 2023-2024 InputLeap Developers
 * Copyright (C) 2012-2016 Symless Ltd.
 * Copyright (C) 2008 Volker Lanz (vl@fidra.de)
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 *
 * This package is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "SettingsDialog.h"
#include "ui_SettingsDialog.h"

#include "AppLocale.h"
#include "QUtility.h"
#include "AppConfig.h"
#include "PeerLink.h"
#include "PhoneServer.h"
#if defined(Q_OS_MAC)
#include "MacInputDevice.h"
#endif

#include <QtCore>
#include <QtGui>
#include <QMessageBox>
#include <QFileDialog>
#include <QDir>

SettingsDialog::SettingsDialog(QWidget* parent, AppConfig& config) :
    QDialog(parent, Qt::WindowTitleHint | Qt::WindowSystemMenuHint | Qt::WindowMaximizeButtonHint),
    ui_{std::make_unique<Ui::SettingsDialog>()},
    app_config_(config)
{
    ui_->setupUi(this);

    connect(ui_->buttonBox, &QDialogButtonBox::accepted, this, &SettingsDialog::accept);
    connect(ui_->buttonBox, &QDialogButtonBox::rejected, this, &SettingsDialog::reject);

    AppLocale locale;
    locale.fillLanguageComboBox(ui_->m_pComboLanguage);
    ui_->m_pLabel_27->hide();
    ui_->m_pComboLanguage->hide();

    ui_->m_pLineEditScreenName->setText(app_config_.screenName());
    ui_->m_pSpinBoxPort->setValue(app_config_.port());
    ui_->m_pLineEditInterface->setText(app_config_.networkInterface());
    ui_->m_pComboLogLevel->setCurrentIndex(app_config_.logLevel());
    ui_->m_pCheckBoxLogToFile->setChecked(app_config_.logToFile());
    ui_->m_pLineEditLogFilename->setText(app_config_.logFilename());
    setIndexFromItemData(ui_->m_pComboLanguage, app_config_.language());
    ui_->m_pCheckBoxAutoHide->setChecked(app_config_.getAutoHide());
    ui_->m_pCheckBoxAutoStart->setChecked(app_config_.getAutoStart());
    ui_->m_pCheckBoxMinimizeToTray->setChecked(app_config_.getMinimizeToTray());
    ui_->m_pCheckBoxEnableCrypto->setChecked(app_config_.getCryptoEnabled());
    ui_->checkbox_require_client_certificate->setChecked(app_config_.getRequireClientCertificate());

    ui_->m_pCheckBoxPhoneEnabled->setChecked(app_config_.phoneEnabled());
    ui_->m_pSpinBoxPhonePort->setValue(app_config_.phonePort());
    ui_->m_pLineEditPhonePin->setText(app_config_.phonePin());
    ui_->m_pCheckBoxPeerEnabled->setChecked(app_config_.peerLinkEnabled());
    ui_->m_pSpinBoxPeerPort->setValue(app_config_.peerLinkPort());
    ui_->m_pLineEditPeerCode->setText(app_config_.peerPairingCode());
    ui_->m_pLineEditPeerAddress->setText(app_config_.peerAddress());

#if defined(Q_OS_MAC)
    const QStringList selectedDeviceIds = app_config_.macLocalInputDevice().split(
        QLatin1Char(','), Qt::SkipEmptyParts);
    QSet<QString> remainingSelectedIds(selectedDeviceIds.begin(), selectedDeviceIds.end());
    for (const auto& device : macInputDevices()) {
        auto* item = new QListWidgetItem(device.name, ui_->m_pListLocalInputDevices);
        item->setData(Qt::UserRole, device.id);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        const bool selected = remainingSelectedIds.remove(device.id);
        item->setCheckState(selected ? Qt::Checked : Qt::Unchecked);
    }
    for (const auto& id : remainingSelectedIds) {
        auto* item = new QListWidgetItem(
            tr("저장된 USB 입력 장치 (현재 연결되지 않음): %1").arg(id), ui_->m_pListLocalInputDevices);
        item->setData(Qt::UserRole, id);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Checked);
    }
    ui_->m_pCheckBoxMacMapFunctionKeys->setChecked(app_config_.getMacMapFunctionKeys());
#else
    ui_->m_pGroupMacInput->hide();
#endif

#if defined(Q_OS_WIN)
    ui_->m_pComboElevate->setCurrentIndex(static_cast<int>(app_config_.elevateMode()));
#else
    // elevate checkbox is only useful on ms windows.
    ui_->m_pLabelElevate->hide();
    ui_->m_pComboElevate->hide();
#endif

#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
    connect(ui_->m_pCheckBoxLogToFile, &QCheckBox::checkStateChanged, this,
            [this](Qt::CheckState state) { logToFileChanged(state == Qt::Checked); });
#else
    connect(ui_->m_pCheckBoxLogToFile, &QCheckBox::stateChanged, this,
            [this](int state) { logToFileChanged(state == 2); });
#endif
    auto refreshPhoneUrls = [this]() {
        const bool on = ui_->m_pCheckBoxPhoneEnabled->isChecked();
        ui_->m_pLabelPhoneUrlsTitle->setVisible(on);
        ui_->m_pLabelPhoneUrls->setVisible(on);
        ui_->m_pLabelPhoneUrls->setText(
            PhoneServer::localUrls(static_cast<quint16>(ui_->m_pSpinBoxPhonePort->value())).join(QLatin1Char('\n')));
    };
    refreshPhoneUrls();
    connect(ui_->m_pCheckBoxPhoneEnabled, &QCheckBox::toggled, this, refreshPhoneUrls);
    connect(ui_->m_pSpinBoxPhonePort, QOverload<int>::of(&QSpinBox::valueChanged), this, refreshPhoneUrls);
    connect(ui_->m_pCheckBoxPeerEnabled, &QCheckBox::toggled, this, [this](bool on) {
        if (on && ui_->m_pLineEditPeerCode->text().trimmed().isEmpty()) {
            ui_->m_pLineEditPeerCode->setText(peerlink::generatePairingCode());
        }
    });
    connect(ui_->m_pButtonPeerNewCode, &QPushButton::clicked, this, [this]() {
        ui_->m_pLineEditPeerCode->setText(peerlink::generatePairingCode());
    });
    connect(ui_->m_pButtonBrowseLog, &QPushButton::clicked, this, &SettingsDialog::browseLogClicked);
    connect(ui_->m_pComboLanguage, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &SettingsDialog::languageChanged);

    // The options scroll and the window is resizable; open at the natural size, capped to the screen
    // so the OK button never ends up off screen.
    setSizeGripEnabled(true);
    setMinimumSize(320, 240);
    const QScreen* screen = parent != nullptr && parent->screen() != nullptr ? parent->screen() : QGuiApplication::primaryScreen();
    const QSize wanted = ui_->m_pScrollContents->sizeHint() + QSize(40, ui_->buttonBox->sizeHint().height() + 40);
    resize(wanted.boundedTo(screen != nullptr ? screen->availableSize() * 0.85 : wanted));
}

void SettingsDialog::accept()
{
    // validate everything before writing anything to AppConfig
    const QString phonePin = ui_->m_pLineEditPhonePin->text().trimmed();
    const bool phonePinOk = phonePin.size() >= 4 && phonePin.size() <= 16;
    if (ui_->m_pCheckBoxPhoneEnabled->isChecked() && !phonePinOk) {
        QMessageBox::warning(this, tr("설정"), tr("휴대폰 트랙패드 PIN은 4~16자여야 합니다."));
        return;
    }
    const QString peerCode = ui_->m_pLineEditPeerCode->text().trimmed();
    const bool peerCodeOk = peerlink::isValidPairingCode(peerCode);
    if (ui_->m_pCheckBoxPeerEnabled->isChecked() && !peerCodeOk) {
        QMessageBox::warning(this, tr("설정"),
            tr("페어링 코드는 8자 이상이어야 합니다. '새 코드 만들기'를 눌러 주세요."));
        return;
    }

    app_config_.setScreenName(ui_->m_pLineEditScreenName->text());
    app_config_.setPort(ui_->m_pSpinBoxPort->value());
    app_config_.setNetworkInterface(ui_->m_pLineEditInterface->text());
    app_config_.setCryptoEnabled(ui_->m_pCheckBoxEnableCrypto->isChecked());
    app_config_.setRequireClientCertificate(ui_->checkbox_require_client_certificate->isChecked());
    app_config_.setLogLevel(ui_->m_pComboLogLevel->currentIndex());
    app_config_.setLogToFile(ui_->m_pCheckBoxLogToFile->isChecked());
    app_config_.setLogFilename(ui_->m_pLineEditLogFilename->text());
    app_config_.setLanguage(ui_->m_pComboLanguage->itemData(ui_->m_pComboLanguage->currentIndex()).toString());
    app_config_.setElevateMode(static_cast<ElevateMode>(ui_->m_pComboElevate->currentIndex()));
    app_config_.setAutoHide(ui_->m_pCheckBoxAutoHide->isChecked());
    app_config_.setAutoStart(ui_->m_pCheckBoxAutoStart->isChecked());
    app_config_.setMinimizeToTray(ui_->m_pCheckBoxMinimizeToTray->isChecked());
    app_config_.setPhoneEnabled(ui_->m_pCheckBoxPhoneEnabled->isChecked());
    app_config_.setPhonePort(ui_->m_pSpinBoxPhonePort->value());
    if (phonePinOk) app_config_.setPhonePin(phonePin);
    app_config_.setPeerLinkEnabled(ui_->m_pCheckBoxPeerEnabled->isChecked());
    app_config_.setPeerLinkPort(ui_->m_pSpinBoxPeerPort->value());
    if (peerCodeOk) app_config_.setPeerPairingCode(peerCode);
    app_config_.setPeerAddress(ui_->m_pLineEditPeerAddress->text().trimmed());
#if defined(Q_OS_MAC)
    QStringList selectedDeviceIds;
    for (int i = 0; i < ui_->m_pListLocalInputDevices->count(); ++i) {
        const auto* item = ui_->m_pListLocalInputDevices->item(i);
        if (item->checkState() == Qt::Checked) {
            selectedDeviceIds << item->data(Qt::UserRole).toString();
        }
    }
    app_config_.setMacLocalInputDevice(selectedDeviceIds.join(QLatin1Char(',')));
    app_config_.setMacMapFunctionKeys(ui_->m_pCheckBoxMacMapFunctionKeys->isChecked());
#endif
    app_config_.saveSettings();
    QDialog::accept();
}

void SettingsDialog::reject()
{
    if (app_config_.language() != ui_->m_pComboLanguage->itemData(ui_->m_pComboLanguage->currentIndex()).toString()) {
        Q_EMIT requestLanguageChange(app_config_.language());
    }
    QDialog::reject();
}

void SettingsDialog::changeEvent(QEvent* event)
{
    if (event != nullptr)
    {
        switch (event->type())
        {
        case QEvent::LanguageChange:
            {
                int logLevelIndex = ui_->m_pComboLogLevel->currentIndex();

                ui_->m_pComboLanguage->blockSignals(true);
                ui_->retranslateUi(this);
                ui_->m_pComboLanguage->blockSignals(false);

                ui_->m_pComboLogLevel->setCurrentIndex(logLevelIndex);
                break;
            }

        default:
            QDialog::changeEvent(event);
        }
    }
}

void SettingsDialog::logToFileChanged(bool checked)
{

    ui_->m_pLineEditLogFilename->setEnabled(checked);
    ui_->m_pButtonBrowseLog->setEnabled(checked);
}

void SettingsDialog::browseLogClicked()
{
    QString fileName = QFileDialog::getSaveFileName(
        this, tr("Save log file to..."),
        ui_->m_pLineEditLogFilename->text(),
        tr("로그 파일 (*.log *.txt)"));

    if (!fileName.isEmpty())
    {
        ui_->m_pLineEditLogFilename->setText(fileName);
    }
}

void SettingsDialog::languageChanged(int index)
{
    Q_EMIT requestLanguageChange(ui_->m_pComboLanguage->itemData(index).toString());
}

SettingsDialog::~SettingsDialog() = default;
