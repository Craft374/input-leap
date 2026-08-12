/*
 * InputLeap -- mouse and keyboard sharing utility
 * Copyright (C) 2026 InputLeafPlus Developers
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "platform/OSXMediaKeySupport.h"

#include <IOKit/hidsystem/ev_keymap.h>
#include <gtest/gtest.h>

#include <utility>

namespace inputleap {

TEST(OSXMediaKeySupportTests, macFunctionKeyForKeyCode_mapsAppleTopRow)
{
    const std::pair<std::uint32_t, int> keys[] = {
        {0x91, 1}, {0x90, 2}, {0xa0, 3}, {0x81, 4},
        {0x83, 4}, {0xb0, 5}, {0xb2, 6}
    };
    for (const auto& key : keys) {
        EXPECT_EQ(key.second, macFunctionKeyForKeyCode(key.first));
    }
    EXPECT_EQ(0, macFunctionKeyForKeyCode(0));
}

TEST(OSXMediaKeySupportTests, macFunctionKeyForSystemKey_mapsAppleTopRow)
{
    const std::pair<std::uint32_t, int> keys[] = {
        {NX_KEYTYPE_BRIGHTNESS_DOWN, 1}, {NX_KEYTYPE_BRIGHTNESS_UP, 2},
        {NX_KEYTYPE_ILLUMINATION_DOWN, 5}, {NX_KEYTYPE_ILLUMINATION_UP, 6},
        {NX_KEYTYPE_PREVIOUS, 7}, {NX_KEYTYPE_PLAY, 8},
        {NX_KEYTYPE_NEXT, 9}, {NX_KEYTYPE_MUTE, 10},
        {NX_KEYTYPE_SOUND_DOWN, 11}, {NX_KEYTYPE_SOUND_UP, 12}
    };
    for (const auto& key : keys) {
        EXPECT_EQ(key.second, macFunctionKeyForSystemKey(key.first));
    }
    EXPECT_EQ(0, macFunctionKeyForSystemKey(NX_KEYTYPE_EJECT));
}

} // namespace inputleap
