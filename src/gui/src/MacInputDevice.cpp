/*
 * InputLeap -- mouse and keyboard sharing utility
 * Copyright (C) 2026 InputLeafPlus Developers
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "MacInputDevice.h"

#include <IOKit/hid/IOHIDKeys.h>
#include <IOKit/hid/IOHIDManager.h>

#include <algorithm>
#include <QObject>

namespace {

long deviceNumber(IOHIDDeviceRef device, CFStringRef key)
{
    CFTypeRef value = IOHIDDeviceGetProperty(device, key);
    if (value == nullptr || CFGetTypeID(value) != CFNumberGetTypeID()) {
        return 0;
    }

    long result = 0;
    CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberLongType, &result);
    return result;
}

QString deviceString(IOHIDDeviceRef device, CFStringRef key)
{
    CFTypeRef value = IOHIDDeviceGetProperty(device, key);
    if (value == nullptr || CFGetTypeID(value) != CFStringGetTypeID()) {
        return {};
    }

    const auto string = static_cast<CFStringRef>(value);
    const CFIndex length = CFStringGetLength(string);
    const CFIndex size = CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;
    QByteArray buffer(size, '\0');
    if (!CFStringGetCString(string, buffer.data(), size, kCFStringEncodingUTF8)) {
        return {};
    }
    return QString::fromUtf8(buffer.constData());
}

CFMutableDictionaryRef keyboardMatchingDictionary()
{
    CFMutableDictionaryRef matching = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 2, &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    int usagePage = kHIDPage_GenericDesktop;
    int usage = kHIDUsage_GD_Keyboard;
    CFNumberRef usagePageValue = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &usagePage);
    CFNumberRef usageValue = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &usage);
    CFDictionarySetValue(matching, CFSTR(kIOHIDDeviceUsagePageKey), usagePageValue);
    CFDictionarySetValue(matching, CFSTR(kIOHIDDeviceUsageKey), usageValue);
    CFRelease(usagePageValue);
    CFRelease(usageValue);
    return matching;
}

} // namespace

QVector<MacInputDevice> macInputDevices()
{
    QVector<MacInputDevice> result;
    IOHIDManagerRef manager = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    if (manager == nullptr) {
        return result;
    }

    CFMutableDictionaryRef matching = keyboardMatchingDictionary();
    IOHIDManagerSetDeviceMatching(manager, matching);
    CFRelease(matching);
    if (IOHIDManagerOpen(manager, kIOHIDOptionsTypeNone) != kIOReturnSuccess) {
        CFRelease(manager);
        return result;
    }

    CFSetRef devices = IOHIDManagerCopyDevices(manager);
    if (devices != nullptr) {
        const CFIndex count = CFSetGetCount(devices);
        QVector<const void*> values(count);
        CFSetGetValues(devices, values.data());

        for (const void* value : values) {
            IOHIDDeviceRef device = static_cast<IOHIDDeviceRef>(const_cast<void*>(value));
            const QString transport = deviceString(device, CFSTR(kIOHIDTransportKey));
            if (transport.compare(QStringLiteral("USB"), Qt::CaseInsensitive) != 0) {
                continue;
            }

            const long vendor = deviceNumber(device, CFSTR(kIOHIDVendorIDKey));
            const long product = deviceNumber(device, CFSTR(kIOHIDProductIDKey));
            const long location = deviceNumber(device, CFSTR(kIOHIDLocationIDKey));
            QString name = deviceString(device, CFSTR(kIOHIDProductKey)).trimmed();
            if (name.isEmpty()) {
                name = QObject::tr("이름 없는 USB 키보드");
            }

            MacInputDevice inputDevice;
            inputDevice.id = QStringLiteral("%1:%2:%3").arg(vendor).arg(product).arg(location);
            inputDevice.name = QStringLiteral("%1 (%2:%3)")
                                   .arg(name)
                                   .arg(vendor, 4, 16, QLatin1Char('0'))
                                   .arg(product, 4, 16, QLatin1Char('0'));
            result.push_back(inputDevice);
        }
        CFRelease(devices);
    }
    IOHIDManagerClose(manager, kIOHIDOptionsTypeNone);
    CFRelease(manager);

    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return left.id < right.id;
    });
    result.erase(std::unique(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return left.id == right.id;
    }), result.end());
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return left.name.localeAwareCompare(right.name) < 0;
    });
    return result;
}
