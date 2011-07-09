/* Copyright (c) 2010-2011 Stanford University
 *
 * Permission to use, copy, modify, and distribute this software for any purpose
 * with or without fee is hereby granted, provided that the above copyright
 * notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR(S) DISCLAIM ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL AUTHORS BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER
 * RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF
 * CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "TestUtil.h"
#include "MockDriver.h"
#include "MockTransport.h"
#include "FastTransport.h"
#include "ServiceManager.h"
#include "UdpDriver.h"

namespace RAMCloud {

class MockReceived : public Driver::Received {
  private:
    void construct(uint32_t fragNumber,
                   uint32_t totalFrags,
                   const char* msg,
                   uint32_t len)
    {
        payload = new char[len];
        memcpy(getContents(), msg, len - sizeof(FastTransport::Header));
        FastTransport::Header *header = new(payload) FastTransport::Header;
        header->fragNumber = downCast<uint16_t>(fragNumber);
        header->totalFrags = downCast<uint16_t>(totalFrags);
    }
  public:
    MockReceived(uint32_t fragNumber,
                 uint32_t totalFrags,
                 const void* msg,
                 uint32_t len)
        : Received()
        , stealCount(0)
    {
        this->len = len + downCast<uint32_t>(sizeof(FastTransport::Header));
        construct(fragNumber, totalFrags,
                  static_cast<const char*>(msg), this->len);
        getHeader()->payloadType = FastTransport::Header::ACK;
    }
    MockReceived(uint32_t fragNumber,
                 uint32_t totalFrags,
                 const char* msg)
        : Received()
        , stealCount(0)
    {
        len = downCast<uint32_t>(strlen(msg) + sizeof(FastTransport::Header));
        construct(fragNumber, totalFrags, msg, len);
        getHeader()->payloadType = FastTransport::Header::DATA;
    }
    FastTransport::Header* getHeader()
    {
        return reinterpret_cast<FastTransport::Header*>(payload);
    }
    char* getContents()
    {
        return payload + sizeof(FastTransport::Header);
    }
    VIRTUAL_FOR_TESTING char *steal(uint32_t *len)
    {
        stealCount++;
        *len = this->len;
        return payload;
    }
    ~MockReceived()
    {
        delete[] payload;
        payload = NULL;
    }
    int stealCount;
    DISALLOW_COPY_AND_ASSIGN(MockReceived);
};

// Used in {add,remove,fire}Timer tests
struct MockTimer : public Dispatch::Timer {
    MockTimer()
        : Dispatch::Timer()
        , onTimerFiredCount(0)
        , expectWhen(0)
    {}
    explicit MockTimer(uint64_t expectWhen)
        : Dispatch::Timer()
        , onTimerFiredCount(0)
        , expectWhen(expectWhen)
    {}
    virtual void operator() ()
    {
        if (expectWhen)
            EXPECT_EQ(expectWhen, dispatch->currentTime);
        onTimerFiredCount++;
    }
    uint32_t onTimerFiredCount;
    uint64_t expectWhen;
};

struct MockSession {
    MockSession(FastTransport* transport, uint32_t sessionHint)
        : expired(true)
        , id(sessionHint)
        , lastActivityTime(0)
        , nextFree(FastTransport::SessionTable<MockSession>::NONE)
        , transport(transport)
    {
    }
    uint64_t getLastActivityTime() {
        return lastActivityTime;
    }
    bool expire() {
        return expired;
    }
    // just used to mock out return from getLastActivityTime
    void setLastActivityTime(uint64_t time) {
        this->lastActivityTime = time;
    }
    // just used to mock out return from expired
    void setExpired(bool expired) {
        this->expired = expired;
    }
    virtual uint32_t getId() {
        return id;
    }
    virtual ~MockSession() {}

    bool expired;
    uint32_t id;
    uint64_t lastActivityTime;
    uint32_t nextFree;
    FastTransport* transport;
    DISALLOW_COPY_AND_ASSIGN(MockSession);
};

// --- FastTransportTest ---

class FastTransportTest : public ::testing::Test {
  public:
    ServiceLocator serviceLocator;
    FastTransport* transport;
    MockDriver* driver;
    const char* address;
    uint16_t port;

    FastTransportTest()
        : serviceLocator("fast+udp: host=1.2.3.4, port=1234")
        , transport(NULL)
        , driver(NULL)
        , address("1.2.3.4")
        , port(1234)
    {
        driver = new MockDriver(FastTransport::Header::headerToString);
        transport = new FastTransport(driver);
        delete serviceManager;
        ServiceManager::init();

        logger.setLogLevels(SILENT_LOG_LEVEL);

        // The following is necessary in case some other tests messed up
        // currentTime (e.g. by resetting it to 0).
        dispatch->currentTime = rdtsc();
    }

    ~FastTransportTest()
    {
        delete transport;
        FastTransport::timeoutCyclesOverride = 0;
        FastTransport::sessionTimeoutCyclesOverride = 0;
    }

    DISALLOW_COPY_AND_ASSIGN(FastTransportTest);
};

TEST_F(FastTransportTest, sanityCheck) {
#if !VALGRIND // RAM-260
    // Create a server and a client and verify that we can
    // send a request, receive it, send a reply, and receive it.
    // Then try a second request with bigger chunks of data.

    ServiceLocator serverLocator("fast+udp: host=localhost, port=11101");
    UdpDriver* serverDriver = new UdpDriver(&serverLocator);
    FastTransport server(serverDriver);
    UdpDriver* clientDriver = new UdpDriver();
    FastTransport client(clientDriver);
    Transport::SessionRef session = client.getSession(serverLocator);

    Buffer request;
    Buffer reply;
    request.fillFromString("abcdefg");
    Transport::ClientRpc* clientRpc = session->clientSend(&request,
            &reply);
    Transport::ServerRpc* serverRpc = serviceManager->waitForRpc(1.0);
    EXPECT_TRUE(serverRpc != NULL);
    EXPECT_EQ("abcdefg/0", TestUtil::toString(&serverRpc->requestPayload));
    EXPECT_FALSE(clientRpc->isReady());
    serverRpc->replyPayload.fillFromString("klmn");
    serverRpc->sendReply();
    EXPECT_TRUE(TestUtil::waitForRpc(*clientRpc));
    EXPECT_EQ("klmn/0", TestUtil::toString(&reply));

    TestUtil::fillLargeBuffer(&request, 100000);
    reply.reset();
    clientRpc = session->clientSend(&request, &reply);
    serverRpc = serviceManager->waitForRpc(1.0);
    EXPECT_TRUE(serverRpc != NULL);
    EXPECT_EQ("ok",
          TestUtil::checkLargeBuffer(&serverRpc->requestPayload, 100000));
    TestUtil::fillLargeBuffer(&serverRpc->replyPayload, 50000);
    serverRpc->sendReply();
    clientRpc->wait();
    EXPECT_EQ("ok", TestUtil::checkLargeBuffer(&reply, 50000));
#endif
}

TEST_F(FastTransportTest, getSession_noneExpirable) {
    EXPECT_EQ(0U, transport->clientSessions.size());
    FastTransport::SessionRef session =
            transport->getSession(serviceLocator);
    EXPECT_TRUE(NULL != session.get());
    EXPECT_EQ(1U, transport->clientSessions.size());
    FastTransport::ClientSession* clientSession =
        static_cast<FastTransport::ClientSession*>(session.get());
    EXPECT_TRUE(NULL != clientSession->serverAddress.get());
}

TEST_F(FastTransportTest, getSession_reuseExpired) {
    dispatch->currentTime = 0;
    EXPECT_EQ(0U, transport->clientSessions.size());
    Transport::Session* firstSession =
        transport->getSession(serviceLocator).get();
    FastTransport::sessionTimeoutCyclesOverride =
            FastTransport::SESSION_TIMEOUT_NS;
    dispatch->currentTime = FastTransport::SESSION_TIMEOUT_NS;
    Transport::Session* lastSession =
        transport->getSession(serviceLocator).get();
    EXPECT_EQ(firstSession, lastSession);
    EXPECT_EQ(1U, transport->clientSessions.size());
}

TEST_F(FastTransportTest, numFrags_fullPacket) {
    Buffer b;
    new(&b, APPEND) char[transport->dataPerFragment()];
    EXPECT_EQ(1U, transport->numFrags(&b));
}

TEST_F(FastTransportTest, numFrags_oneByteTooBig) {
    Buffer b;
    new(&b, APPEND) char[transport->dataPerFragment() + 1];
    EXPECT_EQ(2U, transport->numFrags(&b));
}

TEST_F(FastTransportTest, sendBadSessionError) {
    MockReceived recvd(0, 1, "");
    FastTransport::Header *header = recvd.getHeader();
    header->sessionToken = 0xabcd;
    header->rpcId = 3;
    header->clientSessionHint = 4;
    header->serverSessionHint = 5;
    header->channelId = 6;
    header->payloadType = 7;
    header->direction = 1;
    transport->sendBadSessionError(recvd.getHeader(),
                                    recvd.sender);
    EXPECT_EQ(
        "{ sessionToken:abcd rpcId:3 clientSessionHint:4 "
        "serverSessionHint:5 0/0 frags channel:6 dir:1 reqACK:0 "
        "drop:0 payloadType:4 } ", driver->outputLog);
}

/// A predicate to limit TestLog messages to invoke
static bool
tppPred(string s)
{
    return s == "handleIncomingPacket";
}

TEST_F(FastTransportTest, handleIncomingPacket_tooSmall) {
    TestLog::Enable _(&tppPred);

    MockReceived recvd(0, 1, "");
    // corrupt the size
    recvd.len = 1;

    transport->handleIncomingPacket(&recvd);
    EXPECT_EQ(
        "handleIncomingPacket: "
        "packet too short (1 bytes)", TestLog::get());
}

TEST_F(FastTransportTest, handleIncomingPacket_dropped) {
    TestLog::Enable _(&tppPred);

    MockReceived recvd(0, 1, "");
    recvd.getHeader()->pleaseDrop = 1;

    transport->handleIncomingPacket(&recvd);
    EXPECT_EQ(
        "handleIncomingPacket: "
        "dropped", TestLog::get());
}

TEST_F(FastTransportTest, handleIncomingPacket_c2sBadHintOpenSession) {
    TestLog::Enable _(&tppPred);

    FastTransport::sessionTimeoutCyclesOverride = 2000;
    dispatch->currentTime = FastTransport::sessionTimeoutCycles() * 2;
    FastTransport::SessionOpenResponse sessResp =
            { FastTransport::NUM_CHANNELS_PER_SESSION };
    MockReceived recvd(0, 1, &sessResp, sizeof(sessResp));
    recvd.getHeader()->serverSessionHint =
            FastTransport::ClientSession::INVALID_HINT;
    recvd.getHeader()->payloadType = FastTransport::Header::SESSION_OPEN;
    ServiceLocator sl("mock:");
    recvd.sender = driver->newAddress(sl);

    transport->handleIncomingPacket(&recvd);
    EXPECT_EQ(
        "handleIncomingPacket: "
        "opening session 0", TestLog::get());
    EXPECT_EQ(1U, transport->serverSessions.size());
}

TEST_F(FastTransportTest, handleIncomingPacket_c2sBadSession) {
    TestLog::Enable _(&tppPred);

    MockReceived recvd(0, 1, "");

    transport->handleIncomingPacket(&recvd);
    EXPECT_EQ(
        "handleIncomingPacket: "
        "bad session hint 0", TestLog::get());
    EXPECT_EQ(
        "{ sessionToken:0 rpcId:0 clientSessionHint:0 "
            "serverSessionHint:0 0/0 frags channel:0 dir:1 reqACK:0 "
            "drop:0 payloadType:4 } ", driver->outputLog);
}

TEST_F(FastTransportTest, handleIncomingPacket_c2sGoodHint) {
    TestLog::Enable _(&tppPred);

    FastTransport::ServerSession* session =
            transport->serverSessions.get();
    MockReceived recvd(0, 1, "");
    recvd.getHeader()->sessionToken = session->token;

    transport->handleIncomingPacket(&recvd);
    session->channels[0].state =
            FastTransport::ServerSession::ServerChannel::IDLE;
    EXPECT_EQ(
        "handleIncomingPacket: "
        "calling ServerSession::processInboundPacket", TestLog::get());
}

TEST_F(FastTransportTest, handleIncomingPacket_c2sGoodHintBadToken) {
    TestLog::Enable _(&tppPred);

    FastTransport::ServerSession* session =
            transport->serverSessions.get();
    MockReceived recvd(0, 1, "");
    recvd.getHeader()->sessionToken = session->token+1;

    transport->handleIncomingPacket(&recvd);
    EXPECT_EQ(
        "handleIncomingPacket: "
        "bad session token (0xcccccccccccccccc in session 0, "
        "0xcccccccccccccccd in packet)", TestLog::get());
    EXPECT_EQ(
        "{ sessionToken:cccccccccccccccd rpcId:0 clientSessionHint:0 "
            "serverSessionHint:0 0/0 frags channel:0 dir:1 reqACK:0 "
            "drop:0 payloadType:4 } ", driver->outputLog);
}

TEST_F(FastTransportTest, handleIncomingPacket_s2cGoodHint) {
    TestLog::Enable _(&tppPred);

    FastTransport::SessionRef session =
            transport->getSession(serviceLocator);
    FastTransport::ClientSession* clientSession =
        static_cast<FastTransport::ClientSession*>(session.get());

    MockReceived recvd(0, 1, "");
    recvd.getHeader()->direction = FastTransport::Header::SERVER_TO_CLIENT;
    clientSession->token = recvd.getHeader()->sessionToken;

    transport->handleIncomingPacket(&recvd);
    EXPECT_EQ(
        "handleIncomingPacket: "
        "client session processing packet", TestLog::get());
}

TEST_F(FastTransportTest, handleIncomingPacket_s2cGoodHintBadToken) {
    TestLog::Enable _(&tppPred);

    transport->getSession(serviceLocator);

    MockReceived recvd(0, 1, "");
    recvd.getHeader()->direction =
            FastTransport::Header::SERVER_TO_CLIENT;

    transport->handleIncomingPacket(&recvd);
    EXPECT_EQ(
        "handleIncomingPacket: "
        "client session processing packet | "
        "handleIncomingPacket: "
        "bad fragment token (0xcccccccccccccccc in session 0, "
        "0x0 in packet), client dropping", TestLog::get());
}

TEST_F(FastTransportTest, handleIncomingPacket_s2cBadHint) {
    TestLog::Enable _(&tppPred);

    MockReceived recvd(0, 1, "");
    recvd.getHeader()->direction =
            FastTransport::Header::SERVER_TO_CLIENT;

    transport->handleIncomingPacket(&recvd);
    EXPECT_EQ(
        "handleIncomingPacket: "
        "bad client session hint 0", TestLog::get());
}

// --- ClientRpcTest ---

class ClientRpcTest : public ::testing::Test {
  public:
    Buffer* request;
    Buffer* response;
    FastTransport* transport;
    FastTransport::ClientSession* session;
    MockDriver* driver;
    FastTransport::ClientRpc* rpc;
    const char* address;
    uint16_t port;

    ClientRpcTest()
        : request(NULL)
        , response(NULL)
        , transport(NULL)
        , session(NULL)
        , driver(NULL)
        , rpc(NULL)
        , address("1.2.3.4")
        , port(1234)
    {
        driver = new MockDriver(FastTransport::Header::headerToString);
        transport = new FastTransport(driver);
        session = new FastTransport::ClientSession(transport, 0x98765432);

        request = new Buffer();
        response = new Buffer();

        rpc = new FastTransport::ClientRpc(session, request, response);
    }

    ~ClientRpcTest()
    {
        delete response;
        delete request;
        delete session;
        delete transport;
    }

  private:
    DISALLOW_COPY_AND_ASSIGN(ClientRpcTest);
};

TEST_F(ClientRpcTest, constructor) {
    EXPECT_EQ(request, rpc->requestBuffer);
    EXPECT_EQ(response, rpc->responseBuffer);
    EXPECT_EQ(session, rpc->session);
}

// --- InboundMessageTest ---

class InboundMessageTest : public ::testing::Test {
  public:
    MockDriver* driver;
    FastTransport* transport;
    FastTransport::SessionRef session;
    Buffer* buffer;
    FastTransport::InboundMessage* msg;

    InboundMessageTest()
        : driver(NULL)
        , transport(NULL)
        , session()
        , buffer(NULL)
        , msg(NULL)
    {
        setUp(2, false);
    }

    void
    setUp(uint16_t totalFrags, bool useTimer = false)
    {
        driver = new MockDriver();
        transport = new FastTransport(driver);
        buffer = new Buffer();

        uint32_t channelId = 5;

        msg = new FastTransport::InboundMessage();

        ServiceLocator serviceLocator("fast+udp: host=1.2.3.4, port=1234");
        session = transport->getSession(serviceLocator);

        FastTransport::ClientSession* clientSession =
            static_cast<FastTransport::ClientSession*>(session.get());
        clientSession->numChannels =
                FastTransport::MAX_NUM_CHANNELS_PER_SESSION;
        clientSession->allocateChannels();
        msg->setup(transport, clientSession, channelId, useTimer);

        msg->reset();
        msg->init(totalFrags, buffer);

        // Initialize dataStagingWindow to check invariants after calls
        for (uint32_t i = 1; i <= msg->dataStagingWindow.getLength(); i++)
            msg->dataStagingWindow[i] =
                std::pair<char*, uint32_t>(static_cast<char*>(0), i);

    }

    ~InboundMessageTest()
    {
        if (msg)
            delete msg;
        if (buffer)
            delete buffer;
        session = NULL;
        if (transport)
            delete transport;
    }

    void dataStagingWindowToWindow(
                Window<pair<char*, uint32_t>,
                FastTransport::MAX_STAGING_FRAGMENTS>& w,
                string& s)
    {
        size_t max = 50;
        char tmp[max];

        for (uint32_t i = 0; i < w.getLength(); i++) {
            char* payload = w[w.getOffset() + i].first;
            uint32_t payloadLen = w[w.getOffset() + i].second;

            if (!payload) {
                snprintf(tmp, max, "-, ");
            } else {
                string payloadStr = TestUtil::toString(payload, payloadLen);
                payloadStr.resize(10);

                snprintf(tmp, max, "(%s, %u), ",
                         payloadStr.c_str(), payloadLen);
            }

            s.append(tmp);
        }

        uint32_t trim = 0;
        while (s[s.length() - trim - 1] == ' ')
            trim++;

        s.resize(s.length() - trim);
    }

  private:
    DISALLOW_COPY_AND_ASSIGN(InboundMessageTest);
};

TEST_F(InboundMessageTest, sendAck) {
    // no packets received yet
    msg->sendAck();
    EXPECT_EQ("0 /0 /0",
                            driver->outputLog);
    driver->outputLog = "";

    // first ten received with no drops
    msg->firstMissingFrag = 10;
    msg->dataStagingWindow.advance(10);
    msg->sendAck();
    EXPECT_EQ("10 /0 /0", driver->outputLog);
    driver->outputLog = "";

    // first twenty received, missing 21 and one at other end of window
    msg->firstMissingFrag = 20;
    msg->dataStagingWindow.advance(10);
    char junk[0];
    msg->dataStagingWindow[21] = std::pair<char*, uint32_t>(junk, 1);
    msg->dataStagingWindow[msg->dataStagingWindow.getLength() + 20] =
        std::pair<char*, uint32_t>(junk, 1);
    msg->sendAck();
    EXPECT_EQ("0x10014 /0 /x80", driver->outputLog);
    driver->outputLog = "";
}

TEST_F(InboundMessageTest, init) {
    setUp(2, true);

    Buffer buffer;
    msg->init(999, &buffer);

    EXPECT_EQ(999U, msg->totalFrags);
    EXPECT_EQ(&buffer, msg->dataBuffer);
}

TEST_F(InboundMessageTest, setup) {
    bool useTimer = true;
    uint32_t channelId = 5;
    setUp(2, useTimer);

    msg->timer.start(999);

    FastTransport::ClientSession* clientSession =
        static_cast<FastTransport::ClientSession*>(session.get());
    for (;;) {
        msg->setup(transport, clientSession, channelId, useTimer);
        EXPECT_EQ(clientSession, msg->session);
        EXPECT_EQ(transport, msg->transport);
        EXPECT_EQ(channelId, msg->channelId);
        EXPECT_EQ(useTimer, msg->useTimer);
        if (!useTimer)
            break;
        useTimer = !useTimer;
        channelId = 6;
        setUp(2, useTimer);
    }
}

TEST_F(InboundMessageTest, reset) {
    setUp(2, true);

    char junk[0];
    msg->timer.start(1000);
    msg->dataStagingWindow[11] = std::pair<char*, uint32_t>(junk, 1);
    msg->dataStagingWindow[14] = std::pair<char*, uint32_t>(junk, 1);

    msg->reset();

    EXPECT_EQ(0U, msg->totalFrags);
    EXPECT_EQ(0U, msg->firstMissingFrag);
    string s;
    dataStagingWindowToWindow(msg->dataStagingWindow, s);
    EXPECT_EQ("-, -, -, -, -, -, -, -, -, -, -, -, -, -, "
                            "-, -, -, -, -, -, -, -, -, -, -, -, -, -, "
                            "-, -, -, -,", s);
    EXPECT_EQ(0, msg->dataBuffer);
    EXPECT_EQ(2U, driver->releaseCount);
}

TEST_F(InboundMessageTest, processReceivedData_totalFragMismatch) {
    TestLog::Enable _;

    // NOTE Make sure to keep the MockReceiveds on the stack until
    // none of the data is in use as part of a buffer
    MockReceived recvd(0, msg->totalFrags + 1, "God hates ponies.");
    bool result = msg->processReceivedData(&recvd);
    EXPECT_FALSE(result);
    EXPECT_EQ("", TestUtil::bufferToDebugString(msg->dataBuffer));
    EXPECT_EQ(0U, msg->firstMissingFrag);
    EXPECT_EQ(0, recvd.stealCount);
    EXPECT_EQ(
        "processReceivedData: "
        "header->totalFrags (3) != totalFrags (2)", TestLog::get());
}

/**
 * Ensures that if packets arrive in order according to firstMissingFrag
 * everything works, including connection termination condition.
 */
TEST_F(InboundMessageTest, processReceivedData_addFirstMissing) {
    // first recvd packet - never placed in window
    MockReceived recvd(0, msg->totalFrags, "God hates ponies.");
    bool result = msg->processReceivedData(&recvd);
    EXPECT_FALSE(result);
    EXPECT_EQ("God hates ponies.",
                            TestUtil::bufferToDebugString(msg->dataBuffer));
    EXPECT_EQ(1U, msg->firstMissingFrag);
    EXPECT_EQ(2U, msg->dataStagingWindow[2].second);
    EXPECT_EQ(1, recvd.stealCount);

    // second recvd packet - never placed in window - msg complete
    MockReceived nextRecvd(1, msg->totalFrags, "I hate ponies, also.");
    result = msg->processReceivedData(&nextRecvd);
    EXPECT_TRUE(result);
    EXPECT_EQ("God hates ponies. | I hate ponies, also.",
              TestUtil::bufferToDebugString(msg->dataBuffer));
    EXPECT_EQ(msg->totalFrags, msg->firstMissingFrag);
    EXPECT_EQ(3U, msg->dataStagingWindow[msg->totalFrags+1].second);
    EXPECT_EQ(1, nextRecvd.stealCount);
}

/**
 * Ensures out-of-order packets transition from the window to the buffer
 * when the missing fragments before them are encountered.
 */
TEST_F(InboundMessageTest, processReceivedData_addRunFromWindow) {
    // first recvd packet - out of order - enters window
    MockReceived recvd(1, msg->totalFrags, "I hate ponies, also.");
    bool result = msg->processReceivedData(&recvd);
    EXPECT_FALSE(result);
    EXPECT_EQ("", TestUtil::bufferToDebugString(msg->dataBuffer));
    EXPECT_EQ(recvd.payload, msg->dataStagingWindow[1].first);
    EXPECT_EQ(recvd.len, msg->dataStagingWindow[1].second);

    // second recvd packet - completes connection
    MockReceived nextRecvd(0, msg->totalFrags, "God hates ponies.");
    result = msg->processReceivedData(&nextRecvd);
    EXPECT_TRUE(result);
    EXPECT_EQ("God hates ponies. | I hate ponies, also.",
              TestUtil::bufferToDebugString(msg->dataBuffer));
    EXPECT_EQ(3U, msg->dataStagingWindow[3].second);
}

TEST_F(InboundMessageTest, processReceivedData_fragmentBeyondWindow) {
    // test with longer connection
    uint16_t totalFrags = 100;
    setUp(totalFrags, false);
    TestLog::Enable _;

    // first recvd packet - out of order - ensure in window in right place
    MockReceived recvd(33, totalFrags, "packet");
    bool result = msg->processReceivedData(&recvd);
    EXPECT_FALSE(result);
    EXPECT_EQ("", TestUtil::bufferToDebugString(msg->dataBuffer));
    EXPECT_EQ(0, recvd.stealCount);
    EXPECT_EQ(
        "processReceivedData: "
        "fragNumber 33 out of range (last OK = 32)", TestLog::get());
}

/**
 * Ensures that if an out-of-order packet is encountered it is properly
 * stored in the staging window to be moved in to the result buffer later.
 */
TEST_F(InboundMessageTest, processReceivedData_addToWindow) {
    // test with longer connection
    uint16_t totalFrags = 100;
    setUp(totalFrags, false);

    // first recvd packet - out of order - ensure in window in right place
    MockReceived recvd(2, totalFrags, "pkt two");
    bool result = msg->processReceivedData(&recvd);
    EXPECT_FALSE(result);
    EXPECT_EQ("", TestUtil::bufferToDebugString(msg->dataBuffer));
    EXPECT_EQ(recvd.payload, msg->dataStagingWindow[2].first);
    EXPECT_EQ(recvd.len, msg->dataStagingWindow[2].second);
    EXPECT_EQ(1, recvd.stealCount);

    // 2nd recvd packet - out of order - ensure in window in right place
    MockReceived nextRecvd(1, totalFrags, "pkt one");
    result = msg->processReceivedData(&nextRecvd);
    EXPECT_FALSE(result);
    EXPECT_EQ("", TestUtil::bufferToDebugString(msg->dataBuffer));
    EXPECT_EQ(nextRecvd.payload, msg->dataStagingWindow[1].first);
    EXPECT_EQ(nextRecvd.len, msg->dataStagingWindow[1].second);
    EXPECT_EQ(1, nextRecvd.stealCount);
}

TEST_F(InboundMessageTest, _processReceivedData_duplicateFragment) {
    // test with longer connection
    uint16_t totalFrags = 100;
    setUp(totalFrags, false);
    TestLog::Enable _;

    // first recvd packet - out of order
    MockReceived recvd(2, totalFrags, "first is short");
    bool result = msg->processReceivedData(&recvd);

    // 2nd recvd packet - same fragment
    MockReceived nextRecvd(2, totalFrags, "second is longer");
    result = msg->processReceivedData(&nextRecvd);
    EXPECT_FALSE(result);
    EXPECT_EQ("", TestUtil::bufferToDebugString(msg->dataBuffer));
    EXPECT_EQ(recvd.len, msg->dataStagingWindow[2].second);
    EXPECT_EQ(0, nextRecvd.stealCount);
    EXPECT_EQ(
        "processReceivedData: "
        "duplicate fragment 2 received", TestLog::get());
}

TEST_F(InboundMessageTest, processReceivedData_sendAckCalled) {
    MockReceived recvd(0, msg->totalFrags, "pkt zero");
    recvd.getHeader()->requestAck = 1;
    msg->processReceivedData(&recvd);

    EXPECT_NE("", driver->outputLog);
}

TEST_F(InboundMessageTest, processReceivedData_timerAdded) {
    setUp(2, true);

    msg->timer.stop();
    MockReceived recvd(0, msg->totalFrags, "pkt zero");
    recvd.getHeader()->requestAck = 1;
    msg->processReceivedData(&recvd);
}

// --- OutboundMessageTest ---

class OutboundMessageTest: public ::testing::Test {
  public:
    MockDriver* driver;
    FastTransport* transport;
    FastTransport::SessionRef session;
    Buffer* buffer;
    FastTransport::OutboundMessage* msg;
    uint64_t tsc;

    OutboundMessageTest()
        : driver(NULL)
        , transport(NULL)
        , session()
        , buffer(NULL)
        , msg(NULL)
        , tsc(999 + 2 * FastTransport::TIMEOUT_NS)
    {
        setUp(1600, false);
    }

    void
    setUp(uint32_t messageLen, bool useTimer = false)
    {
        assert(!(messageLen % 10));

        driver = new MockDriver(FastTransport::Header::headerToString);
        transport = new FastTransport(driver);
        buffer = new Buffer();

        const char* testMsg = "abcdefghij";
        size_t testMsgLen = strlen(testMsg);
        char* payload = new(buffer, APPEND) char[testMsgLen *
                                                 (messageLen / 10) + 1];
        for (uint32_t i = 0; i < (messageLen / 10); i++)
            memcpy(payload + i * testMsgLen, testMsg, testMsgLen);
        payload[testMsgLen * (messageLen / 10)] = '\0';

        uint32_t channelId = 5;

        ServiceLocator serviceLocator("fast+udp: host=1.2.3.4, port=1234");
        session = transport->getSession(serviceLocator);
        FastTransport::ClientSession* clientSession =
            static_cast<FastTransport::ClientSession*>(session.get());
        clientSession->numChannels =
                FastTransport::MAX_NUM_CHANNELS_PER_SESSION;
        clientSession->allocateChannels();

        msg = new FastTransport::OutboundMessage();
        msg->setup(transport, clientSession, channelId, useTimer);

        msg->reset();

        // same as a call to beginSending without the implicit call to send()
        msg->sendBuffer = buffer;
        msg->totalFrags = transport->numFrags(buffer);

        mockTSCValue = tsc;
        dispatch->currentTime = tsc;
    }

    ~OutboundMessageTest()
    {
        delete msg;
        session = NULL;
        if (buffer)
            delete buffer;
        if (transport)
            delete transport;
        mockTSCValue = 0;
    }

    static void sentTimesWindowToString(
                Window<uint64_t, FastTransport::MAX_STAGING_FRAGMENTS + 1>& w,
                string& s)
    {
        size_t max = 50;
        char tmp[max];

        for (uint32_t i = 0; i < w.getLength(); i++) {
            uint64_t val = w[w.getOffset() + i];
            if (val == FastTransport::OutboundMessage::ACKED)
                snprintf(tmp, max, "ACKED, ");
            else
                snprintf(tmp, max, "%lu, ", val);
            s.append(tmp);
        }

        uint32_t trim = 0;
        while (s[s.length() - trim - 1] == ' ')
            trim++;

        s.resize(s.length() - trim);
    }

    DISALLOW_COPY_AND_ASSIGN(OutboundMessageTest);
};

/**
 * Simple check, but also checks subtler details regarding timers.
 * If the message was using the timer reset must remove it.
 */
TEST_F(OutboundMessageTest, reset) {
    setUp(1600, true);
    msg->timer.start(999);

    msg->reset();

    EXPECT_EQ(0, msg->sendBuffer);
    EXPECT_EQ(0U, msg->firstMissingFrag);
    EXPECT_EQ(0U, msg->packetsSinceAckReq);
    string s;
    sentTimesWindowToString(msg->sentTimes, s);
    EXPECT_EQ("0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, "
              "0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, "
              "0, 0, 0,", s);
    EXPECT_EQ(0U, msg->numAcked);
}

TEST_F(OutboundMessageTest, setup) {
    uint32_t channelId = 999;
    bool useTimer = false;

    FastTransport::ClientSession* clientSession =
        static_cast<FastTransport::ClientSession*>(session.get());
    msg->setup(transport, clientSession, channelId, useTimer);

    EXPECT_EQ(clientSession, msg->session);
    EXPECT_EQ(transport, msg->transport);
    EXPECT_EQ(channelId, msg->channelId);
    EXPECT_EQ(useTimer, msg->useTimer);
}

TEST_F(OutboundMessageTest, beginSending) {
    msg->totalFrags = 0;
    msg->sendBuffer = 0;
    msg->beginSending(buffer);
    EXPECT_EQ(buffer, msg->sendBuffer);
    EXPECT_NE(0U, msg->totalFrags);
}

TEST_F(OutboundMessageTest, send) {
    msg->send();
    EXPECT_EQ(
        "{ sessionToken:cccccccccccccccc rpcId:0 clientSessionHint:0 "
        "serverSessionHint:cccccccc 0/2 frags channel:5 dir:0 reqACK:0 "
        "drop:0 payloadType:0 } abcdefghij (+1364 more) | "
        "{ sessionToken:cccccccccccccccc rpcId:0 clientSessionHint:0 "
        "serverSessionHint:cccccccc 1/2 frags channel:5 dir:0 reqACK:0 "
        "drop:0 payloadType:0 } efghijabcd (+217 more)",
        driver->outputLog);
    EXPECT_EQ(tsc, msg->sentTimes[0]);
    EXPECT_EQ(tsc, msg->sentTimes[1]);
}

TEST_F(OutboundMessageTest, send_nothingToSend) {
    msg->sentTimes[0] = tsc - FastTransport::timeoutCycles();
    msg->sentTimes[1] = FastTransport::OutboundMessage::ACKED;
    msg->numAcked = 1;

    msg->send();
    EXPECT_EQ("", driver->outputLog);
}

TEST_F(OutboundMessageTest, send_dueToTimeout) {
    // this will get resent due to timeout
    msg->sentTimes[0] = tsc - FastTransport::timeoutCycles() - 1;
    // note that though this is ready to send it will not go out
    // because the protocol out sends out a single packet when
    // a retransmit occurs
    msg->sentTimes[1] = 0;

    msg->send();
    EXPECT_EQ(
        "{ sessionToken:cccccccccccccccc rpcId:0 clientSessionHint:0 "
        "serverSessionHint:cccccccc 0/2 frags channel:5 dir:0 reqACK:1 "
        "drop:0 payloadType:0 } abcdefghij (+1364 more)",
        driver->outputLog);
    EXPECT_EQ(tsc, msg->sentTimes[0]);
    EXPECT_EQ(0UL, msg->sentTimes[1]);
}

TEST_F(OutboundMessageTest, send_ackAfter) {
    setUp(driver->getMaxPacketSize() * 7, false);

    msg->send();
    string s = driver->outputLog;
    EXPECT_EQ("4/8 frags channel:5 dir:0 reqACK:1",
              s.substr(s.find("4/8"), 34));
}

TEST_F(OutboundMessageTest, send_dontAckLast) {
    setUp(driver->getMaxPacketSize() * 4, false);

    msg->send();
    string s = driver->outputLog;
    EXPECT_EQ("4/5 frags channel:5 dir:0 reqACK:0",
              s.substr(s.find("4/5"), 34));
}

TEST_F(OutboundMessageTest, send_timers) {
    setUp(driver->getMaxPacketSize() * 4, true);

    msg->sentTimes[0] = 100;
    msg->sentTimes[1] = FastTransport::OutboundMessage::ACKED;
    msg->sentTimes[2] = 99;
    msg->sentTimes[3] = 0;

    EXPECT_EQ(0, msg->timer.isRunning());
    msg->send();
    EXPECT_EQ(1, msg->timer.isRunning());
}

TEST_F(OutboundMessageTest, processReceivedAck_noSendBuffer) {
    msg->sendBuffer = NULL;
    bool result = msg->processReceivedAck(0);
    EXPECT_FALSE(result);
}

TEST_F(OutboundMessageTest, processReceivedAck_packetTooShort) {
    TestLog::Enable _;
    msg->send();
    FastTransport::AckResponse ackResp(0, 0);
    MockReceived recvd(0, msg->totalFrags, &ackResp,
                        sizeof(FastTransport::AckResponse) - 1);
    bool result = msg->processReceivedAck(&recvd);
    EXPECT_FALSE(result);
    EXPECT_EQ(
        "processReceivedAck: "
        "ACK packet too short (31 bytes)", TestLog::get());
}

TEST_F(OutboundMessageTest, processReceivedAck_ackPastMessageEnd) {
    TestLog::Enable _;
    msg->send();
    FastTransport::AckResponse ackResp(0, 0);
    ackResp.firstMissingFrag = 3;
    MockReceived recvd(0, msg->totalFrags, &ackResp,
                        sizeof(FastTransport::AckResponse));
    bool result = msg->processReceivedAck(&recvd);
    EXPECT_FALSE(result);
    EXPECT_EQ(
        "processReceivedAck: invalid ACK "
        "(firstMissingFrag 3 > totalFrags 2)", TestLog::get());
}

TEST_F(OutboundMessageTest, processReceivedAck_ackPastWindowEnd) {
    TestLog::Enable _;
    msg->send();
    msg->totalFrags = 50;
    FastTransport::AckResponse ackResp(0, 0);
    ackResp.firstMissingFrag = 34;
    MockReceived recvd(0, msg->totalFrags, &ackResp,
                        sizeof(FastTransport::AckResponse));
    bool result = msg->processReceivedAck(&recvd);
    EXPECT_FALSE(result);
    EXPECT_EQ(
        "processReceivedAck: "
        "invalid ACK (firstMissingFrag 34 beyond end of window 33)",
        TestLog::get());
}

/**
 * No packets missing; just ensure that bookkeeping slides up to
 * match the receiver side.
 */
TEST_F(OutboundMessageTest, processReceivedAck_noneMissing) {
    msg->send();
    string s;
    sentTimesWindowToString(msg->sentTimes, s);
    EXPECT_EQ("20000999, 20000999, 0, 0, 0, "
              "0, 0, 0, 0, 0, 0, "
              "0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, "
              "0, 0, 0, 0, 0, 0,", s);
    s = "";

    FastTransport::AckResponse ackResp(0, 0);
    ackResp.firstMissingFrag = 2;
    MockReceived recvd(0, msg->totalFrags, &ackResp,
                        sizeof(FastTransport::AckResponse));

    bool result = msg->processReceivedAck(&recvd);

    EXPECT_TRUE(result);
    sentTimesWindowToString(msg->sentTimes, s);
    EXPECT_EQ("0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, "
              "0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, "
              "0, 0,", s);
    EXPECT_EQ(ackResp.firstMissingFrag, msg->numAcked);
}

/**
 * A packet is missing; eight packets beyond have been acked.
 * Excercises the bit vector.
 */
TEST_F(OutboundMessageTest, processReceivedAck_oneMissing) {
    msg->send();
    msg->totalFrags = 10;

    string s;
    sentTimesWindowToString(msg->sentTimes, s);
    EXPECT_EQ("20000999, 20000999, 0, 0, 0, "
              "0, 0, 0, 0, 0, 0, "
              "0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, "
              "0, 0, 0, 0, 0, 0,", s);
    s = "";

    FastTransport::AckResponse ackResp(3, 0xff);
    MockReceived recvd(0, msg->totalFrags, &ackResp,
                        sizeof(FastTransport::AckResponse));

    bool result = msg->processReceivedAck(&recvd);

    EXPECT_FALSE(result);
    s = "";
    sentTimesWindowToString(msg->sentTimes, s);
    EXPECT_EQ(
        "20000999, ACKED, ACKED, ACKED, ACKED, ACKED, "
        "ACKED, ACKED, ACKED, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, "
        "0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,",
        s);
    EXPECT_EQ(static_cast<unsigned>(ackResp.firstMissingFrag + 8),
              msg->numAcked);
}

TEST_F(OutboundMessageTest, sendOneData_noRequestAck) {
    msg->sendOneData(0, false);
    EXPECT_EQ(
        "{ sessionToken:cccccccccccccccc rpcId:0 clientSessionHint:0 "
        "serverSessionHint:cccccccc 0/2 frags channel:5 dir:0 reqACK:0 "
        "drop:0 payloadType:0 } abcdefghij (+1364 more)",
            driver->outputLog);
    EXPECT_EQ(1U, msg->packetsSinceAckReq);
}

TEST_F(OutboundMessageTest, sendOneData_requestAck) {
    msg->sendOneData(0, true);
    EXPECT_EQ(
        "{ sessionToken:cccccccccccccccc rpcId:0 clientSessionHint:0 "
        "serverSessionHint:cccccccc 0/2 frags channel:5 dir:0 reqACK:1 "
        "drop:0 payloadType:0 } abcdefghij (+1364 more)",
            driver->outputLog);
    EXPECT_EQ(0U, msg->packetsSinceAckReq);
}

// --- ServerSessionTest ---

class ServerSessionTest: public ::testing::Test {
  public:
    MockDriver* driver;
    FastTransport* transport;
    FastTransport::ServerSession* session;
    const uint32_t sessionId;
    Driver::Address* driverAddress;
    const char* address;
    const uint16_t port;

    ServerSessionTest()
        : driver(NULL)
        , transport(NULL)
        , session(NULL)
        , sessionId(0x98765432)
        , driverAddress(NULL)
        , address("1.2.3.4")
        , port(12345)
    {
        dispatch->currentTime = 1000;
        driver = new MockDriver(FastTransport::Header::headerToString);
        transport = new FastTransport(driver);
        session = new FastTransport::ServerSession(transport, sessionId);
        ServiceLocator sl("mock: host=1.2.3.4, port=12345");
        driverAddress = driver->newAddress(sl);
        delete serviceManager;
        ServiceManager::init();
    }

    ~ServerSessionTest()
    {
        delete driverAddress;
        delete session;
        delete transport;
    }

  private:
    DISALLOW_COPY_AND_ASSIGN(ServerSessionTest);
};

TEST_F(ServerSessionTest, beginSending) {
    dispatch->currentTime = 9898;

    uint8_t channelId = 6;
    // Just here to flip us into a state where
    // channel.rpcId == 0 and we have an RPC setup
    EXPECT_EQ(FastTransport::ServerSession::
              ServerChannel::INVALID_RPC_ID,
              session->channels[0].rpcId);
    MockReceived junk(0, 1, "foo");
    junk.getHeader()->channelId = channelId;
    session->processInboundPacket(&junk);

    session->beginSending(channelId);
    EXPECT_EQ(FastTransport::ServerSession::
              ServerChannel::SENDING_WAITING,
              session->channels[channelId].state);
    EXPECT_EQ(dispatch->currentTime, session->lastActivityTime);
}

TEST_F(ServerSessionTest, expire_channelStillProcessing) {
    session->lastActivityTime = 1;
    session->channels[FastTransport::NUM_CHANNELS_PER_SESSION - 1].state =
        FastTransport::ServerSession::ServerChannel::PROCESSING;
    EXPECT_FALSE(session->expire());
    session->channels[FastTransport::NUM_CHANNELS_PER_SESSION - 1].state =
        FastTransport::ServerSession::ServerChannel::IDLE;
}

TEST_F(ServerSessionTest, expire_channelRecvOrSendWait) {
    session->lastActivityTime = 1;
    for (uint32_t i = 0; i < FastTransport::NUM_CHANNELS_PER_SESSION; i++)
        session->channels[i].state =
            FastTransport::ServerSession::ServerChannel::IDLE;
    uint32_t magic = 19281;
    session->channels[0].rpcId = magic;
    session->channels[1].state =
        FastTransport::ServerSession::ServerChannel::RECEIVING;
    session->channels[1].currentRpc.setup(session, 1);
    session->channels[2].state =
        FastTransport::ServerSession::ServerChannel::SENDING_WAITING;
    session->channels[2].currentRpc.setup(session, 2);

    EXPECT_TRUE(session->expire());

    // ensure 0 got skipped
    EXPECT_EQ(FastTransport::ServerSession::ServerChannel::IDLE,
              session->channels[0].state);
    EXPECT_EQ(magic, session->channels[0].rpcId);

    // ensure 1 got reset
    EXPECT_EQ(FastTransport::ServerSession::ServerChannel::IDLE,
              session->channels[1].state);
    EXPECT_EQ(~(0u), session->channels[1].rpcId);
    EXPECT_EQ(0, session->channels[1].currentRpc.session);

    // ensure 2 got reset
    EXPECT_EQ(FastTransport::ServerSession::ServerChannel::IDLE,
              session->channels[2].state);
    EXPECT_EQ(~(0u), session->channels[2].rpcId);
    EXPECT_EQ(0, session->channels[2].currentRpc.session);

    // check the minor tid-bits at the end
    EXPECT_EQ(FastTransport::ServerSession::INVALID_TOKEN,
              session->token);
    EXPECT_EQ(FastTransport::ServerSession::INVALID_HINT,
              session->clientSessionHint);
}

TEST_F(ServerSessionTest, processInboundPacket_badChannel) {
    TestLog::Enable dummy;
    dispatch->currentTime = 9898;

    MockReceived recvd(0, 1, "");
    recvd.getHeader()->channelId = FastTransport::NUM_CHANNELS_PER_SESSION;

    session->processInboundPacket(&recvd);
    EXPECT_EQ(dispatch->currentTime, session->lastActivityTime);
    EXPECT_EQ(
        "processInboundPacket: "
        "invalid channel id 8", TestLog::get());
}

/// A predicate to limit TestLog messages to processInboundPacket
static bool pipPred(string s)
{
    return (s == "processInboundPacket");
}

TEST_F(ServerSessionTest, processInboundPacket_currentRpcReceivedDataPacket) {
    TestLog::Enable _(&pipPred);

    // Just here to flip us into a state where
    // channel.rpcId == 0
    EXPECT_EQ(FastTransport::ServerSession::
              ServerChannel::INVALID_RPC_ID,
              session->channels[0].rpcId);
    MockReceived junk(0, 2, "foo");
    // Just to start the currentRpc
    session->processInboundPacket(&junk);
    TestLog::reset();

    EXPECT_EQ(0U, session->channels[0].rpcId);
    // This one exercises the code path we are interested in
    MockReceived recvd(0, 1, "foo");
    session->processInboundPacket(&recvd);
    EXPECT_EQ(
        "processInboundPacket: "
        "processReceivedData",
        TestLog::get());
}

TEST_F(ServerSessionTest, processInboundPacket_currentRpcReceivedAckPacket) {
    TestLog::Enable _;

    // Just here to flip us into a state where
    // channel.rpcId == 0
    EXPECT_EQ(FastTransport::ServerSession::
              ServerChannel::INVALID_RPC_ID,
              session->channels[0].rpcId);
    MockReceived junk(0, 2, "foo");
    session->processInboundPacket(&junk);
    TestLog::reset();

    FastTransport::AckResponse ackResp(1, 0);
    MockReceived recvd(0, 1, &ackResp, sizeof(FastTransport::AckResponse));

    session->processInboundPacket(&recvd);
    EXPECT_EQ(
        "processInboundPacket: "
        "processReceivedAck",
        TestLog::get());
}

TEST_F(ServerSessionTest, processInboundPacket_currentRpcBadPayloadType) {
    TestLog::Enable _;

    MockReceived junk(0, 2, "foo");
    session->processInboundPacket(&junk);
    TestLog::reset();

    MockReceived recvd(1, 2, "foo");
    recvd.getHeader()->payloadType = FastTransport::Header::RESERVED1;
    session->processInboundPacket(&recvd);
    EXPECT_EQ(
        "processInboundPacket: "
        "current rpcId has bad packet type 3",
        TestLog::get());
}

TEST_F(ServerSessionTest, processInboundPacket_nextRpcReceivedDataPacket) {
    TestLog::Enable _;
    MockReceived recvd(0, 1, "God hates ponies.");

    session->processInboundPacket(&recvd);
    EXPECT_EQ(
        "processInboundPacket: "
        "start a new RPC | "
        "processInboundPacket: "
        "processReceivedData",
        TestLog::get());
    session->channels[0].state =
            FastTransport::ServerSession::ServerChannel::IDLE;
}

TEST_F(ServerSessionTest, processInboundPacket_newRpcBadPayloadType) {
    TestLog::Enable _(&pipPred);

    MockReceived recvd(0, 2, "foo");
    recvd.getHeader()->payloadType = FastTransport::Header::RESERVED1;
    session->processInboundPacket(&recvd);
    EXPECT_EQ(
        "processInboundPacket: "
        "start a new RPC | "
        "processInboundPacket: "
        "new rpcId has bad type 3",
        TestLog::get());
}

TEST_F(ServerSessionTest, startSession) {
    dispatch->currentTime = 9898;
    const uint64_t rand = 0x7676UL;
    MockRandom __(rand);

    uint32_t clientSessionHint = 0x12345678u;
    session->startSession(driverAddress, clientSessionHint);
    EXPECT_TRUE(
        *static_cast<MockDriver::MockAddress*>(driverAddress) ==
        *static_cast<MockDriver::MockAddress*>(
            session->clientAddress.get()));
    EXPECT_EQ(clientSessionHint, session->clientSessionHint);
    EXPECT_EQ(rand, session->token);
    EXPECT_EQ(
        "{ sessionToken:7676 rpcId:0 clientSessionHint:12345678 "
        "serverSessionHint:98765432 0/0 frags channel:0 dir:1 reqACK:0 "
        "drop:0 payloadType:2 } /x08",
        driver->outputLog);
    EXPECT_EQ(9898LU, session->lastActivityTime);
}

TEST_F(ServerSessionTest, processReceivedData_receiving) {
    FastTransport::ServerSession::ServerChannel* channel =
            &session->channels[0];
    channel->state =
            FastTransport::ServerSession::ServerChannel::RECEIVING;
    channel->currentRpc.setup(session, 0);
    uint16_t totalFrags = 2;
    Buffer recvBuffer;
    channel->inboundMsg.init(totalFrags, &recvBuffer);

    // if not taken (not last fragment)
    MockReceived firstRecvd(0, totalFrags, "first");
    session->processReceivedData(channel, &firstRecvd);
    EXPECT_EQ("first", TestUtil::bufferToDebugString(&recvBuffer));
    EXPECT_EQ(FastTransport::ServerSession::
                            ServerChannel::RECEIVING,
                            session->channels[0].state);

    // if taken (last fragment)
    MockReceived lastRecvd(1, totalFrags, "last");
    session->processReceivedData(channel, &lastRecvd);
    EXPECT_EQ("first | last",
              TestUtil::bufferToDebugString(&recvBuffer));
    EXPECT_EQ(&channel->currentRpc,
              serviceManager->waitForRpc(0));
    EXPECT_EQ(FastTransport::ServerSession::
              ServerChannel::PROCESSING,
              session->channels[0].state);
    session->channels[0].state =
            FastTransport::ServerSession::ServerChannel::IDLE;
}

TEST_F(ServerSessionTest, processReceivedData_processing) {
    MockReceived recvd(0, 1, "");
    recvd.getHeader()->requestAck = 1;

    FastTransport::ServerSession::ServerChannel* channel =
            &session->channels[0];
    channel->state =
            FastTransport::ServerSession::ServerChannel::PROCESSING;
    session->processReceivedData(channel, &recvd);

    EXPECT_FALSE(string::npos == driver->outputLog.find("payloadType:1"));
    channel->state = FastTransport::ServerSession::ServerChannel::IDLE;
}

// --- ClientSessionTest ---

class ClientSessionTest: public ::testing::Test {
  public:
    MockDriver* driver;
    FastTransport* transport;
    FastTransport::ClientSession* session;
    Buffer* request;
    Buffer* response;
    const uint32_t sessionId;
    sockaddr_in addr;
    sockaddr* addrp;
    socklen_t addrLen;
    const char* address;
    const uint16_t port;

    ClientSessionTest()
        : driver(NULL)
        , transport(NULL)
        , session(NULL)
        , request(NULL)
        , response(NULL)
        , sessionId(0x98765432)
        , addr()
        , addrp(NULL)
        , addrLen(0)
        , address("1.2.3.4")
        , port(12345)
    {
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        assert(inet_aton(&address[0], &addr.sin_addr));
        addrLen = sizeof(addr);
        addrp = reinterpret_cast<sockaddr*>(&addr);

        driver = new MockDriver(FastTransport::Header::headerToString);
        transport = new FastTransport(driver);
        session = new FastTransport::ClientSession(transport, sessionId);
        request = new Buffer();
        response = new Buffer();
    }

    ~ClientSessionTest()
    {
        delete response;
        delete request;
        delete session;
        delete transport;
    }

  private:
    DISALLOW_COPY_AND_ASSIGN(ClientSessionTest);
};

TEST_F(ClientSessionTest, clientSend_notConnected) {
    EXPECT_FALSE(session->isConnected());
    EXPECT_TRUE(session->channelQueue.empty());
    FastTransport::ClientRpc* rpc = session->clientSend(request, response);
    EXPECT_EQ(rpc, &session->channelQueue.front());
    session->channelQueue.pop_front(); // satisfy boost assertion;
}

TEST_F(ClientSessionTest, clientSend_noAvailableChannel) {
    EXPECT_TRUE(session->channelQueue.empty());
    FastTransport::ClientRpc* rpc = session->clientSend(request, response);
    EXPECT_EQ(rpc, &session->channelQueue.front());
    session->channelQueue.pop_front(); // satisfy boost assertion;
}

TEST_F(ClientSessionTest, clientSend_availableChannel) {
    dispatch->currentTime = 98328;

    session->numChannels = FastTransport::MAX_NUM_CHANNELS_PER_SESSION;
    session->allocateChannels();

    FastTransport::ClientRpc* rpc = session->clientSend(request,
                                                        response);
    FastTransport::ClientSession::ClientChannel* channel =
            &session->channels[0];
    EXPECT_EQ(FastTransport::ClientSession::
              ClientChannel::SENDING,
              channel->state);
    EXPECT_EQ(rpc, channel->currentRpc);
    channel->currentRpc = NULL;
    EXPECT_EQ(dispatch->currentTime, session->lastActivityTime);
}

TEST_F(ClientSessionTest, close) {
    session->numChannels = FastTransport::MAX_NUM_CHANNELS_PER_SESSION;
    session->allocateChannels();

    FastTransport::ClientRpc rpc1(session, request, response);
    FastTransport::ClientRpc rpc2(session, request, response);
    FastTransport::ClientRpc rpc3(session, request, response);
    session->channelQueue.push_back(rpc3);

    session->channels[0].currentRpc = &rpc1;
    session->channels[1].currentRpc = &rpc2;

    session->close();
    EXPECT_TRUE(rpc1.isReady());
    EXPECT_EQ("RPC aborted (session closed)", *rpc1.errorMessage);
    EXPECT_TRUE(rpc2.isReady());
    EXPECT_EQ("RPC aborted (session closed)", *rpc2.errorMessage);
    EXPECT_TRUE(session->channelQueue.empty());
    EXPECT_EQ("RPC aborted (session closed)", *rpc3.errorMessage);
    EXPECT_EQ(FastTransport::ClientSession::INVALID_HINT,
              session->serverSessionHint);
    EXPECT_EQ(FastTransport::ClientSession::INVALID_TOKEN,
              session->token);
}

TEST_F(ClientSessionTest, connect) {
    dispatch->currentTime = 91291;

    ServiceLocator serviceLocator("fast+udp: host=1.2.3.4, port=12345");
    session->init(serviceLocator);

    session->connect();
    EXPECT_TRUE(session->sessionOpenRequestInFlight);

    // ensure the second call doesn't send an additional SessionOpenReq
    session->connect();
    EXPECT_EQ(
        "{ sessionToken:cccccccccccccccc rpcId:0 "
        "clientSessionHint:98765432 serverSessionHint:cccccccc "
        "0/0 frags channel:0 dir:0 reqACK:0 drop:0 payloadType:2 } ",
        driver->outputLog);
    EXPECT_EQ(dispatch->currentTime, session->lastActivityTime);
}

TEST_F(ClientSessionTest, connect_sessionOpenRequestRetransmit) {
    Tub<MockTSC> tsc;
    tsc.construct(91291);

    ServiceLocator serviceLocator("fast+udp: host=1.2.3.4, port=12345");
    session->init(serviceLocator);

    session->connect();
    EXPECT_TRUE(session->sessionOpenRequestInFlight);


    tsc.construct(dispatch->currentTime + FastTransport::timeoutCycles()
        + 1);
    dispatch->poll();

    EXPECT_EQ(
        "{ sessionToken:cccccccccccccccc rpcId:0 "
        "clientSessionHint:98765432 serverSessionHint:cccccccc "
        "0/0 frags channel:0 dir:0 reqACK:0 drop:0 payloadType:2 }  | "
        "{ sessionToken:cccccccccccccccc rpcId:0 "
        "clientSessionHint:98765432 serverSessionHint:cccccccc "
        "0/0 frags channel:0 dir:0 reqACK:0 drop:0 payloadType:2 } ",
        driver->outputLog);
}

TEST_F(ClientSessionTest, connect_sessionOpenRequestTimeout) {
    dispatch->currentTime = 91291;
    Tub<MockTSC> tsc;
    tsc.construct(dispatch->currentTime);

    ServiceLocator serviceLocator("fast+udp: host=1.2.3.4, port=12345");
    session->init(serviceLocator);

    session->connect();
    EXPECT_TRUE(session->sessionOpenRequestInFlight);

    FastTransport::ClientRpc rpc(session, request, response);
    session->channelQueue.push_back(rpc);

    tsc.construct(dispatch->currentTime +
            2*FastTransport::sessionTimeoutCycles());
    dispatch->poll();
    EXPECT_FALSE(session->sessionOpenRequestInFlight);

    EXPECT_EQ("RPC aborted (session closed)", *rpc.errorMessage);
    EXPECT_FALSE(rpc.channelQueueEntries.is_linked());
}

TEST_F(ClientSessionTest, expire_activeRef) {
    session->numChannels = FastTransport::MAX_NUM_CHANNELS_PER_SESSION;
    session->allocateChannels();
    FastTransport::SessionRef s(session);
    bool didClose = session->expire();
    EXPECT_FALSE(didClose);
}

TEST_F(ClientSessionTest, expire_activeOnChannel) {
    session->numChannels = FastTransport::MAX_NUM_CHANNELS_PER_SESSION;
    session->allocateChannels();

    FastTransport::ClientRpc rpc(session, request, response);
    session->channels[0].currentRpc = &rpc;

    bool didClose = session->expire();
    EXPECT_FALSE(didClose);
    session->channels[0].currentRpc = NULL;
}

TEST_F(ClientSessionTest, expire_rpcQueued) {
    session->numChannels = FastTransport::MAX_NUM_CHANNELS_PER_SESSION;
    session->allocateChannels();

    FastTransport::ClientRpc rpc(session, request, response);
    session->channelQueue.push_back(rpc);

    bool didClose = session->expire();
    EXPECT_FALSE(didClose);

    session->channelQueue.pop_front(); // satisfy boost assertion;
}

TEST_F(ClientSessionTest, expire_nothingActive) {
    session->numChannels = FastTransport::MAX_NUM_CHANNELS_PER_SESSION;
    session->allocateChannels();

    bool didClose = session->expire();
    EXPECT_TRUE(didClose);
}

TEST_F(ClientSessionTest, fillHeader) {
    FastTransport::Header header;
    session->numChannels = FastTransport::MAX_NUM_CHANNELS_PER_SESSION;
    session->allocateChannels();
    session->fillHeader(&header, 6);
    EXPECT_EQ(
        "{ sessionToken:cccccccccccccccc rpcId:0 "
        "clientSessionHint:98765432 serverSessionHint:cccccccc "
        "0/0 frags channel:6 dir:0 reqACK:0 drop:0 payloadType:0 }",
        header.toString());
}

TEST_F(ClientSessionTest, init) {
    ServiceLocator serviceLocator("fast+udp: host=1.2.3.4, port=0x3742");
    session->init(serviceLocator);
    EXPECT_TRUE(NULL != session->serverAddress.get());
}

TEST_F(ClientSessionTest, processInboundPacket_sessionOpen) {
    FastTransport::SessionOpenResponse sessResp =
            { FastTransport::NUM_CHANNELS_PER_SESSION };
    MockReceived recvd(0, 1, &sessResp, sizeof(sessResp));
    recvd.getHeader()->payloadType = FastTransport::Header::SESSION_OPEN;

    session->processInboundPacket(&recvd);
    EXPECT_TRUE(session->isConnected());
}

TEST_F(ClientSessionTest, processInboundPacket_invalidChannel) {
    TestLog::Enable _;
    MockReceived recvd(0, 1, "");

    session->processInboundPacket(&recvd);
    EXPECT_FALSE(session->isConnected());
    EXPECT_EQ(
        "processInboundPacket: invalid channel id 0",
        TestLog::get());
}

TEST_F(ClientSessionTest, processInboundPacket_data) {
    dispatch->currentTime = 91291;

    session->numChannels = FastTransport::MAX_NUM_CHANNELS_PER_SESSION;
    session->allocateChannels();
    FastTransport::ClientSession::ClientChannel* channel =
            session->getAvailableChannel();
    EXPECT_TRUE(channel != NULL);
    channel->state = FastTransport::ClientSession::ClientChannel::SENDING;
    channel->currentRpc = new FastTransport::ClientRpc(session,
                                                       request,
                                                       response);

    MockReceived recvd(0, 2, "first of two");
    session->processInboundPacket(&recvd);
    EXPECT_EQ(FastTransport::ClientSession::
              ClientChannel::RECEIVING,
              channel->state);

    EXPECT_EQ(dispatch->currentTime, session->lastActivityTime);
    channel->currentRpc = NULL;
}

TEST_F(ClientSessionTest, processInboundPacket_ack) {
    session->numChannels = FastTransport::MAX_NUM_CHANNELS_PER_SESSION;
    session->allocateChannels();
    FastTransport::ClientSession::ClientChannel* channel =
            session->getAvailableChannel();
    EXPECT_TRUE(channel != NULL);
    channel->state = FastTransport::ClientSession::ClientChannel::SENDING;

    channel->outboundMsg.totalFrags = 5;
    channel->outboundMsg.sendBuffer = request;

    FastTransport::AckResponse ackResp(2, 0);
    MockReceived recvd(0, 5, &ackResp, sizeof(FastTransport::AckResponse));
    recvd.getHeader()->payloadType = FastTransport::Header::ACK;

    session->processInboundPacket(&recvd);
    EXPECT_EQ(2U, channel->outboundMsg.firstMissingFrag);
}

TEST_F(ClientSessionTest, processInboundPacket_badSession) {
    session->numChannels = FastTransport::MAX_NUM_CHANNELS_PER_SESSION;
    session->allocateChannels();
    EXPECT_TRUE(session->channelQueue.empty());

    // Put an RPC on one of the channels
    FastTransport::ClientRpc* rpc =
            new FastTransport::ClientRpc(session, request, response);
    session->channels[1].currentRpc = rpc;

    MockReceived recvd(0, 1, "");
    recvd.getHeader()->payloadType = FastTransport::Header::BAD_SESSION;

    session->processInboundPacket(&recvd);

    // Make sure the RPC made it back onto the queue safely
    EXPECT_EQ(rpc, &session->channelQueue.front());

    EXPECT_EQ(FastTransport::ClientSession::INVALID_HINT,
              session->serverSessionHint);
    EXPECT_EQ(FastTransport::ClientSession::INVALID_TOKEN,
              session->token);
    EXPECT_EQ(
        "{ sessionToken:cccccccccccccccc rpcId:0 "
        "clientSessionHint:98765432 serverSessionHint:cccccccc "
        "0/0 frags channel:0 dir:0 reqACK:0 drop:0 payloadType:2 } ",
        driver->outputLog);
    session->channelQueue.pop_front();
}

TEST_F(ClientSessionTest, processInboundPacket_redundantOpenResponse) {
    TestLog::Enable _;
    session->numChannels = FastTransport::MAX_NUM_CHANNELS_PER_SESSION;
    session->allocateChannels();
    FastTransport::SessionOpenResponse sessResp =
            { FastTransport::NUM_CHANNELS_PER_SESSION };
    MockReceived recvd(0, 1, &sessResp, sizeof(sessResp));
    recvd.getHeader()->payloadType = FastTransport::Header::SESSION_OPEN;
    session->processInboundPacket(&recvd);
    EXPECT_EQ("", TestLog::get());
}

TEST_F(ClientSessionTest, processInboundPacket_badPayloadType) {
    TestLog::Enable _;
    session->numChannels = FastTransport::MAX_NUM_CHANNELS_PER_SESSION;
    session->allocateChannels();
    FastTransport::ClientSession::ClientChannel* channel =
            session->getAvailableChannel();
    EXPECT_TRUE(NULL != channel);
    channel->state = FastTransport::ClientSession::ClientChannel::SENDING;
    channel->currentRpc = new FastTransport::ClientRpc(session,
                                                       request, response);

    MockReceived recvd(0, 2, "packet data");
    recvd.getHeader()->payloadType = FastTransport::Header::RESERVED1;
    session->processInboundPacket(&recvd);
    channel->currentRpc = NULL;
    EXPECT_EQ(
        "processInboundPacket: bad payload type 3",
        TestLog::get());
}

TEST_F(ClientSessionTest, processInboundPacket_stalePacket) {
    TestLog::Enable _;
    session->numChannels = FastTransport::MAX_NUM_CHANNELS_PER_SESSION;
    session->allocateChannels();
    FastTransport::ClientSession::ClientChannel* channel =
            session->getAvailableChannel();
    EXPECT_TRUE(NULL != channel);
    channel->state = FastTransport::ClientSession::ClientChannel::SENDING;
    channel->currentRpc = new FastTransport::ClientRpc(session,
                                                       request, response);

    MockReceived recvd(0, 2, "packet data");
    recvd.getHeader()->rpcId = 3;
    channel->rpcId = 4;
    session->processInboundPacket(&recvd);
    channel->currentRpc = NULL;
    EXPECT_EQ(
        "processInboundPacket: "
        "out-of-order packet (got rpcId 3, current rpcId 4)",
        TestLog::get());
}

TEST_F(ClientSessionTest, sendSessionOpenRequest) {
    dispatch->currentTime = 91291;

    ServiceLocator serviceLocator("fast+udp: host=1.2.3.4, port=12345");
    session->init(serviceLocator);
    session->sendSessionOpenRequest();
    EXPECT_EQ(
        "{ sessionToken:cccccccccccccccc rpcId:0 "
        "clientSessionHint:98765432 serverSessionHint:cccccccc "
        "0/0 frags channel:0 dir:0 reqACK:0 drop:0 payloadType:2 } ",
        driver->outputLog);
    EXPECT_EQ(dispatch->currentTime, session->lastActivityTime);
    session->timer.stop();
}

TEST_F(ClientSessionTest, cancelRpc_rpcAssignedToChannel) {
    session->numChannels = 3;
    session->allocateChannels();
    Buffer request2, response2, request3, response3;
    session->clientSend(request, response);
    session->clientSend(&request2, &response2);
    FastTransport::ClientRpc* rpc3 = session->clientSend(
            &request3, &response3);
    EXPECT_TRUE(session->channels[2].currentRpc != NULL);
    rpc3->cancel();
    EXPECT_TRUE(session->channels[2].currentRpc == NULL);
    session->close();
}

TEST_F(ClientSessionTest, cancelRpc_rpcWaitingForChannel) {
    session->numChannels = 2;
    session->allocateChannels();
    Buffer request2, response2, request3, response3;
    session->clientSend(request, response);
    session->clientSend(&request2, &response2);
    FastTransport::ClientRpc* rpc3 = session->clientSend(
            &request3, &response3);
    EXPECT_EQ(1U, session->channelQueue.size());
    rpc3->cancel();
    EXPECT_EQ(0U, session->channelQueue.size());
    session->close();
}

TEST_F(ClientSessionTest, allocateChannels) {
    EXPECT_TRUE(NULL == session->channels);
    session->allocateChannels();
    EXPECT_TRUE(NULL != session->channels);
    for (uint32_t i = 0; i < session->numChannels; i++)
        EXPECT_EQ(0, session->channels[i].currentRpc);
}

TEST_F(ClientSessionTest, getAvailableChannel) {
    session->numChannels = FastTransport::MAX_NUM_CHANNELS_PER_SESSION;
    session->allocateChannels();
    uint32_t i = 0;
    for (;;) {
        FastTransport::ClientSession::ClientChannel* channel =
            session->getAvailableChannel();
        if (!channel)
            break;
        channel->state =
                FastTransport::ClientSession::ClientChannel::SENDING;
        i++;
    }
    EXPECT_EQ(session->numChannels, i);
}

TEST_F(ClientSessionTest, processReceivedData_transitionSendToReceive) {
    FastTransport::ClientRpc rpc(session, request, response);
    session->channelQueue.push_back(rpc);

    FastTransport::SessionOpenResponse sessResp =
            { FastTransport::NUM_CHANNELS_PER_SESSION };
    MockReceived initRecvd(0, 1, &sessResp, sizeof(sessResp));
    session->processSessionOpenResponse(&initRecvd);

    MockReceived recvd(0, 2, "God hates ponies.");
    FastTransport::ClientSession::ClientChannel* channel =
            &session->channels[0];
    session->processReceivedData(channel, &recvd);
    EXPECT_EQ(FastTransport::ClientSession::
              ClientChannel::RECEIVING,
              channel->state);
    channel->currentRpc = NULL;
}

TEST_F(ClientSessionTest, processReceivedData_rpcFinished) {
    FastTransport::ClientRpc rpc(session, request, response);
    session->channelQueue.push_back(rpc);

    FastTransport::SessionOpenResponse sessResp =
            { FastTransport::NUM_CHANNELS_PER_SESSION };
    MockReceived initRecvd(0, 1, &sessResp, sizeof(sessResp));
    session->processSessionOpenResponse(&initRecvd);

    MockReceived recvd(0, 1, "God hates ponies.");
    FastTransport::ClientSession::ClientChannel* channel =
            &session->channels[0];
    session->processReceivedData(channel, &recvd);
    EXPECT_TRUE(rpc.isReady());
    EXPECT_EQ(0, channel->currentRpc);
}

TEST_F(ClientSessionTest, reassignChannel_queueEmpty) {
    FastTransport::ClientRpc rpc(session, request, response);
    session->channelQueue.push_back(rpc);

    FastTransport::SessionOpenResponse sessResp =
            { FastTransport::NUM_CHANNELS_PER_SESSION };
    MockReceived initRecvd(0, 1, &sessResp, sizeof(sessResp));
    session->processSessionOpenResponse(&initRecvd);

    MockReceived recvd(0, 1, "God hates ponies.");
    FastTransport::ClientSession::ClientChannel* channel =
            &session->channels[0];
    uint32_t prevRpcId = channel->rpcId;
    session->processReceivedData(channel, &recvd);
    EXPECT_EQ(prevRpcId + 1, channel->rpcId);
    EXPECT_EQ(FastTransport::ClientSession::ClientChannel::IDLE,
              channel->state);
    EXPECT_EQ(0, channel->currentRpc);
}

TEST_F(ClientSessionTest, reassignChannel_getWorkFromQueue) {
    FastTransport::ClientRpc rpc1(session, request, response);
    session->channelQueue.push_back(rpc1);

    FastTransport::SessionOpenResponse sessResp =
            { FastTransport::NUM_CHANNELS_PER_SESSION };
    MockReceived initRecvd(0, 1, &sessResp, sizeof(sessResp));
    session->processSessionOpenResponse(&initRecvd);

    FastTransport::ClientRpc rpc2(session, request, response);
    session->channelQueue.push_back(rpc2);

    MockReceived recvd(0, 1, "God hates ponies.");
    FastTransport::ClientSession::ClientChannel* channel =
                &session->channels[0];
    session->processReceivedData(channel, &recvd);
    EXPECT_EQ(1U, channel->rpcId);
    EXPECT_EQ(FastTransport::ClientSession::
              ClientChannel::SENDING,
              channel->state);
    EXPECT_EQ(&rpc2, channel->currentRpc);
    EXPECT_TRUE(session->channelQueue.empty());
    channel->currentRpc = NULL;
}

TEST_F(ClientSessionTest, processSessionOpenResponse) {
    FastTransport::SessionOpenResponse sessResp =
            { FastTransport::NUM_CHANNELS_PER_SESSION };
    MockReceived recvd(0, 1, &sessResp, sizeof(sessResp));
    FastTransport::Header* header = recvd.getHeader();
    header->serverSessionHint = 0x192837;
    header->sessionToken = 0x1212343456567878;

    // Insert an RPC into the work queue
    FastTransport::ClientRpc rpc(session, request, response);
    session->channelQueue.push_back(rpc);

    session->processSessionOpenResponse(&recvd);

    EXPECT_TRUE(session->isConnected());
    EXPECT_EQ(header->serverSessionHint,
              session->serverSessionHint);
    EXPECT_EQ(header->sessionToken, session->token);
    EXPECT_EQ(FastTransport::NUM_CHANNELS_PER_SESSION,
              session->numChannels);

    // Make sure our queued RPC made it onto the channel from the queue
    EXPECT_EQ(&rpc, session->channels[0].currentRpc);
    EXPECT_TRUE(session->channelQueue.empty());

    // Make sure we bailed on once the queue was empty
    EXPECT_EQ(0, session->channels[1].currentRpc);
    session->channels[0].currentRpc = NULL;
}

TEST_F(ClientSessionTest, processSessionOpenResponse_tooManyChannelsOnServer) {
    FastTransport::SessionOpenResponse sessResp =
            { FastTransport::NUM_CHANNELS_PER_SESSION + 1 };
    MockReceived recvd(0, 1, &sessResp, sizeof(sessResp));

    session->processSessionOpenResponse(&recvd);

    EXPECT_EQ(FastTransport::NUM_CHANNELS_PER_SESSION,
              session->numChannels);
}

// --- SessionTableTest ---

class SessionTableTest : public ::testing::Test {

  public:
    SessionTableTest()
    {}

  private:
    DISALLOW_COPY_AND_ASSIGN(SessionTableTest);
};

TEST_F(SessionTableTest, sanity) {
    FastTransport::SessionTable<MockSession> st(NULL);
    MockSession* s[5];

    EXPECT_EQ(FastTransport::SessionTable<MockSession>::TAIL,
              st.firstFree);
    s[0] = st.get();
    EXPECT_EQ(0U, s[0]->id);
    EXPECT_EQ(FastTransport::SessionTable<MockSession>::NONE,
              s[0]->nextFree);

    s[1] = st.get();
    EXPECT_EQ(1U, s[1]->id);
    EXPECT_EQ(FastTransport::SessionTable<MockSession>::NONE,
              s[1]->nextFree);

    s[2] = st.get();
    EXPECT_EQ(2U, s[2]->id);
    EXPECT_EQ(FastTransport::SessionTable<MockSession>::NONE,
              s[2]->nextFree);

    s[3] = st.get();
    EXPECT_EQ(3U, s[3]->id);
    EXPECT_EQ(FastTransport::SessionTable<MockSession>::NONE,
              s[3]->nextFree);

    st.put(s[3]);
    EXPECT_EQ(st.firstFree, s[3]->id);
    EXPECT_EQ(FastTransport::SessionTable<MockSession>::TAIL,
              s[3]->nextFree);
    s[3] = st.get();
    EXPECT_EQ(3U, s[3]->id);
    EXPECT_EQ(FastTransport::SessionTable<MockSession>::NONE,
              s[3]->nextFree);

    st.put(s[2]);
    s[2] = st.get();
    EXPECT_EQ(2U, s[2]->id);
    EXPECT_EQ(FastTransport::SessionTable<MockSession>::NONE,
              s[2]->nextFree);

    st.put(s[0]);
    st.put(s[2]);
    EXPECT_EQ(2U, st.firstFree);
    EXPECT_EQ(0U, s[2]->nextFree);
    EXPECT_EQ(FastTransport::SessionTable<MockSession>::TAIL,
              s[0]->nextFree);

}

TEST_F(SessionTableTest, operator_brackets) {
    FastTransport::SessionTable<MockSession> st(NULL);
    MockSession* s = st.get();
    EXPECT_EQ(s, st[0]);
}

TEST_F(SessionTableTest, get) {
    FastTransport::SessionTable<MockSession> st(NULL);
    MockSession* s = st.get();
    EXPECT_EQ(1U, st.size());

    st.put(s);
    EXPECT_EQ(s, st.get());
    EXPECT_EQ(1U, st.size());

    EXPECT_TRUE(s != st.get());
    EXPECT_EQ(2U, st.size());
}

TEST_F(SessionTableTest, put) {
    FastTransport::SessionTable<MockSession> st(NULL);
    MockSession* s[2];
    s[0] = st.get();
    s[1] = st.get();
    st.put(s[0]);
    EXPECT_EQ(0U, st.firstFree);
    EXPECT_EQ(FastTransport::SessionTable<MockSession>::TAIL,
                            s[0]->nextFree);
    EXPECT_EQ(FastTransport::SessionTable<MockSession>::NONE,
                            s[1]->nextFree);
}

TEST_F(SessionTableTest, expire) {
    dispatch->currentTime = FastTransport::sessionTimeoutCycles();
    FastTransport::SessionTable<MockSession> st(NULL);

    // Make sure it runs/doesn't segfault on 0 length
    st.expire();

    // Non-trivial test - expires some, not others
    for (uint32_t i = 0; i < 3; i++) {
        st.get();
        // even numbered sessions are up for expire
        st[i]->setLastActivityTime(i % 2 ? dispatch->currentTime : 0);
    }

    st.expire();

    // One tricky bit, expire records lastCleanedIndex starting at 0 so
    // the first item to be cleaned is 1
    EXPECT_EQ(0U, st.firstFree);
    EXPECT_EQ(2U, st[0]->nextFree);
    EXPECT_EQ(FastTransport::SessionTable<MockSession>::TAIL,
              st[2]->nextFree);
}

}  // namespace RAMCloud
