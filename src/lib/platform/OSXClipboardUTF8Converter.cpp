/*
 * InputLeap -- mouse and keyboard sharing utility
 * Copyright (C) 2012-2016 Symless Ltd.
 * Copyright (C) 2004 Chris Schoeneman
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "platform/OSXClipboardUTF8Converter.h"

namespace inputleap {

CFStringRef OSXClipboardUTF8Converter::getOSXFormat() const
{
    return CFSTR("public.utf8-plain-text");
}

std::string OSXClipboardUTF8Converter::doFromIClipboard(const std::string& data) const
{
    return data;
}

std::string OSXClipboardUTF8Converter::doToIClipboard(const std::string& data) const
{
    return data;
}

} // namespace inputleap
