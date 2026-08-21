/*
 * InputLeap -- mouse and keyboard sharing utility
 * Copyright (C) 2012-2016 Symless Ltd.
 * Copyright (C) 2004 Chris Schoeneman
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

#include "platform/OSXScreen.h"

#include "base/EventQueue.h"
#include "base/EventQueueTimer.h"
#include "client/Client.h"
#include "platform/OSXClipboard.h"
#include "platform/OSXEventQueueBuffer.h"
#include "platform/OSXKeyState.h"
#include "platform/OSXScreenSaver.h"
#include "platform/OSXDragSimulator.h"
#include "platform/OSXMediaKeySupport.h"
#include "platform/OSXPasteboardPeeker.h"
#include "inputleap/Clipboard.h"
#include "inputleap/KeyMap.h"
#include "inputleap/ClientApp.h"
#include "mt/Thread.h"
#include "arch/XArch.h"
#include "base/Log.h"
#include "base/IEventQueue.h"
#include "base/Time.h"

#include <math.h>
#include <mach-o/dyld.h>
#include <AvailabilityMacros.h>
#include <IOKit/hidsystem/event_status_driver.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <IOKit/hid/IOHIDManager.h>
#include <AppKit/NSEvent.h>

#include <deque>
#include <limits>
#include <mach/mach_time.h>
#include <set>
#include <sstream>
#include <tuple>

namespace inputleap {

// This isn't in any Apple SDK that I know of as of yet.
constexpr int INPUT_LEAP_EVENT_MOUSE_SCROLL = 11;
constexpr int INPUT_LEAP_MOUSE_SCROLL_AXIS_X = 'saxx';
constexpr int INPUT_LEAP_MOUSE_SCROLL_AXIS_Y = 'saxy';

enum {
	kCarbonLoopWaitTimeout = 10
};

// TODO: upgrade deprecated function usage in these functions.
void setZeroSuppressionInterval();
void avoidSupression();
void logCursorVisibility();
void avoidHesitatingCursor();

namespace {

constexpr std::uint64_t kHidEventMatchToleranceNs = 5'000'000;

long hidDeviceNumber(IOHIDDeviceRef device, CFStringRef key)
{
	CFTypeRef value = IOHIDDeviceGetProperty(device, key);
	if (value == nullptr || CFGetTypeID(value) != CFNumberGetTypeID()) {
		return 0;
	}

	long result = 0;
	CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberLongType, &result);
	return result;
}

void setHidMatchingNumber(CFMutableDictionaryRef matching, CFStringRef key, long value)
{
	CFNumberRef number = CFNumberCreate(kCFAllocatorDefault, kCFNumberLongType, &value);
	CFDictionarySetValue(matching, key, number);
	CFRelease(number);
}

std::uint64_t absoluteTimeToNanoseconds(std::uint64_t value)
{
	static mach_timebase_info_data_t timebase = [] {
		mach_timebase_info_data_t result{};
		mach_timebase_info(&result);
		return result;
	}();
	return value * timebase.numer / timebase.denom;
}

CFMutableDictionaryRef hidDeviceMatchingDictionary(long vendor, long product, long location)
{
	CFMutableDictionaryRef matching = CFDictionaryCreateMutable(
		kCFAllocatorDefault, 3, &kCFTypeDictionaryKeyCallBacks,
		&kCFTypeDictionaryValueCallBacks);
	if (matching == nullptr) {
		return nullptr;
	}
	setHidMatchingNumber(matching, CFSTR(kIOHIDVendorIDKey), vendor);
	setHidMatchingNumber(matching, CFSTR(kIOHIDProductIDKey), product);
	setHidMatchingNumber(matching, CFSTR(kIOHIDLocationIDKey), location);
	return matching;
}

} // namespace

struct OSXHidEventMatch
{
	bool local = false;
};

class OSXInputDeviceMonitor
{
public:
	explicit OSXInputDeviceMonitor(const std::string& localDevices) :
		manager_(nullptr)
	{
		std::istringstream deviceList(localDevices);
		std::string entry;
		CFMutableArrayRef matchingArray = CFArrayCreateMutable(
			kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);
		while (std::getline(deviceList, entry, ',')) {
			long vendor = 0;
			long product = 0;
			long location = 0;
			char separator1 = 0;
			char separator2 = 0;
			std::istringstream id(entry);
			if (!(id >> vendor >> separator1 >> product >> separator2 >> location) ||
				separator1 != ':' || separator2 != ':') {
				continue;
			}
			devices_.insert({vendor, product, location});

			CFMutableDictionaryRef matching = hidDeviceMatchingDictionary(vendor, product, location);
			if (matching != nullptr) {
				CFArrayAppendValue(matchingArray, matching);
				CFRelease(matching);
			}
		}
		if (devices_.empty()) {
			CFRelease(matchingArray);
			return;
		}

		manager_ = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
		if (manager_ == nullptr) {
			CFRelease(matchingArray);
			return;
		}
		IOHIDManagerSetDeviceMatchingMultiple(manager_, matchingArray);
		CFRelease(matchingArray);
		IOHIDManagerRegisterInputValueCallback(manager_, inputValueCallback, this);
		IOHIDManagerScheduleWithRunLoop(manager_, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
		const IOReturn openResult = IOHIDManagerOpen(manager_, kIOHIDOptionsTypeNone);
		if (openResult != kIOReturnSuccess) {
			LOG_WARN("failed to monitor selected macOS input devices (0x%08x)",
				static_cast<unsigned int>(openResult));
			return;
		}
		LOG_INFO("keeping input from %zu local-only device(s) on this mac", devices_.size());
	}

	~OSXInputDeviceMonitor()
	{
		if (manager_ != nullptr) {
			IOHIDManagerRegisterInputValueCallback(manager_, nullptr, nullptr);
			IOHIDManagerUnscheduleFromRunLoop(manager_, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
			IOHIDManagerClose(manager_, kIOHIDOptionsTypeNone);
			CFRelease(manager_);
		}
	}

	// keyboardPage: true for keyDown/keyUp/flagsChanged CGEvents (correlate
	// against keyboard-page HID reports only); false for NX_SYSDEFINED media
	// events (correlate against Consumer-page/DND reports only). Either way,
	// a vendor-specific (macro) report from the excluded device always
	// matches, since it isn't on a standard page to classify by.
	OSXHidEventMatch match(CGEventRef event, bool keyboardPage)
	{
		OSXHidEventMatch result;
		const std::uint64_t timestamp = CGEventGetTimestamp(event);
		while (!events_.empty() && events_.front().timestamp + kHidEventMatchToleranceNs < timestamp) {
			events_.pop_front();
		}

		std::uint64_t closestDifference = std::numeric_limits<std::uint64_t>::max();
		for (const auto& input : events_) {
			// A keyboard-page CGEvent (letter/F-key/modifier presses) may
			// only match a keyboard-page (or vendor-specific macro) HID
			// report, never a Consumer-page one, and vice versa. Without
			// this, an unrelated Consumer-page report from the excluded
			// device (e.g. a gaming keyboard's periodic status chatter)
			// could coincidentally land within the timing window and get
			// misattributed to an ordinary keypress from a different,
			// non-excluded keyboard, silently swallowing it.
			if (!input.vendorSpecific && input.keyboardPage != keyboardPage) {
				continue;
			}
			const std::uint64_t difference = input.timestamp > timestamp
				? input.timestamp - timestamp : timestamp - input.timestamp;
			if (difference <= kHidEventMatchToleranceNs && difference < closestDifference) {
				closestDifference = difference;
				result.local = input.local;
			}
		}
		LOG_DEBUG1("hid match: buffered=%zu keyboardPage=%d -> local=%d",
			events_.size(), keyboardPage ? 1 : 0, result.local ? 1 : 0);
		return result;
	}

private:
	struct InputEvent
	{
		std::uint64_t timestamp;
		bool local;
		bool keyboardPage;
		bool vendorSpecific;
	};

	static void inputValueCallback(void* context, IOReturn, void*, IOHIDValueRef value)
	{
		static_cast<OSXInputDeviceMonitor*>(context)->handleInputValue(value);
	}

	void handleInputValue(IOHIDValueRef value)
	{
		IOHIDElementRef element = IOHIDValueGetElement(value);
		const std::uint32_t usagePage = IOHIDElementGetUsagePage(element);
		const std::uint32_t usage = IOHIDElementGetUsage(element);
		IOHIDDeviceRef device = IOHIDElementGetDevice(element);
		const long vendor = hidDeviceNumber(device, CFSTR(kIOHIDVendorIDKey));
		const long product = hidDeviceNumber(device, CFSTR(kIOHIDProductIDKey));
		const long location = hidDeviceNumber(device, CFSTR(kIOHIDLocationIDKey));
		const bool local = devices_.count({vendor, product, location}) != 0;
		const bool vendorSpecific = local && usagePage >= 0xff00;
		const bool relevantUsage = usagePage == kHIDPage_KeyboardOrKeypad ||
			usagePage == kHIDPage_Consumer ||
			(usagePage == kHIDPage_GenericDesktop && usage == kHIDUsage_GD_DoNotDisturb) ||
			vendorSpecific;
		if (!relevantUsage) {
			return;
		}

		LOG_DEBUG1("hid report: page=0x%x usage=0x%x local=%d",
			usagePage, usage, local ? 1 : 0);
		events_.push_back({absoluteTimeToNanoseconds(IOHIDValueGetTimeStamp(value)),
			local, usagePage == kHIDPage_KeyboardOrKeypad, vendorSpecific});
		if (events_.size() > 128) {
			events_.pop_front();
		}
	}

	IOHIDManagerRef manager_;
	std::set<std::tuple<long, long, long>> devices_;
	std::deque<InputEvent> events_;
};

//
// OSXScreen
//

bool					OSXScreen::s_testedForGHOM = false;
bool					OSXScreen::s_hasGHOM	    = false;

OSXScreen::OSXScreen(IEventQueue* events, bool isPrimary, bool autoShowHideCursor,
					 bool mapMacFunctionKeys, const std::string& localInputDevice) :
	m_isPrimary(isPrimary),
	m_isOnScreen(m_isPrimary),
	m_cursorPosValid(false),
	MouseButtonEventMap(NumButtonIDs),
	m_cursorHidden(false),
	m_dragNumButtonsDown(0),
    m_dragTimer(nullptr),
    m_keyState(nullptr),
	m_inputDeviceMonitor(nullptr),
	m_mapMacFunctionKeys(mapMacFunctionKeys),
	m_sequenceNumber(0),
    m_screensaver(nullptr),
	m_screensaverNotify(false),
	m_ownClipboard(false),
    m_clipboardTimer(nullptr),
    m_hiddenWindow(nullptr),
    m_userInputWindow(nullptr),
	m_switchEventHandlerRef(0),
    m_pmWatchThread(nullptr),
	m_pmRootPort(0),
	m_activeModifierHotKey(0),
	m_activeModifierHotKeyMask(0),
	m_eventTapPort(nullptr),
	m_eventTapRLSR(nullptr),
	m_lastClickTime(0),
	m_clickState(1),
	m_lastSingleClickXCursor(0),
	m_lastSingleClickYCursor(0),
	m_autoShowHideCursor(autoShowHideCursor),
	m_events(events),
    m_getDropTargetThread(nullptr),
    m_impl(nullptr)
{
	try {
		m_displayID   = CGMainDisplayID();
		updateScreenShape(m_displayID, 0);
        m_screensaver = new OSXScreenSaver(m_events, get_event_target());
		m_keyState	  = new OSXKeyState(m_events);
		if (m_isPrimary && !localInputDevice.empty()) {
			m_inputDeviceMonitor = new OSXInputDeviceMonitor(localInputDevice);
		}

		// only needed when running as a server.
		if (m_isPrimary) {

#if defined(MAC_OS_X_VERSION_10_9)
			// we can't pass options to show the dialog, this must be done by the gui.
			if (!AXIsProcessTrusted()) {
                throw std::runtime_error("assistive devices does not trust this process, allow it in system settings.");
			}
#else
			// now deprecated in mavericks.
			if (!AXAPIEnabled()) {
                throw std::runtime_error("assistive devices is not enabled, enable it in system settings.");
			}
#endif
		}

		// install display manager notification handler
		CGDisplayRegisterReconfigurationCallback(displayReconfigurationCallback, this);

		// install fast user switching event handler
		EventTypeSpec switchEventTypes[2];
		switchEventTypes[0].eventClass = kEventClassSystem;
		switchEventTypes[0].eventKind  = kEventSystemUserSessionDeactivated;
		switchEventTypes[1].eventClass = kEventClassSystem;
		switchEventTypes[1].eventKind  = kEventSystemUserSessionActivated;
		EventHandlerUPP switchEventHandler =
			NewEventHandlerUPP(userSwitchCallback);
		InstallApplicationEventHandler(switchEventHandler, 2, switchEventTypes,
									   this, &m_switchEventHandlerRef);
		DisposeEventHandlerUPP(switchEventHandler);

		constructMouseButtonEventMap();

		// watch for requests to sleep
        m_events->add_handler(EventType::OSX_SCREEN_CONFIRM_SLEEP, get_event_target(),
                              [this](const auto& e){ handle_confirm_sleep(e); });

		// create thread for monitoring system power state.
        is_pm_thread_ready_ = false;
		LOG_DEBUG("starting watchSystemPowerThread");
        m_pmWatchThread = new Thread([this](){ watchSystemPowerThread(); });
	}
	catch (...) {
        m_events->remove_handler(EventType::OSX_SCREEN_CONFIRM_SLEEP, get_event_target());
		if (m_switchEventHandlerRef != 0) {
			RemoveEventHandler(m_switchEventHandlerRef);
		}

		CGDisplayRemoveReconfigurationCallback(displayReconfigurationCallback, this);

		delete m_inputDeviceMonitor;
		delete m_keyState;
		delete m_screensaver;
		throw;
	}

	// install event handlers
	m_events->add_handler(EventType::SYSTEM, m_events->getSystemTarget(),
                          [this](const auto& e){ handle_system_event(e); });

	// install the platform event queue
    m_events->set_buffer(std::make_unique<OSXEventQueueBuffer>(m_events));
}

OSXScreen::~OSXScreen()
{
	disable();
    m_events->set_buffer(nullptr);
    m_events->remove_handler(EventType::SYSTEM, m_events->getSystemTarget());

	if (m_pmWatchThread) {
		// make sure the thread has setup the runloop.
		{
            std::unique_lock<std::mutex> lock(pm_mutex_);
            pm_thread_ready_cv_.wait(lock, [this](){ return is_pm_thread_ready_; });
		}

		// now exit the thread's runloop and wait for it to exit
		LOG_DEBUG("stopping watchSystemPowerThread");
		CFRunLoopStop(m_pmRunloop);
		m_pmWatchThread->wait();
		delete m_pmWatchThread;
        m_pmWatchThread = nullptr;
	}

    m_events->remove_handler(EventType::OSX_SCREEN_CONFIRM_SLEEP,
                                get_event_target());

	RemoveEventHandler(m_switchEventHandlerRef);

	CGDisplayRemoveReconfigurationCallback(displayReconfigurationCallback, this);

	delete m_inputDeviceMonitor;
	delete m_keyState;
	delete m_screensaver;
}

const EventTarget* OSXScreen::get_event_target() const
{
    return this;
}

bool
OSXScreen::getClipboard(ClipboardID, IClipboard* dst) const
{
	Clipboard::copy(dst, &m_pasteboard);
	return true;
}

void OSXScreen::getShape(std::int32_t& x, std::int32_t& y, std::int32_t& w, std::int32_t& h) const
{
	x = m_x;
	y = m_y;
	w = m_w;
	h = m_h;
}

void OSXScreen::getCursorPos(std::int32_t& x, std::int32_t& y) const
{
    CGEventRef event = CGEventCreate(nullptr);
	CGPoint mouse = CGEventGetLocation(event);
	x                = mouse.x;
	y                = mouse.y;
	m_cursorPosValid = true;
	m_xCursor        = x;
	m_yCursor        = y;
	CFRelease(event);
}

void OSXScreen::reconfigure(std::uint32_t)
{
	// do nothing
}

void OSXScreen::warpCursor(std::int32_t x, std::int32_t y)
{
	// move cursor without generating events
	CGPoint pos;
	pos.x = x;
	pos.y = y;
	CGWarpMouseCursorPosition(pos);

	// save new cursor position
	m_xCursor        = x;
	m_yCursor        = y;
	m_cursorPosValid = true;
}

void
OSXScreen::fakeInputBegin()
{
	// FIXME -- not implemented
}

void
OSXScreen::fakeInputEnd()
{
	// FIXME -- not implemented
}

std::int32_t OSXScreen::getJumpZoneSize() const
{
	return 1;
}

bool OSXScreen::isAnyMouseButtonDown(std::uint32_t& buttonID) const
{
	if (m_buttonState.test(0)) {
		buttonID = kButtonLeft;
		return true;
	}

	return (GetCurrentButtonState() != 0);
}

void OSXScreen::getCursorCenter(std::int32_t& x, std::int32_t& y) const
{
	x = m_xCenter;
	y = m_yCenter;
}

std::uint32_t OSXScreen::registerHotKey(KeyID key, KeyModifierMask mask)
{
    // get mac virtual key and modifier mask matching InputLeap key and mask
	std::uint32_t macKey, macMask;
    if (!m_keyState->map_hot_key_to_mac(key, mask, macKey, macMask)) {
		LOG_DEBUG("could not map hotkey id=%04x mask=%04x", key, mask);
		return 0;
	}

	// choose hotkey id
	std::uint32_t id;
	if (!m_oldHotKeyIDs.empty()) {
		id = m_oldHotKeyIDs.back();
		m_oldHotKeyIDs.pop_back();
	}
	else {
		id = m_hotKeys.size() + 1;
	}

	// if this hot key has modifiers only then we'll handle it specially
    EventHotKeyRef ref = nullptr;
	bool okay;
	if (key == kKeyNone) {
		if (m_modifierHotKeys.count(mask) > 0) {
			// already registered
			okay = false;
		}
		else {
			m_modifierHotKeys[mask] = id;
			okay = true;
		}
	}
	else {
		EventHotKeyID hkid = { 'SNRG', (std::uint32_t)id };
		OSStatus status = RegisterEventHotKey(macKey, macMask, hkid,
								GetApplicationEventTarget(), 0,
								&ref);
		okay = (status == noErr);
		m_hotKeyToIDMap[HotKeyItem(macKey, macMask)] = id;
	}

	if (!okay) {
		m_oldHotKeyIDs.push_back(id);
		m_hotKeyToIDMap.erase(HotKeyItem(macKey, macMask));
		LOG_WARN("failed to register hotkey %s (id=%04x mask=%04x)", inputleap::KeyMap::formatKey(key, mask).c_str(), key, mask);
		return 0;
	}

	m_hotKeys.insert(std::make_pair(id, HotKeyItem(ref, macKey, macMask)));

	LOG_DEBUG("registered hotkey %s (id=%04x mask=%04x) as id=%d", inputleap::KeyMap::formatKey(key, mask).c_str(), key, mask, id);
	return id;
}

void OSXScreen::unregisterHotKey(std::uint32_t id)
{
	// look up hotkey
        auto i = m_hotKeys.find(id);
	if (i == m_hotKeys.end()) {
		return;
	}

	// unregister with OS
	bool okay;
    if (i->second.getRef() != nullptr) {
		okay = (UnregisterEventHotKey(i->second.getRef()) == noErr);
	}
	else {
		okay = false;
		// XXX -- this is inefficient
                for (auto j = m_modifierHotKeys.begin(); j != m_modifierHotKeys.end(); ++j) {
			if (j->second == id) {
				m_modifierHotKeys.erase(j);
				okay = true;
				break;
			}
		}
	}
	if (!okay) {
		LOG_WARN("failed to unregister hotkey id=%d", id);
	}
	else {
		LOG_DEBUG("unregistered hotkey id=%d", id);
	}

	// discard hot key from map and record old id for reuse
	m_hotKeyToIDMap.erase(i->second);
	m_hotKeys.erase(i);
	m_oldHotKeyIDs.push_back(id);
	if (m_activeModifierHotKey == id) {
		m_activeModifierHotKey     = 0;
		m_activeModifierHotKeyMask = 0;
	}
}

void
OSXScreen::constructMouseButtonEventMap()
{
	const CGEventType source[NumButtonIDs][3] = {
		{kCGEventLeftMouseUp, kCGEventLeftMouseDragged, kCGEventLeftMouseDown},
		{kCGEventRightMouseUp, kCGEventRightMouseDragged, kCGEventRightMouseDown},
		{kCGEventOtherMouseUp, kCGEventOtherMouseDragged, kCGEventOtherMouseDown},
		{kCGEventOtherMouseUp, kCGEventOtherMouseDragged, kCGEventOtherMouseDown},
		{kCGEventOtherMouseUp, kCGEventOtherMouseDragged, kCGEventOtherMouseDown},
		{kCGEventOtherMouseUp, kCGEventOtherMouseDragged, kCGEventOtherMouseDown}
	};

	for (std::uint16_t button = 0; button < NumButtonIDs; button++) {
		MouseButtonEventMapType new_map;
		for (std::uint16_t state = (std::uint32_t) kMouseButtonUp; state < kMouseButtonStateMax; state++) {
			CGEventType curEvent = source[button][state];
			new_map[state] = curEvent;
		}
		MouseButtonEventMap[button] = new_map;
	}
}

void
OSXScreen::postMouseEvent(CGPoint& pos) const
{
	// check if cursor position is valid on the client display configuration
	// stkamp@users.sourceforge.net
	CGDisplayCount displayCount = 0;
    CGGetDisplaysWithPoint(pos, 0, nullptr, &displayCount);
	if (displayCount == 0) {
		// cursor position invalid - clamp to bounds of last valid display.
		// find the last valid display using the last cursor position.
		displayCount = 0;
		CGDirectDisplayID displayID;
		CGGetDisplaysWithPoint(CGPointMake(m_xCursor, m_yCursor), 1,
								&displayID, &displayCount);
		if (displayCount != 0) {
			CGRect displayRect = CGDisplayBounds(displayID);
			if (pos.x < displayRect.origin.x) {
				pos.x = displayRect.origin.x;
			}
			else if (pos.x > displayRect.origin.x +
								displayRect.size.width - 1) {
				pos.x = displayRect.origin.x + displayRect.size.width - 1;
			}
			if (pos.y < displayRect.origin.y) {
				pos.y = displayRect.origin.y;
			}
			else if (pos.y > displayRect.origin.y +
								displayRect.size.height - 1) {
				pos.y = displayRect.origin.y + displayRect.size.height - 1;
			}
		}
	}

	CGEventType type = kCGEventMouseMoved;

	std::int8_t button = m_buttonState.getFirstButtonDown();
	if (button != -1) {
		MouseButtonEventMapType thisButtonType = MouseButtonEventMap[button];
		type = thisButtonType[kMouseButtonDragged];
	}

    CGEventRef event = CGEventCreateMouseEvent(nullptr, type, pos, static_cast<CGMouseButton>(button));

    // Dragging events also need the click state
    CGEventSetIntegerValueField(event, kCGMouseEventClickState, m_clickState);

    // Fix for sticky keys
    CGEventFlags modifiers = m_keyState->getModifierStateAsOSXFlags();
    CGEventSetFlags(event, modifiers);

    // Set movement deltas to fix issues with certain 3D programs
    SInt64 deltaX = pos.x;
    deltaX -= m_xCursor;

    SInt64 deltaY = pos.y;
    deltaY -= m_yCursor;

    CGEventSetIntegerValueField(event, kCGMouseEventDeltaX, deltaX);
    CGEventSetIntegerValueField(event, kCGMouseEventDeltaY, deltaY);

    double deltaFX = deltaX;
    double deltaFY = deltaY;

    CGEventSetDoubleValueField(event, kCGMouseEventDeltaX, deltaFX);
    CGEventSetDoubleValueField(event, kCGMouseEventDeltaY, deltaFY);

	CGEventPost(kCGHIDEventTap, event);

	CFRelease(event);
}

void
OSXScreen::fakeMouseButton(ButtonID id, bool press)
{
	// Buttons are indexed from one, but the button down array is indexed from zero
    std::uint32_t index = map_button_to_osx(id) - kButtonLeft;
	if (index >= NumButtonIDs) {
		return;
	}

	CGPoint pos;
	if (!m_cursorPosValid) {
        std::int32_t x, y;
		getCursorPos(x, y);
	}
	pos.x = m_xCursor;
	pos.y = m_yCursor;

	// variable used to detect mouse coordinate differences between
	// old & new mouse clicks. Used in double click detection.
    std::int32_t xDiff = m_xCursor - m_lastSingleClickXCursor;
    std::int32_t yDiff = m_yCursor - m_lastSingleClickYCursor;
	double diff = sqrt(xDiff * xDiff + yDiff * yDiff);
	// max sqrt(x^2 + y^2) difference allowed to double click
	// since we don't have double click distance in NX APIs
	// we define our own defaults.
	const double maxDiff = sqrt(2) + 0.0001;

    double clickTime = [NSEvent doubleClickInterval];

    // As long as the click is within the time window and distance window
    // increase clickState (double click, triple click, etc)
    // This will allow for higher than triple click but the quartz documentation
    // does not specify that this should be limited to triple click
    if (press) {
        if ((inputleap::current_time_seconds() - m_lastClickTime) <= clickTime && diff <= maxDiff) {
            m_clickState++;
        }
        else {
            m_clickState = 1;
        }

        m_lastClickTime = inputleap::current_time_seconds();
    }

    if (m_clickState == 1) {
        m_lastSingleClickXCursor = m_xCursor;
        m_lastSingleClickYCursor = m_yCursor;
    }

    EMouseButtonState state = press ? kMouseButtonDown : kMouseButtonUp;

    LOG_DEBUG1("faking mouse button id: %d press: %s", index, press ? "pressed" : "released");

    MouseButtonEventMapType thisButtonMap = MouseButtonEventMap[index];
    CGEventType type = thisButtonMap[state];

    CGEventRef event = CGEventCreateMouseEvent(nullptr, type, pos, static_cast<CGMouseButton>(index));

    CGEventSetIntegerValueField(event, kCGMouseEventClickState, m_clickState);

    // Fix for sticky keys
    CGEventFlags modifiers = m_keyState->getModifierStateAsOSXFlags();
    CGEventSetFlags(event, modifiers);

    m_buttonState.set(index, state);
    CGEventPost(kCGHIDEventTap, event);

    CFRelease(event);

	if (!press && (id == kButtonLeft)) {
		if (m_fakeDraggingStarted) {
            m_getDropTargetThread = new Thread([this](){ get_drop_target_thread(); });
		}

		m_draggingStarted = false;
	}
}

void OSXScreen::get_drop_target_thread()
{
#if defined(MAC_OS_X_VERSION_10_7)
    char* cstr = nullptr;

	// wait for 5 secs for the drop destinaiton string to be filled.
    std::uint32_t timeout = inputleap::current_time_seconds() + 5;

    while (inputleap::current_time_seconds() < timeout) {
		CFStringRef cfstr = getCocoaDropTarget();
		cstr = CFStringRefToUTF8String(cfstr);
		CFRelease(cfstr);

        if (cstr != nullptr) {
			break;
		}
		inputleap::this_thread_sleep(.1f);
	}

    if (cstr != nullptr) {
		LOG_DEBUG("drop target: %s", cstr);
		m_dropTarget = cstr;
	}
	else {
		LOG_ERR("failed to get drop target");
		m_dropTarget.clear();
	}
#else
	LOG_WARN("drag drop not supported");
#endif
	m_fakeDraggingStarted = false;
}

void OSXScreen::fakeMouseMove(std::int32_t x, std::int32_t y)
{
	if (m_fakeDraggingStarted) {
		m_buttonState.set(0, kMouseButtonDown);
	}

	// index 0 means left mouse button
	if (m_buttonState.test(0)) {
		m_draggingStarted = true;
	}

	// synthesize event
	CGPoint pos;
	pos.x = x;
	pos.y = y;
	postMouseEvent(pos);

	// save new cursor position
    m_xCursor = static_cast<std::int32_t>(pos.x);
    m_yCursor = static_cast<std::int32_t>(pos.y);
	m_cursorPosValid = true;
}

void OSXScreen::fakeMouseRelativeMove(std::int32_t dx, std::int32_t dy) const
{
	// OS X does not appear to have a fake relative mouse move function.
	// simulate it by getting the current mouse position and adding to
	// that.  this can yield the wrong answer but there's not much else
	// we can do.

	// get current position
    CGEventRef event = CGEventCreate(nullptr);
	CGPoint oldPos = CGEventGetLocation(event);
	CFRelease(event);

	// synthesize event
	CGPoint pos;
    m_xCursor = static_cast<std::int32_t>(oldPos.x);
    m_yCursor = static_cast<std::int32_t>(oldPos.y);
	pos.x     = oldPos.x + dx;
	pos.y     = oldPos.y + dy;
	postMouseEvent(pos);

	// we now assume we don't know the current cursor position
	m_cursorPosValid = false;
}

void OSXScreen::fakeMouseWheel(std::int32_t xDelta, std::int32_t yDelta) const
{
	if (xDelta != 0 || yDelta != 0) {
		// create a scroll event, post it and release it.  not sure if kCGScrollEventUnitLine
		// is the right choice here over kCGScrollEventUnitPixel
		CGEventRef scrollEvent = CGEventCreateScrollWheelEvent(
            nullptr, kCGScrollEventUnitLine, 2,
            map_scroll_wheel_to_osx(yDelta),
            -map_scroll_wheel_to_osx(xDelta));

        // Fix for sticky keys
        CGEventFlags modifiers = m_keyState->getModifierStateAsOSXFlags();
        CGEventSetFlags(scrollEvent, modifiers);

		CGEventPost(kCGHIDEventTap, scrollEvent);
		CFRelease(scrollEvent);
	}
}

void
OSXScreen::showCursor()
{
	LOG_DEBUG("showing cursor");

    CFStringRef propertyString = CFStringCreateWithCString(nullptr, "SetsCursorInBackground",
                                                           kCFStringEncodingMacRoman);

	CGSSetConnectionProperty(
		_CGSDefaultConnection(), _CGSDefaultConnection(),
		propertyString, kCFBooleanTrue);

	CFRelease(propertyString);

	CGError error = CGDisplayShowCursor(m_displayID);
	if (error != kCGErrorSuccess) {
		LOG_ERR("failed to show cursor, error=%d", error);
	}

	// appears to fix "mouse randomly not showing" bug
	CGAssociateMouseAndMouseCursorPosition(true);

	logCursorVisibility();

	m_cursorHidden = false;
}

void
OSXScreen::hideCursor()
{
	LOG_DEBUG("hiding cursor");

    CFStringRef propertyString = CFStringCreateWithCString(nullptr, "SetsCursorInBackground",
                                                           kCFStringEncodingMacRoman);

	CGSSetConnectionProperty(
		_CGSDefaultConnection(), _CGSDefaultConnection(),
		propertyString, kCFBooleanTrue);

	CFRelease(propertyString);

	CGError error = CGDisplayHideCursor(m_displayID);
	if (error != kCGErrorSuccess) {
		LOG_ERR("failed to hide cursor, error=%d", error);
	}

	// appears to fix "mouse randomly not hiding" bug
	CGAssociateMouseAndMouseCursorPosition(true);

	logCursorVisibility();

	m_cursorHidden = true;
}

void
OSXScreen::enable()
{
	// watch the clipboard
    m_clipboardTimer = m_events->newTimer(1.0, nullptr);
	m_events->add_handler(EventType::TIMER, m_clipboardTimer,
                          [this](const auto& e){ handle_clipboard_check(); });

	if (m_isPrimary) {
		// FIXME -- start watching jump zones

		// kCGEventTapOptionDefault = 0x00000000 (Missing in 10.4, so specified literally)
		m_eventTapPort = CGEventTapCreate(kCGHIDEventTap, kCGHeadInsertEventTap, kCGEventTapOptionDefault,
										kCGEventMaskForAllEvents,
										handleCGInputEvent,
										this);
	}
	else {
		// FIXME -- prevent system from entering power save mode

		if (m_autoShowHideCursor) {
			hideCursor();
		}

		// warp the mouse to the cursor center
		fakeMouseMove(m_xCenter, m_yCenter);

                // there may be a better way to do this, but we register an event handler even if we're
                // not on the primary display (acting as a client). This way, if a local event comes in
                // (either keyboard or mouse), we can make sure to show the cursor if we've hidden it.
		m_eventTapPort = CGEventTapCreate(kCGHIDEventTap, kCGHeadInsertEventTap, kCGEventTapOptionDefault,
										kCGEventMaskForAllEvents,
										handleCGInputEventSecondary,
										this);
	}

	if (!m_eventTapPort) {
		LOG_ERR("failed to create quartz event tap");
	}

	m_eventTapRLSR = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, m_eventTapPort, 0);
	if (!m_eventTapRLSR) {
		LOG_ERR("failed to create a CFRunLoopSourceRef for the quartz event tap");
	}

	CFRunLoopAddSource(CFRunLoopGetCurrent(), m_eventTapRLSR, kCFRunLoopDefaultMode);
}

void
OSXScreen::disable()
{
	if (m_autoShowHideCursor) {
		showCursor();
	}

	// FIXME -- stop watching jump zones, stop capturing input

	if (m_eventTapRLSR) {
		CFRunLoopRemoveSource(CFRunLoopGetCurrent(), m_eventTapRLSR, kCFRunLoopDefaultMode);
		CFRelease(m_eventTapRLSR);
		m_eventTapRLSR = nullptr;
	}

	if (m_eventTapPort) {
		CGEventTapEnable(m_eventTapPort, false);
		CFRelease(m_eventTapPort);
		m_eventTapPort = nullptr;
	}
	// FIXME -- allow system to enter power saving mode

	// disable drag handling
	m_dragNumButtonsDown = 0;
	enableDragTimer(false);

	// uninstall clipboard timer
    if (m_clipboardTimer != nullptr) {
        m_events->remove_handler(EventType::TIMER, m_clipboardTimer);
		m_events->deleteTimer(m_clipboardTimer);
        m_clipboardTimer = nullptr;
	}

	m_isOnScreen = m_isPrimary;
}

void
OSXScreen::enter()
{
    // Mark as on screen so other events are handled as on screen.
    // Mitigates https://github.com/input-leap/input-leap/issues/1043 from the bogus movement check
	m_isOnScreen = true;

	showCursor();

	if (m_isPrimary) {
		setZeroSuppressionInterval();
	}
	else {
		// reset buttons
		m_buttonState.reset();

		// patch by Yutaka Tsutano
		// wakes the client screen
		// http://symless.com/spit/issues/details/3287#c12
		io_registry_entry_t entry = IORegistryEntryFromPath(
			kIOMasterPortDefault,
			"IOService:/IOResources/IODisplayWrangler");

		if (entry != MACH_PORT_NULL) {
			IORegistryEntrySetCFProperty(entry, CFSTR("IORequestIdle"), kCFBooleanFalse);
			IOObjectRelease(entry);
		}

		// IOKit API for declaring a power management assertion
		IOPMAssertionDeclareUserActivity(CFSTR("Input Leap - entering screen"), kIOPMUserActiveLocal, &assertionID);

		avoidSupression();
	}
}

bool
OSXScreen::canLeave()
{
    return true;
}

void
OSXScreen::leave()
{
    hideCursor();

	if (isDraggingStarted()) {
        std::string& fileList = getDraggingFilename();

		if (!m_isPrimary) {
			if (fileList.empty() == false) {
				ClientApp& app = ClientApp::instance();
				Client* client = app.getClientPtr();

				DragInformation di;
				di.setFilename(fileList);
				DragFileList dragFileList;
				dragFileList.push_back(di);
                std::string info;
				std::uint32_t fileCount = DragInformation::setupDragInfo(dragFileList, info);
				client->sendDragInfo(fileCount, info, info.size());
				LOG_DEBUG("send dragging file to server");

				// TODO: what to do with multiple file or even
				// a folder
				client->sendFileToServer(fileList.c_str());
			}
		}
		m_draggingStarted = false;
	}

	if (m_isPrimary) {
		avoidHesitatingCursor();

	}

	// now off screen
	m_isOnScreen = false;
}

bool
OSXScreen::setClipboard(ClipboardID, const IClipboard* src)
{
    if (src != nullptr) {
		LOG_DEBUG("setting clipboard");
		return Clipboard::copy(&m_pasteboard, src);
	}
	return true;
}

void
OSXScreen::checkClipboards()
{
	LOG_DEBUG2("checking clipboard");
	if (m_pasteboard.synchronize()) {
		LOG_DEBUG("clipboard changed");
        sendClipboardEvent(EventType::CLIPBOARD_GRABBED, kClipboardClipboard);
        sendClipboardEvent(EventType::CLIPBOARD_GRABBED, kClipboardSelection);
	}
}

void
OSXScreen::openScreensaver(bool notify)
{
	m_screensaverNotify = notify;
	if (!m_screensaverNotify) {
		m_screensaver->disable();
	}
}

void
OSXScreen::closeScreensaver()
{
	if (!m_screensaverNotify) {
		m_screensaver->enable();
	}
}

void
OSXScreen::screensaver(bool activate)
{
	if (activate) {
		m_screensaver->activate();
	}
	else {
		m_screensaver->deactivate();
	}
}

void
OSXScreen::resetOptions()
{
	// no options
}

void
OSXScreen::setOptions(const OptionsList&)
{
	// no options
}

void OSXScreen::setSequenceNumber(std::uint32_t seqNum)
{
	m_sequenceNumber = seqNum;
}

bool
OSXScreen::isPrimary() const
{
	return m_isPrimary;
}

void OSXScreen::sendEvent(EventType type, EventDataBase* data) const
{
    m_events->add_event(type, get_event_target(), data);
}

void OSXScreen::sendClipboardEvent(EventType type, ClipboardID id) const
{
    ClipboardInfo info;
    info.m_id = id;
    info.m_sequenceNumber = m_sequenceNumber;
    sendEvent(type, create_event_data<ClipboardInfo>(info));
}

void OSXScreen::handle_system_event(const Event& event)
{
    EventRef* carbonEvent = event.get_data_as<EventRef*>();
    assert(carbonEvent != nullptr);

	std::uint32_t eventClass = GetEventClass(*carbonEvent);

	switch (eventClass) {
	case kEventClassMouse:
		switch (GetEventKind(*carbonEvent)) {
        case INPUT_LEAP_EVENT_MOUSE_SCROLL:
		{
			OSStatus r;
			long xScroll;
			long yScroll;

			// get scroll amount
            r = GetEventParameter(*carbonEvent, INPUT_LEAP_MOUSE_SCROLL_AXIS_X,
                                  typeSInt32, nullptr, sizeof(xScroll), nullptr, &xScroll);
			if (r != noErr) {
				xScroll = 0;
			}
            r = GetEventParameter(*carbonEvent, INPUT_LEAP_MOUSE_SCROLL_AXIS_Y,
                                  typeSInt32, nullptr, sizeof(yScroll), nullptr, &yScroll);
			if (r != noErr) {
				yScroll = 0;
			}

			if (xScroll != 0 || yScroll != 0) {
                onMouseWheel(-map_scroll_wheel_from_osx(xScroll),
                             map_scroll_wheel_from_osx(yScroll));
			}
		}
		}
		break;

	case kEventClassKeyboard:
			switch (GetEventKind(*carbonEvent)) {
				case kEventHotKeyPressed:
				case kEventHotKeyReleased:
					onHotKey(*carbonEvent);
					break;
			}

			break;

	case kEventClassWindow:
		// 2nd param was formerly GetWindowEventTarget(m_userInputWindow) which is 32-bit only,
		// however as m_userInputWindow is never initialized to anything we can take advantage of
        // the fact that GetWindowEventTarget(nullptr) == nullptr
        SendEventToEventTarget(*carbonEvent, nullptr);
		switch (GetEventKind(*carbonEvent)) {
		case kEventWindowActivated:
			LOG_DEBUG1("window activated");
			break;

		case kEventWindowDeactivated:
			LOG_DEBUG1("window deactivated");
			break;

		case kEventWindowFocusAcquired:
			LOG_DEBUG1("focus acquired");
			break;

		case kEventWindowFocusRelinquish:
			LOG_DEBUG1("focus released");
			break;
		}
		break;

	default:
		SendEventToEventTarget(*carbonEvent, GetEventDispatcherTarget());
		break;
	}
}

bool
OSXScreen::onMouseMove(CGFloat mx, CGFloat my)
{
	LOG_DEBUG2("mouse move %+f,%+f", mx, my);

	CGFloat x = mx - m_xCursor;
	CGFloat y = my - m_yCursor;

	if ((x == 0 && y == 0) || (mx == m_xCenter && mx == m_yCenter)) {
		return true;
	}

	// save position to compute delta of next motion
    m_xCursor = (std::int32_t)mx;
    m_yCursor = (std::int32_t)my;

	if (m_isOnScreen) {
		// motion on primary screen
        sendEvent(EventType::PRIMARY_SCREEN_MOTION_ON_PRIMARY,
                  create_event_data<MotionInfo>(MotionInfo{m_xCursor, m_yCursor}));
		if (m_buttonState.test(0)) {
			m_draggingStarted = true;
		}
	}
	else {
		// motion on secondary screen.  warp mouse back to
		// center.
		warpCursor(m_xCenter, m_yCenter);

		// examine the motion.  if it's about the distance
		// from the center of the screen to an edge then
		// it's probably a bogus motion that we want to
		// ignore (see warpCursorNoFlush() for a further
		// description).
        static std::int32_t bogusZoneSize = 10;
		if (-x + bogusZoneSize > m_xCenter - m_x ||
			 x + bogusZoneSize > m_x + m_w - m_xCenter ||
			-y + bogusZoneSize > m_yCenter - m_y ||
			 y + bogusZoneSize > m_y + m_h - m_yCenter) {
			LOG_DEBUG("dropped bogus motion %+.2f,%+.2f", x, y);
		}
		else {
			// send motion
			// Accumulate together the move into the running total
			static CGFloat m_xFractionalMove = 0;
			static CGFloat m_yFractionalMove = 0;

			m_xFractionalMove += x;
			m_yFractionalMove += y;

			// Return the integer part
            std::int32_t intX = (std::int32_t)m_xFractionalMove;
            std::int32_t intY = (std::int32_t)m_yFractionalMove;

			// And keep only the fractional part
			m_xFractionalMove -= intX;
			m_yFractionalMove -= intY;
            sendEvent(EventType::PRIMARY_SCREEN_MOTION_ON_SECONDARY,
                      create_event_data<MotionInfo>(MotionInfo{intX, intY}));
		}
	}

	return true;
}

bool OSXScreen::onMouseButton(bool pressed, std::uint16_t macButton)
{
	// Buttons 2 and 3 are inverted on the mac
    ButtonID button = map_button_from_osx(macButton);

	if (pressed) {
		LOG_DEBUG1("event: button press button=%d", button);
		if (button != kButtonNone) {
			KeyModifierMask mask = m_keyState->getActiveModifiers();
            sendEvent(EventType::PRIMARY_SCREEN_BUTTON_DOWN,
                      create_event_data<ButtonInfo>(ButtonInfo{button, mask}));
		}
	}
	else {
		LOG_DEBUG1("event: button release button=%d", button);
		if (button != kButtonNone) {
			KeyModifierMask mask = m_keyState->getActiveModifiers();
            sendEvent(EventType::PRIMARY_SCREEN_BUTTON_UP,
                      create_event_data<ButtonInfo>(ButtonInfo{button, mask}));
		}
	}

	// handle drags with any button other than button 1 or 2
	if (macButton > 2) {
		if (pressed) {
			// one more button
			if (m_dragNumButtonsDown++ == 0) {
				enableDragTimer(true);
			}
		}
		else {
			// one less button
			if (--m_dragNumButtonsDown == 0) {
				enableDragTimer(false);
			}
		}
	}

	if (macButton == kButtonLeft) {
		EMouseButtonState state = pressed ? kMouseButtonDown : kMouseButtonUp;
		m_buttonState.set(kButtonLeft - 1, state);
		if (pressed) {
			m_draggingFilename.clear();
			LOG_DEBUG2("dragging file directory is cleared");
		}
		else {
			if (m_fakeDraggingStarted) {
                m_getDropTargetThread = new Thread([this](){ get_drop_target_thread(); });
			}

			m_draggingStarted = false;
		}
	}

	return true;
}

bool OSXScreen::onMouseWheel(std::int32_t xDelta, std::int32_t yDelta) const
{
	LOG_DEBUG1("event: button wheel delta=%+d,%+d", xDelta, yDelta);
    sendEvent(EventType::PRIMARY_SCREEN_WHEEL,
              create_event_data<WheelInfo>(WheelInfo{xDelta, yDelta}));
	return true;
}

void OSXScreen::handle_clipboard_check()
{
	checkClipboards();
}

void
OSXScreen::displayReconfigurationCallback(CGDirectDisplayID displayID, CGDisplayChangeSummaryFlags flags, void* inUserData)
{
	OSXScreen* screen = (OSXScreen*)inUserData;

	// Closing or opening the lid when an external monitor is
    // connected causes an kCGDisplayBeginConfigurationFlag event
	CGDisplayChangeSummaryFlags mask = kCGDisplayBeginConfigurationFlag | kCGDisplayMovedFlag |
		kCGDisplaySetModeFlag | kCGDisplayAddFlag | kCGDisplayRemoveFlag |
		kCGDisplayEnabledFlag | kCGDisplayDisabledFlag |
		kCGDisplayMirrorFlag | kCGDisplayUnMirrorFlag |
		kCGDisplayDesktopShapeChangedFlag;

	LOG_DEBUG1("event: display was reconfigured: %x %x %x", flags, mask, flags & mask);

	if (flags & mask) { /* Something actually did change */

		LOG_DEBUG1("event: screen changed shape; refreshing dimensions");
		screen->updateScreenShape(displayID, flags);
	}
}

bool
OSXScreen::onKey(CGEventRef event)
{
	CGEventType eventKind = CGEventGetType(event);

	// get the key and active modifiers
	std::uint32_t virtualKey = CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode);
	CGEventFlags macMask = CGEventGetFlags(event);
	LOG_DEBUG1("event: Key event kind: %d, keycode=%d", eventKind, virtualKey);

	// Special handling to track state of modifiers
	if (eventKind == kCGEventFlagsChanged) {
		// Right Command acts as a Hangul/English toggle for the Windows
		// client's Korean IME; it is never forwarded as a Command modifier.
		// NX_DEVICERCMDKEYMASK tells us the right Command specifically
		// is down, independent of the left Command.
		if (virtualKey == kVK_RightCommand) {
			bool down = (macMask & NX_DEVICERCMDKEYMASK) != 0;
			m_keyState->sendKeyEvent(get_event_target(), down, false,
									 kKeyHangul, 0, 1, 0);
			return true;
		}

		// get old and new modifier state
		KeyModifierMask oldMask = getActiveModifiers();
		KeyModifierMask newMask = m_keyState->mapModifiersFromOSX(macMask);
        m_keyState->handleModifierKeys(get_event_target(), virtualKey, oldMask, newMask);

		// if the current set of modifiers exactly matches a modifiers-only
		// hot key then generate a hot key down event.
		if (m_activeModifierHotKey == 0) {
			if (m_modifierHotKeys.count(newMask) > 0) {
				m_activeModifierHotKey     = m_modifierHotKeys[newMask];
				m_activeModifierHotKeyMask = newMask;
                m_events->add_event(EventType::PRIMARY_SCREEN_HOTKEY_DOWN, get_event_target(),
                                    create_event_data<HotKeyInfo>(HotKeyInfo{m_activeModifierHotKey}));
			}
		}

		// if a modifiers-only hot key is active and should no longer be
		// then generate a hot key up event.
		else if (m_activeModifierHotKey != 0) {
			KeyModifierMask mask = (newMask & m_activeModifierHotKeyMask);
			if (mask != m_activeModifierHotKeyMask) {
                m_events->add_event(EventType::PRIMARY_SCREEN_HOTKEY_UP, get_event_target(),
                                    create_event_data<HotKeyInfo>(HotKeyInfo{m_activeModifierHotKey}));
                m_activeModifierHotKey     = 0;
				m_activeModifierHotKeyMask = 0;
			}
		}

		return true;
	}

        auto i = m_hotKeyToIDMap.find(HotKeyItem(virtualKey, m_keyState->mapModifiersToCarbon(macMask) & 0xff00u));
	if (i != m_hotKeyToIDMap.end()) {
		std::uint32_t id = i->second;
		// determine event type
        EventType type;
		//std::uint32_t eventKind = GetEventKind(event);
		if (eventKind == kCGEventKeyDown) {
            type = EventType::PRIMARY_SCREEN_HOTKEY_DOWN;
		}
		else if (eventKind == kCGEventKeyUp) {
            type = EventType::PRIMARY_SCREEN_HOTKEY_UP;
		}
		else {
			return false;
		}
        m_events->add_event(type, get_event_target(), create_event_data<HotKeyInfo>(HotKeyInfo{id}));
		return true;
	}

	// decode event type
	bool down	  = (eventKind == kCGEventKeyDown);
	bool up		  = (eventKind == kCGEventKeyUp);
	bool isRepeat = (CGEventGetIntegerValueField(event, kCGKeyboardEventAutorepeat) == 1);

	// map event to keys
	KeyModifierMask mask;
	OSXKeyState::KeyIDs keys;
	KeyButton button = m_keyState->mapKeyFromEvent(keys, &mask, event);
	if (button == 0) {
		return false;
	}

	// check for AltGr in mask.  if set we send neither the AltGr nor
	// the super modifiers to clients then remove AltGr before passing
	// the modifiers to onKey.
	KeyModifierMask sendMask = (mask & ~KeyModifierAltGr);
	if ((mask & KeyModifierAltGr) != 0) {
		sendMask &= ~KeyModifierSuper;
	}
	mask &= ~KeyModifierAltGr;

	// update button state
	if (down) {
		m_keyState->onKey(button, true, mask);
	}
	else if (up) {
		if (!m_keyState->isKeyDown(button)) {
			// up event for a dead key.  throw it away.
			return false;
		}
		m_keyState->onKey(button, false, mask);
	}

	// send key events
        for (auto i = keys.begin(); i != keys.end(); ++i) {
        m_keyState->sendKeyEvent(get_event_target(), down, isRepeat,
							*i, sendMask, 1, button);
	}

	return true;
}

void
OSXScreen::onMediaKey(CGEventRef event)
{
	KeyID keyID;
	bool down;
	bool isRepeat;

	if (!getMediaKeyEventInfo (event, &keyID, &down, &isRepeat)) {
		LOG_ERR("Failed to decode media key event");
		return;
	}

	LOG_DEBUG2("Media key event: keyID=0x%02x, %s, repeat=%s",
						keyID, (down ? "down": "up"),
						(isRepeat ? "yes" : "no"));

	KeyButton button = 0;
	KeyModifierMask mask = m_keyState->getActiveModifiers();
    m_keyState->sendKeyEvent(get_event_target(), down, isRepeat, keyID, mask, 1, button);
}

void
OSXScreen::onFunctionKey(int number, bool down, bool isRepeat)
{
	struct FunctionKey {
		KeyID id;
		std::uint32_t virtualKey;
	};
	static const FunctionKey functionKeys[] = {
		{kKeyF1, kVK_F1}, {kKeyF2, kVK_F2}, {kKeyF3, kVK_F3},
		{kKeyF4, kVK_F4}, {kKeyF5, kVK_F5}, {kKeyF6, kVK_F6},
		{kKeyF7, kVK_F7}, {kKeyF8, kVK_F8}, {kKeyF9, kVK_F9},
		{kKeyF10, kVK_F10}, {kKeyF11, kVK_F11}, {kKeyF12, kVK_F12}
	};
	if (number < 1 || number > static_cast<int>(sizeof(functionKeys) / sizeof(functionKeys[0]))) {
		return;
	}

	const KeyModifierMask mask = m_keyState->getActiveModifiers();
	const FunctionKey& functionKey = functionKeys[number - 1];
	const KeyButton button = OSXKeyState::mapVirtualKeyToKeyButton(functionKey.virtualKey);
	m_keyState->onKey(button, down, mask);
	m_keyState->sendKeyEvent(get_event_target(), down, isRepeat,
		functionKey.id, mask, 1, button);
}

bool
OSXScreen::onHotKey(EventRef event) const
{
	// get the hotkey id
	EventHotKeyID hkid;
	GetEventParameter(event, kEventParamDirectObject, typeEventHotKeyID,
                      nullptr, sizeof(EventHotKeyID), nullptr, &hkid);
	std::uint32_t id = hkid.id;

	// determine event type
    EventType type;
	std::uint32_t eventKind = GetEventKind(event);
	if (eventKind == kEventHotKeyPressed) {
        type = EventType::PRIMARY_SCREEN_HOTKEY_DOWN;
	}
	else if (eventKind == kEventHotKeyReleased) {
        type = EventType::PRIMARY_SCREEN_HOTKEY_UP;
	}
	else {
		return false;
	}

    m_events->add_event(type, get_event_target(), create_event_data<HotKeyInfo>(HotKeyInfo{id}));

	return true;
}

ButtonID OSXScreen::map_button_to_osx(std::uint16_t button) const
{
    switch (button) {
    case 1:
        return kButtonLeft;
    case 2:
        return kMacButtonMiddle;
    case 3:
        return kMacButtonRight;
    }

    return static_cast<ButtonID>(button);
}

ButtonID OSXScreen::map_button_from_osx(std::uint16_t macButton) const
{
	switch (macButton) {
	case 1:
		return kButtonLeft;

	case 2:
		return kButtonRight;

	case 3:
		return kButtonMiddle;
	}

	return static_cast<ButtonID>(macButton);
}

std::int32_t OSXScreen::map_scroll_wheel_from_osx(float x) const
{
	// return accelerated scrolling but not exponentially scaled as it is
	// on the mac.
	double d = (1.0 + getScrollSpeed()) * x / getScrollSpeedFactor();
    return static_cast<std::int32_t>(120.0 * d);
}

std::int32_t OSXScreen::map_scroll_wheel_to_osx(float x) const
{
	// use server's acceleration with a little boost since other platforms
	// take one wheel step as a larger step than the mac does.
    return static_cast<std::int32_t>(3.0 * x / 120.0);
}

double
OSXScreen::getScrollSpeed() const
{
	double scaling = 0.0;

	CFPropertyListRef pref = ::CFPreferencesCopyValue(
							CFSTR("com.apple.scrollwheel.scaling") ,
							kCFPreferencesAnyApplication,
							kCFPreferencesCurrentUser,
							kCFPreferencesAnyHost);
    if (pref != nullptr) {
		CFTypeID id = CFGetTypeID(pref);
		if (id == CFNumberGetTypeID()) {
			CFNumberRef value = static_cast<CFNumberRef>(pref);
			if (CFNumberGetValue(value, kCFNumberDoubleType, &scaling)) {
				if (scaling < 0.0) {
					scaling = 0.0;
				}
			}
		}
		CFRelease(pref);
	}

	return scaling;
}

double
OSXScreen::getScrollSpeedFactor() const
{
	return pow(10.0, getScrollSpeed());
}

void
OSXScreen::enableDragTimer(bool enable)
{
    if (enable && m_dragTimer == nullptr) {
        m_dragTimer = m_events->newTimer(0.01, nullptr);
        m_events->add_handler(EventType::TIMER, m_dragTimer,
                              [this](const auto& e){ handle_drag(); });
        CGEventRef event = CGEventCreate(nullptr);
		CGPoint mouse = CGEventGetLocation(event);
		m_dragLastPoint.h = (short)mouse.x;
		m_dragLastPoint.v = (short)mouse.y;
		CFRelease(event);
	}
    else if (!enable && m_dragTimer != nullptr) {
        m_events->remove_handler(EventType::TIMER, m_dragTimer);
		m_events->deleteTimer(m_dragTimer);
        m_dragTimer = nullptr;
	}
}

void OSXScreen::handle_drag()
{
    CGEventRef event = CGEventCreate(nullptr);
	CGPoint p = CGEventGetLocation(event);
	CFRelease(event);

	if ((short)p.x != m_dragLastPoint.h || (short)p.y != m_dragLastPoint.v) {
		m_dragLastPoint.h = (short)p.x;
		m_dragLastPoint.v = (short)p.y;
        onMouseMove((std::int32_t)p.x, (std::int32_t)p.y);
	}
}

void
OSXScreen::updateButtons()
{
	std::uint32_t buttons = GetCurrentButtonState();

	m_buttonState.overwrite(buttons);
}

IKeyState*
OSXScreen::getKeyState() const
{
	return m_keyState;
}

void
OSXScreen::updateScreenShape(const CGDirectDisplayID, const CGDisplayChangeSummaryFlags flags)
{
	updateScreenShape();
}

void
OSXScreen::updateScreenShape()
{
	// get info for each display
	CGDisplayCount displayCount = 0;

    if (CGGetActiveDisplayList(0, nullptr, &displayCount) != CGDisplayNoErr) {
		return;
	}

	if (displayCount == 0) {
		return;
	}

	CGDirectDisplayID* displays = new CGDirectDisplayID[displayCount];
    if (displays == nullptr) {
		return;
	}

	if (CGGetActiveDisplayList(displayCount,
							displays, &displayCount) != CGDisplayNoErr) {
		delete[] displays;
		return;
	}

	// get smallest rect enclosing all display rects
	CGRect totalBounds = CGRectZero;
	for (CGDisplayCount i = 0; i < displayCount; ++i) {
		CGRect bounds = CGDisplayBounds(displays[i]);
		totalBounds   = CGRectUnion(totalBounds, bounds);
	}

	// get shape of default screen
    m_x = (std::int32_t)totalBounds.origin.x;
    m_y = (std::int32_t)totalBounds.origin.y;
    m_w = (std::int32_t)totalBounds.size.width;
    m_h = (std::int32_t)totalBounds.size.height;

	// get center of default screen
  CGDirectDisplayID main = CGMainDisplayID();
  const CGRect rect = CGDisplayBounds(main);
  m_xCenter = (rect.origin.x + rect.size.width) / 2;
  m_yCenter = (rect.origin.y + rect.size.height) / 2;

	delete[] displays;
	// We want to notify the peer screen whether we are primary screen or not
    sendEvent(EventType::SCREEN_SHAPE_CHANGED);

	LOG_DEBUG("screen shape: center=%d,%d size=%dx%d on %u %s",
         m_x, m_y, m_w, m_h, displayCount,
         (displayCount == 1) ? "display" : "displays");
}

#pragma mark -

//
// FAST USER SWITCH NOTIFICATION SUPPORT
//
// OSXScreen::userSwitchCallback(void*)
//
// gets called if a fast user switch occurs
//

pascal OSStatus
OSXScreen::userSwitchCallback(EventHandlerCallRef nextHandler,
								EventRef theEvent,
								void* inUserData)
{
	OSXScreen* screen = (OSXScreen*)inUserData;
    std::uint32_t kind = GetEventKind(theEvent);
	IEventQueue* events = screen->getEvents();

	if (kind == kEventSystemUserSessionDeactivated) {
		LOG_DEBUG("user session deactivated");
        events->add_event(EventType::SCREEN_SUSPEND, screen->get_event_target());
	}
	else if (kind == kEventSystemUserSessionActivated) {
		LOG_DEBUG("user session activated");
        events->add_event(EventType::SCREEN_RESUME, screen->get_event_target());
	}
	return (CallNextEventHandler(nextHandler, theEvent));
}

#pragma mark -

//
// SLEEP/WAKEUP NOTIFICATION SUPPORT
//
// OSXScreen::watchSystemPowerThread(void*)
//
// main of thread monitoring system power (sleep/wakeup) using a CFRunLoop
//

void OSXScreen::watchSystemPowerThread()
{
	io_object_t				notifier;
	IONotificationPortRef	notificationPortRef;
	CFRunLoopSourceRef		runloopSourceRef = 0;

	m_pmRunloop = CFRunLoopGetCurrent();
	// install system power change callback
	m_pmRootPort = IORegisterForSystemPower(this, &notificationPortRef,
											powerChangeCallback, &notifier);
	if (m_pmRootPort == 0) {
		LOG_WARN("IORegisterForSystemPower failed");
	}
	else {
		runloopSourceRef =
			IONotificationPortGetRunLoopSource(notificationPortRef);
		CFRunLoopAddSource(m_pmRunloop, runloopSourceRef,
								kCFRunLoopCommonModes);
	}

	// thread is ready
	{
        std::lock_guard<std::mutex> lock(pm_mutex_);
        is_pm_thread_ready_ = true;
        pm_thread_ready_cv_.notify_one();
	}

	// if we were unable to initialize then exit.  we must do this after
    // setting is_pm_thread_ready_ to true otherwise the parent thread will
	// block waiting for it.
	if (m_pmRootPort == 0) {
		LOG_WARN("failed to init watchSystemPowerThread");
		return;
	}

	LOG_DEBUG("started watchSystemPowerThread");

	LOG_DEBUG("waiting for event loop");
	m_events->waitForReady();

#if defined(MAC_OS_X_VERSION_10_7)
    {
        std::lock_guard<std::mutex> lock_carbon(carbon_loop_mutex_);
        if (!is_carbon_loop_ready_) {

			// we signalling carbon loop ready before starting
			// unless we know how to do it within the loop
			LOG_DEBUG("signalling carbon loop ready");

            is_carbon_loop_ready_ = true;
            cardon_loop_ready_cv_.notify_one();
		}
	}
#endif

	// start the run loop
	LOG_DEBUG("starting carbon loop");
	CFRunLoopRun();
	LOG_DEBUG("carbon loop has stopped");

	// cleanup
	if (notificationPortRef) {
		CFRunLoopRemoveSource(m_pmRunloop,
								runloopSourceRef, kCFRunLoopDefaultMode);
		CFRunLoopSourceInvalidate(runloopSourceRef);
		CFRelease(runloopSourceRef);
	}

    std::lock_guard<std::mutex> lock(pm_mutex_);
	IODeregisterForSystemPower(&notifier);
	m_pmRootPort = 0;
	LOG_DEBUG("stopped watchSystemPowerThread");
}

void
OSXScreen::powerChangeCallback(void* refcon, io_service_t service,
						  natural_t messageType, void* messageArg)
{
	((OSXScreen*)refcon)->handlePowerChangeRequest(messageType, messageArg);
}

void
OSXScreen::handlePowerChangeRequest(natural_t messageType, void* messageArg)
{
	// we've received a power change notification
	switch (messageType) {
	case kIOMessageSystemWillSleep:
		// OSXScreen has to handle this in the main thread so we have to
		// queue a confirm sleep event here.  we actually don't allow the
		// system to sleep until the event is handled.
        m_events->add_event(EventType::OSX_SCREEN_CONFIRM_SLEEP, get_event_target(),
                            create_event_data<long>(reinterpret_cast<long>(messageArg)));
		return;

	case kIOMessageSystemHasPoweredOn:
		LOG_DEBUG("system wakeup");
        m_events->add_event(EventType::SCREEN_RESUME, get_event_target());
		break;

	default:
		break;
	}

    std::lock_guard<std::mutex> lock(pm_mutex_);
	if (m_pmRootPort != 0) {
		IOAllowPowerChange(m_pmRootPort, (long)messageArg);
	}
}

void OSXScreen::handle_confirm_sleep(const Event& event)
{
    long messageArg = event.get_data_as<long>();
	if (messageArg != 0) {
        std::lock_guard<std::mutex> lock(pm_mutex_);
		if (m_pmRootPort != 0) {
			// deliver suspend event immediately.
            m_events->add_event(EventType::SCREEN_SUSPEND, get_event_target(),
                                nullptr, Event::kDeliverImmediately);

			LOG_DEBUG("system will sleep");
			IOAllowPowerChange(m_pmRootPort, messageArg);
		}
	}
}

#pragma mark -

//
// GLOBAL HOTKEY OPERATING MODE SUPPORT (10.3)
//
// CoreGraphics private API (OSX 10.3)
// Source: http://ichiro.nnip.org/osx/Cocoa/GlobalHotkey.html
//
// We load the functions dynamically because they're not available in
// older SDKs.  We don't use weak linking because we want users of
// older SDKs to build an app that works on newer systems and older
// SDKs will not provide the symbols.
//
// FIXME: This is hosed as of OS 10.5; patches to repair this are
// a good thing.
//
#if 0

typedef int CGSConnection;
typedef enum {
	CGSGlobalHotKeyEnable = 0,
	CGSGlobalHotKeyDisable = 1,
} CGSGlobalHotKeyOperatingMode;

extern CGSConnection _CGSDefaultConnection(void) WEAK_IMPORT_ATTRIBUTE;
extern CGError CGSGetGlobalHotKeyOperatingMode(CGSConnection connection, CGSGlobalHotKeyOperatingMode *mode) WEAK_IMPORT_ATTRIBUTE;
extern CGError CGSSetGlobalHotKeyOperatingMode(CGSConnection connection, CGSGlobalHotKeyOperatingMode mode) WEAK_IMPORT_ATTRIBUTE;

typedef CGSConnection (*_CGSDefaultConnection_t)(void);
typedef CGError (*CGSGetGlobalHotKeyOperatingMode_t)(CGSConnection connection, CGSGlobalHotKeyOperatingMode *mode);
typedef CGError (*CGSSetGlobalHotKeyOperatingMode_t)(CGSConnection connection, CGSGlobalHotKeyOperatingMode mode);

static _CGSDefaultConnection_t				s__CGSDefaultConnection;
static CGSGetGlobalHotKeyOperatingMode_t	s_CGSGetGlobalHotKeyOperatingMode;
static CGSSetGlobalHotKeyOperatingMode_t	s_CGSSetGlobalHotKeyOperatingMode;

#define LOOKUP(name_)													\
    s_ ## name_ = nullptr;													\
	if (NSIsSymbolNameDefinedWithHint("_" #name_, "CoreGraphics")) {	\
		s_ ## name_ = (name_ ## _t)NSAddressOfSymbol(					\
							NSLookupAndBindSymbolWithHint(				\
								"_" #name_, "CoreGraphics"));			\
	}

bool
OSXScreen::isGlobalHotKeyOperatingModeAvailable()
{
	if (!s_testedForGHOM) {
		s_testedForGHOM = true;
		LOOKUP(_CGSDefaultConnection);
		LOOKUP(CGSGetGlobalHotKeyOperatingMode);
		LOOKUP(CGSSetGlobalHotKeyOperatingMode);
        s_hasGHOM = (s__CGSDefaultConnection != nullptr &&
                    s_CGSGetGlobalHotKeyOperatingMode != nullptr &&
                    s_CGSSetGlobalHotKeyOperatingMode != nullptr);
	}
	return s_hasGHOM;
}

void
OSXScreen::setGlobalHotKeysEnabled(bool enabled)
{
	if (isGlobalHotKeyOperatingModeAvailable()) {
		CGSConnection conn = s__CGSDefaultConnection();

		CGSGlobalHotKeyOperatingMode mode;
		s_CGSGetGlobalHotKeyOperatingMode(conn, &mode);

		if (enabled && mode == CGSGlobalHotKeyDisable) {
			s_CGSSetGlobalHotKeyOperatingMode(conn, CGSGlobalHotKeyEnable);
		}
		else if (!enabled && mode == CGSGlobalHotKeyEnable) {
			s_CGSSetGlobalHotKeyOperatingMode(conn, CGSGlobalHotKeyDisable);
		}
	}
}

bool
OSXScreen::getGlobalHotKeysEnabled()
{
	CGSGlobalHotKeyOperatingMode mode;
	if (isGlobalHotKeyOperatingModeAvailable()) {
		CGSConnection conn = s__CGSDefaultConnection();
		s_CGSGetGlobalHotKeyOperatingMode(conn, &mode);
	}
	else {
		mode = CGSGlobalHotKeyEnable;
	}
	return (mode == CGSGlobalHotKeyEnable);
}

#endif

//
// OSXScreen::HotKeyItem
//

OSXScreen::HotKeyItem::HotKeyItem(std::uint32_t keycode, std::uint32_t mask) :
    m_ref(nullptr),
	m_keycode(keycode),
	m_mask(mask)
{
	// do nothing
}

OSXScreen::HotKeyItem::HotKeyItem(EventHotKeyRef ref, std::uint32_t keycode, std::uint32_t mask) :
	m_ref(ref),
	m_keycode(keycode),
	m_mask(mask)
{
	// do nothing
}

EventHotKeyRef
OSXScreen::HotKeyItem::getRef() const
{
	return m_ref;
}

bool
OSXScreen::HotKeyItem::operator<(const HotKeyItem& x) const
{
	return (m_keycode < x.m_keycode ||
			(m_keycode == x.m_keycode && m_mask < x.m_mask));
}

// Quartz event tap support for the secondary display. This makes sure that we
// will show the cursor if a local event comes in while InputLeap has the cursor
// off the screen.
CGEventRef
OSXScreen::handleCGInputEventSecondary(
	CGEventTapProxy proxy,
	CGEventType type,
	CGEventRef event,
	void* refcon)
{
	// this fix is really screwing with the correct show/hide behavior. it
	// should be tested better before reintroducing.
	return event;

	OSXScreen* screen = (OSXScreen*)refcon;
	if (screen->m_cursorHidden && type == kCGEventMouseMoved) {

		CGPoint pos = CGEventGetLocation(event);
		if (pos.x != screen->m_xCenter || pos.y != screen->m_yCenter) {

			LOG_DEBUG("show cursor on secondary, type=%d pos=%.2f,%.2f",
					type, pos.x, pos.y);
			screen->showCursor();
		}
	}
	return event;
}

// Quartz event tap support
CGEventRef
OSXScreen::handleCGInputEvent(CGEventTapProxy proxy,
							   CGEventType type,
							   CGEventRef event,
							   void* refcon)
{
	OSXScreen* screen = (OSXScreen*)refcon;
	CGPoint pos;

	const bool keyEvent = type == kCGEventKeyDown || type == kCGEventKeyUp ||
		type == kCGEventFlagsChanged || type == NX_SYSDEFINED;
	if (keyEvent && screen->m_inputDeviceMonitor != nullptr) {
		if (type == kCGEventKeyDown) {
			const auto keyCode = static_cast<std::uint32_t>(
				CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode));
			// autorepeat keeps sending keyDown for a key already held; reuse
			// its original verdict instead of re-matching (a held key stops
			// producing HID reports, so a fresh match would wrongly miss).
			auto it = screen->m_localKeyCodes.find(keyCode);
			const bool local = it != screen->m_localKeyCodes.end() ? it->second :
				screen->m_inputDeviceMonitor->match(event, true).local;
			screen->m_localKeyCodes[keyCode] = local;
			if (local) {
				return event;
			}
		}
		else if (type == kCGEventKeyUp) {
			const auto keyCode = static_cast<std::uint32_t>(
				CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode));
			auto it = screen->m_localKeyCodes.find(keyCode);
			bool local;
			if (it != screen->m_localKeyCodes.end()) {
				// use the matching keyDown's verdict so a down forwarded to
				// the remote screen always gets its up forwarded too.
				local = it->second;
				screen->m_localKeyCodes.erase(it);
			}
			else {
				local = screen->m_inputDeviceMonitor->match(event, true).local;
			}
			if (local) {
				return event;
			}
		}
		else if (screen->m_inputDeviceMonitor->match(event, type == kCGEventFlagsChanged).local) {
			return event;
		}
	}

	if (!screen->m_isOnScreen && screen->m_mapMacFunctionKeys &&
		(type == kCGEventKeyDown || type == kCGEventKeyUp)) {
		const auto keyCode = static_cast<std::uint32_t>(
			CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode));
		const int functionKey = macFunctionKeyForKeyCode(keyCode);
		if (functionKey != 0) {
			const bool isRepeat = type == kCGEventKeyDown &&
				CGEventGetIntegerValueField(event, kCGKeyboardEventAutorepeat) == 1;
			screen->onFunctionKey(functionKey, type == kCGEventKeyDown, isRepeat);
			return nullptr;
		}
	}

	switch(type) {
		case kCGEventLeftMouseDown:
		case kCGEventRightMouseDown:
		case kCGEventOtherMouseDown:
			screen->onMouseButton(true, CGEventGetIntegerValueField(event, kCGMouseEventButtonNumber) + 1);
			break;
		case kCGEventLeftMouseUp:
		case kCGEventRightMouseUp:
		case kCGEventOtherMouseUp:
			screen->onMouseButton(false, CGEventGetIntegerValueField(event, kCGMouseEventButtonNumber) + 1);
			break;
		case kCGEventLeftMouseDragged:
		case kCGEventRightMouseDragged:
		case kCGEventOtherMouseDragged:
		case kCGEventMouseMoved:
			pos = CGEventGetLocation(event);
			screen->onMouseMove(pos.x, pos.y);

			// The system ignores our cursor-centering calls if
			// we don't return the event. This should be harmless,
			// but might register as slight movement to other apps
			// on the system. It hasn't been a problem before, though.
			return event;
			break;
		case kCGEventScrollWheel:
            screen->onMouseWheel(screen->map_scroll_wheel_from_osx(
								 CGEventGetIntegerValueField(event, kCGScrollWheelEventFixedPtDeltaAxis2) / 65536.0f),
                                 screen->map_scroll_wheel_from_osx(
								 CGEventGetIntegerValueField(event, kCGScrollWheelEventFixedPtDeltaAxis1) / 65536.0f));
			break;
		case kCGEventKeyDown:
		case kCGEventKeyUp:
		case kCGEventFlagsChanged:
			screen->onKey(event);
			break;
		case kCGEventTapDisabledByTimeout:
			// Re-enable our event-tap
			CGEventTapEnable(screen->m_eventTapPort, true);
			LOG_INFO("quartz event tap was disabled by timeout, re-enabling");
			break;
		case kCGEventTapDisabledByUserInput:
			LOG_ERR("quartz event tap was disabled by user input");
			break;
		case NX_NULLEVENT:
			break;
		default:
			if (type == NX_SYSDEFINED) {
			if (!screen->m_isOnScreen && screen->m_mapMacFunctionKeys) {
				std::uint32_t keyType = 0;
				bool down = false;
				bool isRepeat = false;
				if (getSystemKeyEventInfo(event, &keyType, &down, &isRepeat)) {
					const int functionKey = macFunctionKeyForSystemKey(keyType);
					if (functionKey != 0) {
						screen->onFunctionKey(functionKey, down, isRepeat);
						break;
					}
				}
			}
			if (isMediaKeyEvent (event)) {
				LOG_DEBUG2("detected media key event");
				screen->onMediaKey (event);
			} else {
				LOG_DEBUG2("ignoring unknown system defined event");
				return event;
			}
			break;
			}

			LOG_DEBUG3("unknown quartz event type: 0x%02x", type);
	}

	if (screen->m_isOnScreen) {
		return event;
	} else {
        return nullptr;
	}
}

void OSXScreen::MouseButtonState::set(std::uint32_t button, EMouseButtonState state)
{
	bool newState = (state == kMouseButtonDown);
	m_buttons.set(button, newState);
}

bool
OSXScreen::MouseButtonState::any()
{
	return m_buttons.any();
}

void
OSXScreen::MouseButtonState::reset()
{
	m_buttons.reset();
}

void OSXScreen::MouseButtonState::overwrite(std::uint32_t buttons)
{
	m_buttons = std::bitset<NumButtonIDs>(buttons);
}

bool OSXScreen::MouseButtonState::test(std::uint32_t button) const
{
	return m_buttons.test(button);
}

std::int8_t OSXScreen::MouseButtonState::getFirstButtonDown() const
{
	if (m_buttons.any()) {
		for (unsigned short button = 0; button < m_buttons.size(); button++) {
			if (m_buttons.test(button)) {
				return button;
			}
		}
	}
	return -1;
}

char*
OSXScreen::CFStringRefToUTF8String(CFStringRef aString)
{
    if (aString == nullptr) {
        return nullptr;
	}

	CFIndex length = CFStringGetLength(aString);
	CFIndex maxSize = CFStringGetMaximumSizeForEncoding(
		length,
		kCFStringEncodingUTF8);
	char* buffer = (char*)malloc(maxSize);
	if (CFStringGetCString(aString, buffer, maxSize, kCFStringEncodingUTF8)) {
		return buffer;
	}
    return nullptr;
}

void
OSXScreen::fakeDraggingFiles(DragFileList fileList)
{
	m_fakeDraggingStarted = true;
    std::string fileExt;
	if (fileList.size() == 1) {
		fileExt = DragInformation::getDragFileExtension(
			fileList.at(0).getFilename());
	}

#if defined(MAC_OS_X_VERSION_10_7)
	fakeDragging(fileExt.c_str(), m_xCursor, m_yCursor);
#else
	LOG_WARN("drag drop not supported");
#endif
}

std::string& OSXScreen::getDraggingFilename()
{
	if (m_draggingStarted) {
		CFStringRef dragInfo = getDraggedFileURL();
        char* info = nullptr;
		info = CFStringRefToUTF8String(dragInfo);
        if (info == nullptr) {
			m_draggingFilename.clear();
		}
		else {
			LOG_DEBUG("drag info: %s", info);
			CFRelease(dragInfo);
            std::string fileList = info;
			m_draggingFilename = fileList;
		}

		// fake a escape key down and up then left mouse button up
		fakeKeyDown(kKeyEscape, 8192, 1);
		fakeKeyUp(1);
		fakeMouseButton(kButtonLeft, false);
	}
	return m_draggingFilename;
}

void
OSXScreen::waitForCarbonLoop() const
{
#if defined(MAC_OS_X_VERSION_10_7)
    if (is_carbon_loop_ready_) {
		LOG_DEBUG("carbon loop already ready");
		return;
	}

    std::unique_lock<std::mutex> lock(carbon_loop_mutex_);

	LOG_DEBUG("waiting for carbon loop");

    while (!cardon_loop_ready_cv_.wait_for(lock, std::chrono::seconds{kCarbonLoopWaitTimeout},
                                           [this](){ return is_carbon_loop_ready_; })) {
        LOG_DEBUG("carbon loop not ready, waiting again");
    }

	LOG_DEBUG("carbon loop ready");
#endif

}

#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

void
setZeroSuppressionInterval()
{
	CGSetLocalEventsSuppressionInterval(0.0);
}

void
avoidSupression()
{
	// avoid suppression of local hardware events
	// stkamp@users.sourceforge.net
	CGSetLocalEventsFilterDuringSupressionState(
							kCGEventFilterMaskPermitAllEvents,
							kCGEventSupressionStateSupressionInterval);
	CGSetLocalEventsFilterDuringSupressionState(
							(kCGEventFilterMaskPermitLocalKeyboardEvents |
							kCGEventFilterMaskPermitSystemDefinedEvents),
							kCGEventSupressionStateRemoteMouseDrag);
}

void
logCursorVisibility()
{
	// CGCursorIsVisible is probably deprecated because its unreliable.
	if (!CGCursorIsVisible()) {
		LOG_WARN("cursor may not be visible");
	}
}

void
avoidHesitatingCursor()
{
	// This used to be necessary to get smooth mouse motion on other screens,
	// but now is just to avoid a hesitating cursor when transitioning to
	// the primary (this) screen.
	CGSetLocalEventsSuppressionInterval(0.0001);
}

#pragma GCC diagnostic error "-Wdeprecated-declarations"

} // namespace inputleap
