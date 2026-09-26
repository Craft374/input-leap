/*
 * InputLeap -- mouse and keyboard sharing utility
 * Copyright (C) 2016 Symless
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

#define INPUTLEAP_TEST_ENV

#include "inputleap/KeyMap.h"

#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include <algorithm>

using ::testing::_;
using ::testing::NiceMock;
using ::testing::Invoke;
using ::testing::Return;
using ::testing::ReturnRef;
using ::testing::SaveArg;

namespace inputleap {

TEST(KeyMapTests, findBestKey_requiredDown_matchExactFirstItem)
{
    KeyMap keyMap;
    KeyMap::KeyEntryList entryList;
    KeyMap::KeyItemList itemList;
    KeyMap::KeyItem item;
    item.m_required = KeyModifierShift;
    item.m_sensitive = KeyModifierShift;
    KeyModifierMask currentState = KeyModifierShift;
    KeyModifierMask desiredState = KeyModifierShift;
    itemList.push_back(item);
    entryList.push_back(itemList);

    EXPECT_EQ(0, keyMap.findBestKey(entryList, currentState, desiredState));
}

TEST(KeyMapTests, findBestKey_requiredAndExtraSensitiveDown_matchExactFirstItem)
{
    KeyMap keyMap;
    KeyMap::KeyEntryList entryList;
    KeyMap::KeyItemList itemList;
    KeyMap::KeyItem item;
    item.m_required = KeyModifierShift;
    item.m_sensitive = KeyModifierShift | KeyModifierAlt;
    KeyModifierMask currentState = KeyModifierShift;
    KeyModifierMask desiredState = KeyModifierShift;
    itemList.push_back(item);
    entryList.push_back(itemList);

    EXPECT_EQ(0, keyMap.findBestKey(entryList, currentState, desiredState));
}

TEST(KeyMapTests, findBestKey_requiredAndExtraSensitiveDown_matchExactSecondItem)
{
    KeyMap keyMap;
    KeyMap::KeyEntryList entryList;
    KeyMap::KeyItemList itemList1;
    KeyMap::KeyItem item1;
    item1.m_required = KeyModifierAlt;
    item1.m_sensitive = KeyModifierShift | KeyModifierAlt;
    KeyMap::KeyItemList itemList2;
    KeyMap::KeyItem item2;
    item2.m_required = KeyModifierShift;
    item2.m_sensitive = KeyModifierShift | KeyModifierAlt;
    KeyModifierMask currentState = KeyModifierShift;
    KeyModifierMask desiredState = KeyModifierShift;
    itemList1.push_back(item1);
    itemList2.push_back(item2);
    entryList.push_back(itemList1);
    entryList.push_back(itemList2);

    EXPECT_EQ(1, keyMap.findBestKey(entryList, currentState, desiredState));
}

TEST(KeyMapTests, findBestKey_extraSensitiveDown_matchExactSecondItem)
{
    KeyMap keyMap;
    KeyMap::KeyEntryList entryList;
    KeyMap::KeyItemList itemList1;
    KeyMap::KeyItem item1;
    item1.m_required = 0;
    item1.m_sensitive = KeyModifierAlt;
    KeyMap::KeyItemList itemList2;
    KeyMap::KeyItem item2;
    item2.m_required = 0;
    item2.m_sensitive = KeyModifierShift;
    KeyModifierMask currentState = KeyModifierAlt;
    KeyModifierMask desiredState = KeyModifierAlt;
    itemList1.push_back(item1);
    itemList2.push_back(item2);
    entryList.push_back(itemList1);
    entryList.push_back(itemList2);

    EXPECT_EQ(1, keyMap.findBestKey(entryList, currentState, desiredState));
}

TEST(KeyMapTests, findBestKey_noRequiredDown_matchOneRequiredChangeItem)
{
    KeyMap keyMap;
    KeyMap::KeyEntryList entryList;
    KeyMap::KeyItemList itemList1;
    KeyMap::KeyItem item1;
    item1.m_required = KeyModifierShift | KeyModifierAlt;
    item1.m_sensitive = KeyModifierShift | KeyModifierAlt;
    KeyMap::KeyItemList itemList2;
    KeyMap::KeyItem item2;
    item2.m_required = KeyModifierShift;
    item2.m_sensitive = KeyModifierShift | KeyModifierAlt;
    KeyModifierMask currentState = 0;
    KeyModifierMask desiredState = 0;
    itemList1.push_back(item1);
    itemList2.push_back(item2);
    entryList.push_back(itemList1);
    entryList.push_back(itemList2);

    EXPECT_EQ(1, keyMap.findBestKey(entryList, currentState, desiredState));
}

TEST(KeyMapTests, findBestKey_onlyOneRequiredDown_matchTwoRequiredChangesItem)
{
    KeyMap keyMap;
    KeyMap::KeyEntryList entryList;
    KeyMap::KeyItemList itemList1;
    KeyMap::KeyItem item1;
    item1.m_required = KeyModifierShift | KeyModifierAlt | KeyModifierControl;
    item1.m_sensitive = KeyModifierShift | KeyModifierAlt | KeyModifierControl;
    KeyMap::KeyItemList itemList2;
    KeyMap::KeyItem item2;
    item2.m_required = KeyModifierShift| KeyModifierAlt;
    item2.m_sensitive = KeyModifierShift | KeyModifierAlt | KeyModifierControl;
    KeyModifierMask currentState = 0;
    KeyModifierMask desiredState = 0;
    itemList1.push_back(item1);
    itemList2.push_back(item2);
    entryList.push_back(itemList1);
    entryList.push_back(itemList2);

    EXPECT_EQ(1, keyMap.findBestKey(entryList, currentState, desiredState));
}

TEST(KeyMapTests, findBestKey_noRequiredDown_cannotMatch)
{
    KeyMap keyMap;
    KeyMap::KeyEntryList entryList;
    KeyMap::KeyItemList itemList;
    KeyMap::KeyItem item;
    item.m_required = 0xffffffff;
    item.m_sensitive = 0xffffffff;
    KeyModifierMask currentState = 0;
    KeyModifierMask desiredState = 0;
    itemList.push_back(item);
    entryList.push_back(itemList);

    EXPECT_EQ(-1, keyMap.findBestKey(entryList, currentState, desiredState));
}

TEST(KeyMapTests, isCommand_shiftMask_returnFalse)
{
    KeyMap keyMap;
    KeyModifierMask mask= KeyModifierShift;

    EXPECT_FALSE(keyMap.isCommand(mask));
}

TEST(KeyMapTests, isCommand_controlMask_returnTrue)
{
    KeyMap keyMap;
    KeyModifierMask mask= KeyModifierControl;

    EXPECT_EQ(true, keyMap.isCommand(mask));
}

TEST(KeyMapTests, isCommand_alternateMask_returnTrue)
{
    KeyMap keyMap;
    KeyModifierMask mask= KeyModifierAlt;

    EXPECT_EQ(true, keyMap.isCommand(mask));
}

TEST(KeyMapTests, isCommand_alternateGraphicMask_returnTrue)
{
    KeyMap keyMap;
    KeyModifierMask mask= KeyModifierAltGr;

    EXPECT_EQ(true, keyMap.isCommand(mask));
}

TEST(KeyMapTests, isCommand_metaMask_returnTrue)
{
    KeyMap keyMap;
    KeyModifierMask mask= KeyModifierMeta;

    EXPECT_EQ(true, keyMap.isCommand(mask));
}

TEST(KeyMapTests, isCommand_superMask_returnTrue)
{
    KeyMap keyMap;
    KeyModifierMask mask= KeyModifierSuper;

    EXPECT_EQ(true, keyMap.isCommand(mask));
}

// CapsLock on button 1, the 'a' key on button 3 laid out like the Windows
// client's keymap, space on button 4.
static const KeyButton kCapsButton = 1;

static KeyMap::KeyItem addTestKey(KeyMap& keyMap, KeyID id, KeyButton button,
                                  KeyModifierMask required, KeyModifierMask sensitive)
{
    KeyMap::KeyItem item{};
    item.m_id = id;
    item.m_button = button;
    item.m_required = required;
    item.m_sensitive = sensitive;
    KeyMap::initModifierKey(item);
    keyMap.addKeyEntry(item);
    return item;
}

static bool pressesCapsLock(const KeyMap::Keystrokes& keys)
{
    return std::any_of(keys.begin(), keys.end(), [](const KeyMap::Keystroke& key) {
        return key.m_type == KeyMap::Keystroke::kButton &&
               key.m_data.m_button.m_button == kCapsButton;
    });
}

static KeyMap::KeyItem buildCapsTestMap(KeyMap& keyMap)
{
    const KeyModifierMask letter = KeyModifierShift | KeyModifierCapsLock;
    const KeyMap::KeyItem caps = addTestKey(keyMap, kKeyCapsLock, kCapsButton, 0, 0);
    addTestKey(keyMap, kKeyShift_L, 2, 0, 0);
    addTestKey(keyMap, 'a', 3, 0, letter);
    addTestKey(keyMap, 'A', 3, KeyModifierShift, letter);
    addTestKey(keyMap, 'A', 3, KeyModifierCapsLock, letter);
    addTestKey(keyMap, 'a', 3, letter, letter);
    addTestKey(keyMap, ' ', 4, 0, 0);
    keyMap.finish();
    return caps;
}

TEST(KeyMapTests, mapKey_ownCapsLock_clientKeepsItsOwnCapsLock)
{
    KeyMap keyMap;
    const KeyMap::KeyItem caps = buildCapsTestMap(keyMap);
    keyMap.setOwnCapsLock(true);

    // local CapsLock on, server's off: type 'a' without touching CapsLock
    KeyMap::Keystrokes keys;
    KeyMap::ModifierToKeys active{{KeyModifierCapsLock, caps}};
    KeyModifierMask state = KeyModifierCapsLock;
    ASSERT_NE(nullptr, keyMap.mapKey(keys, 'a', 0, active, state, 0, false));
    EXPECT_FALSE(pressesCapsLock(keys));
    EXPECT_EQ(KeyModifierCapsLock, state);

    // server's CapsLock on, local off: a non-letter doesn't flash CapsLock
    keys.clear();
    active.clear();
    state = 0;
    ASSERT_NE(nullptr, keyMap.mapKey(keys, ' ', 0, active, state,
                                     KeyModifierCapsLock, false));
    EXPECT_FALSE(pressesCapsLock(keys));
    EXPECT_EQ(0u, state);

    // the CapsLock key itself still toggles it, and it stays toggled
    keys.clear();
    ASSERT_NE(nullptr, keyMap.mapKey(keys, kKeyCapsLock, 0, active, state, 0, false));
    EXPECT_TRUE(pressesCapsLock(keys));
    EXPECT_EQ(KeyModifierCapsLock, state);
}

TEST(KeyMapTests, mapKey_defaultCapsLock_followsServerCapsLock)
{
    KeyMap keyMap;
    buildCapsTestMap(keyMap);

    // other clients still toggle CapsLock to match the server's
    KeyMap::Keystrokes keys;
    KeyMap::ModifierToKeys active;
    KeyModifierMask state = 0;
    ASSERT_NE(nullptr, keyMap.mapKey(keys, ' ', 0, active, state,
                                     KeyModifierCapsLock, false));
    EXPECT_TRUE(pressesCapsLock(keys));
}

}
