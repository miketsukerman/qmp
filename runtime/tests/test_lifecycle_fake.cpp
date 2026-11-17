// SPDX-License-Identifier: Apache-2.0
// Lifecycle semantics over the fake transport, which mirrors the kernel: one-way calls,
// deferred replies vs timeout/disconnect, the ACL hook, reconnect + server generation.
#include <gtest/gtest.h>

#include <qmp/qmp_client.h>
#include <qmp/qmp_server.h>
#include <qmp/qmp_transport.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include <unistd.h>

#include "core/internal.h"

namespace {

constexpr uint16_t kSvc = 0x0404;
constexpr uint16_t kMEcho = 0x0001; // two-way, deferrable
constexpr uint16_t kMFire = 0x0002; // one-way
constexpr uint16_t kMDeny = 0x0003; // two-way, denied by the ACL in some tests

struct EchoIn {
    int32_t value;
    uint8_t _pad[4];
};
struct EchoOut {
    int32_t value;
    uint8_t _pad[4];
};

struct Model {
    std::mutex mu;
    std::condition_variable cv;
    qmp_req_t *parked = nullptr; // set when Echo defers
    bool defer_next = false;     // Echo returns QMP_DEFERRED
    std::atomic<int> fire_calls{0};
    std::atomic<int> cancels{0};
    std::atomic<int> clients_gone{0};
    std::atomic<int32_t> gone_pid{-1};
    std::atomic<bool> fire_release{false}; // one-way handler spins until this
    std::atomic<bool> fire_running{false};
    uint16_t denied_resource = 0xFFFF;

    void park(qmp_req_t *req) {
        std::lock_guard<std::mutex> lk(mu);
        parked = req;
        cv.notify_all();
    }
    qmp_req_t *wait_parked() {
        std::unique_lock<std::mutex> lk(mu);
        cv.wait_for(lk, std::chrono::seconds(5), [&] { return parked != nullptr; });
        return parked;
    }
};

int echo_handler(const void *, void *ctx, qmp_req_t *req, const void *in, void *out, int32_t *st) {
    auto *m = static_cast<Model *>(ctx);
    if (m->defer_next) {
        m->park(req);
        return QMP_DEFERRED;
    }
    static_cast<EchoOut *>(out)->value = static_cast<const EchoIn *>(in)->value + 1;
    *st = 0;
    return QMP_HANDLED;
}

int fire_handler(const void *, void *ctx, qmp_req_t *, const void *, void *out, int32_t *) {
    auto *m = static_cast<Model *>(ctx);
    EXPECT_EQ(out, nullptr); // one-way handlers get no reply buffer
    m->fire_running = true;
    while (!m->fire_release) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    m->fire_calls++;
    return QMP_HANDLED;
}

int acl_hook(void *ctx, const qmp_cred_t *cred, uint16_t resource_id) {
    auto *m = static_cast<Model *>(ctx);
    EXPECT_NE(cred, nullptr);
    EXPECT_GT(cred->pid, 0); // the fake reports the test process
    EXPECT_GE(cred->uid, 0);
    return resource_id == m->denied_resource ? EACCES : EOK;
}

void on_cancel_cb(void *ctx, qmp_req_t *req) {
    auto *m = static_cast<Model *>(ctx);
    std::lock_guard<std::mutex> lk(m->mu);
    if (m->parked == req) {
        m->parked = nullptr;
    }
    m->cancels++;
}

void on_client_gone_cb(void *ctx, int32_t pid) {
    auto *m = static_cast<Model *>(ctx);
    m->clients_gone++;
    m->gone_pid = pid;
}

const qmp_method_desc_t kMethods[] = {
    {kMEcho, 0, sizeof(EchoIn), sizeof(EchoOut), 0, nullptr, echo_handler, "Echo"},
    {kMFire, QMP_MF_ONEWAY, sizeof(EchoIn), 0, 0, nullptr, fire_handler, "Fire"},
    {kMDeny, 0, sizeof(EchoIn), sizeof(EchoOut), 0, nullptr, echo_handler, "Deny"},
};

const qmp_service_desc_t kDesc = {kSvc, 1, 0, QMP_HDR_SIZE + 64, "test.Lifecycle", kMethods, 3};

class Lifecycle : public ::testing::Test {
  protected:
    static constexpr int kThreads = 2;
    static constexpr size_t kPool = 2;

    void start_server() {
        qmp_server_cfg_t cfg{};
        cfg.transport = qmp_transport_fake();
        cfg.nthreads = kThreads;
        cfg.workbuf = workbuf_;
        cfg.workbuf_size = sizeof(workbuf_);
        cfg.deferred_pool = pool_;
        cfg.deferred_count = kPool;
        cfg.acl_check = acl_hook;
        cfg.on_cancel = on_cancel_cb;
        cfg.on_client_gone = on_client_gone_cb;
        ASSERT_EQ(qmp_server_init(&server_, &cfg), EOK);
        ASSERT_EQ(qmp_server_register(&server_, &kDesc, 0, &dummy_ops_, &model_), EOK);
        runner_ = std::thread([this] { EXPECT_EQ(qmp_server_run(&server_), EOK); });
    }

    void SetUp() override { start_server(); }

    void TearDown() override {
        if (runner_.joinable()) {
            EXPECT_EQ(qmp_server_stop(&server_), EOK);
            runner_.join();
            EXPECT_EQ(qmp_server_close(&server_), EOK);
        }
    }

    void stop_server() {
        EXPECT_EQ(qmp_server_stop(&server_), EOK);
        runner_.join();
        EXPECT_EQ(qmp_server_close(&server_), EOK);
    }

    void connect(qmp_client_t &c, uint64_t timeout_ns = 0) {
        qmp_client_cfg_t ccfg{};
        ccfg.transport = qmp_transport_fake();
        ccfg.default_timeout_ns = timeout_ns;
        ccfg.reconnect_initial_ms = 1;
        ccfg.reconnect_max_ms = 8;
        ccfg.reconnect_attempts = 20;
        ASSERT_EQ(qmp_client_init(&c, &ccfg), EOK);
        ASSERT_EQ(qmp_client_connect(&c, kSvc, 0, 1, 0), EOK);
    }

    Model model_;
    int dummy_ops_ = 0;
    qmp_server_t server_;
    alignas(8) qmp_req_t pool_[kPool];
    alignas(8) unsigned char workbuf_[QMP_SERVER_WORKBUF_SIZE(QMP_HDR_SIZE + 64, kThreads)];
    std::thread runner_;
};

TEST_F(Lifecycle, OnewayRepliesBeforeTheHandlerFinishes) {
    qmp_client_t c;
    connect(c);
    EchoIn in{7, {}};
    EXPECT_EQ(qmp_client_oneway(&c, kMFire, &in, sizeof(in), nullptr), EOK);
    // We returned while the handler is still spinning: receipt precedes the work.
    for (int i = 0; i < 5000 && !model_.fire_running; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_TRUE(model_.fire_running.load());
    EXPECT_EQ(model_.fire_calls.load(), 0);
    model_.fire_release = true;
    qmp_client_close(&c);
}

TEST_F(Lifecycle, OnewayFlagMustMatchTheMethod) {
    qmp_client_t c;
    connect(c);
    EchoIn in{1, {}};
    EchoOut out{};
    int32_t st = 0;
    model_.fire_release = true; // in case the handler does run
    EXPECT_EQ(qmp_client_call(&c, kMFire, &in, sizeof(in), nullptr, 0, &st, nullptr), EINVAL);
    EXPECT_EQ(qmp_client_oneway(&c, kMEcho, &in, sizeof(in), nullptr), EINVAL);
    // Sanity: the proper forms still work.
    EXPECT_EQ(qmp_client_call(&c, kMEcho, &in, sizeof(in), &out, sizeof(out), &st, nullptr), EOK);
    EXPECT_EQ(out.value, 2);
    qmp_client_close(&c);
}

TEST_F(Lifecycle, DeferredReplyCompletesFromAnotherThread) {
    qmp_client_t c;
    connect(c);
    model_.defer_next = true;
    std::thread completer([&] {
        qmp_req_t *req = model_.wait_parked();
        ASSERT_NE(req, nullptr);
        EchoOut out{41, {}};
        EXPECT_EQ(qmp_req_reply(req, &out, sizeof(out), 0), EOK);
        EXPECT_EQ(qmp_req_reply(req, &out, sizeof(out), 0), EINVAL); // slot already freed
    });
    EchoIn in{1, {}};
    EchoOut out{};
    int32_t st = -1;
    EXPECT_EQ(qmp_client_call(&c, kMEcho, &in, sizeof(in), &out, sizeof(out), &st, nullptr), EOK);
    EXPECT_EQ(st, 0);
    EXPECT_EQ(out.value, 41);
    completer.join();
    qmp_client_close(&c);
}

TEST_F(Lifecycle, DeferredPoolExhaustionIsEAGAIN) {
    qmp_client_t c1;
    qmp_client_t c2;
    qmp_client_t c3;
    connect(c1);
    connect(c2);
    connect(c3);
    model_.defer_next = true;
    auto call = [](qmp_client_t *c, int *rc) {
        EchoIn in{1, {}};
        EchoOut out{};
        int32_t st = 0;
        *rc = qmp_client_call(c, kMEcho, &in, sizeof(in), &out, sizeof(out), &st, nullptr);
    };
    int rc1 = -1;
    int rc2 = -1;
    std::thread t1(call, &c1, &rc1);
    std::thread t2(call, &c2, &rc2);
    // Wait until both pool slots are parked. parked is overwritten; count via pool scan
    // under the pool mutex (the server mutates slot states concurrently).
    auto *si = reinterpret_cast<qmp_server_impl_t *>(&server_);
    auto parked_slots = [&] {
        std::vector<qmp_req_t *> v;
        pthread_mutex_lock(&si->pool_mu);
        for (size_t p = 0; p < kPool; p++) {
            if (si->pool[p].state == QMP_REQ_PARKED) {
                v.push_back(reinterpret_cast<qmp_req_t *>(&si->pool[p]));
            }
        }
        pthread_mutex_unlock(&si->pool_mu);
        return v;
    };
    for (int i = 0; i < 5000 && parked_slots().size() != 2; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    int rc3 = -1;
    call(&c3, &rc3); // no slot left: QMP_DEFERRED degrades to EAGAIN
    EXPECT_EQ(rc3, EAGAIN);
    // Release the two parked calls.
    for (qmp_req_t *req : parked_slots()) {
        EchoOut out{9, {}};
        EXPECT_EQ(qmp_req_reply(req, &out, sizeof(out), 0), EOK);
    }
    t1.join();
    t2.join();
    EXPECT_EQ(rc1, EOK);
    EXPECT_EQ(rc2, EOK);
    qmp_client_close(&c1);
    qmp_client_close(&c2);
    qmp_client_close(&c3);
}

TEST_F(Lifecycle, ClientTimeoutCancelsAParkedRequest) {
    qmp_client_t c;
    connect(c);
    model_.defer_next = true;
    EchoIn in{1, {}};
    EchoOut out{};
    int32_t st = 0;
    qmp_call_opts_t opts{};
    opts.timeout_ns = 20ull * 1000 * 1000;
    // REPLY-blocked timeout: unblock pulse -> on_cancel -> MsgError(ECANCELED), which the
    // client maps to ETIMEDOUT because its own deadline has passed (DESIGN §3.8).
    EXPECT_EQ(qmp_client_call(&c, kMEcho, &in, sizeof(in), &out, sizeof(out), &st, &opts),
              ETIMEDOUT);
    EXPECT_EQ(model_.cancels.load(), 1);
    qmp_client_close(&c);
}

TEST_F(Lifecycle, DisconnectCancelsParkedRequestsAndReportsTheClient) {
    qmp_client_t caller;
    qmp_client_t watcher;
    connect(caller);
    connect(watcher);
    model_.defer_next = true;
    std::thread t([&] {
        EchoIn in{1, {}};
        EchoOut out{};
        int32_t st = 0;
        // The park side never replies; the timeout turns into an unblock, but first the
        // disconnect from close() must have cancelled the request.
        qmp_call_opts_t opts{};
        opts.timeout_ns = 500ull * 1000 * 1000;
        int rc = qmp_client_call(&caller, kMEcho, &in, sizeof(in), &out, sizeof(out), &st, &opts);
        EXPECT_EQ(rc, ETIMEDOUT); // cancelled requests release the client with ETIMEDOUT
    });
    ASSERT_NE(model_.wait_parked(), nullptr);
    // watcher disconnects: its process (this test) has no parked requests under its scoid.
    qmp_client_close(&watcher);
    for (int i = 0; i < 5000 && model_.clients_gone.load() == 0; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_GE(model_.clients_gone.load(), 1);
    EXPECT_EQ(model_.gone_pid.load(), static_cast<int32_t>(getpid()));
    t.join();
    EXPECT_EQ(model_.cancels.load(), 1); // the unblock path cancelled the caller's request
    qmp_client_close(&caller);
}

TEST_F(Lifecycle, AclDeniesPerMethod) {
    qmp_client_t c;
    connect(c); // HELLO is exempt from the ACL
    model_.denied_resource = kMDeny;
    EchoIn in{1, {}};
    EchoOut out{};
    int32_t st = 0;
    EXPECT_EQ(qmp_client_call(&c, kMDeny, &in, sizeof(in), &out, sizeof(out), &st, nullptr),
              EACCES);
    EXPECT_EQ(qmp_client_call(&c, kMEcho, &in, sizeof(in), &out, sizeof(out), &st, nullptr), EOK);
    qmp_client_close(&c);
}

TEST_F(Lifecycle, UndersizedReplyBufferIsDetected) {
    qmp_client_t c;
    connect(c);
    EchoIn in{1, {}};
    uint8_t small[4];
    int32_t st = 0;
    // The kernel would silently truncate; the server detects it via dstmsglen.
    EXPECT_EQ(qmp_client_call(&c, kMEcho, &in, sizeof(in), small, sizeof(small), &st, nullptr),
              EMSGSIZE);
    qmp_client_close(&c);
}

TEST_F(Lifecycle, ReconnectAfterRestartSeesANewGeneration) {
    qmp_client_t c;
    connect(c);
    uint32_t gen1 = qmp_client_server_generation(&c);
    stop_server();
    EchoIn in{1, {}};
    EchoOut out{};
    int32_t st = 0;
    EXPECT_EQ(qmp_client_call(&c, kMEcho, &in, sizeof(in), &out, sizeof(out), &st, nullptr), ESRCH);
    EXPECT_FALSE(qmp_client_is_connected(&c));
    EXPECT_EQ(qmp_client_call(&c, kMEcho, &in, sizeof(in), &out, sizeof(out), &st, nullptr),
              ENOTCONN);

    std::thread restarter([this] {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        start_server();
    });
    EXPECT_EQ(qmp_client_reconnect(&c), EOK); // backs off until the restart finishes
    restarter.join();
    EXPECT_TRUE(qmp_client_is_connected(&c));
    EXPECT_NE(qmp_client_server_generation(&c), gen1); // restarted server, new generation
    EXPECT_EQ(qmp_client_call(&c, kMEcho, &in, sizeof(in), &out, sizeof(out), &st, nullptr), EOK);
    EXPECT_EQ(out.value, 2);
    qmp_client_close(&c);
}

TEST_F(Lifecycle, ReconnectWhileConnectedIsEISCONN) {
    qmp_client_t c;
    connect(c);
    EXPECT_EQ(qmp_client_reconnect(&c), EISCONN);
    qmp_client_close(&c);
}

} // namespace
