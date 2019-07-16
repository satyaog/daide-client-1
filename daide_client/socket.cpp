// JPN_Socket.cpp: implementation of the JPN::Socket class and related material.

// Copyright (C) 2012, John Newbury. See "Conditions of Use" in johnnewbury.co.cc/diplomacy/conditions-of-use.htm.

// Release 8~2~b

/////////////////////////////////////////////////////////////////////////////

#include <algorithm>
#include <iostream>

#include <sys/ioctl.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>

#include "ai_client.h"
#include "error_log.h"
#include "socket.h"

/////////////////////////////////////////////////////////////////////////////

DAIDE::Socket *DAIDE::Socket::SocketTab[FD_SETSIZE];

int DAIDE::Socket::SocketCnt;

DAIDE::Socket::~Socket() {
    // Destructor.
    // FIXME - Avoid using C-casts and delete
    RemoveSocket();
    delete[] OutgoingMessage;
    if (IncomingMessage != (char *) &Header) delete[] IncomingMessage;
    while (!OutgoingMessageQueue.empty()) {
        delete[] OutgoingMessageQueue.front();
        OutgoingMessageQueue.pop();
    }
    while (!IncomingMessageQueue.empty()) {
        delete[] IncomingMessageQueue.front();
        IncomingMessageQueue.pop();
    }
}

void DAIDE::Socket::InsertSocket() {
    // Insert `this` into SocketTab. Must not already be present; should be full, else system socket table would already be full.
    ASSERT(!FindSocket(MySocket) && SocketCnt < FD_SETSIZE);
    SocketTab[SocketCnt++] = this;
}

void DAIDE::Socket::RemoveSocket() {
    // Remove `this` from SocketTab iff present.
    Socket **s = FindSocket(MySocket);
    if (s) *s = SocketTab[--SocketCnt]; // found; replace by last elem
}

void DAIDE::Socket::SendData() {
    // Send all available data to socket, while space is available.
    ASSERT(Connected);
    for (;;) { // while data available to send and space avalable
        if (!OutgoingMessage) { // no outgoing message in progress
            if (OutgoingMessageQueue.empty()) return; // nothing more to send
            OutgoingMessage = OutgoingMessageQueue.front(); // next message to send
            OutgoingMessageQueue.pop();
            OutgoingNext = 0;
            short length = ((MessageHeader *) OutgoingMessage)->length;
            AdjustOrdering(OutgoingMessage, length);
            OutgoingLength = sizeof(MessageHeader) + length;
        }

        // # bytes sent, or SOCKET_ERROR
        int sent = send(MySocket, OutgoingMessage + OutgoingNext, OutgoingLength - OutgoingNext, 0);

        if (sent == SOCKET_ERROR) {
            int error = WSAGetLastError();
            if (error != WSAEWOULDBLOCK) {
                log_error("Failure %d during SendDatar", error);
            }
            return;
        }
        OutgoingNext += sent;
        if (OutgoingNext < OutgoingLength) return; // current message not fully sent
        delete[] OutgoingMessage;
        OutgoingMessage = 0;
    }
}

void DAIDE::Socket::ReceiveData() {
    // Receive all data available from socket.
    ASSERT(Connected);

    const int bufferLength = 1024; // arbitrary
    char buffer[bufferLength];
    int bufferNext = 0; // index of next byte in `buffer`
    int received = recv(MySocket, buffer, bufferLength, 0);

    if (!received) {
        log_error("Failure: closed socket during read from Server");
        return;
    }
    if (received == SOCKET_ERROR) {
        int error = WSAGetLastError();
        if (error != WSAEWOULDBLOCK) {
            log_error("Failure %d during ReceiveData", error);
        }
        return;
    }
    for (;;) { // while incoming data available
        // max # bytes that may be read into current message
        int count = std::min(IncomingLength - IncomingNext, received - bufferNext);
        if (!count) return; // no more data available

        // copy `count` bytes from `buffer` to IncomingMessage
        ASSERT(count > 0);
        memcpy(IncomingMessage + IncomingNext, buffer + bufferNext, count);

        IncomingNext += count;
        bufferNext += count;

        if (IncomingNext == sizeof(MessageHeader)) { // just completed reading Header; length now known
            short length = ((MessageHeader *) (IncomingMessage))->length; // length of body
            AdjustOrdering(length);
            IncomingLength = sizeof(MessageHeader) + length; // full message length
            IncomingMessage = new char[IncomingLength]; // space for full message

            // Copy Header to full IncomingMessage
            ((MessageHeader *) IncomingMessage)->type = Header.type;
            ((MessageHeader *) IncomingMessage)->pad = Header.pad;
            ((MessageHeader *) IncomingMessage)->length = Header.length;

            IncomingNext = sizeof(MessageHeader);
        }

        if (IncomingNext >= IncomingLength) { // current incoming message is complete
            AdjustOrdering(IncomingMessage, IncomingLength - sizeof(MessageHeader));
            PushIncomingMessage(IncomingMessage);

            // Make IncomingMessage use Header, pending known length of next full message.
            IncomingMessage = (char *) &Header;
            IncomingNext = 0;
            IncomingLength = sizeof(MessageHeader);
        }
    }
}

void DAIDE::Socket::Start() {
    // Start using the socket.
    Connected = true;
    log("connected");
    SendData();
}

void DAIDE::Socket::OnConnect(int error) {
    // Handle socket Connect event; NZ `error` indicates failure.
    if (!error) Start();
    else {
        log_error("Failure %d during OnConnect", error);
        std::cerr << "Failed to connect" << std::endl;
    }
}

void DAIDE::Socket::OnClose(int error) {
    // Handle socket Close event; NZ `error` indicates failure.
    if (error) log_error("Failure %d during OnClose", error);
}

void DAIDE::Socket::OnReceive(int error) {
    // Handle socket Receive event; NZ `error` indicates failure.
    if (!error) ReceiveData();
    else {
        log_error("Failure %d during OnReceive", error);
    }
}

void DAIDE::Socket::OnSend(int error) {
    // Handle socket Send event; NZ `error` indicates failure.
    if (!error) SendData();
    else {
        log_error("Failure %d during OnSend", error);
    }
}

bool DAIDE::Socket::Connect(const char *address, int port) {
    // Initiate asynchronous connection of socket to `address` and `port`; return true iff OK.
    int err;

    MySocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (MySocket < 0) {
        log_error("Failure %d during socket", WSAGetLastError());
        return false;
    }

    unsigned long mode = 1; // non-blocking
    if (ioctlsocket(MySocket, FIONBIO, &mode)) {
        log_error("Failure %d during ioctlsocket", WSAGetLastError());
        return false;
    }

    BOOL val = true;
    if (setsockopt(MySocket, SOL_SOCKET, SO_KEEPALIVE, (const char *) &val, sizeof(BOOL))) {
        log_error("Failure %d during setsockopt", WSAGetLastError());
        return false;
    }

    // Prepare `saPtr` and `saLen` args for connect, from `server` and `port`. Requires mystical incantations!
    // Assumed fast enough to do synchronously, albeit may use external name server;
    // else needs separate thread!
    sockaddr *saPtr;
    struct sockaddr_in sa;
    int saLen;

    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = inet_addr(address);

    if (sa.sin_addr.s_addr == INADDR_NONE) { // not valid IP number
        log_error("Invalid IP address %s", address);
        return false;
    }

    saPtr = (sockaddr*) &sa;
    saLen = sizeof sa;

    // Connect to server.
    if (connect(MySocket, saPtr, saLen)) {
        err = WSAGetLastError();
        if (err != WSAEWOULDBLOCK) {
            log_error("Failure %d during Connect", err);
            if (MySocket) close(MySocket);
            return false;
        }
    }

    // Make IncomingMessage use Header, pending known length of full message.
    IncomingMessage = (char *) &Header;
    IncomingNext = 0;
    IncomingLength = sizeof(MessageHeader);
    OutgoingMessage = 0;

    InsertSocket();

    return true;
}

void DAIDE::Socket::PushIncomingMessage(char *message) {
    // Push incoming `message` on end of IncomingMessageQueue.
    IncomingMessageQueue.push(message); // safe to follow the post as we are in the Main thread
}

void DAIDE::Socket::PushOutgoingMessage(char *message) {
    // Push outgoing `message` on end of OutgoingMessageQueue.

    OutgoingMessageQueue.push(message);
    if (!OutgoingMessage && Connected) SendData();
}

char *DAIDE::Socket::PullIncomingMessage() {
    // Pull next message from front of IncomingMessageQueue. Post a new message event if more messages remain on queue.

    ASSERT(!IncomingMessageQueue.empty());

    char *incomingMessage = IncomingMessageQueue.front(); // message to be processed
    IncomingMessageQueue.pop(); // remove it
    // trigger recall if more messages remain

    return incomingMessage;
}

DAIDE::Socket **DAIDE::Socket::FindSocket(int socket) {
    // Return the Socket** in SocketTab that owns `socket`; 0 iff not found.
    Socket **s;
    for (int i = 0; i < SocketCnt; ++i) {
        s = &SocketTab[i];
        if ((*s)->MySocket == socket) return s;
    }
    return 0;
}

void DAIDE::Socket::AdjustOrdering(short &x) {
    // Adjust the byte ordering of `x`, to or from network ordering of a `short`.

#if LITTLE_ENDIAN
    ASSERT(htons(1) != 1);

    x = (x << 8) | (((unsigned short) x) >> 8);
#else
    ASSERT(htons(1) == 1);
#endif
}

void DAIDE::Socket::AdjustOrdering(char *message, short length) {
    // Adjust 16-bit aligned `message`, having body `length`, to or from network ordering of its components.
    // 'length` must be specified, as that in the header may or may not be in internal order.
    // Header `type` and `pad` are single byte, so need no reordering; the rest are byte-pair `short`, so may need reordering.

#if LITTLE_ENDIAN
    AdjustOrdering(((MessageHeader *) message)->length);
    message += sizeof(MessageHeader);
    char *end = message + length;
    for (; message < end; message += sizeof(short)) AdjustOrdering(*(short *) message);
#endif
}
