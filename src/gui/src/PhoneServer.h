/*
 * InputLeap -- mouse and keyboard sharing utility
 * Copyright (C) 2026 InputLeafPlus Developers
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#pragma once

#include "PhoneInject.h"

#include <QElapsedTimer>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTcpServer>

#include <functional>
#include <memory>

class QTcpSocket;

// Tiny HTTP + WebSocket server for the phone trackpad page. Messages are injected into this
// machine's OS through phone::InputInjector. See PhoneProtocol.h for the wire codec.
class PhoneServer : public QObject {
    Q_OBJECT
public:
    explicit PhoneServer(QObject* parent = nullptr);
    ~PhoneServer() override;

    bool start(quint16 port, const QString& pin);          // false => errorString(); pin must be 4..16 chars
    void stop();                                            // closes sockets, injector->releaseAll()
    bool isRunning() const;
    QString errorString() const;                            // Korean
    int clientCount() const;                                // authenticated sockets
    // Called when a client authenticates; its keys ("name", "role", extras) are merged into hello.
    void setInfoProvider(std::function<QJsonObject()> provider);
    static QStringList localUrls(quint16 port);             // "http://192.168.0.5:24803/" per non-loopback up IPv4 interface

Q_SIGNALS:
    void clientCountChanged(int count);

private:
    struct Conn {
        QByteArray buf;
        bool ws = false;
        bool authed = false;
        bool closing = false;
    };
    struct Failures {
        QList<qint64> times;        // recent failure times (ms on clock_)
        qint64 blockedUntil = 0;
    };

    Conn* conn(QTcpSocket* s) { auto it = conns_.find(s); return it == conns_.end() ? nullptr : &it.value(); }
    void onNewConnection();
    void onReadyRead(QTcpSocket* s);
    void onDisconnected(QTcpSocket* s);
    bool processHttp(QTcpSocket* s);
    bool processFrame(QTcpSocket* s);
    void handleMessage(QTcpSocket* s, const QByteArray& text);
    void handleAuth(QTcpSocket* s, const QJsonObject& msg);
    void handleInput(QTcpSocket* s, const QString& type, const QJsonObject& msg);
    void typeText(QTcpSocket* s, const QString& text);
    void pressKey(phone::Key key, bool down);
    void respond(QTcpSocket* s, int code, const QByteArray& contentType, const QByteArray& body);
    void sendJson(QTcpSocket* s, const QJsonObject& obj);
    void closeAfterFlush(QTcpSocket* s, quint16 wsCode);
    QString peerKey(QTcpSocket* s) const;

    QTcpServer server_;
    std::unique_ptr<phone::InputInjector> injector_;
    std::function<QJsonObject()> infoProvider_;
    QHash<QTcpSocket*, Conn> conns_;
    QHash<QString, Failures> failures_;
    Failures globalFail_;           // failed auths from any peer
    QElapsedTimer clock_;
    QByteArray pin_;
    QString error_;
    int authedCount_ = 0;
    bool shiftHeld_ = false;
};
