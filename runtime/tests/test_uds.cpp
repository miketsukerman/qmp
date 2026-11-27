// SPDX-License-Identifier: Apache-2.0
// UDS transport (ADR-004, bench and host-test only): the transport contract over real
// AF_UNIX/SOCK_SEQPACKET sockets, the core end to end on top of it, and two-process
// scenarios (server killed and restarted, subscriber killed mid-stream).
#include <gtest/gtest.h>

#include <qmp/qmp_client.h>
#include <qmp/qmp_server.h>
#include <qmp/qmp_transport.h>

#if defined(QMP_TRANSPORT_UDS_BENCH_ONLY)

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <dirent.h>
#include <sys/wait.h>
#include <unistd.h>

#include "joint.qmp.h"

namespace {

using namespace std::chrono_literals;

constexpr uint64_t kMs = 1000ull * 1000;

/* Every test process gets a private socket directory, so parallel ctest runs (one
 * process per test) and stale files from an aborted run can never collide. */
std::string g_dir;

void remove_dir() {
    if (g_dir.empty()) {
        return;
    }
    /* Servers killed with SIGKILL leave their socket files behind. */
    if (DIR *d = opendir(g_dir.c_str())) {
        while (const dirent *e = readdir(d)) {
            if (e->d_name[0] != '.') {
                (void)unlink((g_dir + "/" + e->d_name).c_str());
            }
        }
        (void)closedir(d);
    }
    (void)rmdir(g_dir.c_str());
}

void use_private_socket_dir() {
    if (!g_dir.empty()) {
        return;
    }
    char tmpl[] = "/tmp/qmp-uds-test-XXXXXX";
    ASSERT_NE(mkdtemp(tmpl), nullptr);
    g_dir = tmpl;
    ASSERT_EQ(setenv("QMP_UDS_DIR", g_dir.c_str(), 1), 0);
    (void)atexit(remove_dir);
}

const qmp_transport_t *uds() {
    return qmp_transport_uds();
}

template <typename Pred> bool wait_for(Pred p, std::chrono::milliseconds limit = 5000ms) {
    auto end = std::chrono::steady_clock::now() + limit;
    while (!p()) {
        if (std::chrono::steady_clock::now() > end) {
            return false;
        }
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

// ---- transport contract -------------------------------------------------------------

/* A scripted raw server: the first payload byte of a message picks the behaviour. */
struct RawServer {
    qmp_endpoint_t ep{};
    std::thread thr;
    std::mutex mu;
    std::vector<qmp_rcv_info_t> messages;
    std::vector<qmp_rcv_info_t> unblocks;
    std::vector<qmp_rcv_info_t> disconnects;
    std::atomic<int> nmsg{0};
    std::atomic<int> nunblock{0};
    std::atomic<int> ndisc{0};
    std::atomic<int> unblock_req_seen{-1};
    size_t cap = 256;

    int listen(const char *path) { return uds()->listen(&ep, path, 0); }

    void start() {
        thr = std::thread([this] {
            std::vector<uint8_t> buf(cap);
            for (;;) {
                qmp_rcv_info_t info{};
                int rc = uds()->receive(&ep, buf.data(), buf.size(), &info);
                if (rc == ECANCELED) {
                    return;
                }
                ASSERT_EQ(rc, EOK);
                std::lock_guard<std::mutex> lk(mu);
                if (info.kind == QMP_RCV_UNBLOCK) {
                    unblocks.push_back(info);
                    unblock_req_seen = uds()->unblock_req(&ep, info.rcvid);
                    nunblock++;
                    continue;
                }
                if (info.kind == QMP_RCV_DISCONNECT) {
                    disconnects.push_back(info);
                    ndisc++;
                    continue;
                }
                messages.push_back(info);
                nmsg++;
                if (info.srcmsglen > info.msglen) {
                    EXPECT_EQ(uds()->error(&ep, info.rcvid, EMSGSIZE), EOK);
                    continue;
                }
                switch (info.msglen > 0 ? buf[0] : 'r') {
                case 'e':
                    EXPECT_EQ(uds()->error(&ep, info.rcvid, EXDEV), EOK);
                    break;
                case 'n': /* never reply: the test replies late through last_rcvid() */
                    break;
                default: { /* echo in two parts */
                    size_t half = info.msglen / 2;
                    qmp_iov_t iov[2] = {{buf.data(), half},
                                        {buf.data() + half, info.msglen - half}};
                    EXPECT_EQ(uds()->reply(&ep, info.rcvid, iov, 2), EOK);
                }
                }
            }
        });
    }

    qmp_rcv_info_t last_message() {
        std::lock_guard<std::mutex> lk(mu);
        return messages.back();
    }

    void stop() {
        if (thr.joinable()) {
            EXPECT_EQ(uds()->unblock(&ep), EOK);
            thr.join();
        }
        EXPECT_EQ(uds()->close(&ep), EOK);
    }
};

int raw_call(qmp_conn_t *c, const std::string &msg, std::string *reply, uint64_t timeout_ns,
             size_t reply_cap = 256, size_t *reply_len = nullptr) {
    std::string out(reply_cap, '\0');
    std::string m = msg;
    size_t half = m.size() / 2;
    qmp_iov_t siov[2] = {{&m[0], half}, {&m[half], m.size() - half}};
    qmp_iov_t riov[1] = {{&out[0], out.size()}};
    size_t len = 0;
    int rc = uds()->call(c, siov, 2, riov, 1, timeout_ns, &len);
    if (reply_len != nullptr) {
        *reply_len = len;
    }
    if (rc == EOK && reply != nullptr) {
        *reply = out.substr(0, len < out.size() ? len : out.size());
    }
    return rc;
}

class UdsRaw : public ::testing::Test {
  protected:
    void SetUp() override { use_private_socket_dir(); }
};

TEST_F(UdsRaw, NameAndLookup) {
    EXPECT_STREQ(uds()->name, "uds");
    EXPECT_EQ(qmp_transport_by_name("uds"), uds());
    EXPECT_EQ(qmp_transport_by_name("fake"), qmp_transport_fake());
    EXPECT_EQ(qmp_transport_by_name("nope"), nullptr);
    EXPECT_EQ(qmp_transport_by_name(nullptr), nullptr);
#if !defined(__QNX__)
    EXPECT_EQ(qmp_transport_default(), uds()); /* host default since W3 */
    EXPECT_EQ(qmp_transport_by_name("qnx"), nullptr);
#else
    EXPECT_STREQ(qmp_transport_default()->name, "qnx"); /* native stays the default */
#endif
}

TEST_F(UdsRaw, ConnectWithoutListenerIsENOENT) {
    qmp_conn_t c{};
    EXPECT_EQ(uds()->connect(&c, "qmp/raw0-0000/v1"), ENOENT);
}

TEST_F(UdsRaw, SecondListenIsEADDRINUSEAndCloseFreesTheName) {
    RawServer a;
    ASSERT_EQ(a.listen("qmp/raw1-0000/v1"), EOK);
    qmp_endpoint_t b{};
    EXPECT_EQ(uds()->listen(&b, "qmp/raw1-0000/v1", 0), EADDRINUSE);
    a.stop();
    ASSERT_EQ(uds()->listen(&b, "qmp/raw1-0000/v1", 0), EOK);
    EXPECT_EQ(uds()->close(&b), EOK);
    qmp_conn_t c{};
    EXPECT_EQ(uds()->connect(&c, "qmp/raw1-0000/v1"), ENOENT); /* file is unlinked */
}

TEST_F(UdsRaw, StaleSocketFileOfACrashedServerIsReclaimed) {
    pid_t pid = fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        qmp_endpoint_t ep{};
        _exit(uds()->listen(&ep, "qmp/raw2-0000/v1", 0) == EOK ? 0 : 1); /* no close */
    }
    int st = 0;
    ASSERT_EQ(waitpid(pid, &st, 0), pid);
    ASSERT_TRUE(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    qmp_conn_t c{};
    EXPECT_EQ(uds()->connect(&c, "qmp/raw2-0000/v1"), ENOENT); /* file exists, nobody home */
    RawServer s;
    ASSERT_EQ(s.listen("qmp/raw2-0000/v1"), EOK);
    s.stop();
}

TEST_F(UdsRaw, CallReplyErrorAndRcvInfo) {
    RawServer s;
    ASSERT_EQ(s.listen("qmp/raw3-0000/v1"), EOK);
    s.start();
    qmp_conn_t c{};
    ASSERT_EQ(uds()->connect(&c, "qmp/raw3-0000/v1"), EOK);

    std::string rep;
    ASSERT_EQ(raw_call(&c, "request-bytes", &rep, 0), EOK);
    EXPECT_EQ(rep, "request-bytes");
    qmp_rcv_info_t info = s.last_message();
    EXPECT_EQ(info.kind, QMP_RCV_MESSAGE);
    EXPECT_EQ(info.msglen, 13u);
    EXPECT_EQ(info.srcmsglen, 13u);
    EXPECT_EQ(info.dstmsglen, 256u);
    EXPECT_EQ(info.pid, static_cast<int32_t>(getpid()));
    EXPECT_EQ(info.cred_valid, 1);
    EXPECT_EQ(info.uid, static_cast<int32_t>(geteuid()));
    EXPECT_EQ(info.gid, static_cast<int32_t>(getegid()));
    EXPECT_GT(info.scoid, 0);
    EXPECT_EQ(info.priority, 0); /* sockets carry no priority: nothing to inherit */

    EXPECT_EQ(raw_call(&c, "error please", &rep, 0), EXDEV);
    ASSERT_EQ(raw_call(&c, "again", &rep, 500 * kMs), EOK); /* the timed path too */
    EXPECT_EQ(rep, "again");
    EXPECT_EQ(s.last_message().scoid, info.scoid); /* stable per connection */
    EXPECT_NE(s.last_message().rcvid, info.rcvid);

    EXPECT_EQ(uds()->disconnect(&c), EOK);
    ASSERT_TRUE(wait_for([&] { return s.ndisc.load() == 1; }));
    EXPECT_EQ(s.disconnects[0].scoid, info.scoid);
    EXPECT_EQ(s.disconnects[0].pid, static_cast<int32_t>(getpid()));
    s.stop();
}

TEST_F(UdsRaw, OversizedRequestAndReplyAreReportedNotHidden) {
    RawServer s;
    s.cap = 16;
    ASSERT_EQ(s.listen("qmp/raw4-0000/v1"), EOK);
    s.start();
    qmp_conn_t c{};
    ASSERT_EQ(uds()->connect(&c, "qmp/raw4-0000/v1"), EOK);
    std::string rep;
    /* 40 bytes into a 16-byte server buffer: srcmsglen says so, the server refuses. */
    EXPECT_EQ(raw_call(&c, std::string(40, 'x'), &rep, 0), EMSGSIZE);
    qmp_rcv_info_t info = s.last_message();
    EXPECT_EQ(info.msglen, 16u);
    EXPECT_EQ(info.srcmsglen, 40u);
    /* 12-byte reply into a 4-byte client buffer: reply_len reports the real size. */
    size_t len = 0;
    ASSERT_EQ(raw_call(&c, "twelve bytes", &rep, 0, 4, &len), EOK);
    EXPECT_EQ(len, 12u);
    EXPECT_EQ(rep, "twel");
    EXPECT_EQ(s.last_message().dstmsglen, 4u);
    EXPECT_EQ(uds()->disconnect(&c), EOK);
    s.stop();
}

TEST_F(UdsRaw, TimeoutIsLocalCancelReachesTheServerAndLateReplyIsSkipped) {
    RawServer s;
    ASSERT_EQ(s.listen("qmp/raw5-0000/v1"), EOK);
    s.start();
    qmp_conn_t c{};
    ASSERT_EQ(uds()->connect(&c, "qmp/raw5-0000/v1"), EOK);
    auto t0 = std::chrono::steady_clock::now();
    EXPECT_EQ(raw_call(&c, "never", nullptr, 30 * kMs), ETIMEDOUT);
    auto dt = std::chrono::steady_clock::now() - t0;
    EXPECT_GE(dt, 25ms);
    EXPECT_LT(dt, 2000ms);
    /* The CANCEL frame surfaces as an unblock event for exactly that request. */
    ASSERT_TRUE(wait_for([&] { return s.nunblock.load() == 1; }));
    int64_t stale = s.last_message().rcvid;
    EXPECT_EQ(s.unblocks[0].rcvid, stale);
    EXPECT_EQ(s.unblock_req_seen.load(), 1);
    /* The server answers late; the client must not mistake it for the next reply. */
    qmp_iov_t late = {const_cast<char *>("LATE"), 4};
    EXPECT_EQ(uds()->reply(&s.ep, stale, &late, 1), EOK);
    std::string rep;
    ASSERT_EQ(raw_call(&c, "fresh", &rep, 0), EOK);
    EXPECT_EQ(rep, "fresh");
    EXPECT_EQ(uds()->unblock_req(&s.ep, s.last_message().rcvid), 0);
    EXPECT_EQ(uds()->disconnect(&c), EOK);
    /* A reply to a connection that is gone is ESRCH, never a crash or a block. */
    ASSERT_TRUE(wait_for([&] { return s.ndisc.load() == 1; }));
    EXPECT_EQ(uds()->reply(&s.ep, stale, &late, 1), ESRCH);
    EXPECT_EQ(uds()->error(&s.ep, stale, EIO), ESRCH);
    s.stop();
}

TEST_F(UdsRaw, TimeoutWhenNobodyReceives) {
    RawServer s; /* listening, but no thread in receive() */
    ASSERT_EQ(s.listen("qmp/raw6-0000/v1"), EOK);
    qmp_conn_t c{};
    ASSERT_EQ(uds()->connect(&c, "qmp/raw6-0000/v1"), EOK);
    EXPECT_EQ(raw_call(&c, "hello?", nullptr, 20 * kMs), ETIMEDOUT);
    /* The call and its CANCEL are both queued: a worker that picks the call up now
     * learns from unblock_req() that the client already gave up. */
    uint8_t buf[64];
    qmp_rcv_info_t info{};
    ASSERT_EQ(uds()->receive(&s.ep, buf, sizeof(buf), &info), EOK);
    ASSERT_EQ(info.kind, QMP_RCV_MESSAGE);
    EXPECT_EQ(uds()->unblock_req(&s.ep, info.rcvid), 1);
    EXPECT_EQ(uds()->disconnect(&c), EOK);
    s.stop();
}

TEST_F(UdsRaw, ServerCloseFailsABlockedCallerWithESRCH) {
    RawServer s;
    ASSERT_EQ(s.listen("qmp/raw7-0000/v1"), EOK);
    s.start();
    qmp_conn_t c{};
    ASSERT_EQ(uds()->connect(&c, "qmp/raw7-0000/v1"), EOK);
    std::atomic<int> rc{-1};
    std::thread caller([&] { rc = raw_call(&c, "never", nullptr, 0); });
    ASSERT_TRUE(wait_for([&] { return s.nmsg.load() == 1; }));
    s.stop(); /* unblock + close: the connection is closed under the blocked client */
    caller.join();
    EXPECT_EQ(rc.load(), ESRCH);
    EXPECT_EQ(raw_call(&c, "after", nullptr, 0), ESRCH);
    EXPECT_EQ(uds()->disconnect(&c), EOK);
}

TEST_F(UdsRaw, UnblockWakesEveryReceiver) {
    qmp_endpoint_t ep{};
    ASSERT_EQ(uds()->listen(&ep, "qmp/raw8-0000/v1", 0), EOK);
    std::atomic<int> cancelled{0};
    std::vector<std::thread> ts;
    for (int i = 0; i < 4; i++) {
        ts.emplace_back([&] {
            uint8_t buf[64];
            qmp_rcv_info_t info{};
            if (uds()->receive(&ep, buf, sizeof(buf), &info) == ECANCELED) {
                cancelled++;
            }
        });
    }
    std::this_thread::sleep_for(20ms);
    EXPECT_EQ(uds()->unblock(&ep), EOK);
    for (auto &t : ts) {
        t.join();
    }
    EXPECT_EQ(cancelled.load(), 4);
    EXPECT_EQ(uds()->close(&ep), EOK);
}

TEST_F(UdsRaw, NotificationsRegisterDeliverUnregister) {
    RawServer s;
    ASSERT_EQ(s.listen("qmp/raw9-0000/v1"), EOK);
    s.start();
    qmp_conn_t c{};
    qmp_conn_t other{};
    ASSERT_EQ(uds()->connect(&c, "qmp/raw9-0000/v1"), EOK);
    ASSERT_EQ(uds()->connect(&other, "qmp/raw9-0000/v1"), EOK);
    std::string rep;
    ASSERT_EQ(raw_call(&c, "sub", &rep, 0), EOK);
    int64_t rcvid = s.last_message().rcvid; /* stays usable for deliver() after the reply */
    ASSERT_EQ(raw_call(&other, "other", &rep, 0), EOK);
    int64_t other_rcvid = s.last_message().rcvid;

    uint8_t blob[QMP_EVENT_BLOB_MAX] = {0};
    size_t blen = 0;
    EXPECT_EQ(uds()->event_register(&c, blob, sizeof(blob), &blen), EINVAL); /* no channel */
    ASSERT_EQ(uds()->notify_open(&c), EOK);
    ASSERT_EQ(uds()->notify_open(&c), EOK); /* idempotent */
    ASSERT_EQ(uds()->event_register(&c, blob, sizeof(blob), &blen), EOK);
    ASSERT_GT(blen, 0u);
    ASSERT_LE(blen, static_cast<size_t>(QMP_EVENT_BLOB_MAX));

    int64_t value = 0;
    EXPECT_EQ(uds()->notify_wait(&c, 20 * kMs, &value), ETIMEDOUT);
    ASSERT_EQ(uds()->deliver(&s.ep, rcvid, blob, blen, 41), EOK);
    ASSERT_EQ(uds()->deliver(&s.ep, rcvid, blob, blen, 42), EOK);
    ASSERT_EQ(uds()->notify_wait(&c, 2000 * kMs, &value), EOK);
    EXPECT_EQ(value, 41);
    ASSERT_EQ(uds()->notify_wait(&c, 0, &value), EOK);
    EXPECT_EQ(value, 42);

    /* Only the connection that registered an event may be notified through it. */
    EXPECT_EQ(uds()->deliver(&s.ep, other_rcvid, blob, blen, 1), EACCES);
    uint8_t junk[QMP_EVENT_BLOB_MAX] = {0};
    EXPECT_EQ(uds()->deliver(&s.ep, rcvid, junk, blen, 1), EACCES);
    EXPECT_EQ(uds()->deliver(&s.ep, rcvid, blob, blen - 1, 1), EINVAL);

    /* A subscriber that never reads must not block or fail the publisher: the socket
     * buffer fills, further notifications are dropped like a pulse that cannot queue. */
    for (int i = 0; i < 20000; i++) {
        ASSERT_EQ(uds()->deliver(&s.ep, rcvid, blob, blen, i), EOK);
    }
    ASSERT_EQ(uds()->notify_wait(&c, 2000 * kMs, &value), EOK);

    ASSERT_EQ(uds()->event_unregister(&c, blob, blen), EOK);
    EXPECT_EQ(uds()->deliver(&s.ep, rcvid, blob, blen, 1), ESRCH);
    EXPECT_EQ(uds()->event_unregister(&c, blob, blen), ESRCH);

    /* Registrations die with the registering connection. */
    ASSERT_EQ(uds()->event_register(&c, blob, sizeof(blob), &blen), EOK);
    ASSERT_EQ(uds()->deliver(&s.ep, rcvid, blob, blen, 1), EOK);
    EXPECT_EQ(uds()->notify_close(&c), EOK);
    EXPECT_EQ(uds()->disconnect(&c), EOK);
    ASSERT_TRUE(wait_for([&] { return s.ndisc.load() == 1; }));
    EXPECT_EQ(uds()->deliver(&s.ep, rcvid, blob, blen, 1), ESRCH);
    EXPECT_EQ(uds()->disconnect(&other), EOK);
    s.stop();
}

TEST_F(UdsRaw, ServerDeathWakesNotifyWaitWithESRCH) {
    RawServer s;
    ASSERT_EQ(s.listen("qmp/rawa-0000/v1"), EOK);
    s.start();
    qmp_conn_t c{};
    ASSERT_EQ(uds()->connect(&c, "qmp/rawa-0000/v1"), EOK);
    ASSERT_EQ(uds()->notify_open(&c), EOK);
    std::atomic<int> rc{-1};
    std::thread waiter([&] {
        int64_t value = 0;
        rc = uds()->notify_wait(&c, 0, &value);
    });
    std::this_thread::sleep_for(20ms);
    s.stop();
    waiter.join();
    EXPECT_EQ(rc.load(), ESRCH);
    EXPECT_EQ(uds()->notify_close(&c), EOK);
    EXPECT_EQ(uds()->disconnect(&c), EOK);
}

// ---- the core end to end over UDS -----------------------------------------------------

struct JointModel {
    std::atomic<int> calls{0};
    std::atomic<int> last_priority{-1};
    std::atomic<int32_t> last_pid{0};
    std::atomic<int> burn_ms{0};
    std::atomic<int32_t> gone_pid{0};
    std::atomic<int> gone{0};
};

int on_set_target(void *ctx, qmp_req_t *req, const demo_joint_v1_SetTargetRequest *in,
                  demo_joint_v1_SetTargetReply *out, int32_t *) {
    auto *m = static_cast<JointModel *>(ctx);
    qmp_req_info_t info{};
    if (qmp_req_get_info(req, &info) != EOK) {
        return EIO;
    }
    m->last_priority = info.priority;
    m->last_pid = info.pid;
    m->calls++;
    if (m->burn_ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(m->burn_ms.load()));
    }
    out->result = static_cast<int32_t>(in->position_rad);
    return QMP_HANDLED;
}

int on_get_state(void *ctx, qmp_req_t *, demo_joint_v1_JointState *out, int32_t *) {
    auto *m = static_cast<JointModel *>(ctx);
    out->seq = static_cast<uint64_t>(m->calls.load());
    out->mode.len = 4;
    memcpy(out->mode.data, "hold", 4);
    return QMP_HANDLED;
}

void on_gone(void *ctx, int32_t pid) {
    auto *m = static_cast<JointModel *>(ctx);
    m->gone_pid = pid;
    m->gone++;
}

/* One Joint server in this process, on worker threads. */
struct JointServer {
    static constexpr int kThreads = 2;
    alignas(8) unsigned char workbuf[QMP_SERVER_WORKBUF_SIZE(DEMO_JOINT_V1_JOINT_MAX_MSG_SIZE,
                                                             kThreads)]{};
    alignas(8) unsigned char topicbuf[DEMO_JOINT_V1_JOINT_TOPIC_STORAGE_SIZE]{};
    qmp_server_t server{};
    demo_joint_v1_Joint_ops ops{};
    JointModel model;
    std::thread runner;

    int start(int nthreads = kThreads) {
        ops.SetTarget = on_set_target;
        ops.GetState = on_get_state;
        qmp_server_cfg_t cfg{};
        cfg.transport = uds();
        cfg.nthreads = nthreads;
        cfg.workbuf = workbuf;
        cfg.workbuf_size = sizeof(workbuf);
        cfg.topicbuf = topicbuf;
        cfg.topicbuf_size = sizeof(topicbuf);
        cfg.on_client_gone = on_gone;
        int rc = qmp_server_init(&server, &cfg);
        if (rc == EOK) {
            rc = demo_joint_v1_Joint_register(&server, 0, &ops, &model);
        }
        if (rc == EOK) {
            runner = std::thread([this] { (void)qmp_server_run(&server); });
        }
        return rc;
    }

    void stop() {
        if (runner.joinable()) {
            EXPECT_EQ(qmp_server_stop(&server), EOK);
            runner.join();
            EXPECT_EQ(qmp_server_close(&server), EOK);
        }
    }

    int publish(uint64_t n) {
        demo_joint_v1_JointState st{};
        st.seq = n;
        st.position_rad = static_cast<float>(n);
        return demo_joint_v1_Joint_publish_StateChanged(&server, &st);
    }
};

qmp_client_cfg_t uds_client_cfg(uint64_t timeout_ns) {
    qmp_client_cfg_t cfg{};
    cfg.transport = uds();
    cfg.default_timeout_ns = timeout_ns;
    cfg.reconnect_initial_ms = 5;
    cfg.reconnect_max_ms = 50;
    cfg.reconnect_attempts = 100;
    return cfg;
}

int connect_joint(qmp_client_t &c, uint64_t timeout_ns = 0) {
    qmp_client_cfg_t cfg = uds_client_cfg(timeout_ns);
    int rc = qmp_client_init(&c, &cfg);
    return rc != EOK ? rc : demo_joint_v1_Joint_connect(&c, 0, &cfg);
}

struct Seen {
    std::vector<uint64_t> seqs;      /* topic sequence numbers */
    std::vector<uint64_t> state_seq; /* JointState.seq payloads */
    uint64_t dropped = 0;
};

void record_state(void *ctx, const demo_joint_v1_JointState *sample, uint64_t seq,
                  uint64_t dropped) {
    auto *s = static_cast<Seen *>(ctx);
    s->seqs.push_back(seq);
    s->state_seq.push_back(sample->seq);
    s->dropped += dropped;
}

class UdsE2E : public ::testing::Test {
  protected:
    JointServer srv_;
    void SetUp() override {
        use_private_socket_dir();
        ASSERT_EQ(srv_.start(), EOK);
    }
    void TearDown() override { srv_.stop(); }
};

TEST_F(UdsE2E, HandshakeAndRoundTripWithoutInheritedPriority) {
    qmp_client_t c;
    ASSERT_EQ(connect_joint(c), EOK);
    EXPECT_EQ(qmp_client_server_generation(&c), qmp_server_generation(&srv_.server));
    demo_joint_v1_SetTargetRequest req{3.0F, 1.0F};
    demo_joint_v1_SetTargetReply rep{};
    int32_t status = -1;
    ASSERT_EQ(demo_joint_v1_Joint_SetTarget(&c, &req, &rep, &status), EOK);
    EXPECT_EQ(status, 0);
    EXPECT_EQ(rep.result, 3);
    EXPECT_EQ(srv_.model.last_pid.load(), static_cast<int32_t>(getpid()));
    /* The documented limitation, pinned by a test: the handler is told nothing about
     * the caller's priority, because the socket did not carry it. */
    EXPECT_EQ(srv_.model.last_priority.load(), 0);
    demo_joint_v1_JointState st{};
    ASSERT_EQ(demo_joint_v1_Joint_GetState(&c, &st, &status), EOK);
    EXPECT_EQ(st.seq, 1u);
    EXPECT_EQ(std::string(st.mode.data, st.mode.len), "hold");
    /* Transport errors still come back as errnos. */
    EXPECT_EQ(qmp_client_call(&c, 0x0042, nullptr, 0, nullptr, 0, &status, nullptr), ENOENT);
    EXPECT_EQ(qmp_client_close(&c), EOK);
}

TEST_F(UdsE2E, ConcurrentClientsAcrossWorkers) {
    constexpr int kClients = 4;
    constexpr int kIter = 200;
    std::vector<std::thread> ts;
    std::atomic<int> failures{0};
    for (int i = 0; i < kClients; i++) {
        ts.emplace_back([&, i] {
            qmp_client_t c;
            if (connect_joint(c) != EOK) {
                failures++;
                return;
            }
            for (int k = 0; k < kIter; k++) {
                demo_joint_v1_SetTargetRequest req{static_cast<float>(i * 1000 + k), 1.0F};
                demo_joint_v1_SetTargetReply rep{};
                int32_t status = -1;
                if (demo_joint_v1_Joint_SetTarget(&c, &req, &rep, &status) != EOK || status != 0 ||
                    rep.result != i * 1000 + k) {
                    failures++;
                }
            }
            qmp_client_close(&c);
        });
    }
    for (auto &t : ts) {
        t.join();
    }
    EXPECT_EQ(failures.load(), 0);
    EXPECT_EQ(srv_.model.calls.load(), kClients * kIter);
}

TEST_F(UdsE2E, ClientTimeoutDoesNotWaitForABusyServer) {
    qmp_client_t c;
    ASSERT_EQ(connect_joint(c, 30 * kMs), EOK);
    srv_.model.burn_ms = 300;
    demo_joint_v1_SetTargetRequest req{1.0F, 1.0F};
    demo_joint_v1_SetTargetReply rep{};
    int32_t status = -1;
    auto t0 = std::chrono::steady_clock::now();
    EXPECT_EQ(demo_joint_v1_Joint_SetTarget(&c, &req, &rep, &status), ETIMEDOUT);
    EXPECT_LT(std::chrono::steady_clock::now() - t0, 250ms);
    /* The connection stays usable; the abandoned call's late reply is skipped. */
    srv_.model.burn_ms = 0;
    qmp_call_opts_t opts{};
    opts.timeout_ns = 5000 * kMs;
    req.position_rad = 9.0F;
    ASSERT_EQ(demo_joint_v1_Joint_SetTarget_ex(&c, &req, &rep, &status, &opts), EOK);
    EXPECT_EQ(rep.result, 9);
    EXPECT_EQ(qmp_client_close(&c), EOK);
}

TEST_F(UdsE2E, DisconnectReachesOnClientGone) {
    qmp_client_t c;
    ASSERT_EQ(connect_joint(c), EOK);
    EXPECT_EQ(qmp_client_close(&c), EOK);
    ASSERT_TRUE(wait_for([&] { return srv_.model.gone.load() == 1; }));
    EXPECT_EQ(srv_.model.gone_pid.load(), static_cast<int32_t>(getpid()));
}

TEST_F(UdsE2E, TopicsSubscribePublishPollAndUnsubscribe) {
    qmp_client_t c;
    ASSERT_EQ(connect_joint(c), EOK);
    Seen seen;
    demo_joint_v1_Joint_StateChanged_sub_t sub{};
    ASSERT_EQ(demo_joint_v1_Joint_subscribe_StateChanged(&c, &sub, record_state, &seen), EOK);
    EXPECT_EQ(qmp_client_poll(&c, 20 * kMs), ETIMEDOUT); /* nothing published yet */
    for (uint64_t n = 1; n <= 3; n++) {
        ASSERT_EQ(srv_.publish(n), EOK);
    }
    ASSERT_EQ(qmp_client_poll(&c, 2000 * kMs), EOK);
    ASSERT_EQ(seen.seqs.size(), 1u); /* LATEST: newest wins, the loss is reported */
    EXPECT_EQ(seen.seqs[0], 3u);
    EXPECT_EQ(seen.state_seq[0], 3u);
    EXPECT_EQ(seen.dropped, 2u);
    while (qmp_client_poll(&c, 20 * kMs) == EOK) { /* drain leftover notifications */
    }
    ASSERT_EQ(demo_joint_v1_Joint_unsubscribe_StateChanged(&c, &sub), EOK);
    ASSERT_EQ(srv_.publish(4), EOK); /* nobody to notify: still fine */
    EXPECT_EQ(seen.seqs.size(), 1u);
    EXPECT_EQ(qmp_client_close(&c), EOK);
}

TEST_F(UdsE2E, ServerRestartReconnectAndResubscribe) {
    qmp_client_t c;
    ASSERT_EQ(connect_joint(c), EOK);
    Seen seen;
    demo_joint_v1_Joint_StateChanged_sub_t sub{};
    ASSERT_EQ(demo_joint_v1_Joint_subscribe_StateChanged(&c, &sub, record_state, &seen), EOK);
    uint32_t gen = qmp_client_server_generation(&c);
    srv_.stop();
    /* A blocked subscriber learns about the dead server from its notification socket. */
    EXPECT_EQ(qmp_client_poll(&c, 2000 * kMs), ESRCH);
    EXPECT_FALSE(qmp_client_is_connected(&c));
    ASSERT_EQ(srv_.start(), EOK);
    ASSERT_EQ(qmp_client_reconnect(&c), EOK);
    EXPECT_NE(qmp_client_server_generation(&c), gen);
    ASSERT_EQ(srv_.publish(7), EOK);
    ASSERT_EQ(qmp_client_poll(&c, 2000 * kMs), EOK); /* resubscribed on the new session */
    ASSERT_EQ(seen.seqs.size(), 1u);
    EXPECT_EQ(seen.state_seq[0], 7u);
    EXPECT_EQ(qmp_client_close(&c), EOK);
}

// ---- deferred replies and cancellation over UDS -----------------------------------------

constexpr uint16_t kDSvc = 0x0505;
struct DIn {
    int32_t value;
    uint8_t _pad[4];
};
struct DModel {
    std::mutex mu;
    std::condition_variable cv;
    qmp_req_t *parked = nullptr;
    std::atomic<int> cancels{0};
};

int defer_handler(const void *, void *ctx, qmp_req_t *req, const void *, void *, int32_t *) {
    auto *m = static_cast<DModel *>(ctx);
    std::lock_guard<std::mutex> lk(m->mu);
    m->parked = req;
    m->cv.notify_all();
    return QMP_DEFERRED;
}

void defer_cancel(void *ctx, qmp_req_t *req) {
    auto *m = static_cast<DModel *>(ctx);
    std::lock_guard<std::mutex> lk(m->mu);
    if (m->parked == req) {
        m->parked = nullptr;
    }
    m->cancels++;
}

const qmp_method_desc_t kDMethods[] = {
    {1, 0, sizeof(DIn), sizeof(DIn), 0, nullptr, defer_handler, "Defer"}};
const qmp_service_desc_t kDDesc = {kDSvc, 1,    0, QMP_HDR_SIZE + 64, "test.UdsDefer", kDMethods,
                                   1,     NULL, 0};

TEST(UdsDeferred, ReplyLaterAndCancelOnClientTimeout) {
    use_private_socket_dir();
    DModel model;
    int dummy_ops = 0;
    qmp_server_t server;
    alignas(8) qmp_req_t pool[2];
    alignas(8) unsigned char workbuf[QMP_SERVER_WORKBUF_SIZE(QMP_HDR_SIZE + 64, 2)];
    qmp_server_cfg_t cfg{};
    cfg.transport = uds();
    cfg.nthreads = 2;
    cfg.workbuf = workbuf;
    cfg.workbuf_size = sizeof(workbuf);
    cfg.deferred_pool = pool;
    cfg.deferred_count = 2;
    cfg.on_cancel = defer_cancel;
    ASSERT_EQ(qmp_server_init(&server, &cfg), EOK);
    ASSERT_EQ(qmp_server_register(&server, &kDDesc, 0, &dummy_ops, &model), EOK);
    std::thread runner([&] { EXPECT_EQ(qmp_server_run(&server), EOK); });

    qmp_client_t c;
    qmp_client_cfg_t ccfg = uds_client_cfg(0);
    ASSERT_EQ(qmp_client_init(&c, &ccfg), EOK);
    ASSERT_EQ(qmp_client_connect(&c, kDSvc, 0, 1, 0), EOK);

    /* 1. Deferred, answered later from another thread. */
    std::thread replier([&] {
        std::unique_lock<std::mutex> lk(model.mu);
        ASSERT_TRUE(model.cv.wait_for(lk, 5s, [&] { return model.parked != nullptr; }));
        qmp_req_t *req = model.parked;
        model.parked = nullptr;
        lk.unlock();
        DIn out{99, {}};
        EXPECT_EQ(qmp_req_reply(req, &out, sizeof(out), 0), EOK);
    });
    DIn in{1, {}};
    DIn out{};
    int32_t status = -1;
    ASSERT_EQ(qmp_client_call(&c, 1, &in, sizeof(in), &out, sizeof(out), &status, nullptr), EOK);
    EXPECT_EQ(out.value, 99);
    replier.join();

    /* 2. Deferred, never answered: the client times out locally and the CANCEL frame
     * makes the server drop the parked request through on_cancel. */
    qmp_call_opts_t opts{};
    opts.timeout_ns = 30 * kMs;
    EXPECT_EQ(qmp_client_call(&c, 1, &in, sizeof(in), &out, sizeof(out), &status, &opts),
              ETIMEDOUT);
    ASSERT_TRUE(wait_for([&] { return model.cancels.load() == 1; }));
    {
        std::lock_guard<std::mutex> lk(model.mu);
        EXPECT_EQ(model.parked, nullptr);
    }

    EXPECT_EQ(qmp_client_close(&c), EOK);

    EXPECT_EQ(qmp_server_stop(&server), EOK);
    runner.join();
    EXPECT_EQ(qmp_server_close(&server), EOK);
}

// ---- two processes ------------------------------------------------------------------------
// fork() happens while this process is single-threaded (every earlier test joined its
// threads), which is what QNX requires and what keeps the sanitizers happy. Children
// leave with _exit() or SIGKILL and never return into gtest.

/* Child: a Joint server that publishes StateChanged every 2 ms until it is killed. */
[[noreturn]] void run_server_child() {
    static JointServer srv; /* static: too large for comfort on a small target stack */
    if (srv.start() != EOK) {
        _exit(3);
    }
    for (uint64_t n = 1;; n++) {
        (void)srv.publish(n);
        std::this_thread::sleep_for(2ms);
    }
}

pid_t spawn_server() {
    pid_t pid = fork();
    if (pid == 0) {
        run_server_child();
    }
    return pid;
}

void kill9(pid_t pid) {
    ASSERT_EQ(kill(pid, SIGKILL), 0);
    int st = 0;
    ASSERT_EQ(waitpid(pid, &st, 0), pid);
    ASSERT_TRUE(WIFSIGNALED(st));
}

int connect_with_retry(qmp_client_t &c, uint64_t timeout_ns) {
    int rc = ENOENT;
    for (int i = 0; i < 500 && rc != EOK; i++) {
        rc = connect_joint(c, timeout_ns);
        if (rc != EOK) {
            std::this_thread::sleep_for(10ms);
        }
    }
    return rc;
}

TEST(UdsTwoProcess, ServerKilledMidStreamThenRestartAndResubscribe) {
    use_private_socket_dir();
    pid_t server_pid = spawn_server();
    ASSERT_GT(server_pid, 0);
    qmp_client_t c;
    ASSERT_EQ(connect_with_retry(c, 2000 * kMs), EOK);
    uint32_t gen = qmp_client_server_generation(&c);

    demo_joint_v1_SetTargetRequest req{5.0F, 1.0F};
    demo_joint_v1_SetTargetReply rep{};
    int32_t status = -1;
    ASSERT_EQ(demo_joint_v1_Joint_SetTarget(&c, &req, &rep, &status), EOK);
    EXPECT_EQ(rep.result, 5);

    Seen seen;
    demo_joint_v1_Joint_StateChanged_sub_t sub{};
    ASSERT_EQ(demo_joint_v1_Joint_subscribe_StateChanged(&c, &sub, record_state, &seen), EOK);
    ASSERT_TRUE(wait_for([&] {
        (void)qmp_client_poll(&c, 200 * kMs);
        return seen.seqs.size() >= 3;
    }));
    for (size_t i = 1; i < seen.seqs.size(); i++) {
        EXPECT_GT(seen.seqs[i], seen.seqs[i - 1]); /* never out of order or repeated */
    }

    kill9(server_pid);
    /* kill -9 leaves the socket file behind; the dead server is still detected at once. */
    int rc = EOK;
    for (int i = 0; i < 100 && rc == EOK; i++) {
        rc = qmp_client_poll(&c, 200 * kMs); /* drains what was already queued, then... */
    }
    EXPECT_EQ(rc, ESRCH);
    EXPECT_FALSE(qmp_client_is_connected(&c));
    EXPECT_EQ(demo_joint_v1_Joint_SetTarget(&c, &req, &rep, &status), ENOTCONN);

    server_pid = spawn_server(); /* reclaims the stale socket file */
    ASSERT_GT(server_pid, 0);
    ASSERT_EQ(qmp_client_reconnect(&c), EOK);
    EXPECT_NE(qmp_client_server_generation(&c), gen);
    size_t before = seen.seqs.size();
    ASSERT_TRUE(wait_for([&] {
        (void)qmp_client_poll(&c, 200 * kMs);
        return seen.seqs.size() >= before + 2;
    }));
    ASSERT_EQ(demo_joint_v1_Joint_SetTarget(&c, &req, &rep, &status), EOK);
    EXPECT_EQ(qmp_client_close(&c), EOK);
    kill9(server_pid);
}

TEST(UdsTwoProcess, SubscriberKilledMidStreamIsDroppedAndServerKeepsServing) {
    use_private_socket_dir();
    int ready[2];
    ASSERT_EQ(pipe(ready), 0);
    pid_t sub_pid = fork();
    ASSERT_GE(sub_pid, 0);
    if (sub_pid == 0) {
        /* Child: subscribe, report readiness, then consume updates until killed. */
        (void)close(ready[0]);
        static qmp_client_t c;
        static Seen seen;
        static demo_joint_v1_Joint_StateChanged_sub_t sub{};
        if (connect_with_retry(c, 2000 * kMs) != EOK ||
            demo_joint_v1_Joint_subscribe_StateChanged(&c, &sub, record_state, &seen) != EOK) {
            _exit(3);
        }
        char ok = 1;
        if (write(ready[1], &ok, 1) != 1) {
            _exit(4);
        }
        for (;;) {
            (void)qmp_client_poll(&c, 0);
            if (seen.seqs.size() == 3) {
                ok = 2;
                (void)!write(ready[1], &ok, 1);
            }
        }
    }
    (void)close(ready[1]);
    static JointServer srv;
    srv.model.gone = 0;
    ASSERT_EQ(srv.start(), EOK);
    char b = 0;
    ASSERT_EQ(read(ready[0], &b, 1), 1); /* subscribed */
    ASSERT_EQ(b, 1);
    for (uint64_t n = 1; n <= 3; n++) {
        ASSERT_EQ(srv.publish(n), EOK);
        std::this_thread::sleep_for(20ms); /* LATEST: give the child time to pull each */
    }
    ASSERT_EQ(read(ready[0], &b, 1), 1); /* the child really received them */
    ASSERT_EQ(b, 2);

    kill9(sub_pid);
    (void)close(ready[0]);
    /* The dead subscriber's connection closes; an idle worker reports the disconnect and
     * the core drops its subscription. Publishing never fails or blocks meanwhile. */
    ASSERT_TRUE(wait_for([&] {
        EXPECT_EQ(srv.publish(100), EOK);
        return srv.model.gone.load() >= 1;
    }));
    EXPECT_EQ(srv.model.gone_pid.load(), static_cast<int32_t>(sub_pid));
    /* The server keeps serving everyone else. */
    qmp_client_t c;
    ASSERT_EQ(connect_joint(c), EOK);
    Seen seen;
    demo_joint_v1_Joint_StateChanged_sub_t sub{};
    ASSERT_EQ(demo_joint_v1_Joint_subscribe_StateChanged(&c, &sub, record_state, &seen), EOK);
    ASSERT_EQ(srv.publish(101), EOK);
    ASSERT_EQ(qmp_client_poll(&c, 2000 * kMs), EOK);
    ASSERT_EQ(seen.state_seq.size(), 1u);
    EXPECT_EQ(seen.state_seq[0], 101u);
    EXPECT_EQ(qmp_client_close(&c), EOK);
    srv.stop();
}

} // namespace

#endif /* QMP_TRANSPORT_UDS_BENCH_ONLY */
