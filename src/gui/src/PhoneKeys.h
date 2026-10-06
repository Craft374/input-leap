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

namespace phone {

// Physical keys of a US-QWERTY keyboard. The phone sends key POSITIONS, never
// layout dependent characters, so the receiving PC's IME/layout decides what appears.
enum class Key {
    None,
    A, B, C, D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
    Digit0, Digit1, Digit2, Digit3, Digit4, Digit5, Digit6, Digit7, Digit8, Digit9,
    Minus, Equal, LeftBracket, RightBracket, Backslash, Semicolon, Quote, Comma, Period, Slash, Backquote,
    Space, Enter, Tab, Backspace, Delete, Escape,
    Up, Down, Left, Right, Home, End, PageUp, PageDown,
    F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
    Ctrl, Alt, Shift, Meta,   // Alt = Option on Mac, Meta = Windows key / Command on Mac
    HangulToggle,             // the 한/영 key (Windows VK_HANGUL; on Mac the Right-Command Hangul trick used by this fork)
};

struct KeyStroke {
    Key key = Key::None;
    bool shift = false;       // key must be typed with Shift held
};

bool isModifier(Key key);                           // Ctrl, Alt, Shift, Meta
// Wire names: "a".."z", "0".."9", "-", "=", "[", "]", "\\", ";", "'", ",", ".", "/", "`",
// "Space", "Enter", "Tab", "Backspace", "Delete", "Escape", "Up", "Down", "Left", "Right",
// "Home", "End", "PageUp", "PageDown", "F1".."F12", "Ctrl", "Alt", "Shift", "Meta", "HangulToggle".
bool keyFromName(const QString& name, Key* out);    // case-sensitive; false for unknown names
QString keyName(Key key);                           // inverse of keyFromName; empty for Key::None

// Converts typed text into key strokes (US-QWERTY positions, '\n' -> Enter, '\t' -> Tab).
// Hangul syllables and compatibility jamo are decomposed into 두벌식 key positions so that
// the receiving IME (in Hangul mode) composes them. Unsupported characters are skipped and
// counted in *skipped when given.
QVector<KeyStroke> textToStrokes(const QString& text, int* skipped = nullptr);

} // namespace phone
