/*
 * InputLeap -- mouse and keyboard sharing utility
 * Copyright (C) 2026 InputLeafPlus Developers
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "PhoneInject.h"

#include "inputleap/PhoneInputMarker.h"

#include <QString>
#include <QtGlobal>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <set>

#if defined(Q_OS_MAC)
#include <ApplicationServices/ApplicationServices.h>
#elif defined(Q_OS_WIN)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace phone {

namespace {

// The server also clamps these; repeated here because the injector is the last line before the OS.
[[maybe_unused]] constexpr int kMaxMove = 2000;
[[maybe_unused]] constexpr int kMaxScroll = 50;

// Remembers what is held so releaseAll() is exact and a key-up without a key-down is ignored.
// Platforms implement only the postXxx() hooks, called AFTER the held state was updated.
class TrackingInjector : public InputInjector {
public:
    void mouseButton(int button, bool down) final
    {
        if (button < 0 || button > 2 || buttons_[button] == down) {
            return;
        }
        buttons_[button] = down;
        postButton(button, down);
    }

    void key(Key key, bool down) final
    {
        if (key == Key::None) {
            return;
        }
        const bool held = keys_.count(key) != 0;
        if (!down) {
            if (held) {
                keys_.erase(key);
                postKey(key, false, false);
            }
            return;
        }
        if (held && (isModifier(key) || key == Key::HangulToggle)) {
            return;
        }
        keys_.insert(key);
        postKey(key, true, held);   // held && regular key = autorepeat
    }

    void releaseAll() final
    {
        for (int button = 0; button < 3; ++button) {
            mouseButton(button, false);
        }
        // regular keys first so their key-up still carries the modifier flags
        for (const bool modifiers : {false, true}) {
            const std::set<Key> snapshot = keys_;
            for (const Key held : snapshot) {
                if (isModifier(held) == modifiers) {
                    key(held, false);
                }
            }
        }
    }

protected:
    virtual void postButton(int button, bool down) = 0;
    virtual void postKey(Key key, bool down, bool repeat) = 0;

    bool buttonHeld(int button) const { return buttons_[button]; }
    bool keyHeld(Key key) const { return keys_.count(key) != 0; }

private:
    bool buttons_[3] = {false, false, false};
    std::set<Key> keys_;
};

#if defined(Q_OS_MAC)

// Key codes are the macOS virtual key codes (kVK_* in HIToolbox/Events.h); -1 = not mapped.
int macKeyCode(Key key)
{
    static const int letters[26] = {
        0x00, 0x0B, 0x08, 0x02, 0x0E, 0x03, 0x05, 0x04, 0x22, 0x26, 0x28, 0x25, 0x2E,   // a-m
        0x2D, 0x1F, 0x23, 0x0C, 0x0F, 0x01, 0x11, 0x20, 0x09, 0x0D, 0x07, 0x10, 0x06};  // n-z
    static const int digits[10] = {0x1D, 0x12, 0x13, 0x14, 0x15, 0x17, 0x16, 0x1A, 0x1C, 0x19};   // 0-9
    static const int fkeys[12] = {0x7A, 0x78, 0x63, 0x76, 0x60, 0x61, 0x62, 0x64, 0x65, 0x6D, 0x67, 0x6F};

    if (key >= Key::A && key <= Key::Z) {
        return letters[static_cast<int>(key) - static_cast<int>(Key::A)];
    }
    if (key >= Key::Digit0 && key <= Key::Digit9) {
        return digits[static_cast<int>(key) - static_cast<int>(Key::Digit0)];
    }
    if (key >= Key::F1 && key <= Key::F12) {
        return fkeys[static_cast<int>(key) - static_cast<int>(Key::F1)];
    }
    switch (key) {
    case Key::Minus: return 0x1B;
    case Key::Equal: return 0x18;
    case Key::LeftBracket: return 0x21;
    case Key::RightBracket: return 0x1E;
    case Key::Backslash: return 0x2A;
    case Key::Semicolon: return 0x29;
    case Key::Quote: return 0x27;
    case Key::Comma: return 0x2B;
    case Key::Period: return 0x2F;
    case Key::Slash: return 0x2C;
    case Key::Backquote: return 0x32;
    case Key::Space: return 0x31;
    case Key::Enter: return 0x24;       // kVK_Return
    case Key::Tab: return 0x30;
    case Key::Backspace: return 0x33;   // kVK_Delete
    case Key::Delete: return 0x75;      // kVK_ForwardDelete
    case Key::Escape: return 0x35;
    case Key::Up: return 0x7E;
    case Key::Down: return 0x7D;
    case Key::Left: return 0x7B;
    case Key::Right: return 0x7C;
    case Key::Home: return 0x73;
    case Key::End: return 0x77;
    case Key::PageUp: return 0x74;
    case Key::PageDown: return 0x79;
    default: return -1;
    }
}

struct MacModifier {
    Key key;
    CGKeyCode code;           // left-hand key
    std::uint64_t flag;       // kCGEventFlagMask*
    std::uint64_t deviceFlag; // NX_DEVICE*KEYMASK (device dependent bits, as real hardware sets them)
};

const MacModifier kMacModifiers[] = {
    {Key::Ctrl, 0x3B, kCGEventFlagMaskControl, 0x00000001},     // kVK_Control, NX_DEVICELCTLKEYMASK
    {Key::Alt, 0x3A, kCGEventFlagMaskAlternate, 0x00000020},    // kVK_Option, NX_DEVICELALTKEYMASK
    {Key::Shift, 0x38, kCGEventFlagMaskShift, 0x00000002},      // kVK_Shift, NX_DEVICELSHIFTKEYMASK
    {Key::Meta, 0x37, kCGEventFlagMaskCommand, 0x00000008},     // kVK_Command, NX_DEVICELCMDKEYMASK
};

constexpr CGKeyCode kMacRightCommand = 0x36;                    // kVK_RightCommand
constexpr std::uint64_t kMacRightCommandDevice = 0x00000010;    // NX_DEVICERCMDKEYMASK

bool cursorPos(CGPoint* pos)
{
    CGEventRef event = CGEventCreate(nullptr);
    if (event == nullptr) {
        return false;
    }
    *pos = CGEventGetLocation(event);
    CFRelease(event);
    return true;
}

// ponytail: bounding box of all displays; a cursor pushed into a void of a non-rectangular
// layout is left to the OS to clamp.
CGPoint clampToDisplays(CGPoint pos)
{
    CGDirectDisplayID ids[16];
    uint32_t count = 0;
    if (CGGetActiveDisplayList(16, ids, &count) != kCGErrorSuccess || count == 0) {
        return pos;
    }
    CGRect bounds = CGDisplayBounds(ids[0]);
    for (uint32_t i = 1; i < count; ++i) {
        bounds = CGRectUnion(bounds, CGDisplayBounds(ids[i]));
    }
    // the last pixel row/column is what reaches the edge (jump zone size is 1)
    pos.x = std::min(std::max(pos.x, bounds.origin.x), bounds.origin.x + bounds.size.width - 1);
    pos.y = std::min(std::max(pos.y, bounds.origin.y), bounds.origin.y + bounds.size.height - 1);
    return pos;
}

class MacInjector final : public TrackingInjector {
public:
    const char* platformName() const override { return "mac"; }

    bool isAvailable(QString* why) const override
    {
        if (AXIsProcessTrusted()) {
            return true;
        }
        if (why != nullptr) {
            *why = QStringLiteral("손쉬운 사용 권한이 필요합니다");
        }
        return false;
    }

    void moveRelative(int dx, int dy) override
    {
        dx = std::clamp(dx, -kMaxMove, kMaxMove);
        dy = std::clamp(dy, -kMaxMove, kMaxMove);
        // steps of <= 50 px: the server's remote-motion code and apps expect small moves
        const int steps = (std::max(std::abs(dx), std::abs(dy)) + 49) / 50;
        // CGEventPost is asynchronous: advance our own position instead of re-reading the cursor per step
        CGPoint pos;
        if (steps == 0 || !cursorPos(&pos)) {
            return;
        }
        for (int i = 1; i <= steps; ++i) {
            const int stepX = dx * i / steps - dx * (i - 1) / steps;
            const int stepY = dy * i / steps - dy * (i - 1) / steps;
            pos = clampToDisplays(CGPointMake(pos.x + stepX, pos.y + stepY));
            postMove(pos, stepX, stepY);
        }
    }

    void scroll(int dx, int dy) override
    {
        dx = std::clamp(dx, -kMaxScroll, kMaxScroll);
        dy = std::clamp(dy, -kMaxScroll, kMaxScroll);
        if (dx == 0 && dy == 0) {
            return;
        }
        // ponytail: one notch = one line. Horizontal: the wheel2 argument is positive = left (as
        // OSXScreen::fakeMouseWheel assumes) but the server tap forwards FixedPtDeltaAxis2 as is,
        // so that field gets the "right is positive" value. Verify the direction on the devices.
        CGEventRef event = CGEventCreateScrollWheelEvent(nullptr, kCGScrollEventUnitLine, 2, dy, -dx);
        if (event == nullptr) {
            return;
        }
        CGEventSetIntegerValueField(event, kCGScrollWheelEventFixedPtDeltaAxis1, static_cast<int64_t>(dy) * 65536);
        CGEventSetIntegerValueField(event, kCGScrollWheelEventFixedPtDeltaAxis2, static_cast<int64_t>(dx) * 65536);
        CGEventSetFlags(event, modifierFlags());
        CGEventPost(kCGHIDEventTap, event);
        CFRelease(event);
    }

protected:
    void postButton(int button, bool down) override
    {
        static const CGEventType downTypes[3] = {kCGEventLeftMouseDown, kCGEventRightMouseDown, kCGEventOtherMouseDown};
        static const CGEventType upTypes[3] = {kCGEventLeftMouseUp, kCGEventRightMouseUp, kCGEventOtherMouseUp};

        CGPoint pos;
        if (!cursorPos(&pos)) {
            return;
        }
        if (down) {
            // the server ignores the click state but local apps use it for double clicks
            const auto now = std::chrono::steady_clock::now();
            const bool again = button == lastButton_ && now - lastDown_ < std::chrono::milliseconds(500) &&
                std::hypot(pos.x - lastPos_.x, pos.y - lastPos_.y) <= 5;
            clickCount_ = again ? clickCount_ + 1 : 1;
            lastButton_ = button;
            lastDown_ = now;
            lastPos_ = pos;
        }
        CGEventRef event = CGEventCreateMouseEvent(nullptr, down ? downTypes[button] : upTypes[button], pos,
                                                   static_cast<CGMouseButton>(button));
        if (event == nullptr) {
            return;
        }
        CGEventSetIntegerValueField(event, kCGMouseEventClickState, clickCount_);
        CGEventSetFlags(event, modifierFlags());
        CGEventPost(kCGHIDEventTap, event);
        CFRelease(event);
    }

    void postKey(Key key, bool down, bool repeat) override
    {
        if (key == Key::HangulToggle) {
            // this fork maps the Right-Command flagsChanged event to the Hangul key (OSXScreen::onKey)
            std::uint64_t flags = modifierFlags();
            if (down) {
                flags |= kCGEventFlagMaskCommand | kMacRightCommandDevice;
            }
            postKeyEvent(kMacRightCommand, down, true, false, flags);
            return;
        }
        for (const MacModifier& modifier : kMacModifiers) {
            if (modifier.key == key) {
                postKeyEvent(modifier.code, down, true, false, modifierFlags());
                return;
            }
        }
        const int code = macKeyCode(key);
        if (code >= 0) {
            postKeyEvent(static_cast<CGKeyCode>(code), down, false, repeat, modifierFlags());
        }
    }

private:
    // Modifier flags of what the PHONE holds (hardware modifiers are not merged in), plus the live
    // Caps Lock bit: the Mac keeps AlphaShift on and OSXScreen::onKey would see its loss as a modifier release.
    CGEventFlags modifierFlags() const
    {
        std::uint64_t flags =
            CGEventSourceFlagsState(kCGEventSourceStateHIDSystemState) & kCGEventFlagMaskAlphaShift;
        for (const MacModifier& modifier : kMacModifiers) {
            if (keyHeld(modifier.key)) {
                flags |= modifier.flag | modifier.deviceFlag;
            }
        }
        return static_cast<CGEventFlags>(flags);
    }

    static void postKeyEvent(CGKeyCode code, bool down, bool flagsChanged, bool repeat, std::uint64_t flags)
    {
        CGEventRef event = CGEventCreateKeyboardEvent(nullptr, code, down);
        if (event == nullptr) {
            return;
        }
        if (flagsChanged) {
            // a real modifier press: flagsChanged carrying the key's code and the new flags
            CGEventSetType(event, kCGEventFlagsChanged);
        }
        else if (repeat) {
            CGEventSetIntegerValueField(event, kCGKeyboardEventAutorepeat, 1);
        }
        CGEventSetFlags(event, static_cast<CGEventFlags>(flags));
        CGEventPost(kCGHIDEventTap, event);
        CFRelease(event);
    }

    // one move of <= 50 px at the (already clamped) location; the deltas and the marker let the
    // server (OSXScreen::handleCGInputEvent) use them directly while the cursor is on a remote screen
    void postMove(CGPoint pos, int dx, int dy)
    {
        CGEventType type = kCGEventMouseMoved;
        CGMouseButton button = kCGMouseButtonLeft;
        if (buttonHeld(0)) {
            type = kCGEventLeftMouseDragged;
        }
        else if (buttonHeld(1)) {
            type = kCGEventRightMouseDragged;
            button = kCGMouseButtonRight;
        }
        else if (buttonHeld(2)) {
            type = kCGEventOtherMouseDragged;
            button = kCGMouseButtonCenter;
        }
        CGEventRef event = CGEventCreateMouseEvent(nullptr, type, pos, button);
        if (event == nullptr) {
            return;
        }
        if (type != kCGEventMouseMoved) {
            CGEventSetIntegerValueField(event, kCGMouseEventClickState, clickCount_);
        }
        CGEventSetIntegerValueField(event, kCGMouseEventDeltaX, dx);
        CGEventSetIntegerValueField(event, kCGMouseEventDeltaY, dy);
        CGEventSetDoubleValueField(event, kCGMouseEventDeltaX, dx);
        CGEventSetDoubleValueField(event, kCGMouseEventDeltaY, dy);
        CGEventSetIntegerValueField(event, kCGEventSourceUserData, inputleap::kPhoneInjectMarker);
        CGEventSetFlags(event, modifierFlags());
        CGEventPost(kCGHIDEventTap, event);
        CFRelease(event);
    }

    int clickCount_ = 1;
    int lastButton_ = -1;
    std::chrono::steady_clock::time_point lastDown_;
    CGPoint lastPos_ = {0, 0};
};

std::unique_ptr<InputInjector> makeInjector() { return std::make_unique<MacInjector>(); }

#elif defined(Q_OS_WIN)

// Virtual key of a phone key; *extended = needs KEYEVENTF_EXTENDEDKEY. false = not mapped.
bool winVirtualKey(Key key, WORD* vk, bool* extended)
{
    *extended = false;
    if (key >= Key::A && key <= Key::Z) {
        *vk = static_cast<WORD>('A' + (static_cast<int>(key) - static_cast<int>(Key::A)));
        return true;
    }
    if (key >= Key::Digit0 && key <= Key::Digit9) {
        *vk = static_cast<WORD>('0' + (static_cast<int>(key) - static_cast<int>(Key::Digit0)));
        return true;
    }
    if (key >= Key::F1 && key <= Key::F12) {
        *vk = static_cast<WORD>(VK_F1 + (static_cast<int>(key) - static_cast<int>(Key::F1)));
        return true;
    }
    switch (key) {
    case Key::Minus: *vk = VK_OEM_MINUS; return true;
    case Key::Equal: *vk = VK_OEM_PLUS; return true;
    case Key::LeftBracket: *vk = VK_OEM_4; return true;
    case Key::RightBracket: *vk = VK_OEM_6; return true;
    case Key::Backslash: *vk = VK_OEM_5; return true;
    case Key::Semicolon: *vk = VK_OEM_1; return true;
    case Key::Quote: *vk = VK_OEM_7; return true;
    case Key::Comma: *vk = VK_OEM_COMMA; return true;
    case Key::Period: *vk = VK_OEM_PERIOD; return true;
    case Key::Slash: *vk = VK_OEM_2; return true;
    case Key::Backquote: *vk = VK_OEM_3; return true;
    case Key::Space: *vk = VK_SPACE; return true;
    case Key::Enter: *vk = VK_RETURN; return true;
    case Key::Tab: *vk = VK_TAB; return true;
    case Key::Backspace: *vk = VK_BACK; return true;
    case Key::Escape: *vk = VK_ESCAPE; return true;
    case Key::Ctrl: *vk = VK_LCONTROL; return true;
    case Key::Alt: *vk = VK_LMENU; return true;
    case Key::Shift: *vk = VK_LSHIFT; return true;
    // never VK_CANCEL: the InputLeap hook uses it (scancode 0) as its own "fake input" marker
    case Key::HangulToggle: *vk = VK_HANGUL; return true;   // not extended (the Korean layout tells it from Hanja)
    case Key::Delete: *vk = VK_DELETE; break;
    case Key::Up: *vk = VK_UP; break;
    case Key::Down: *vk = VK_DOWN; break;
    case Key::Left: *vk = VK_LEFT; break;
    case Key::Right: *vk = VK_RIGHT; break;
    case Key::Home: *vk = VK_HOME; break;
    case Key::End: *vk = VK_END; break;
    case Key::PageUp: *vk = VK_PRIOR; break;
    case Key::PageDown: *vk = VK_NEXT; break;
    case Key::Meta: *vk = VK_LWIN; break;
    default: return false;
    }
    *extended = true;   // the cases that broke out of the switch
    return true;
}

class WinInjector final : public TrackingInjector {
public:
    const char* platformName() const override { return "win"; }
    bool isAvailable(QString*) const override { return true; }

    void moveRelative(int dx, int dy) override
    {
        dx = std::clamp(dx, -kMaxMove, kMaxMove);
        dy = std::clamp(dy, -kMaxMove, kMaxMove);
        if (dx == 0 && dy == 0) {
            return;
        }
        INPUT input = mouseInput(MOUSEEVENTF_MOVE);
        input.mi.dx = dx;
        input.mi.dy = dy;
        send(&input, 1);
    }

    void scroll(int dx, int dy) override
    {
        dx = std::clamp(dx, -kMaxScroll, kMaxScroll);
        dy = std::clamp(dy, -kMaxScroll, kMaxScroll);
        INPUT inputs[2];
        UINT count = 0;
        if (dy != 0) {
            inputs[count] = mouseInput(MOUSEEVENTF_WHEEL);
            inputs[count++].mi.mouseData = static_cast<DWORD>(dy * WHEEL_DELTA);
        }
        if (dx != 0) {
            inputs[count] = mouseInput(MOUSEEVENTF_HWHEEL);
            inputs[count++].mi.mouseData = static_cast<DWORD>(dx * WHEEL_DELTA);
        }
        if (count != 0) {
            send(inputs, count);
        }
    }

protected:
    void postButton(int button, bool down) override
    {
        static const DWORD downFlags[3] = {MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_MIDDLEDOWN};
        static const DWORD upFlags[3] = {MOUSEEVENTF_LEFTUP, MOUSEEVENTF_RIGHTUP, MOUSEEVENTF_MIDDLEUP};
        INPUT input = mouseInput(down ? downFlags[button] : upFlags[button]);
        send(&input, 1);
    }

    void postKey(Key key, bool down, bool) override
    {
        WORD vk = 0;
        bool extended = false;
        if (!winVirtualKey(key, &vk, &extended)) {
            return;
        }
        INPUT input = {};
        input.type = INPUT_KEYBOARD;
        input.ki.wVk = vk;
        input.ki.wScan = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
        if (input.ki.wScan == 0 && vk == VK_HANGUL) {
            input.ki.wScan = 0xF2;   // Korean Hangul/English key; the server drops scancode 0
        }
        input.ki.dwFlags = (extended ? KEYEVENTF_EXTENDEDKEY : 0) | (down ? 0 : KEYEVENTF_KEYUP);
        input.ki.dwExtraInfo = static_cast<ULONG_PTR>(inputleap::kPhoneInjectMarker);
        send(&input, 1);
    }

private:
    static INPUT mouseInput(DWORD flags)
    {
        INPUT input = {};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = flags;
        input.mi.dwExtraInfo = static_cast<ULONG_PTR>(inputleap::kPhoneInjectMarker);
        return input;
    }

    // Fewer events accepted than sent = blocked (UIPI: elevated foreground window, UAC/lock screen).
    void send(INPUT* inputs, UINT count)
    {
        const bool ok = SendInput(count, inputs, sizeof(INPUT)) == count;
        if (!ok && !failing_) {
            qWarning("SendInput failed (error %lu): input is blocked, e.g. by an elevated window or the lock screen",
                     static_cast<unsigned long>(GetLastError()));
        }
        failing_ = !ok;   // log once per failing streak, not once per mouse move
    }

    bool failing_ = false;
};

std::unique_ptr<InputInjector> makeInjector() { return std::make_unique<WinInjector>(); }

#else

class OtherInjector final : public InputInjector {
public:
    void moveRelative(int, int) override {}
    void mouseButton(int, bool) override {}
    void scroll(int, int) override {}
    void key(Key, bool) override {}
    void releaseAll() override {}
    const char* platformName() const override { return "other"; }

    bool isAvailable(QString* why) const override
    {
        if (why != nullptr) {
            *why = QStringLiteral("이 운영체제에서는 지원하지 않습니다");
        }
        return false;
    }
};

std::unique_ptr<InputInjector> makeInjector() { return std::make_unique<OtherInjector>(); }

#endif

} // namespace

std::unique_ptr<InputInjector> createInputInjector()
{
    return makeInjector();
}

} // namespace phone
