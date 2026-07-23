/*
 * InputLeap -- mouse and keyboard sharing utility
 * Copyright (C) 2026 InputLeap Authors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "KeyboardSettingsDialog.h"

#include "inputleap/key_types.h"

#include <QAbstractItemView>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

KeyboardSettingsDialog::KeyboardSettingsDialog(
    QWidget* parent,
    const QList<Screen::Modifier>& modifiers,
    const QMap<QString, QString>& keyMappings) :
    QDialog(parent),
    m_MappingTable(new QTableWidget(this)),
    m_Modifiers(modifiers),
    m_KeyMappings(keyMappings)
{
    setWindowTitle(tr("Keyboard Settings"));
    resize(620, 520);

    auto* mainLayout = new QVBoxLayout(this);
    auto* description = new QLabel(
        tr("Configure keys sent from the server to this client screen."), this);
    description->setWordWrap(true);
    mainLayout->addWidget(description);

    auto* modifierGroup = new QGroupBox(tr("Modifier keys"), this);
    auto* modifierLayout = new QGridLayout(modifierGroup);
    modifierLayout->addWidget(new QLabel(tr("Server key"), modifierGroup), 0, 0);
    modifierLayout->addWidget(new QLabel(tr("Send as"), modifierGroup), 0, 1);

    const QStringList modifierNames = {
        tr("Shift"), tr("Ctrl"), tr("Alt (Option on macOS)"), tr("Meta"),
        tr("Super (Command on macOS)"), tr("None")
    };
    const QStringList serverModifierNames = {
        tr("Shift"), tr("Ctrl"), tr("Alt"), tr("Meta"), tr("Super / Windows")
    };

    for (int i = 0; i < static_cast<int>(m_ModifierBoxes.size()); ++i) {
        modifierLayout->addWidget(new QLabel(serverModifierNames[i], modifierGroup), i + 1, 0);
        auto* combo = new QComboBox(modifierGroup);
        combo->addItems(modifierNames);
        const int current = i < m_Modifiers.size() ? static_cast<int>(m_Modifiers[i]) : i;
        combo->setCurrentIndex(current >= 0 ? current : i);
        m_ModifierBoxes[i] = combo;
        modifierLayout->addWidget(combo, i + 1, 1);
    }

    auto* presetButton = new QPushButton(
        tr("Preset: Alt to Command, Windows to Option"), modifierGroup);
    connect(presetButton, &QPushButton::clicked, this,
            &KeyboardSettingsDialog::applyWindowsMacPreset);
    modifierLayout->addWidget(presetButton, 6, 0);

    auto* resetButton = new QPushButton(tr("Reset modifiers"), modifierGroup);
    connect(resetButton, &QPushButton::clicked, this,
            &KeyboardSettingsDialog::resetModifiers);
    modifierLayout->addWidget(resetButton, 6, 1);
    mainLayout->addWidget(modifierGroup);

    auto* mappingGroup = new QGroupBox(tr("Key mappings"), this);
    auto* mappingLayout = new QVBoxLayout(mappingGroup);
    m_MappingTable->setColumnCount(2);
    m_MappingTable->setHorizontalHeaderLabels({tr("Server key"), tr("Client key")});
    m_MappingTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_MappingTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_MappingTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_MappingTable->verticalHeader()->setVisible(false);
    mappingLayout->addWidget(m_MappingTable);

    auto* mappingButtons = new QHBoxLayout;
    auto* addButton = new QPushButton(tr("Add"), mappingGroup);
    connect(addButton, &QPushButton::clicked, this, [this]() { addMappingRow(); });
    mappingButtons->addWidget(addButton);

    auto* removeButton = new QPushButton(tr("Remove"), mappingGroup);
    connect(removeButton, &QPushButton::clicked, this,
            &KeyboardSettingsDialog::removeSelectedMapping);
    mappingButtons->addWidget(removeButton);

    auto* capsLockButton = new QPushButton(tr("Caps Lock to F18"), mappingGroup);
    connect(capsLockButton, &QPushButton::clicked, this,
            &KeyboardSettingsDialog::setCapsLockPreset);
    mappingButtons->addWidget(capsLockButton);
    mappingButtons->addStretch();
    mappingLayout->addLayout(mappingButtons);
    mainLayout->addWidget(mappingGroup, 1);

    for (auto mapping = m_KeyMappings.cbegin(); mapping != m_KeyMappings.cend(); ++mapping) {
        addMappingRow(mapping.key(), mapping.value());
    }

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &KeyboardSettingsDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &KeyboardSettingsDialog::reject);
    mainLayout->addWidget(buttons);
}

QStringList KeyboardSettingsDialog::availableKeys()
{
    QStringList keys;
    for (int character = 0x21; character <= 0x7e; ++character) {
        keys.append(QString(QChar(character)));
    }
    for (const KeyNameMapEntry* key = kKeyNameMap; key->m_name != nullptr; ++key) {
        keys.append(QString::fromLatin1(key->m_name));
    }
    keys.removeDuplicates();
    keys.sort(Qt::CaseInsensitive);
    return keys;
}

bool KeyboardSettingsDialog::canonicalKey(const QString& text, QString& canonical)
{
    const QString trimmed = text.trimmed();
    KeyID key = kKeyNone;
    for (const KeyNameMapEntry* entry = kKeyNameMap; entry->m_name != nullptr; ++entry) {
        if (trimmed.compare(QString::fromLatin1(entry->m_name), Qt::CaseInsensitive) == 0) {
            key = entry->m_id;
            break;
        }
    }
    if (key == kKeyNone && trimmed.size() == 1) {
        const ushort character = trimmed[0].unicode();
        if (character >= 0x21 && character <= 0x7e) {
            key = character;
        }
    }
    if (key == kKeyNone && trimmed.size() == 6 && trimmed.startsWith(QStringLiteral("\\u"))) {
        bool parsed = false;
        const uint value = trimmed.mid(2).toUInt(&parsed, 16);
        if (parsed && value != 0) {
            key = value;
        }
    }
    if (key == kKeyNone) {
        return false;
    }

    QString keyName;
    for (const KeyNameMapEntry* entry = kKeyNameMap; entry->m_name != nullptr; ++entry) {
        if (entry->m_id == key) {
            keyName = QString::fromLatin1(entry->m_name);
        }
    }
    if (!keyName.isEmpty()) {
        canonical = keyName;
        return true;
    }
    if (key >= 0x21 && key <= 0x7e) {
        canonical = QString(QChar(static_cast<ushort>(key)));
    }
    else {
        canonical = QStringLiteral("\\u%1").arg(key, 4, 16, QChar('0'));
    }
    return true;
}

void KeyboardSettingsDialog::addMappingRow(const QString& source, const QString& destination)
{
    const int row = m_MappingTable->rowCount();
    m_MappingTable->insertRow(row);

    static const QStringList keys = availableKeys();
    for (int column = 0; column < 2; ++column) {
        auto* combo = new QComboBox(m_MappingTable);
        combo->setEditable(true);
        combo->setMaxVisibleItems(20);
        combo->addItems(keys);
        combo->setEditText(column == 0 ? source : destination);
        m_MappingTable->setCellWidget(row, column, combo);
    }
    m_MappingTable->selectRow(row);
}

void KeyboardSettingsDialog::applyWindowsMacPreset()
{
    // Swap Alt and the Windows key so the mac client sees them where a mac
    // keyboard has them: Alt sits next to the space bar like Command does.
    resetModifiers();
    m_ModifierBoxes[static_cast<int>(Screen::Modifier::Alt)]->setCurrentIndex(
        static_cast<int>(Screen::Modifier::Super));
    m_ModifierBoxes[static_cast<int>(Screen::Modifier::Super)]->setCurrentIndex(
        static_cast<int>(Screen::Modifier::Alt));
}

void KeyboardSettingsDialog::resetModifiers()
{
    for (int i = 0; i < static_cast<int>(m_ModifierBoxes.size()); ++i) {
        m_ModifierBoxes[i]->setCurrentIndex(i);
    }
}

void KeyboardSettingsDialog::removeSelectedMapping()
{
    const int row = m_MappingTable->currentRow();
    if (row >= 0) {
        m_MappingTable->removeRow(row);
    }
}

void KeyboardSettingsDialog::setCapsLockPreset()
{
    for (int row = 0; row < m_MappingTable->rowCount(); ++row) {
        auto* source = qobject_cast<QComboBox*>(m_MappingTable->cellWidget(row, 0));
        QString canonical;
        if (source != nullptr && canonicalKey(source->currentText(), canonical) &&
            canonical == QStringLiteral("CapsLock")) {
            auto* destination = qobject_cast<QComboBox*>(m_MappingTable->cellWidget(row, 1));
            destination->setEditText(QStringLiteral("F18"));
            m_MappingTable->selectRow(row);
            return;
        }
    }
    addMappingRow(QStringLiteral("CapsLock"), QStringLiteral("F18"));
}

void KeyboardSettingsDialog::accept()
{
    QMap<QString, QString> mappings;
    for (int row = 0; row < m_MappingTable->rowCount(); ++row) {
        auto* sourceBox = qobject_cast<QComboBox*>(m_MappingTable->cellWidget(row, 0));
        auto* destinationBox = qobject_cast<QComboBox*>(m_MappingTable->cellWidget(row, 1));
        QString source;
        QString destination;
        if (sourceBox == nullptr || destinationBox == nullptr ||
            !canonicalKey(sourceBox->currentText(), source) ||
            !canonicalKey(destinationBox->currentText(), destination)) {
            QMessageBox::warning(this, tr("Invalid key mapping"),
                                 tr("Select a valid server and client key for every row."));
            return;
        }
        if (mappings.contains(source)) {
            QMessageBox::warning(this, tr("Duplicate key mapping"),
                                 tr("Each server key can only be mapped once."));
            return;
        }
        if (source != destination) {
            mappings.insert(source, destination);
        }
    }

    m_Modifiers.clear();
    for (auto* combo : m_ModifierBoxes) {
        m_Modifiers.append(static_cast<Screen::Modifier>(combo->currentIndex()));
    }
    m_KeyMappings = mappings;
    QDialog::accept();
}
