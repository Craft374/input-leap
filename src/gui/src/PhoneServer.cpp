/*
 * InputLeap -- mouse and keyboard sharing utility
 * Copyright (C) 2026 InputLeafPlus Developers
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "PhoneServer.h"

#include "PhoneProtocol.h"

#include <QCryptographicHash>
#include <QFile>
#include <QHostAddress>
#include <QJsonDocument>
#include <QNetworkInterface>
#include <QTcpSocket>
#include <QTimer>

#include <cmath>

namespace {

constexpr int kMaxAuthed = 3;
constexpr int kMaxConns = 16;               // authenticated or not, so idle sockets cannot pile up
constexpr int kAuthTimeoutMs = 10000;       // a socket must authenticate within this time
constexpr int kIdleTimeoutMs = 45000;       // no data for this long => dropped (the page pings every 15 s)
constexpr int kMaxUnauthPerPeer = 3;
constexpr int kMaxGlobalFailures = 30;      // failed auths per kFailWindowMs from anywhere
constexpr int kMaxFailures = 5;
constexpr qint64 kFailWindowMs = 60000;
constexpr int kMaxMove = 2000;
constexpr int kMaxScroll = 50;
constexpr int kMaxTextChars = 500;
constexpr int kMaxFailureEntries = 256;

QJsonObject message(const char* type)
{
    QJsonObject obj;
    obj["t"] = QString::fromLatin1(type);
    return obj;
}

QJsonObject warning(const QString& text)
{
    QJsonObject obj = message("warn");
    obj["m"] = text;
    return obj;
}

// Finite JSON number; a non-numeric or non-finite value is rejected.
bool number(const QJsonObject& msg, const char* key, double* out)
{
    const QJsonValue v = msg.value(QLatin1String(key));
    if (!v.isDouble() || !std::isfinite(v.toDouble())) {
        return false;
    }
    *out = v.toDouble();
    return true;
}

bool clampedInt(const QJsonObject& msg, const char* key, int limit, int* out)
{
    double v = 0;
    if (!number(msg, key, &v)) {
        return false;
    }
    *out = int(std::lround(qBound(double(-limit), v, double(limit))));
    return true;
}

// Exact integer in [lo, hi]; anything else is rejected (no clamping).
bool exactInt(const QJsonObject& msg, const char* key, int lo, int hi, int* out)
{
    double v = 0;
    if (!number(msg, key, &v) || v != std::floor(v) || v < lo || v > hi) {
        return false;
    }
    *out = int(v);
    return true;
}

bool flag(const QJsonObject& msg, const char* key, bool* out)
{
    int v = 0;
    if (!exactInt(msg, key, 0, 1, &v)) {
        return false;
    }
    *out = v == 1;
    return true;
}

} // namespace

PhoneServer::PhoneServer(QObject* parent)
    : QObject(parent), injector_(phone::createInputInjector())
{
    clock_.start();
    connect(&server_, &QTcpServer::newConnection, this, &PhoneServer::onNewConnection);
}

PhoneServer::~PhoneServer()
{
    stop();
}

bool PhoneServer::start(quint16 port, const QString& pin)
{
    stop();
    if (pin.size() < 4 || pin.size() > 16) {
        error_ = tr("PIN은 4~16자여야 합니다");
        return false;
    }
    if (!server_.listen(QHostAddress::Any, port)) {
        error_ = tr("포트 %1을(를) 열 수 없습니다: %2").arg(port).arg(server_.errorString());
        return false;
    }
    pin_ = pin.toUtf8();
    error_.clear();
    return true;
}

void PhoneServer::stop()
{
    server_.close();
    const QList<QTcpSocket*> sockets = conns_.keys();
    for (QTcpSocket* s : sockets) {
        s->disconnect(this);        // no onDisconnected callbacks while we tear down
        s->abort();
        s->deleteLater();
    }
    conns_.clear();
    failures_.clear();
    globalFail_ = Failures();
    injector_->releaseAll();
    shiftHeld_ = false;
    if (authedCount_ != 0) {
        authedCount_ = 0;
        Q_EMIT clientCountChanged(0);
    }
}

bool PhoneServer::isRunning() const { return server_.isListening(); }
QString PhoneServer::errorString() const { return error_; }
int PhoneServer::clientCount() const { return authedCount_; }
void PhoneServer::setInfoProvider(std::function<QJsonObject()> provider) { infoProvider_ = std::move(provider); }

QStringList PhoneServer::localUrls(quint16 port)
{
    QStringList urls;
    for (const QNetworkInterface& iface : QNetworkInterface::allInterfaces()) {
        const auto flags = iface.flags();
        if (!(flags & QNetworkInterface::IsUp) || !(flags & QNetworkInterface::IsRunning)
            || (flags & QNetworkInterface::IsLoopBack)) {
            continue;
        }
        for (const QNetworkAddressEntry& entry : iface.addressEntries()) {
            const QHostAddress ip = entry.ip();
            if (ip.protocol() == QAbstractSocket::IPv4Protocol && !ip.isLoopback()) {
                urls << QStringLiteral("http://%1:%2/").arg(ip.toString()).arg(port);
            }
        }
    }
    return urls;
}

QString PhoneServer::peerKey(QTcpSocket* s) const
{
    QHostAddress addr = s->peerAddress();
    bool ok = false;
    const quint32 v4 = addr.toIPv4Address(&ok);     // dual-stack sockets report ::ffff:a.b.c.d
    if (ok) {
        return QHostAddress(v4).toString();
    }
    Q_IPV6ADDR a6 = addr.toIPv6Address();
    for (int i = 8; i < 16; ++i) {                  // throttle per /64: a LAN host owns the whole prefix
        a6.c[i] = 0;
    }
    return QHostAddress(a6).toString();
}

void PhoneServer::onNewConnection()
{
    while (QTcpSocket* s = server_.nextPendingConnection()) {
        if (conns_.size() >= kMaxConns) {
            s->abort();
            s->deleteLater();
            continue;
        }
        int unauth = 0;
        const QString key = peerKey(s);
        for (auto it = conns_.constBegin(); it != conns_.constEnd(); ++it) {
            if (!it->authed && peerKey(it.key()) == key) {
                ++unauth;
            }
        }
        if (unauth >= kMaxUnauthPerPeer) {          // one host cannot fill the global cap with idle sockets
            s->abort();
            s->deleteLater();
            continue;
        }
        conns_.insert(s, Conn());
        s->setSocketOption(QAbstractSocket::LowDelayOption, 1);
        s->setSocketOption(QAbstractSocket::KeepAliveOption, 1);
        // Idle timeout: abort() runs the normal disconnect path (authedCount_, releaseAll).
        QTimer* idle = new QTimer(s);
        idle->setSingleShot(true);
        idle->setInterval(kIdleTimeoutMs);
        connect(idle, &QTimer::timeout, s, &QTcpSocket::abort);
        connect(s, &QTcpSocket::readyRead, idle, qOverload<>(&QTimer::start));
        idle->start();
        connect(s, &QTcpSocket::readyRead, this, [this, s] { onReadyRead(s); });
        connect(s, &QTcpSocket::disconnected, this, [this, s] { onDisconnected(s); });
        QTimer::singleShot(kAuthTimeoutMs, s, [this, s] {
            Conn* c = conn(s);
            if (c && !c->authed) {
                s->abort();
            }
        });
    }
}

void PhoneServer::onDisconnected(QTcpSocket* s)
{
    auto it = conns_.find(s);
    if (it == conns_.end()) {
        return;
    }
    const bool wasAuthed = it->authed;
    conns_.erase(it);
    s->deleteLater();
    if (wasAuthed) {
        --authedCount_;
        injector_->releaseAll();
        shiftHeld_ = false;
        Q_EMIT clientCountChanged(authedCount_);
    }
}

void PhoneServer::onReadyRead(QTcpSocket* s)
{
    Conn* c = conn(s);
    if (!c || c->closing) {
        return;
    }
    c->buf += s->readAll();
    // Handlers may close the socket (which removes the Conn), so re-look it up every round.
    for (;;) {
        c = conn(s);
        if (!c || c->closing) {
            return;
        }
        if (!(c->ws ? processFrame(s) : processHttp(s))) {
            return;
        }
    }
}

// Returns true when the connection became a WebSocket and more bytes may be waiting.
bool PhoneServer::processHttp(QTcpSocket* s)
{
    Conn* c = conn(s);
    phone::HttpRequest req;
    int used = 0;
    const phone::ParseStatus status = phone::parseHttpRequestHead(c->buf, &req, &used);
    if (status == phone::ParseStatus::NeedMore) {
        return false;
    }
    if (status == phone::ParseStatus::Error) {
        respond(s, 400, "text/plain; charset=utf-8", "잘못된 요청입니다");
        return false;
    }
    c->buf.remove(0, used);

    if (req.path == "/ws") {
        if (!phone::isWebSocketUpgrade(req)) {
            respond(s, 400, "text/plain; charset=utf-8", "WebSocket 요청이 아닙니다");
            return false;
        }
        // Browsers always send Origin; a page from another site must not reach the PIN prompt.
        QByteArray origin = req.headers.value("origin").trimmed();
        if (!origin.isEmpty()) {
            origin = origin.mid(origin.indexOf("://") + 3);
            if (origin.toLower() != req.headers.value("host").trimmed().toLower()) {
                respond(s, 403, "text/plain; charset=utf-8", "허용되지 않은 접속입니다");
                return false;
            }
        }
        s->write("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                 "Sec-WebSocket-Accept: " + phone::websocketAcceptKey(req.headers.value("sec-websocket-key"))
                 + "\r\n\r\n");
        c->ws = true;
        return true;
    }
    if (req.path == "/" || req.path == "/index.html") {
        QFile page(QStringLiteral(":/res/phone/phone.html"));
        if (!page.open(QIODevice::ReadOnly)) {
            respond(s, 500, "text/plain; charset=utf-8", "휴대폰 페이지 파일을 찾을 수 없습니다");
        } else {
            QByteArray html = page.readAll();
            html.replace("__PAGE_VER__", pageVersion());
            respond(s, 200, "text/html; charset=utf-8", html);
        }
        return false;
    }
    respond(s, 404, "text/plain; charset=utf-8", "찾을 수 없습니다");
    return false;
}

// Returns true when a frame was consumed and another one may follow.
bool PhoneServer::processFrame(QTcpSocket* s)
{
    Conn* c = conn(s);
    phone::WsFrame frame;
    int used = 0;
    const phone::ParseStatus status = phone::decodeWsFrame(c->buf, &frame, &used);
    if (status == phone::ParseStatus::NeedMore) {
        return false;
    }
    if (status == phone::ParseStatus::Error) {
        closeAfterFlush(s, 1002);
        return false;
    }
    c->buf.remove(0, used);

    switch (frame.opcode) {
    case phone::WsOpcode::Text:
        handleMessage(s, frame.payload);
        break;
    case phone::WsOpcode::Ping:
        s->write(phone::encodeWsFrame(phone::WsOpcode::Pong, frame.payload));
        break;
    case phone::WsOpcode::Close:
        closeAfterFlush(s, 1000);
        return false;
    case phone::WsOpcode::Pong:
        break;
    }
    return true;
}

// Changes whenever the page changes, so a tab opened before an app update reloads itself.
QByteArray PhoneServer::pageVersion()
{
    QFile page(QStringLiteral(":/res/phone/phone.html"));
    if (!page.open(QIODevice::ReadOnly)) {
        return QByteArray();
    }
    return QCryptographicHash::hash(page.readAll(), QCryptographicHash::Md5).toHex().left(12);
}

void PhoneServer::respond(QTcpSocket* s, int code, const QByteArray& contentType, const QByteArray& body)
{
    const char* reason = code == 200 ? "OK" : code == 400 ? "Bad Request" : code == 403 ? "Forbidden" : code == 404 ? "Not Found" : "Internal Server Error";
    s->write("HTTP/1.1 " + QByteArray::number(code) + " " + reason + "\r\nContent-Type: " + contentType
             + "\r\nContent-Length: " + QByteArray::number(body.size())
             + "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n" + body);
    closeAfterFlush(s, 0);
}

// wsCode != 0 sends a WebSocket close frame first. disconnectFromHost() lets queued bytes drain.
void PhoneServer::closeAfterFlush(QTcpSocket* s, quint16 wsCode)
{
    Conn* c = conn(s);
    if (!c || c->closing) {
        return;
    }
    c->closing = true;
    if (wsCode != 0) {
        s->write(phone::encodeWsClose(wsCode));
    }
    s->disconnectFromHost();        // may emit disconnected() synchronously; do not touch c afterwards
}

void PhoneServer::sendJson(QTcpSocket* s, const QJsonObject& obj)
{
    s->write(phone::encodeWsFrame(phone::WsOpcode::Text, QJsonDocument(obj).toJson(QJsonDocument::Compact)));
}

void PhoneServer::handleMessage(QTcpSocket* s, const QByteArray& text)
{
    const QJsonDocument doc = QJsonDocument::fromJson(text);
    Conn* c = conn(s);
    if (!doc.isObject()) {
        if (c && !c->authed) {
            closeAfterFlush(s, 1008);
        }
        return;
    }
    const QJsonObject msg = doc.object();
    const QString type = msg.value(QLatin1String("t")).toString();
    if (!c->authed) {
        if (type == QLatin1String("auth")) {
            handleAuth(s, msg);
        } else {
            closeAfterFlush(s, 1008);
        }
        return;
    }
    if (type == QLatin1String("ping")) {
        sendJson(s, message("pong"));
    } else if (type == QLatin1String("rel")) {
        injector_->releaseAll();
        shiftHeld_ = false;
    } else {
        handleInput(s, type, msg);
    }
}

void PhoneServer::handleAuth(QTcpSocket* s, const QJsonObject& msg)
{
    const auto refuse = [this, s](const QString& why) {
        QJsonObject hello = message("hello");
        hello["ok"] = false;
        hello["m"] = why;
        sendJson(s, hello);
        closeAfterFlush(s, 1000);
    };

    const QString ip = peerKey(s);
    const qint64 now = clock_.elapsed();
    if (now < globalFail_.blockedUntil) {
        refuse(tr("시도가 너무 많습니다. 잠시 후 다시 시도하세요"));
        return;
    }
    if (failures_.size() > kMaxFailureEntries) {    // ponytail: wipes per-peer throttling if flooded; globalFail_ still caps it
        failures_.clear();
    }
    Failures& fail = failures_[ip];
    if (now < fail.blockedUntil) {
        refuse(tr("시도가 너무 많습니다. 잠시 후 다시 시도하세요"));
        return;
    }

    const QJsonValue pin = msg.value(QLatin1String("pin"));
    if (!pin.isString() || !phone::constantTimeEquals(pin.toString().toUtf8(), pin_)) {
        const auto record = [now](Failures& f, int limit) {
            while (!f.times.isEmpty() && now - f.times.first() > kFailWindowMs) {
                f.times.removeFirst();
            }
            f.times.append(now);
            if (f.times.size() >= limit) {
                f.blockedUntil = now + kFailWindowMs;
                f.times.clear();
            }
        };
        record(fail, kMaxFailures);
        record(globalFail_, kMaxGlobalFailures);
        refuse(tr("PIN이 맞지 않습니다"));
        return;
    }
    if (authedCount_ >= kMaxAuthed) {
        refuse(tr("접속 한도"));
        return;
    }

    failures_.remove(ip);
    conn(s)->authed = true;
    ++authedCount_;

    QJsonObject hello;
    hello["name"] = QString();
    hello["role"] = QStringLiteral("unknown");
    const QJsonObject info = infoProvider_ ? infoProvider_() : QJsonObject();
    for (auto it = info.begin(); it != info.end(); ++it) {
        hello[it.key()] = it.value();
    }
    hello["t"] = QStringLiteral("hello");
    hello["ok"] = true;
    hello["os"] = QString::fromLatin1(injector_->platformName());
    hello["ver"] = QString::fromLatin1(pageVersion());
    sendJson(s, hello);

    QString why;
    if (!injector_->isAvailable(&why)) {
        sendJson(s, warning(why));
    }
    Q_EMIT clientCountChanged(authedCount_);
}

void PhoneServer::pressKey(phone::Key key, bool down)
{
    if (key == phone::Key::Shift) {
        shiftHeld_ = down;
    }
    injector_->key(key, down);
}

void PhoneServer::typeText(QTcpSocket* s, const QString& text)
{
    QString chars = text;
    if (chars.size() > kMaxTextChars) {
        chars.truncate(kMaxTextChars);
        sendJson(s, warning(tr("한 번에 %1글자까지만 입력됩니다").arg(kMaxTextChars)));
    }
    int skipped = 0;
    for (const phone::KeyStroke& stroke : phone::textToStrokes(chars, &skipped)) {
        const bool pressShift = stroke.shift && !shiftHeld_;
        if (pressShift) {
            pressKey(phone::Key::Shift, true);
        }
        pressKey(stroke.key, true);
        pressKey(stroke.key, false);
        if (pressShift) {
            pressKey(phone::Key::Shift, false);
        }
    }
    if (skipped > 0) {
        sendJson(s, warning(tr("%1글자는 입력할 수 없어 건너뜀").arg(skipped)));
    }
}

void PhoneServer::handleInput(QTcpSocket* s, const QString& type, const QJsonObject& msg)
{
    int a = 0;
    int b = 0;
    bool down = false;
    phone::Key key = phone::Key::None;

    if (type == QLatin1String("mv")) {
        if (clampedInt(msg, "dx", kMaxMove, &a) && clampedInt(msg, "dy", kMaxMove, &b)) {
            injector_->moveRelative(a, b);
        }
    } else if (type == QLatin1String("btn")) {
        if (exactInt(msg, "b", 0, 2, &a) && flag(msg, "d", &down)) {
            injector_->mouseButton(a, down);
        }
    } else if (type == QLatin1String("sc")) {
        if (clampedInt(msg, "dx", kMaxScroll, &a) && clampedInt(msg, "dy", kMaxScroll, &b)) {
            injector_->scroll(a, b);
        }
    } else if (type == QLatin1String("key")) {
        if (phone::keyFromName(msg.value(QLatin1String("k")).toString(), &key) && flag(msg, "d", &down)) {
            pressKey(key, down);
        }
    } else if (type == QLatin1String("tap")) {
        if (phone::keyFromName(msg.value(QLatin1String("k")).toString(), &key)) {
            pressKey(key, true);
            pressKey(key, false);
        }
    } else if (type == QLatin1String("text")) {
        const QJsonValue text = msg.value(QLatin1String("s"));
        if (text.isString()) {
            typeText(s, text.toString());
        }
    }
}
