/*
 * InputLeap -- mouse and keyboard sharing utility
 * Copyright (C) 2026 InputLeap Authors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "server/Config.h"

#include "inputleap/key_types.h"

#include <gtest/gtest.h>

#include <sstream>

namespace inputleap {

TEST(ConfigTests, keyMappings_areParsedAppliedAndSerialized)
{
    std::istringstream input(
        "section: screens\n"
        "\tserver:\n"
        "\tmac:\n"
        "\t\tkeyMap = CapsLock F18\n"
        "\t\tkeyMap = NumLock F19\n"
        "\t\tkeyMap = Alt_L F20\n"
        "\t\tkeyMap = : ;\n"
        "\t\tkeyMap = a b\n"
        "end\n");
    Config config;
    input >> config;

    EXPECT_EQ(kKeyF18, config.mapKey("mac", kKeyCapsLock));
    EXPECT_EQ(kKeyF19, config.mapKey("mac", kKeyNumLock));
    EXPECT_EQ(static_cast<KeyID>(';'), config.mapKey("mac", static_cast<KeyID>(':')));
    EXPECT_EQ(static_cast<KeyID>('b'), config.mapKey("mac", static_cast<KeyID>('a')));
    EXPECT_EQ(static_cast<KeyID>('a'), config.mapKey("server", static_cast<KeyID>('a')));

    const KeyModifierMask mask = KeyModifierShift | KeyModifierAlt |
                                 KeyModifierCapsLock | KeyModifierNumLock |
                                 KeyModifierScrollLock;
    EXPECT_EQ(KeyModifierShift | KeyModifierScrollLock,
              config.mapModifierMask("mac", mask));
    EXPECT_EQ(mask, config.mapModifierMask("server", mask));

    std::ostringstream output;
    output << config;
    std::istringstream serialized(output.str());
    Config reparsed;
    serialized >> reparsed;
    EXPECT_EQ(kKeyF18, reparsed.mapKey("mac", kKeyCapsLock));
    EXPECT_EQ(kKeyF19, reparsed.mapKey("mac", kKeyNumLock));
    EXPECT_EQ(static_cast<KeyID>(';'), reparsed.mapKey("mac", static_cast<KeyID>(':')));
    EXPECT_EQ(static_cast<KeyID>('b'), reparsed.mapKey("mac", static_cast<KeyID>('a')));
    EXPECT_EQ(KeyModifierShift | KeyModifierScrollLock,
              reparsed.mapModifierMask("mac", mask));
}

TEST(ConfigTests, keyMappings_rejectModifiedKeys)
{
    std::istringstream input(
        "section: screens\n"
        "\tmac:\n"
        "\t\tkeyMap = Shift+a b\n"
        "end\n");
    Config config;

    EXPECT_THROW(input >> config, XConfigRead);
}

} // namespace inputleap
