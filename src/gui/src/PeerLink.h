/*
 * InputLeap -- mouse and keyboard sharing utility
 * Copyright (C) 2026 InputLeafPlus Developers
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#pragma once

#include <QByteArray>
#include <QHash>
#include <QHostAddress>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QVariantMap>
#include <QVector>

#include <functional>

class QTcpServer;

namespace peerlink {

struct SwapRequest {
    QString fromName;          // sender screen name
    QString fromRole;          // "server" | "client"  (sender's role BEFORE the swap)
    QString fromFingerprint;   // sender local SHA-256 TLS fingerprint, "" if none
    QVariantMap layout;        // server layout, non-empty only when fromRole == "server"
    QString fromAddress;       // filled by the RECEIVING PeerLink: sender IPv4 as seen on the socket ("" if not IPv4)
    int port = 0;              // sender's InputLeap port setting (0 = unknown); the new client connects to the new server's
};

struct SwapReply {
    bool ok = false;
    QString why;               // Korean, shown to the user when !ok (also used for transport errors)
    QString name;              // replier screen name
    QString fingerprint;
    QVariantMap layout;        // non-empty only when the replier is currently the server
    QString address;           // filled by the REQUESTING PeerLink: replier IPv4 (the address that answered)
    int port = 0;              // replier's InputLeap port setting (0 = unknown)
    bool unreachable = false;  // filled by the REQUESTING PeerLink: no connection was made (bad name, wrong IP, offline)
};

QString generatePairingCode();            // 8 chars from an unambiguous alphabet, QRandomGenerator::system()
bool isValidPairingCode(const QString&);  // >= 8 chars
QByteArray deriveKey(const QString& code); // PBKDF2 stretch (slow, ~tens of ms); the HMAC key for all proofs

// Pure protocol helpers (no sockets), unit tested.
QByteArray hmacHex(const QByteArray& key, const QByteArray& message);   // lowercase hex HMAC-SHA256
bool constantTimeEquals(const QByteArray& a, const QByteArray& b);
bool isHexNonce(const QByteArray& nonce);                               // exactly 32 lowercase/uppercase hex chars
QString ipv4String(const QHostAddress& address);                        // "" unless IPv4 or IPv4-mapped IPv6

// Splits one '\n' terminated line off the front of buffer. Returns false with *overflow=false if the
// line is not complete yet, false with *overflow=true if it is (or would be) longer than maxLine.
constexpr int kMaxLine = 512 * 1024;
bool takeLine(QByteArray& buffer, QByteArray* line, bool* overflow, int maxLine = kMaxLine);

// JSON text of the messages (the exact bytes covered by the MAC).
QByteArray encodeRequest(const SwapRequest& request);
QByteArray encodeReply(const SwapReply& reply);
bool decodeRequest(const QByteArray& json, SwapRequest* request);       // false on malformed/out-of-range input
bool decodeReply(const QByteArray& json, SwapReply* reply);

// 3 authentication failures within 60 s from one IP => that IP is refused for 60 s.
class FailureLimiter {
public:
    bool blocked(const QString& ip, qint64 nowMs);
    void fail(const QString& ip, qint64 nowMs);
private:
    struct Entry { QVector<qint64> times; qint64 blockedUntil = 0; };
    QHash<QString, Entry> entries_;
};

} // namespace peerlink

class PeerLink : public QObject {
    Q_OBJECT
public:
    using Handler = std::function<peerlink::SwapReply(const peerlink::SwapRequest&)>;

    explicit PeerLink(QObject* parent = nullptr);
    ~PeerLink() override;

    bool listen(quint16 port, const QString& pairingCode);  // false => errorString(); invalid code => false
    void close();
    bool isListening() const;
    QString errorString() const;
    quint16 serverPort() const;                              // bound port (0 when not listening), for tests
    void setHandler(Handler handler);                        // invoked on the GUI thread for each AUTHENTICATED swap request

    // done() is called exactly once on the GUI thread (success, auth failure, timeout, refused connection, ...)
    void requestSwap(const QString& host, quint16 port, const QString& pairingCode,
                     const peerlink::SwapRequest& request, std::function<void(const peerlink::SwapReply&)> done);

private:
    void onNewConnection();

    QTcpServer* server_ = nullptr;
    QByteArray pairingKey_;           // deriveKey(pairing code)
    QString error_;
    Handler handler_;
    bool busy_ = false;
    int pending_ = 0;                 // incoming connections still in the handshake
    peerlink::FailureLimiter failures_;
};
