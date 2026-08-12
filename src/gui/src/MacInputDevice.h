/*
 * InputLeap -- mouse and keyboard sharing utility
 * Copyright (C) 2026 InputLeafPlus Developers
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#pragma once

#include <QString>
#include <QVector>

struct MacInputDevice
{
    QString id;
    QString name;
};

QVector<MacInputDevice> macInputDevices();
