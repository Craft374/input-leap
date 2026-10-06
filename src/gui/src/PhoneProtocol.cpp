/*
 * InputLeap -- mouse and keyboard sharing utility
 * Copyright (C) 2026 InputLeafPlus Developers
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "PhoneProtocol.h"

#include <QCryptographicHash>

#include <algorithm>

namespace phone {

namespace {

bool hasToken(const QByteArray& list, const QByteArray& token)
{
    for (const QByteArray& part : list.split(',')) {
        if (part.trimmed().toLower() == token) {
            return true;
        }
    }
    return false;
}

} // namespace

ParseStatus parseHttpRequestHead(const QByteArray& buf, HttpRequest* out, int* consumed)
{
    const int end = buf.indexOf("\r\n\r\n");
    if (end < 0) {
        return buf.size() > kMaxHttpHead ? ParseStatus::Error : ParseStatus::NeedMore;
    }
    if (end + 4 > kMaxHttpHead) {
        return ParseStatus::Error;
    }

    const QList<QByteArray> lines = buf.left(end).split('\n');
    HttpRequest req;
    for (int i = 0; i < lines.size(); ++i) {
        QByteArray line = lines[i];
        if (line.endsWith('\r')) {
            line.chop(1);
        }
        if (line.contains('\r')) {
            return ParseStatus::Error;
        }
        if (i == 0) {
            const QList<QByteArray> parts = line.split(' ');
            if (parts.size() != 3 || parts[0] != "GET" || !parts[1].startsWith('/')
                || (parts[2] != "HTTP/1.1" && parts[2] != "HTTP/1.0")) {
                return ParseStatus::Error;
            }
            req.method = parts[0];
            const int query = parts[1].indexOf('?');
            req.path = query < 0 ? parts[1] : parts[1].left(query);
        } else {
            const int colon = line.indexOf(':');
            if (colon <= 0) {
                return ParseStatus::Error;
            }
            const QByteArray name = line.left(colon);
            if (name != name.trimmed()) {   // no whitespace before the colon (also rejects obs-fold)
                return ParseStatus::Error;
            }
            req.headers.insert(name.toLower(), line.mid(colon + 1).trimmed());
        }
    }
    *out = req;
    *consumed = end + 4;
    return ParseStatus::Ok;
}

bool isWebSocketUpgrade(const HttpRequest& req)
{
    return req.headers.value("upgrade").trimmed().toLower() == "websocket"
        && hasToken(req.headers.value("connection"), "upgrade")
        && QByteArray::fromBase64(req.headers.value("sec-websocket-key")).size() == 16
        && req.headers.value("sec-websocket-version") == "13";
}

QByteArray websocketAcceptKey(const QByteArray& clientKey)
{
    return QCryptographicHash::hash(clientKey + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11",
                                    QCryptographicHash::Sha1).toBase64();
}

ParseStatus decodeWsFrame(const QByteArray& buf, WsFrame* out, int* consumed)
{
    if (buf.size() < 2) {
        return ParseStatus::NeedMore;
    }
    const auto* p = reinterpret_cast<const uchar*>(buf.constData());
    const int opcode = p[0] & 0x0F;
    const bool fin = (p[0] & 0x80) != 0;
    const bool masked = (p[1] & 0x80) != 0;
    // Reserved bits, fragmentation (FIN=0 / continuation), binary and unknown opcodes are refused.
    if ((p[0] & 0x70) != 0 || !fin || !masked
        || (opcode != 1 && opcode != 8 && opcode != 9 && opcode != 10)) {
        return ParseStatus::Error;
    }

    quint64 len = p[1] & 0x7F;
    int pos = 2;
    if (len == 126) {
        if (buf.size() < 4) {
            return ParseStatus::NeedMore;
        }
        len = (quint64(p[2]) << 8) | p[3];
        pos = 4;
    } else if (len == 127) {
        if (buf.size() < 10) {
            return ParseStatus::NeedMore;
        }
        len = 0;
        for (int i = 0; i < 8; ++i) {
            len = (len << 8) | p[2 + i];
        }
        pos = 10;
    }
    // Checked before waiting for the payload so a huge announced length never makes us buffer.
    if (len > quint64(kMaxWsPayload) || (opcode >= 8 && len > 125)) {
        return ParseStatus::Error;
    }
    const int total = pos + 4 + int(len);
    if (buf.size() < total) {
        return ParseStatus::NeedMore;
    }

    const uchar* mask = p + pos;
    QByteArray payload(int(len), Qt::Uninitialized);
    for (int i = 0; i < int(len); ++i) {
        payload[i] = char(p[pos + 4 + i] ^ mask[i & 3]);
    }
    out->opcode = static_cast<WsOpcode>(opcode);
    out->payload = payload;
    *consumed = total;
    return ParseStatus::Ok;
}

QByteArray encodeWsFrame(WsOpcode opcode, const QByteArray& payload)
{
    QByteArray frame;
    frame.append(char(0x80 | int(opcode)));
    const qsizetype len = payload.size();
    if (len < 126) {
        frame.append(char(len));
    } else if (len <= 0xFFFF) {
        frame.append(char(126));
        frame.append(char(len >> 8));
        frame.append(char(len & 0xFF));
    } else {
        frame.append(char(127));
        for (int shift = 56; shift >= 0; shift -= 8) {
            frame.append(char((quint64(len) >> shift) & 0xFF));
        }
    }
    return frame + payload;
}

QByteArray encodeWsClose(quint16 code)
{
    QByteArray payload;
    payload.append(char(code >> 8));
    payload.append(char(code & 0xFF));
    return encodeWsFrame(WsOpcode::Close, payload);
}

bool constantTimeEquals(const QByteArray& a, const QByteArray& b)
{
    int diff = a.size() ^ b.size();
    const int n = std::max(a.size(), b.size());
    for (int i = 0; i < n; ++i) {
        diff |= (i < a.size() ? uchar(a[i]) : 0) ^ (i < b.size() ? uchar(b[i]) : 0);
    }
    return diff == 0;
}

} // namespace phone
