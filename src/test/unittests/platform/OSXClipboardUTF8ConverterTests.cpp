/*
 * InputLeap -- mouse and keyboard sharing utility
 * Copyright (C) 2012-2016 Symless Ltd.
 * Copyright (C) 2011 Nick Bolton
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "platform/OSXClipboardUTF8Converter.h"

#include <gtest/gtest.h>

namespace inputleap {

TEST(OSXClipboardUTF8ConverterTests, format_isUtf8PlainText)
{
    OSXClipboardUTF8Converter converter;

    EXPECT_EQ(IClipboard::kText, converter.getFormat());
    EXPECT_TRUE(CFEqual(CFSTR("public.utf8-plain-text"), converter.getOSXFormat()));
}

TEST(OSXClipboardUTF8ConverterTests, conversion_preservesUtf8AndNormalizesLineEndings)
{
    OSXClipboardUTF8Converter converter;

    EXPECT_EQ("한글 test\r", converter.fromIClipboard("한글 test\n"));
    EXPECT_EQ("한글 test\n", converter.toIClipboard("한글 test\r"));
}

} // namespace inputleap
