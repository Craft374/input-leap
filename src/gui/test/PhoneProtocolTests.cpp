/*
 * InputLeap -- mouse and keyboard sharing utility
 * Copyright (C) 2026 InputLeafPlus Developers
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "../src/PhoneProtocol.h"
#include <gtest/gtest.h>

using namespace phone;

namespace {

QByteArray maskedFrame(int firstByte, const QByteArray& payload, bool mask = true)
{
    const char key[4] = {0x37, char(0xfa), 0x21, 0x3d};
    QByteArray f;
    f.append(char(firstByte));
    const int maskBit = mask ? 0x80 : 0;
    if (payload.size() < 126) {
        f.append(char(maskBit | payload.size()));
    } else {
        f.append(char(maskBit | 126));
        f.append(char(payload.size() >> 8));
        f.append(char(payload.size() & 0xFF));
    }
    if (mask) {
        f.append(key, 4);
    }
    for (int i = 0; i < payload.size(); ++i) {
        f.append(mask ? char(payload[i] ^ key[i & 3]) : payload[i]);
    }
    return f;
}

const char* kUpgrade =
    "GET /ws?x=1 HTTP/1.1\r\nHost: a\r\nUpgrade: WebSocket\r\nConnection: keep-alive, Upgrade\r\n"
    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";

} // namespace

TEST(PhoneProtocolTests, acceptKeyMatchesRfc6455Example)
{
    EXPECT_EQ(websocketAcceptKey("dGhlIHNhbXBsZSBub25jZQ=="), QByteArray("s3pPLMBiTxaQ9kYGzzhZRbK+xOo="));
}

TEST(PhoneProtocolTests, httpHeadParsesAndStripsQuery)
{
    HttpRequest req;
    int used = 0;
    const QByteArray buf = QByteArray(kUpgrade) + "extra";
    ASSERT_EQ(parseHttpRequestHead(buf, &req, &used), ParseStatus::Ok);
    EXPECT_EQ(used, buf.size() - 5);
    EXPECT_EQ(req.method, QByteArray("GET"));
    EXPECT_EQ(req.path, QByteArray("/ws"));
    EXPECT_EQ(req.headers.value("host"), QByteArray("a"));
    EXPECT_TRUE(isWebSocketUpgrade(req));
}

TEST(PhoneProtocolTests, httpHeadIncrementalAndErrors)
{
    HttpRequest req;
    int used = 0;
    EXPECT_EQ(parseHttpRequestHead("GET / HTTP/1.1\r\nHost: a\r\n", &req, &used), ParseStatus::NeedMore);
    EXPECT_EQ(parseHttpRequestHead("POST / HTTP/1.1\r\n\r\n", &req, &used), ParseStatus::Error);
    EXPECT_EQ(parseHttpRequestHead("GET / HTTP/2\r\n\r\n", &req, &used), ParseStatus::Error);
    EXPECT_EQ(parseHttpRequestHead("GET nopath HTTP/1.1\r\n\r\n", &req, &used), ParseStatus::Error);
    EXPECT_EQ(parseHttpRequestHead("GET / HTTP/1.1\r\nNoColon\r\n\r\n", &req, &used), ParseStatus::Error);
    EXPECT_EQ(parseHttpRequestHead("GET / HTTP/1.1\r\nBad : x\r\n\r\n", &req, &used), ParseStatus::Error);
    // never terminated and too large => Error instead of buffering forever
    EXPECT_EQ(parseHttpRequestHead("GET / HTTP/1.1\r\nX: " + QByteArray(kMaxHttpHead, 'a'), &req, &used),
              ParseStatus::Error);
    // terminated but larger than the cap
    EXPECT_EQ(parseHttpRequestHead("GET / HTTP/1.1\r\nX: " + QByteArray(kMaxHttpHead, 'a') + "\r\n\r\n", &req, &used),
              ParseStatus::Error);
    ASSERT_EQ(parseHttpRequestHead("GET / HTTP/1.0\r\n\r\n", &req, &used), ParseStatus::Ok);
    EXPECT_FALSE(isWebSocketUpgrade(req));
}

TEST(PhoneProtocolTests, upgradeRequiresAllHeaders)
{
    HttpRequest req;
    int used = 0;
    QByteArray bad = kUpgrade;
    bad.replace("Version: 13", "Version: 8");
    ASSERT_EQ(parseHttpRequestHead(bad, &req, &used), ParseStatus::Ok);
    EXPECT_FALSE(isWebSocketUpgrade(req));
    bad = kUpgrade;
    bad.replace("dGhlIHNhbXBsZSBub25jZQ==", "c2hvcnQ=");
    ASSERT_EQ(parseHttpRequestHead(bad, &req, &used), ParseStatus::Ok);
    EXPECT_FALSE(isWebSocketUpgrade(req));
}

TEST(PhoneProtocolTests, decodesRfcMaskedHello)
{
    // RFC 6455 section 5.7: masked "Hello"
    const QByteArray frame = QByteArray::fromHex("818537fa213d7f9f4d5158");
    WsFrame out;
    int used = 0;
    ASSERT_EQ(decodeWsFrame(frame, &out, &used), ParseStatus::Ok);
    EXPECT_EQ(used, frame.size());
    EXPECT_EQ(out.opcode, WsOpcode::Text);
    EXPECT_EQ(out.payload, QByteArray("Hello"));
}

TEST(PhoneProtocolTests, decodesIncrementallyAndBackToBack)
{
    const QByteArray a = maskedFrame(0x81, "one");
    const QByteArray b = maskedFrame(0x89, "p");
    const QByteArray both = a + b;
    WsFrame out;
    int used = 0;
    for (int i = 0; i < a.size(); ++i) {
        EXPECT_EQ(decodeWsFrame(both.left(i), &out, &used), ParseStatus::NeedMore) << i;
    }
    ASSERT_EQ(decodeWsFrame(both, &out, &used), ParseStatus::Ok);
    EXPECT_EQ(used, a.size());
    EXPECT_EQ(out.payload, QByteArray("one"));
    ASSERT_EQ(decodeWsFrame(both.mid(used), &out, &used), ParseStatus::Ok);
    EXPECT_EQ(out.opcode, WsOpcode::Ping);
    EXPECT_EQ(out.payload, QByteArray("p"));
}

TEST(PhoneProtocolTests, decodesExtendedLength)
{
    const QByteArray payload(300, 'x');
    WsFrame out;
    int used = 0;
    ASSERT_EQ(decodeWsFrame(maskedFrame(0x81, payload), &out, &used), ParseStatus::Ok);
    EXPECT_EQ(out.payload, payload);
}

TEST(PhoneProtocolTests, rejectsBadFrames)
{
    WsFrame out;
    int used = 0;
    EXPECT_EQ(decodeWsFrame(maskedFrame(0x81, "x", false), &out, &used), ParseStatus::Error);   // unmasked
    EXPECT_EQ(decodeWsFrame(maskedFrame(0x01, "x"), &out, &used), ParseStatus::Error);          // FIN=0
    EXPECT_EQ(decodeWsFrame(maskedFrame(0x80, "x"), &out, &used), ParseStatus::Error);          // continuation
    EXPECT_EQ(decodeWsFrame(maskedFrame(0x82, "x"), &out, &used), ParseStatus::Error);          // binary
    EXPECT_EQ(decodeWsFrame(maskedFrame(0xC1, "x"), &out, &used), ParseStatus::Error);          // RSV1
    EXPECT_EQ(decodeWsFrame(maskedFrame(0x89, QByteArray(126, 'x')), &out, &used), ParseStatus::Error);   // big ping
    // announced 64 KiB + 1 is refused from the header alone (no payload bytes present)
    QByteArray huge = QByteArray::fromHex("81ff") + QByteArray::fromHex("0000000000010001");
    EXPECT_EQ(decodeWsFrame(huge, &out, &used), ParseStatus::Error);
    // exactly 64 KiB is accepted once complete, but is only NeedMore until then
    QByteArray limit = QByteArray::fromHex("81ff") + QByteArray::fromHex("0000000000010000");
    EXPECT_EQ(decodeWsFrame(limit, &out, &used), ParseStatus::NeedMore);
    // 64-bit length with the top bit set
    EXPECT_EQ(decodeWsFrame(QByteArray::fromHex("81ff8000000000000000"), &out, &used), ParseStatus::Error);
}

TEST(PhoneProtocolTests, encodesServerFrames)
{
    EXPECT_EQ(encodeWsFrame(WsOpcode::Text, "hi"), QByteArray::fromHex("81026869"));
    EXPECT_EQ(encodeWsFrame(WsOpcode::Pong, ""), QByteArray::fromHex("8a00"));
    EXPECT_EQ(encodeWsClose(1002), QByteArray::fromHex("880203ea"));
    const QByteArray mid = encodeWsFrame(WsOpcode::Text, QByteArray(300, 'a'));
    EXPECT_EQ(mid.left(4), QByteArray::fromHex("817e012c"));
    EXPECT_EQ(mid.size(), 304);
    const QByteArray big = encodeWsFrame(WsOpcode::Text, QByteArray(70000, 'a'));
    EXPECT_EQ(big.left(10), QByteArray::fromHex("817f0000000000011170"));
    EXPECT_EQ(big.size(), 70010);
}

TEST(PhoneProtocolTests, constantTimeEquals)
{
    EXPECT_TRUE(constantTimeEquals("123456", "123456"));
    EXPECT_FALSE(constantTimeEquals("123456", "123457"));
    EXPECT_FALSE(constantTimeEquals("123456", "12345"));
    EXPECT_FALSE(constantTimeEquals("", "x"));
    EXPECT_TRUE(constantTimeEquals("", ""));
}
