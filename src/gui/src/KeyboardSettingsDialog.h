/*
 * InputLeap -- mouse and keyboard sharing utility
 * Copyright (C) 2026 InputLeap Authors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#pragma once

#include "Screen.h"

#include <QDialog>
#include <QMap>

#include <array>

class QComboBox;
class QTableWidget;
class QWidget;

class KeyboardSettingsDialog : public QDialog
{
public:
    KeyboardSettingsDialog(QWidget* parent,
                           const QList<Screen::Modifier>& modifiers,
                           const QMap<QString, QString>& keyMappings);

    const QList<Screen::Modifier>& modifiers() const { return m_Modifiers; }
    const QMap<QString, QString>& keyMappings() const { return m_KeyMappings; }

public slots:
    void accept() override;

private:
    static QStringList availableKeys();
    static bool canonicalKey(const QString& text, QString& canonical);
    void addMappingRow(const QString& source = {}, const QString& destination = {});
    void applyWindowsMacPreset();
    void resetModifiers();
    void removeSelectedMapping();
    void setCapsLockPreset();

private:
    std::array<QComboBox*, 5> m_ModifierBoxes;
    QTableWidget* m_MappingTable;
    QList<Screen::Modifier> m_Modifiers;
    QMap<QString, QString> m_KeyMappings;
};
