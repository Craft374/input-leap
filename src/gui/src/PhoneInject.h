/*
 * InputLeap -- mouse and keyboard sharing utility
 * Copyright (C) 2026 InputLeafPlus Developers
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#pragma once

#include "PhoneKeys.h"

#include <memory>

namespace phone {

// Injects phone input into the OS of THIS machine (CGEventPost on macOS, SendInput on Windows).
// When InputLeap's server runs here, its own hooks/event tap see the injected events like real
// hardware input and forward them to the active screen. All methods are called on the GUI thread.
class InputInjector {
public:
    virtual ~InputInjector() = default;

    // Relative pointer motion in pixels (already scaled by the phone page).
    virtual void moveRelative(int dx, int dy) = 0;
    // button: 0 = left, 1 = right, 2 = middle.
    virtual void mouseButton(int button, bool down) = 0;
    // Wheel in notches. dy > 0 = wheel up (content moves down), dx > 0 = wheel right.
    virtual void scroll(int dx, int dy) = 0;
    // Press/release one key. Modifier keys are tracked so later keys carry the right flags.
    virtual void key(Key key, bool down) = 0;
    // Releases every button/key/modifier this injector still holds down.
    virtual void releaseAll() = 0;

    virtual const char* platformName() const = 0;   // "mac", "win" or "other"
    // false when injection cannot work (e.g. "other" platform); *why gets a Korean message.
    virtual bool isAvailable(QString* why) const = 0;
};

std::unique_ptr<InputInjector> createInputInjector();

} // namespace phone
