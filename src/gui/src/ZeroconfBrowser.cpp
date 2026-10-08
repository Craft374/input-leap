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

#include "ZeroconfBrowser.h"

#include <QtCore/QSocketNotifier>

ZeroconfBrowser::ZeroconfBrowser(QObject* parent) :
    QObject(parent),
    m_DnsServiceRef(nullptr)
{
}

ZeroconfBrowser::~ZeroconfBrowser()
{
    if (m_DnsServiceRef) {
        DNSServiceRefDeallocate(m_DnsServiceRef);
        m_DnsServiceRef = nullptr;
    }
}

void ZeroconfBrowser::browseForType(const QString& type)
{
    DNSServiceErrorType err = DNSServiceBrowse(&m_DnsServiceRef, 0, 0,
        type.toUtf8().constData(), nullptr, browseReply, this);

    if (err != kDNSServiceErr_NoError) {
        Q_EMIT error(err);
    }
    else {
        int sockFD = DNSServiceRefSockFD(m_DnsServiceRef);
        if (sockFD == -1) {
            Q_EMIT error(kDNSServiceErr_Invalid);
        }
        else {
            socket_ = std::make_unique<QSocketNotifier>(sockFD, QSocketNotifier::Read, this);
            connect(socket_.get(), &QSocketNotifier::activated, this, &ZeroconfBrowser::socketReadyRead);
        }
    }
}

void ZeroconfBrowser::socketReadyRead()
{
    DNSServiceErrorType err = DNSServiceProcessResult(m_DnsServiceRef);
    if (err != kDNSServiceErr_NoError) {
        Q_EMIT error(err);
    }
}

void ZeroconfBrowser::browseReply(DNSServiceRef, DNSServiceFlags flags,
            quint32, DNSServiceErrorType errorCode, const char* serviceName,
            const char* regType, const char* replyDomain, void* context)
{
    ZeroconfBrowser* browser = static_cast<ZeroconfBrowser*>(context);
    if (errorCode != kDNSServiceErr_NoError) {
        Q_EMIT browser->error(errorCode);
    }
    else {
        ZeroconfRecord record(serviceName, regType, replyDomain);
        if (flags & kDNSServiceFlagsAdd) {
            if (!browser->m_Records.contains(record)) {
                browser->m_Records.append(record);
            }
        }
        else {
            browser->m_Records.removeAll(record);
        }
        if (!(flags & kDNSServiceFlagsMoreComing)) {
            Q_EMIT browser->currentRecordsChanged(browser->m_Records);
        }
    }
}

ZeroconfResolver::ZeroconfResolver(QObject* parent) :
    QObject(parent),
    m_DnsServiceRef(nullptr),
    m_Done(false)
{
}

ZeroconfResolver::~ZeroconfResolver()
{
    if (m_DnsServiceRef) {
        DNSServiceRefDeallocate(m_DnsServiceRef);
        m_DnsServiceRef = nullptr;
    }
}

void ZeroconfResolver::resolve(const ZeroconfRecord& record)
{
    DNSServiceErrorType err = DNSServiceResolve(&m_DnsServiceRef, 0, 0,
        record.serviceName.toUtf8().constData(), record.registeredType.toUtf8().constData(),
        record.replyDomain.toUtf8().constData(), resolveReply, this);

    int sockFD = err == kDNSServiceErr_NoError ? DNSServiceRefSockFD(m_DnsServiceRef) : -1;
    if (sockFD == -1) {
        Q_EMIT error(err != kDNSServiceErr_NoError ? err : kDNSServiceErr_Invalid);
        return;
    }
    socket_ = std::make_unique<QSocketNotifier>(sockFD, QSocketNotifier::Read, this);
    connect(socket_.get(), &QSocketNotifier::activated, this, &ZeroconfResolver::socketReadyRead);
}

void ZeroconfResolver::socketReadyRead()
{
    DNSServiceErrorType err = DNSServiceProcessResult(m_DnsServiceRef);
    if (err != kDNSServiceErr_NoError) {
        Q_EMIT error(err);
    }
}

void ZeroconfResolver::resolveReply(DNSServiceRef, DNSServiceFlags, quint32,
            DNSServiceErrorType errorCode, const char*, const char* hostTarget,
            quint16, quint16 txtLen, const unsigned char* txtRecord, void* context)
{
    ZeroconfResolver* resolver = static_cast<ZeroconfResolver*>(context);
    if (resolver->m_Done) {
        return;   // the same service answers once per network interface
    }
    resolver->m_Done = true;
    if (errorCode != kDNSServiceErr_NoError) {
        Q_EMIT resolver->error(errorCode);
    }
    else {
        Q_EMIT resolver->resolved(QString::fromUtf8(hostTarget),
            QByteArray(reinterpret_cast<const char*>(txtRecord), txtLen));
    }
}
