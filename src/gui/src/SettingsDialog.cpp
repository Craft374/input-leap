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
#if defined(Q_OS_MAC)
#include "MacInputDevice.h"
#endif

#include <QtCore>
#include <QtGui>
#include <QMessageBox>
#include <QFileDialog>
#include <QDir>

SettingsDialog::SettingsDialog(QWidget* parent, AppConfig& config) :
    QDialog(parent, Qt::WindowTitleHint | Qt::WindowSystemMenuHint),
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
    connect(ui_->m_pButtonBrowseLog, &QPushButton::clicked, this, &SettingsDialog::browseLogClicked);
    connect(ui_->m_pComboLanguage, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &SettingsDialog::languageChanged);
}

void SettingsDialog::accept()
{
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
