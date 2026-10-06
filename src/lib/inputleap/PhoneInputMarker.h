/*
 * InputLeap -- mouse and keyboard sharing utility
 * Copyright (C) 2026 InputLeafPlus Developers
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#pragma once

namespace inputleap {

// Tags input injected by the phone trackpad (GUI) so the server can tell it from hardware:
// macOS: kCGEventSourceUserData of injected mouse-move events, Windows: dwExtraInfo.
constexpr long long kPhoneInjectMarker = 0x504E4F53;

} // namespace inputleap
