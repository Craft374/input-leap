/*
 * InputLeap -- mouse and keyboard sharing utility
 * Copyright (C) 2014-2016 Symless Ltd.
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

#include "ZeroconfService.h"

#include "MainWindow.h"
#include "ZeroconfRegister.h"
#include "ZeroconfBrowser.h"

#include <QtNetwork>
#include <QMessageBox>
#define _MSL_STDINT_H
#include <stdint.h>
#include <dns_sd.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <stdlib.h>
#endif

static const QStringList preferedIPAddress(
                QStringList() <<
                "192.168." <<
                "10." <<
                "172.");

const char* ZeroconfService:: m_ServerServiceName = "_inputLeapServerZeroconf._tcp";
const char* ZeroconfService:: m_ClientServiceName = "_inputLeapClientZeroconf._tcp";

static void silence_avahi_warning()
{
    // the libavahi folks seemingly find Apple's bonjour API distasteful
    // and are quite liberal in taking it out on users...unless we set
    // this environmental variable before calling the avahi library.
    // additionally, Microsoft does not give us a POSIX setenv() so
    // we use their OS-specific API instead
    const char *name  = "AVAHI_COMPAT_NOWARN";
    const char *value = "1";
#ifdef _WIN32
#if QT_VERSION_MAJOR < 6
    SetEnvironmentVariable(name, value);
#else
    SetEnvironmentVariable(reinterpret_cast<LPCWSTR>(name), reinterpret_cast<LPCWSTR>(value));
#endif
#else
    setenv(name, value, 1);
#endif
}

// The TXT record carries this PC's IPv4 addresses ("ip=a,b"), so the peer does not depend on
// resolving our host name (which fails on many Windows/Mac combinations).
static QByteArray localIpTxtRecord()
{
    QStringList ips;
    for (const QHostAddress& address : QNetworkInterface::allAddresses()) {
        if (address.protocol() == QAbstractSocket::IPv4Protocol && !address.isLoopback()
            && !address.isInSubnet(QHostAddress(QStringLiteral("169.254.0.0")), 16)) {
            ips << address.toString();
        }
    }
    const QByteArray entry = "ip=" + ips.join(QLatin1Char(',')).toLatin1();
    if (ips.isEmpty() || entry.size() > 255) {
        return QByteArray();
    }
    return QByteArray(1, static_cast<char>(entry.size())) + entry;
}

static QStringList ipsFromTxtRecord(const QByteArray& txt)
{
    for (int i = 0; i < txt.size();) {
        const int len = static_cast<unsigned char>(txt[i]);
        const QByteArray entry = txt.mid(i + 1, len);
        if (entry.startsWith("ip=")) {
            return QString::fromLatin1(entry.mid(3)).split(QLatin1Char(','), Qt::SkipEmptyParts);
        }
        i += len + 1;
    }
    return QStringList();
}

// Prefers an address on the same /24 as one of our own interfaces (the wifi both PCs share).
static QString pickReachableIp(const QStringList& ips)
{
    const auto locals = QNetworkInterface::allAddresses();
    for (const QString& ip : ips) {
        const QHostAddress candidate(ip);
        for (const QHostAddress& local : locals) {
            if (local.protocol() == QAbstractSocket::IPv4Protocol && candidate.isInSubnet(local, 24)) {
                return ip;
            }
        }
    }
    return ips.value(0);
}

ZeroconfService::ZeroconfService(MainWindow* mainWindow) :
    m_pMainWindow(mainWindow),
    m_ServiceRegistered(false)
{
    silence_avahi_warning();
    if (m_pMainWindow->app_role() == AppRole::Server) {
        if (registerService(true)) {
            zeroconf_browser_ = std::make_unique<ZeroconfBrowser>(this);
            connect(zeroconf_browser_.get(), &ZeroconfBrowser::currentRecordsChanged, this, &ZeroconfService::clientDetected);
            zeroconf_browser_->browseForType(QLatin1String(m_ClientServiceName));
        }
    }
    else {
        zeroconf_browser_ = std::make_unique<ZeroconfBrowser>(this);
        connect(zeroconf_browser_.get(), &ZeroconfBrowser::currentRecordsChanged, this, &ZeroconfService::serverDetected);
        zeroconf_browser_->browseForType(QLatin1String(m_ServerServiceName));
    }

    connect(zeroconf_browser_.get(), &ZeroconfBrowser::error, this, &ZeroconfService::errorHandle);
}

ZeroconfService::~ZeroconfService() = default;

void ZeroconfService::serverDetected(const QList<ZeroconfRecord>& list)
{
    for (const ZeroconfRecord& record : list) {
        registerService(false);
        m_pMainWindow->appendLogInfo(tr("zeroconf server detected: %1").arg(
            record.serviceName));
        resolvePeer(record);
    }
}

void ZeroconfService::clientDetected(const QList<ZeroconfRecord>& list)
{
    for (const ZeroconfRecord& record : list) {
        m_pMainWindow->appendLogInfo(tr("zeroconf client detected: %1").arg(
            record.serviceName));
        m_pMainWindow->autoAddScreen(record.serviceName);
        resolvePeer(record);
    }
}

void ZeroconfService::resolvePeer(const ZeroconfRecord& record)
{
    auto* resolver = new ZeroconfResolver(this);
    connect(resolver, &ZeroconfResolver::resolved, this, &ZeroconfService::peerResolved);
    connect(resolver, &ZeroconfResolver::resolved, resolver, &QObject::deleteLater);
    connect(resolver, &ZeroconfResolver::error, resolver, &QObject::deleteLater);
    connect(resolver, &ZeroconfResolver::error, this, [this, name = record.serviceName](DNSServiceErrorType code) {
        m_pMainWindow->appendLogError(tr("zeroconf could not resolve %1 (error %2)").arg(name).arg(code));
    });
    resolver->resolve(record);
}

void ZeroconfService::peerResolved(const QString& host, const QByteArray& txtRecord)
{
    const bool toServer = m_pMainWindow->app_role() == AppRole::Client;   // a client browses for servers
    const auto report = [this, toServer](const QString& ip) {
        m_pMainWindow->appendLogInfo(tr("zeroconf peer address: %1").arg(ip));
        if (toServer) {
            m_pMainWindow->serverDetected(ip);
        }
        else {
            m_pMainWindow->peerDetected(ip);
        }
    };

    const QString ip = pickReachableIp(ipsFromTxtRecord(txtRecord));
    if (!ip.isEmpty()) {
        report(ip);
        return;
    }
    // older peer without the TXT record: fall back to its mDNS host name
    QHostInfo::lookupHost(host, this, [this, host, report](const QHostInfo& info) {
        for (const QHostAddress& address : info.addresses()) {
            if (address.protocol() == QAbstractSocket::IPv4Protocol) {
                report(address.toString());
                return;
            }
        }
        m_pMainWindow->appendLogError(tr("zeroconf could not find an address for %1").arg(host));
    });
}

void ZeroconfService::errorHandle(DNSServiceErrorType errorCode)
{
    QMessageBox::critical(nullptr, tr("Zero configuration service"),
        tr("Error code: %1.").arg(errorCode));
}

bool ZeroconfService::registerService(bool server)
{
    bool result = true;

    if (!m_ServiceRegistered) {
        if (!m_zeroconfServer.listen()) {
            QMessageBox::critical(nullptr, tr("Zero configuration service"),
                tr("Unable to start the zeroconf: %1.")
                .arg(m_zeroconfServer.errorString()));
            result = false;
        }
        else {
            zeroconf_register_ = std::make_unique<ZeroconfRegister>(this);
            if (server) {
                zeroconf_register_->registerService(
                    ZeroconfRecord(tr("%1").arg(m_pMainWindow->getScreenName()),
                    QLatin1String(m_ServerServiceName), QString()),
                    m_zeroconfServer.serverPort(), localIpTxtRecord());
            }
            else {
                zeroconf_register_->registerService(
                    ZeroconfRecord(tr("%1").arg(m_pMainWindow->getScreenName()),
                    QLatin1String(m_ClientServiceName), QString()),
                    m_zeroconfServer.serverPort(), localIpTxtRecord());
            }

            m_ServiceRegistered = true;
        }
    }

    return result;
}
