/*
 * InputLeap -- mouse and keyboard sharing utility
 * Copyright (C) 2012-2016 Symless Ltd.
 * Copyright (C) 2004 Chris Schoeneman
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#pragma once

#include "platform/OSXClipboardAnyTextConverter.h"

namespace inputleap {

class OSXClipboardUTF8Converter : public OSXClipboardAnyTextConverter {
public:
    CFStringRef getOSXFormat() const override;

private:
    std::string doFromIClipboard(const std::string& data) const override;
    std::string doToIClipboard(const std::string& data) const override;
};

} // namespace inputleap
