/*
 * InputLeap -- mouse and keyboard sharing utility
 * Copyright (C) 2026 InputLeafPlus Developers
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "../src/PeerLink.h"
#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>

using namespace peerlink;

namespace {

// The loopback tests need an event loop. Reuse an existing application object if some other test made one.
void ensureApp()
{
    static int argc = 1;
    static char arg0[] = "guiunittests";
    static char* argv[] = {arg0, nullptr};
    if (!QCoreApplication::instance()) {
        static QCoreApplication app(argc, argv);
        (void) app;
    }
}

SwapReply swap(PeerLink& client, quint16 port, const QString& code, const SwapRequest& request, int* calls = nullptr)
{
    SwapReply out;
    int count = 0;
    QEventLoop loop;
    QTimer::singleShot(10000, &loop, &QEventLoop::quit);
    client.requestSwap("127.0.0.1", port, code, request, [&](const SwapReply& r) {
        out = r;
        ++count;
        loop.quit();
    });
    loop.exec();
    // let any stray second callback happen before we read count
    QEventLoop settle;
    QTimer::singleShot(100, &settle, &QEventLoop::quit);
    settle.exec();
    if (calls)
        *calls = count;
    return out;
}

} // namespace

TEST(PeerLinkTests, HmacMatchesRfc4231)
{
    EXPECT_EQ(hmacHex(QByteArray(20, '\x0b'), "Hi There"),
              QByteArray("b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"));
    EXPECT_EQ(hmacHex("Jefe", "what do ya want for nothing?"),
              QByteArray("5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"));
}

TEST(PeerLinkTests, ConstantTimeEquals)
{
    EXPECT_TRUE(constantTimeEquals("abc", "abc"));
    EXPECT_TRUE(constantTimeEquals("", ""));
    EXPECT_FALSE(constantTimeEquals("abc", "abd"));
    EXPECT_FALSE(constantTimeEquals("abc", "ab"));
}

TEST(PeerLinkTests, HexNonce)
{
    EXPECT_TRUE(isHexNonce("00112233445566778899aabbccddeeff"));
    EXPECT_TRUE(isHexNonce("00112233445566778899AABBCCDDEEFF"));
    EXPECT_FALSE(isHexNonce("00112233445566778899aabbccddeef"));
    EXPECT_FALSE(isHexNonce("00112233445566778899aabbccddeefff"));
    EXPECT_FALSE(isHexNonce("00112233445566778899aabbccddeefg"));
    EXPECT_FALSE(isHexNonce(""));
}

TEST(PeerLinkTests, PairingCode)
{
    const QString alphabet = "abcdefghjkmnpqrstuvwxyz23456789";
    QString previous;
    for (int i = 0; i < 50; ++i) {
        const QString code = generatePairingCode();
        ASSERT_EQ(code.size(), 8);
        for (QChar c : code)
            EXPECT_TRUE(alphabet.contains(c)) << qPrintable(code);
        EXPECT_TRUE(isValidPairingCode(code));
        previous = code;
    }
    EXPECT_FALSE(isValidPairingCode("abcdefg"));
    EXPECT_FALSE(isValidPairingCode(""));
    EXPECT_TRUE(isValidPairingCode("abcdefgh"));
}

TEST(PeerLinkTests, DeriveKey)
{
    const QByteArray key = deriveKey("abcd2345");
    EXPECT_EQ(key.size(), 32);
    EXPECT_EQ(key, deriveKey("abcd2345"));
    EXPECT_NE(key, deriveKey("abcd2346"));
    EXPECT_NE(key, QByteArray("abcd2345"));
}

TEST(PeerLinkTests, Ipv4String)
{
    EXPECT_EQ(ipv4String(QHostAddress("192.168.0.5")), QString("192.168.0.5"));
    EXPECT_EQ(ipv4String(QHostAddress("::ffff:192.168.0.5")), QString("192.168.0.5"));
    EXPECT_EQ(ipv4String(QHostAddress("fe80::1")), QString());
    EXPECT_EQ(ipv4String(QHostAddress()), QString());
}

TEST(PeerLinkTests, TakeLine)
{
    QByteArray buf = "abc";
    QByteArray line;
    bool overflow = true;
    EXPECT_FALSE(takeLine(buf, &line, &overflow, 10));
    EXPECT_FALSE(overflow);
    buf += "d\nsecond\nrest";
    EXPECT_TRUE(takeLine(buf, &line, &overflow, 10));
    EXPECT_EQ(line, QByteArray("abcd"));
    EXPECT_TRUE(takeLine(buf, &line, &overflow, 10));
    EXPECT_EQ(line, QByteArray("second"));
    EXPECT_FALSE(takeLine(buf, &line, &overflow, 10));
    EXPECT_FALSE(overflow);
    EXPECT_EQ(buf, QByteArray("rest"));

    QByteArray big(11, 'x');             // no newline and longer than the limit
    EXPECT_FALSE(takeLine(big, &line, &overflow, 10));
    EXPECT_TRUE(overflow);
    QByteArray bigLine(11, 'x');         // complete but too long
    bigLine += '\n';
    EXPECT_FALSE(takeLine(bigLine, &line, &overflow, 10));
    EXPECT_TRUE(overflow);
    QByteArray exact(10, 'x');
    exact += '\n';
    EXPECT_TRUE(takeLine(exact, &line, &overflow, 10));
    EXPECT_EQ(line.size(), 10);
}

TEST(PeerLinkTests, RequestRoundTrip)
{
    SwapRequest in;
    in.fromName = QString::fromUtf8("맥");
    in.fromRole = "server";
    in.fromFingerprint = "AB:CD";
    in.layout = {{"numColumns", 3}, {"flag", true}, {"name", QString::fromUtf8("화면")}, {"ratio", 0.5}};
    in.port = 24798;
    SwapRequest out;
    ASSERT_TRUE(decodeRequest(encodeRequest(in), &out));
    EXPECT_EQ(out.port, 24798);
    EXPECT_EQ(out.fromName, in.fromName);
    EXPECT_EQ(out.fromRole, QString("server"));
    EXPECT_EQ(out.fromFingerprint, in.fromFingerprint);
    EXPECT_EQ(out.layout.size(), 4);
    EXPECT_EQ(out.layout["numColumns"].userType(), QMetaType::Int);
    EXPECT_EQ(out.layout["numColumns"].toInt(), 3);
    EXPECT_TRUE(out.layout["flag"].toBool());
    EXPECT_EQ(out.layout["flag"].userType(), QMetaType::Bool);
    EXPECT_EQ(out.layout["name"].toString(), in.layout["name"].toString());
    EXPECT_DOUBLE_EQ(out.layout["ratio"].toDouble(), 0.5);
    EXPECT_TRUE(out.fromAddress.isEmpty());   // never transmitted
}

TEST(PeerLinkTests, RequestRejectsBadInput)
{
    SwapRequest out;
    EXPECT_FALSE(decodeRequest("", &out));
    EXPECT_FALSE(decodeRequest("[]", &out));
    EXPECT_FALSE(decodeRequest(R"({"name":"a","role":"admin","fp":"","layout":{}})", &out));
    EXPECT_FALSE(decodeRequest(R"({"name":"","role":"server","fp":"","layout":{}})", &out));
    EXPECT_FALSE(decodeRequest(R"({"name":"a","role":"server","fp":"","layout":[]})", &out));
    EXPECT_FALSE(decodeRequest(R"({"name":"a","role":"server","fp":"","layout":{"x":{"y":1}}})", &out));
    EXPECT_FALSE(decodeRequest(R"({"name":"a","role":"server","fp":"","layout":{"x":null}})", &out));
    EXPECT_FALSE(decodeRequest(R"({"name":"a","role":"server","fp":"","layout":{"x":[1]}})", &out));
    EXPECT_FALSE(decodeRequest(QByteArray(R"({"name":")") + QByteArray(300, 'a') + R"(","role":"server","fp":"","layout":{}})", &out));
    // a missing or out-of-range port means "unknown"
    ASSERT_TRUE(decodeRequest(R"({"name":"a","role":"client","fp":"","layout":{},"port":70000})", &out));
    EXPECT_EQ(out.port, 0);
    ASSERT_TRUE(decodeRequest(R"({"name":"a","role":"client","fp":"","layout":{}})", &out));
    EXPECT_EQ(out.port, 0);
    // a client never carries a layout: dropped, not trusted
    ASSERT_TRUE(decodeRequest(R"({"name":"a","role":"client","fp":"","layout":{"x":1}})", &out));
    EXPECT_TRUE(out.layout.isEmpty());
}

TEST(PeerLinkTests, ReplyRoundTripAndReject)
{
    SwapReply in;
    in.ok = true;
    in.name = "win";
    in.fingerprint = "FF";
    in.layout = {{"a", 1}};
    SwapReply out;
    ASSERT_TRUE(decodeReply(encodeReply(in), &out));
    EXPECT_TRUE(out.ok);
    EXPECT_EQ(out.name, QString("win"));
    EXPECT_EQ(out.layout["a"].toInt(), 1);
    EXPECT_FALSE(decodeReply(R"({"ok":"yes","why":"","name":"","fp":"","layout":{}})", &out));
    EXPECT_FALSE(decodeReply(R"({"why":"","name":"","fp":"","layout":{}})", &out));
}

TEST(PeerLinkTests, FailureLimiter)
{
    FailureLimiter limiter;
    EXPECT_FALSE(limiter.blocked("1.2.3.4", 0));
    limiter.fail("1.2.3.4", 0);
    limiter.fail("1.2.3.4", 1000);
    EXPECT_FALSE(limiter.blocked("1.2.3.4", 1500));
    limiter.fail("1.2.3.4", 2000);
    EXPECT_TRUE(limiter.blocked("1.2.3.4", 2001));
    EXPECT_TRUE(limiter.blocked("1.2.3.4", 61999));
    EXPECT_FALSE(limiter.blocked("1.2.3.4", 62001));
    EXPECT_FALSE(limiter.blocked("5.6.7.8", 2001));

    // failures spread over more than 60 s never accumulate
    FailureLimiter slow;
    slow.fail("a", 0);
    slow.fail("a", 61000);
    slow.fail("a", 122000);
    EXPECT_FALSE(slow.blocked("a", 122001));
}

TEST(PeerLinkTests, LoopbackSwap)
{
    ensureApp();
    PeerLink server;
    PeerLink client;
    const QString code = "abcd2345";
    ASSERT_TRUE(server.listen(0, code)) << qPrintable(server.errorString());
    ASSERT_TRUE(server.isListening());
    ASSERT_NE(server.serverPort(), 0);

    int handlerCalls = 0;
    SwapRequest seen;
    server.setHandler([&](const SwapRequest& request) {
        ++handlerCalls;
        seen = request;
        SwapReply reply;
        reply.ok = true;
        reply.name = "win-pc";
        reply.fingerprint = "FP2";
        reply.layout = {{"numColumns", 2}, {"name", QString::fromUtf8("화면")}};
        return reply;
    });

    SwapRequest request;
    request.fromName = "mac";
    request.fromRole = "server";
    request.fromFingerprint = "FP1";
    request.layout = {{"numColumns", 3}, {"flag", true}};

    int calls = 0;
    const SwapReply reply = swap(client, server.serverPort(), code, request, &calls);
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(handlerCalls, 1);
    EXPECT_TRUE(reply.ok) << qPrintable(reply.why);
    EXPECT_EQ(reply.name, QString("win-pc"));
    EXPECT_EQ(reply.fingerprint, QString("FP2"));
    EXPECT_EQ(reply.layout["numColumns"].toInt(), 2);
    EXPECT_EQ(reply.layout["name"].toString(), QString::fromUtf8("화면"));
    EXPECT_EQ(reply.address, QString("127.0.0.1"));

    EXPECT_EQ(seen.fromName, QString("mac"));
    EXPECT_EQ(seen.fromRole, QString("server"));
    EXPECT_EQ(seen.fromFingerprint, QString("FP1"));
    EXPECT_EQ(seen.layout["numColumns"].toInt(), 3);
    EXPECT_TRUE(seen.layout["flag"].toBool());
    EXPECT_EQ(seen.fromAddress, QString("127.0.0.1"));
}

TEST(PeerLinkTests, LoopbackRefusal)
{
    ensureApp();
    PeerLink server;
    PeerLink client;
    server.setHandler([](const SwapRequest&) {
        SwapReply reply;
        reply.ok = false;
        reply.why = QString::fromUtf8("지금은 교환할 수 없습니다");
        return reply;
    });
    ASSERT_TRUE(server.listen(0, "abcd2345"));
    SwapRequest request;
    request.fromName = "mac";
    request.fromRole = "client";
    int calls = 0;
    const SwapReply reply = swap(client, server.serverPort(), "abcd2345", request, &calls);
    EXPECT_EQ(calls, 1);
    EXPECT_FALSE(reply.ok);
    EXPECT_EQ(reply.why, QString::fromUtf8("지금은 교환할 수 없습니다"));
}

TEST(PeerLinkTests, LoopbackWrongCode)
{
    ensureApp();
    PeerLink server;
    PeerLink client;
    int handlerCalls = 0;
    server.setHandler([&](const SwapRequest&) {
        ++handlerCalls;
        return SwapReply();
    });
    ASSERT_TRUE(server.listen(0, "abcd2345"));
    SwapRequest request;
    request.fromName = "mac";
    request.fromRole = "server";
    int calls = 0;
    const SwapReply reply = swap(client, server.serverPort(), "zzzz9999", request, &calls);
    EXPECT_EQ(calls, 1);
    EXPECT_FALSE(reply.ok);
    EXPECT_FALSE(reply.why.isEmpty());
    EXPECT_EQ(handlerCalls, 0);
}

TEST(PeerLinkTests, InvalidCodeAndRefusedConnection)
{
    ensureApp();
    PeerLink server;
    PeerLink client;
    EXPECT_FALSE(server.listen(0, "short"));
    EXPECT_FALSE(server.errorString().isEmpty());
    EXPECT_FALSE(server.isListening());

    SwapRequest request;
    request.fromName = "mac";
    request.fromRole = "server";
    int calls = 0;
    SwapReply reply = swap(client, 1, "short", request, &calls);   // invalid code never touches the network
    EXPECT_EQ(calls, 1);
    EXPECT_FALSE(reply.ok);

    ASSERT_TRUE(server.listen(0, "abcd2345"));
    const quint16 port = server.serverPort();
    server.close();
    EXPECT_FALSE(server.isListening());
    reply = swap(client, port, "abcd2345", request, &calls);       // nobody listens any more
    EXPECT_EQ(calls, 1);
    EXPECT_FALSE(reply.ok);
    EXPECT_FALSE(reply.why.isEmpty());
}
