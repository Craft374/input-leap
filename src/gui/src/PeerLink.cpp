/*
 * InputLeap -- mouse and keyboard sharing utility
 * Copyright (C) 2026 InputLeafPlus Developers
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "PeerLink.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonValue>
#include <QMessageAuthenticationCode>
#include <QPasswordDigestor>
#include <QPointer>
#include <QRandomGenerator>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include <cmath>
#include <memory>

namespace peerlink {

namespace {

constexpr int kHandshakeMs = 5000;      // connect + handshake
constexpr int kReplyMs = 45000;         // receiver may block its GUI (modal dialogs) for a while
constexpr int kMaxPendingIncoming = 16;
constexpr int kMaxFields = 10000;       // layout keys (ServerConfig::fromVariantMap validates semantics)
constexpr int kMaxText = 1024;

QByteArray randomNonce()
{
    quint32 words[4];
    QRandomGenerator::system()->fillRange(words);
    return QByteArray(reinterpret_cast<const char*>(words), sizeof(words)).toHex();
}

QJsonObject toJson(const QVariantMap& map)
{
    QJsonObject object;
    for (auto it = map.constBegin(); it != map.constEnd(); ++it)
        object.insert(it.key(), QJsonValue::fromVariant(it.value()));
    return object;
}

// Scalars only: bool, string, number (integral values come back as int so QSettings round trips).
bool fromJson(const QJsonObject& object, QVariantMap* out)
{
    if (object.size() > kMaxFields)
        return false;
    QVariantMap map;
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        const QJsonValue& v = it.value();
        if (v.isBool()) {
            map.insert(it.key(), v.toBool());
        } else if (v.isString()) {
            map.insert(it.key(), v.toString());
        } else if (v.isDouble()) {
            const double d = v.toDouble();
            if (!std::isfinite(d))
                return false;
            if (d == std::floor(d) && std::fabs(d) <= 2147483647.0)
                map.insert(it.key(), static_cast<int>(d));
            else
                map.insert(it.key(), d);
        } else {
            return false;
        }
    }
    *out = map;
    return true;
}

bool shortText(const QJsonValue& v, QString* out, int maxLen)
{
    if (!v.isString() || v.toString().size() > maxLen)
        return false;
    *out = v.toString();
    return true;
}

QString ipKey(const QHostAddress& address)
{
    const QString v4 = ipv4String(address);
    return v4.isEmpty() ? address.toString() : v4;
}

QByteArray sessionKey(const QByteArray& code, const QByteArray& nc, const QByteArray& ns)
{
    return hmacHex(code, "K|" + nc + "|" + ns);
}

void sendLine(QTcpSocket* socket, const QJsonObject& object)
{
    socket->write(QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n');
}

void sendErrorAndClose(QTcpSocket* socket, const QString& message)
{
    sendLine(socket, QJsonObject{{"t", "error"}, {"m", message}});
    socket->disconnectFromHost();
}

} // namespace

QString generatePairingCode()
{
    static const char alphabet[] = "abcdefghjkmnpqrstuvwxyz23456789";
    constexpr int n = sizeof(alphabet) - 1;
    QString code;
    for (int i = 0; i < 8; ++i)
        code += QLatin1Char(alphabet[QRandomGenerator::system()->bounded(n)]);
    return code;
}

bool isValidPairingCode(const QString& code)
{
    return code.size() >= 8;
}

// The 8 char code is brute-forceable offline from one hello/proof pair; PBKDF2 makes every guess cost ~60000 hashes.
QByteArray deriveKey(const QString& code)
{
    return QPasswordDigestor::deriveKeyPbkdf2(QCryptographicHash::Sha256, code.toUtf8(),
                                              "inputleafplus-peerlink-v1", 60000, 32);
}

QByteArray hmacHex(const QByteArray& key, const QByteArray& message)
{
    return QMessageAuthenticationCode::hash(message, key, QCryptographicHash::Sha256).toHex();
}

bool constantTimeEquals(const QByteArray& a, const QByteArray& b)
{
    if (a.size() != b.size())
        return false;
    unsigned char diff = 0;
    for (int i = 0; i < a.size(); ++i)
        diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
    return diff == 0;
}

bool isHexNonce(const QByteArray& nonce)
{
    if (nonce.size() != 32)
        return false;
    for (char c : nonce) {
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!hex)
            return false;
    }
    return true;
}

QString ipv4String(const QHostAddress& address)
{
    bool ok = false;
    const quint32 v4 = address.toIPv4Address(&ok);
    return ok ? QHostAddress(v4).toString() : QString();
}

bool takeLine(QByteArray& buffer, QByteArray* line, bool* overflow, int maxLine)
{
    *overflow = false;
    const int end = buffer.indexOf('\n');
    if (end < 0 ? buffer.size() > maxLine : end > maxLine) {
        *overflow = true;
        return false;
    }
    if (end < 0)
        return false;
    *line = buffer.left(end);
    buffer.remove(0, end + 1);
    return true;
}

QByteArray encodeRequest(const SwapRequest& request)
{
    return QJsonDocument(QJsonObject{{"name", request.fromName}, {"role", request.fromRole},
                                     {"fp", request.fromFingerprint}, {"layout", toJson(request.layout)}})
        .toJson(QJsonDocument::Compact);
}

QByteArray encodeReply(const SwapReply& reply)
{
    return QJsonDocument(QJsonObject{{"ok", reply.ok}, {"why", reply.why.left(kMaxText)}, {"name", reply.name},
                                     {"fp", reply.fingerprint}, {"layout", toJson(reply.layout)}})
        .toJson(QJsonDocument::Compact);
}

bool decodeRequest(const QByteArray& json, SwapRequest* request)
{
    const QJsonObject o = QJsonDocument::fromJson(json).object();
    SwapRequest r;
    if (!shortText(o.value("name"), &r.fromName, 256) || r.fromName.isEmpty()
        || !shortText(o.value("role"), &r.fromRole, 16)
        || (r.fromRole != "server" && r.fromRole != "client")
        || !shortText(o.value("fp"), &r.fromFingerprint, 256)
        || !o.value("layout").isObject() || !fromJson(o.value("layout").toObject(), &r.layout))
        return false;
    if (r.fromRole != "server")
        r.layout.clear();
    *request = r;
    return true;
}

bool decodeReply(const QByteArray& json, SwapReply* reply)
{
    const QJsonObject o = QJsonDocument::fromJson(json).object();
    SwapReply r;
    if (!o.value("ok").isBool() || !shortText(o.value("why"), &r.why, kMaxText)
        || !shortText(o.value("name"), &r.name, 256) || !shortText(o.value("fp"), &r.fingerprint, 256)
        || !o.value("layout").isObject() || !fromJson(o.value("layout").toObject(), &r.layout))
        return false;
    r.ok = o.value("ok").toBool();
    *reply = r;
    return true;
}

bool FailureLimiter::blocked(const QString& ip, qint64 nowMs)
{
    const auto it = entries_.constFind(ip);
    return it != entries_.constEnd() && it->blockedUntil > nowMs;
}

void FailureLimiter::fail(const QString& ip, qint64 nowMs)
{
    // ponytail: wholesale reset bounds memory against many source IPs; per-IP LRU if that ever matters
    if (entries_.size() >= 4096)
        entries_.clear();
    Entry& e = entries_[ip];
    e.times.append(nowMs);
    while (!e.times.isEmpty() && e.times.first() <= nowMs - 60000)
        e.times.removeFirst();
    if (e.times.size() >= 3) {
        e.blockedUntil = nowMs + 60000;
        e.times.clear();
    }
}

} // namespace peerlink

using namespace peerlink;

PeerLink::PeerLink(QObject* parent) : QObject(parent) {}

PeerLink::~PeerLink() { close(); }

bool PeerLink::listen(quint16 port, const QString& pairingCode)
{
    close();
    if (!isValidPairingCode(pairingCode)) {
        error_ = tr("페어링 코드는 8자 이상이어야 합니다");
        return false;
    }
    pairingKey_ = deriveKey(pairingCode);   // once here, never per hello
    server_ = new QTcpServer(this);
    connect(server_, &QTcpServer::newConnection, this, &PeerLink::onNewConnection);
    if (!server_->listen(QHostAddress::Any, port)) {
        error_ = tr("포트 %1 을(를) 열 수 없습니다: %2").arg(port).arg(server_->errorString());
        delete server_;
        server_ = nullptr;
        return false;
    }
    error_.clear();
    return true;
}

void PeerLink::close()
{
    if (!server_)
        return;
    server_->close();
    for (QTcpSocket* socket : server_->findChildren<QTcpSocket*>()) {
        socket->abort();
        socket->deleteLater();
    }
    server_->deleteLater();   // may be called from inside a handler's nested event loop
    server_ = nullptr;   // aborted sockets release their pending_ slot themselves
}

bool PeerLink::isListening() const { return server_ && server_->isListening(); }

QString PeerLink::errorString() const { return error_; }

quint16 PeerLink::serverPort() const { return server_ ? server_->serverPort() : 0; }

void PeerLink::setHandler(Handler handler) { handler_ = std::move(handler); }

namespace {
struct ServerConn {
    QByteArray code, buf, nc, ns;
    int stage = 0;          // 0 await hello, 1 await request, 2 request accepted
    bool counted = true;    // still counted in PeerLink::pending_
};
}

void PeerLink::onNewConnection()
{
    while (server_ && server_->hasPendingConnections()) {
        QTcpSocket* sock = server_->nextPendingConnection();
        const QString ip = ipKey(sock->peerAddress());
        if (failures_.blocked(ip, QDateTime::currentMSecsSinceEpoch()) || pending_ >= kMaxPendingIncoming) {
            sock->abort();
            sock->deleteLater();
            continue;
        }
        ++pending_;

        auto st = std::make_shared<ServerConn>();
        st->code = pairingKey_;
        QPointer<PeerLink> self(this);
        auto release = [self, st] {
            if (st->counted && self) {
                st->counted = false;
                --self->pending_;
            }
        };

        auto* timer = new QTimer(sock);
        timer->setSingleShot(true);
        connect(timer, &QTimer::timeout, sock, &QTcpSocket::abort);
        timer->start(kHandshakeMs);

        connect(sock, &QTcpSocket::disconnected, sock, &QObject::deleteLater);
        connect(sock, &QTcpSocket::disconnected, this, release);
        connect(sock, &QObject::destroyed, this, release);
        connect(sock, &QTcpSocket::readyRead, this, [this, self, sock, st, timer, ip, release] {
            if (st->stage >= 2) {
                sock->readAll();    // one request per connection
                return;
            }
            st->buf += sock->readAll();
            QByteArray line;
            bool overflow = false;
            while (st->stage < 2 && takeLine(st->buf, &line, &overflow)) {
                const QJsonObject o = QJsonDocument::fromJson(line).object();
                const QString type = o.value("t").toString();
                if (st->stage == 0) {
                    const QByteArray nc = o.value("nc").toString().toLatin1();
                    if (type != "hello" || !isHexNonce(nc)) {
                        sendErrorAndClose(sock, tr("잘못된 요청"));
                        return;
                    }
                    st->nc = nc;
                    st->ns = randomNonce();
                    sendLine(sock, QJsonObject{{"t", "challenge"}, {"ns", QString::fromLatin1(st->ns)},
                                               {"proof", QString::fromLatin1(hmacHex(st->code, "S|" + st->nc + "|" + st->ns))}});
                    st->stage = 1;
                    continue;
                }
                // stage 1: the request
                const QByteArray body = o.value("body").toString().toUtf8();
                const QByteArray key = sessionKey(st->code, st->nc, st->ns);
                const bool authed = type == "request"
                    && constantTimeEquals(o.value("proof").toString().toLatin1(), hmacHex(st->code, "C|" + st->ns + "|" + st->nc))
                    && constantTimeEquals(o.value("mac").toString().toLatin1(), hmacHex(key, "Q|" + body));
                if (!authed) {
                    failures_.fail(ip, QDateTime::currentMSecsSinceEpoch());
                    sendErrorAndClose(sock, tr("페어링 코드 불일치"));
                    return;
                }
                st->stage = 2;
                timer->stop();
                release();
                SwapRequest request;
                const Handler handler = handler_;
                if (busy_) {
                    sendErrorAndClose(sock, tr("상대가 다른 교환을 처리 중"));
                    return;
                }
                if (!decodeRequest(body, &request)) {
                    sendErrorAndClose(sock, tr("잘못된 요청"));
                    return;
                }
                if (!handler) {
                    sendErrorAndClose(sock, tr("상대가 교환을 처리할 수 없는 상태입니다"));
                    return;
                }
                request.fromAddress = ipv4String(sock->peerAddress());
                QPointer<QTcpSocket> guard(sock);
                busy_ = true;
                const SwapReply reply = handler(request);   // may run a nested event loop
                if (!self)
                    return;
                busy_ = false;
                if (!guard || guard->state() != QAbstractSocket::ConnectedState)
                    return;
                const QByteArray replyBody = encodeReply(reply);
                sendLine(guard, QJsonObject{{"t", "reply"}, {"body", QString::fromUtf8(replyBody)},
                                            {"mac", QString::fromLatin1(hmacHex(key, "R|" + replyBody))}});
                guard->disconnectFromHost();
                return;
            }
            if (overflow)
                sendErrorAndClose(sock, tr("요청이 너무 깁니다"));
        });
    }
}

namespace {
struct ClientJob {
    QByteArray code, buf, nc, ns, requestBody;
    int stage = 0;          // 0 await challenge, 1 await reply
    bool finished = false;
    bool connected = false;
    std::function<void(const SwapReply&)> done;
};
}

void PeerLink::requestSwap(const QString& host, quint16 port, const QString& pairingCode,
                           const SwapRequest& request, std::function<void(const SwapReply&)> done)
{
    if (!isValidPairingCode(pairingCode)) {
        SwapReply r;
        r.why = tr("페어링 코드는 8자 이상이어야 합니다");
        QTimer::singleShot(0, [done, r] { done(r); });   // never call back re-entrantly
        return;
    }

    auto job = std::make_shared<ClientJob>();
    job->code = deriveKey(pairingCode);
    job->requestBody = encodeRequest(request);
    job->done = std::move(done);

    auto* sock = new QTcpSocket();   // owns itself: deleted by finish()
    auto* timer = new QTimer(sock);
    timer->setSingleShot(true);

    auto finish = [job, sock](const QString& why, const SwapReply& result = SwapReply()) {
        if (job->finished)
            return;
        job->finished = true;
        SwapReply reply = result;
        if (!reply.ok)
            reply.why = why.isEmpty() ? reply.why : why;
        reply.address = ipv4String(sock->peerAddress());
        reply.unreachable = !job->connected;
        sock->abort();
        sock->deleteLater();
        job->done(reply);
    };

    connect(timer, &QTimer::timeout, sock, [finish] { finish(QObject::tr("응답 없음")); });
    connect(sock, &QTcpSocket::errorOccurred, sock, [finish, sock] {
        finish(QObject::tr("연결할 수 없습니다: %1").arg(sock->errorString()));
    });
    connect(sock, &QTcpSocket::disconnected, sock, [finish] { finish(QObject::tr("연결이 끊어졌습니다")); });
    connect(sock, &QTcpSocket::connected, sock, [job, sock] {
        job->connected = true;
        job->nc = randomNonce();
        sendLine(sock, QJsonObject{{"t", "hello"}, {"nc", QString::fromLatin1(job->nc)}});
    });
    connect(sock, &QTcpSocket::readyRead, sock, [job, sock, timer, finish] {
        job->buf += sock->readAll();
        QByteArray line;
        bool overflow = false;
        while (!job->finished && takeLine(job->buf, &line, &overflow)) {
            const QJsonObject o = QJsonDocument::fromJson(line).object();
            const QString type = o.value("t").toString();
            if (type == "error") {
                finish(o.value("m").toString().left(kMaxText));
                return;
            }
            if (job->stage == 0) {
                const QByteArray ns = o.value("ns").toString().toLatin1();
                if (type != "challenge" || !isHexNonce(ns)
                    || !constantTimeEquals(o.value("proof").toString().toLatin1(),
                                           hmacHex(job->code, "S|" + job->nc + "|" + ns))) {
                    finish(QObject::tr("페어링 코드 불일치"));
                    return;
                }
                job->ns = ns;
                const QByteArray key = sessionKey(job->code, job->nc, ns);
                sendLine(sock, QJsonObject{{"t", "request"},
                                           {"proof", QString::fromLatin1(hmacHex(job->code, "C|" + ns + "|" + job->nc))},
                                           {"body", QString::fromUtf8(job->requestBody)},
                                           {"mac", QString::fromLatin1(hmacHex(key, "Q|" + job->requestBody))}});
                job->stage = 1;
                timer->start(kReplyMs);
                continue;
            }
            const QByteArray body = o.value("body").toString().toUtf8();
            const QByteArray key = sessionKey(job->code, job->nc, job->ns);
            SwapReply reply;
            if (type != "reply"
                || !constantTimeEquals(o.value("mac").toString().toLatin1(), hmacHex(key, "R|" + body))
                || !decodeReply(body, &reply)) {
                finish(QObject::tr("응답이 올바르지 않습니다"));
                return;
            }
            finish(QString(), reply);
            return;
        }
        if (overflow)
            finish(QObject::tr("응답이 너무 깁니다"));
    });

    timer->start(kHandshakeMs);
    sock->connectToHost(host, port);
}
