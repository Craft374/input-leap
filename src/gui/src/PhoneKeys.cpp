/*
 * InputLeap -- mouse and keyboard sharing utility
 * Copyright (C) 2026 InputLeafPlus Developers
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "PhoneKeys.h"

namespace phone {

namespace {

struct NamedKey {
    Key key;
    const char* name;
};

// Names for everything except the contiguous ranges A-Z, Digit0-9 and F1-F12 (handled in code).
const NamedKey kNamedKeys[] = {
    {Key::Minus, "-"}, {Key::Equal, "="}, {Key::LeftBracket, "["}, {Key::RightBracket, "]"},
    {Key::Backslash, "\\"}, {Key::Semicolon, ";"}, {Key::Quote, "'"}, {Key::Comma, ","},
    {Key::Period, "."}, {Key::Slash, "/"}, {Key::Backquote, "`"},
    {Key::Space, "Space"}, {Key::Enter, "Enter"}, {Key::Tab, "Tab"}, {Key::Backspace, "Backspace"},
    {Key::Delete, "Delete"}, {Key::Escape, "Escape"},
    {Key::Up, "Up"}, {Key::Down, "Down"}, {Key::Left, "Left"}, {Key::Right, "Right"},
    {Key::Home, "Home"}, {Key::End, "End"}, {Key::PageUp, "PageUp"}, {Key::PageDown, "PageDown"},
    {Key::Ctrl, "Ctrl"}, {Key::Alt, "Alt"}, {Key::Shift, "Shift"}, {Key::Meta, "Meta"},
    {Key::HangulToggle, "HangulToggle"},
};

constexpr int idx(Key k) { return static_cast<int>(k); }

// US layout: unshifted symbol keys and what they type with Shift, index-aligned.
const char kSymbolKeys[] = "-=[]\\;',./`";
const char kSymbolKeysShifted[] = "_+{}|:\"<>?~";
const Key kSymbolKeyIds[] = {Key::Minus, Key::Equal, Key::LeftBracket, Key::RightBracket, Key::Backslash,
                             Key::Semicolon, Key::Quote, Key::Comma, Key::Period, Key::Slash, Key::Backquote};
const char kDigitsShifted[] = ")!@#$%^&*(";   // Shift+0 .. Shift+9

bool asciiStroke(char c, KeyStroke* out)
{
    if (c >= 'a' && c <= 'z') {
        *out = {static_cast<Key>(idx(Key::A) + (c - 'a')), false};
    } else if (c >= 'A' && c <= 'Z') {
        *out = {static_cast<Key>(idx(Key::A) + (c - 'A')), true};
    } else if (c >= '0' && c <= '9') {
        *out = {static_cast<Key>(idx(Key::Digit0) + (c - '0')), false};
    } else if (c == ' ') {
        *out = {Key::Space, false};
    } else if (c == '\n') {
        *out = {Key::Enter, false};
    } else if (c == '\t') {
        *out = {Key::Tab, false};
    } else {
        for (int i = 0; kSymbolKeys[i]; ++i) {
            if (c == kSymbolKeys[i]) { *out = {kSymbolKeyIds[i], false}; return true; }
            if (c == kSymbolKeysShifted[i]) { *out = {kSymbolKeyIds[i], true}; return true; }
        }
        for (int i = 0; i < 10; ++i) {
            if (c == kDigitsShifted[i]) { *out = {static_cast<Key>(idx(Key::Digit0) + i), true}; return true; }
        }
        return false;
    }
    return true;
}

// Dubeolsik key positions; an uppercase letter means "that key with Shift".
// Initial consonants in Unicode order (19).
const char* const kInitials[] = {"r", "R", "s", "e", "E", "f", "a", "q", "Q", "t", "T", "d", "w", "W", "c", "z", "x", "v", "g"};
// Vowels in Unicode order (21); identical order for syllables and compatibility jamo U+314F..U+3163.
const char* const kVowels[] = {"k", "o", "i", "O", "j", "p", "u", "P", "h", "hk", "ho", "hl",
                               "y", "n", "nj", "np", "nl", "b", "m", "ml", "l"};
// Final consonants (28, index 0 = none).
const char* const kFinals[] = {"", "r", "R", "rt", "s", "sw", "sg", "e", "f", "fr", "fa", "fq", "ft", "fx", "fv", "fg",
                               "a", "q", "qt", "t", "T", "d", "w", "c", "z", "x", "v", "g"};
// Compatibility jamo U+3131..U+314E (consonants, 30), incl. compound ones.
const char* const kCompatConsonants[] = {"r", "R", "rt", "s", "sw", "sg", "e", "E", "f", "fr", "fa", "fq", "ft", "fx", "fv", "fg",
                                         "a", "q", "Q", "qt", "t", "T", "d", "w", "W", "c", "z", "x", "v", "g"};

void appendKeys(QVector<KeyStroke>& out, const char* keys)
{
    for (; *keys; ++keys) {
        KeyStroke s;
        if (asciiStroke(*keys, &s))
            out.append(s);
    }
}

} // namespace

bool isModifier(Key key)
{
    return key == Key::Ctrl || key == Key::Alt || key == Key::Shift || key == Key::Meta;
}

QString keyName(Key key)
{
    const int k = idx(key);
    if (k >= idx(Key::A) && k <= idx(Key::Z))
        return QString(QChar('a' + (k - idx(Key::A))));
    if (k >= idx(Key::Digit0) && k <= idx(Key::Digit9))
        return QString(QChar('0' + (k - idx(Key::Digit0))));
    if (k >= idx(Key::F1) && k <= idx(Key::F12))
        return QStringLiteral("F") + QString::number(k - idx(Key::F1) + 1);
    for (const NamedKey& n : kNamedKeys) {
        if (n.key == key)
            return QString::fromLatin1(n.name);
    }
    return QString();
}

bool keyFromName(const QString& name, Key* out)
{
    // Wire input is untrusted: only exact names are accepted. Key names are short, so a linear scan is fine.
    // ponytail: linear scan over ~100 names per key event; use a QHash if profiling ever says so.
    for (int k = idx(Key::A); k <= idx(Key::HangulToggle); ++k) {
        if (keyName(static_cast<Key>(k)) == name) {
            if (out)
                *out = static_cast<Key>(k);
            return true;
        }
    }
    return false;
}

QVector<KeyStroke> textToStrokes(const QString& text, int* skipped)
{
    QVector<KeyStroke> out;
    int skip = 0;
    for (int i = 0; i < text.size(); ++i) {
        const ushort u = text[i].unicode();
        if (u < 0x80) {
            KeyStroke s;
            if (asciiStroke(static_cast<char>(u), &s))
                out.append(s);
            else if (u != '\r')   // CRLF text: the '\n' already gives Enter, '\r' is dropped silently
                ++skip;
        } else if (u >= 0xAC00 && u <= 0xD7A3) {
            const int s = u - 0xAC00;
            appendKeys(out, kInitials[s / 588]);
            appendKeys(out, kVowels[(s % 588) / 28]);
            appendKeys(out, kFinals[s % 28]);
        } else if (u >= 0x3131 && u <= 0x314E) {
            appendKeys(out, kCompatConsonants[u - 0x3131]);
        } else if (u >= 0x314F && u <= 0x3163) {
            appendKeys(out, kVowels[u - 0x314F]);
        } else {
            if (text[i].isHighSurrogate() && i + 1 < text.size() && text[i + 1].isLowSurrogate())
                ++i;   // one non-BMP character (e.g. emoji) counts as one skipped character
            ++skip;
        }
    }
    if (skipped)
        *skipped = skip;
    return out;
}

} // namespace phone
