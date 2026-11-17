// SPDX-License-Identifier: Apache-2.0
// In-flight cancellation of running synchronous handlers over the fake transport, which
// models the kernel: a REPLY-blocked client whose timeout fires sends one unblock pulse and
// stays blocked until the server replies or errors; the server replies a neutral ECANCELED
// (it cannot tell a timeout from a signal) and the client maps it to ETIMEDOUT only when its
// own deadline has passed (DESIGN §3.8).
#include <gtest/gtest.h>

#include <qmp/qmp_client.h>
#include <qmp/qmp_server.h>
#include <qmp/qmp_transport.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

#include "core/internal.h"

namespace {

constexpr uint16_t kSvc = 0x0405;
constexpr uint16_t kMQuick = 0x0001; // immediate echo
constexpr uint16_t kMSlow = 0x0002;  // polls qmp_req_canceled() until released or cancelled
constexpr uint16_t kMBlock = 0x0003; // never polls; waits for release only

struct EchoIn {
    int32_t value;
    uint8_t _pad[4];
};
struct EchoOut {
    int32_t value;
    uint8_t _pad[4];
};

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

// Handlers give up after this long so a regression fails instead of hanging. It is a wall
// clock deadline, not an iteration count: sleep_for(1ms) rounds up to the next tick (about
// 2 ms on QNX), which would silently double an iteration-based cap.
constexpr milliseconds kHandlerCap{5000};

// Polls pred every millisecond until it holds or `limit` elapses; returns the last value.
template <typename Pred> bool wait_until(Pred pred, milliseconds limit = kHandlerCap) {
    const auto deadline = Clock::now() + limit;
    while (!pred()) {
        if (Clock::now() >= deadline) {
            return pred();
        }
        std::this_thread::sleep_for(milliseconds(1));
    }
    return true;
}

struct Model {
    std::atomic<int> slow_running{0};
    std::atomic<int> slow_canceled{0};  // handler observed qmp_req_canceled() == 1
    std::atomic<int> slow_completed{0}; // handler finished normally
    std::atomic<int> block_running{0};
    std::atomic<int> block_completed{0};
    std::atomic<bool> release{false};
    std::atomic<int> cancels{0}; // on_cancel (parked requests only)
    std::atomic<int> clients_gone{0};
    std::atomic<int64_t> last_rcvid{0};
    std::atomic<int32_t> last_scoid{0};
};

int quick_handler(const void *, void *ctx, qmp_req_t *req, const void *in, void *out, int32_t *st) {
    auto *m = static_cast<Model *>(ctx);
    m->last_rcvid = reinterpret_cast<qmp_req_impl_t *>(req)->rcvid;
    static_cast<EchoOut *>(out)->value = static_cast<const EchoIn *>(in)->value + 1;
    *st = 0;
    return QMP_HANDLED;
}

int slow_handler(const void *, void *ctx, qmp_req_t *req, const void *in, void *out, int32_t *st) {
    auto *m = static_cast<Model *>(ctx);
    qmp_req_info_t info{};
    EXPECT_EQ(qmp_req_get_info(req, &info), EOK);
    m->last_rcvid = reinterpret_cast<qmp_req_impl_t *>(req)->rcvid;
    m->last_scoid = info.scoid;
    m->slow_running++;
    const auto deadline = Clock::now() + kHandlerCap;
    while (Clock::now() < deadline) {
        if (qmp_req_canceled(req)) {
            m->slow_canceled++;
            // The result is written anyway: dispatch must discard it, not reply with it.
            static_cast<EchoOut *>(out)->value = -1;
            return QMP_HANDLED;
        }
        if (m->release) {
            break;
        }
        std::this_thread::sleep_for(milliseconds(1));
    }
    static_cast<EchoOut *>(out)->value = static_cast<const EchoIn *>(in)->value + 1;
    *st = 0;
    m->slow_completed++;
    return QMP_HANDLED;
}

int block_handler(const void *, void *ctx, qmp_req_t *req, const void *in, void *out, int32_t *st) {
    auto *m = static_cast<Model *>(ctx);
    qmp_req_info_t info{};
    EXPECT_EQ(qmp_req_get_info(req, &info), EOK);
    m->last_scoid = info.scoid;
    m->block_running++;
    (void)wait_until([m] { return m->release.load(); });
    static_cast<EchoOut *>(out)->value = static_cast<const EchoIn *>(in)->value + 1;
    *st = 0;
    m->block_completed++;
    return QMP_HANDLED;
}

void on_cancel_cb(void *ctx, qmp_req_t *) {
    static_cast<Model *>(ctx)->cancels++;
}
void on_client_gone_cb(void *ctx, int32_t) {
    static_cast<Model *>(ctx)->clients_gone++;
}

const qmp_method_desc_t kMethods[] = {
    {kMQuick, 0, sizeof(EchoIn), sizeof(EchoOut), 0, nullptr, quick_handler, "Quick"},
    {kMSlow, 0, sizeof(EchoIn), sizeof(EchoOut), 0, nullptr, slow_handler, "Slow"},
    {kMBlock, 0, sizeof(EchoIn), sizeof(EchoOut), 0, nullptr, block_handler, "Block"},
};

const qmp_service_desc_t kDesc = {kSvc, 1,    0, QMP_HDR_SIZE + 64, "test.Cancel", kMethods,
                                  3,    NULL, 0};

class Cancel : public ::testing::Test {
  protected:
    static constexpr int kThreads = 2;
    static constexpr size_t kPool = 2;

    void SetUp() override {
        qmp_server_cfg_t cfg{};
        cfg.transport = qmp_transport_fake();
        cfg.nthreads = kThreads;
        cfg.workbuf = workbuf_;
        cfg.workbuf_size = sizeof(workbuf_);
        cfg.deferred_pool = pool_;
        cfg.deferred_count = kPool;
        cfg.on_cancel = on_cancel_cb;
        cfg.on_client_gone = on_client_gone_cb;
        ASSERT_EQ(qmp_server_init(&server_, &cfg), EOK);
        ASSERT_EQ(qmp_server_register(&server_, &kDesc, 0, &dummy_ops_, &model_), EOK);
        runner_ = std::thread([this] { EXPECT_EQ(qmp_server_run(&server_), EOK); });
    }

    void TearDown() override {
        model_.release = true; // never leave a handler spinning past the test
        EXPECT_EQ(qmp_server_stop(&server_), EOK);
        runner_.join();
        EXPECT_EQ(qmp_server_close(&server_), EOK);
    }

    void connect(qmp_client_t &c) {
        qmp_client_cfg_t ccfg{};
        ccfg.transport = qmp_transport_fake();
        ASSERT_EQ(qmp_client_init(&c, &ccfg), EOK);
        ASSERT_EQ(qmp_client_connect(&c, kSvc, 0, 1, 0), EOK);
    }

    int call(qmp_client_t &c, uint16_t method, uint64_t timeout_ns, int32_t in_value = 1,
             int32_t *out_value = nullptr) {
        EchoIn in{in_value, {}};
        EchoOut out{};
        int32_t st = 0;
        qmp_call_opts_t opts{};
        opts.timeout_ns = timeout_ns;
        int rc = qmp_client_call(&c, method, &in, sizeof(in), &out, sizeof(out), &st,
                                 timeout_ns != 0 ? &opts : nullptr);
        if (out_value != nullptr) {
            *out_value = out.value;
        }
        return rc;
    }

    qmp_server_impl_t *impl() { return reinterpret_cast<qmp_server_impl_t *>(&server_); }

    Model model_;
    int dummy_ops_ = 0;
    qmp_server_t server_;
    alignas(8) qmp_req_t pool_[kPool];
    alignas(8) unsigned char workbuf_[QMP_SERVER_WORKBUF_SIZE(QMP_HDR_SIZE + 64, kThreads)];
    std::thread runner_;
};

constexpr uint64_t kMs = 1000ull * 1000;

TEST_F(Cancel, TimeoutCancelsARunningHandlerAndDiscardsItsResult) {
    qmp_client_t c;
    connect(c);
    auto t0 = std::chrono::steady_clock::now();
    EXPECT_EQ(call(c, kMSlow, 50 * kMs), ETIMEDOUT);
    auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
            .count();
    EXPECT_LT(ms, 1000); // returned near the timeout, not after the handler's 5 s cap
    (void)wait_until([&] { return model_.slow_canceled.load() != 0; });
    EXPECT_EQ(model_.slow_canceled.load(), 1); // the handler saw qmp_req_canceled()
    EXPECT_EQ(model_.slow_completed.load(), 0);
    EXPECT_EQ(model_.cancels.load(), 0); // on_cancel is for parked requests only
    // The discarded result must not leak into the next call on the reused slot: a second
    // reply to the cancelled rcvid would corrupt it.
    int32_t out = 0;
    EXPECT_EQ(call(c, kMQuick, 0, 10, &out), EOK);
    EXPECT_EQ(out, 11);
    qmp_client_close(&c);
}

TEST_F(Cancel, PulsesAreServedWhileEveryWorkerIsBusy) {
    // Both workers run Slow handlers that only return once cancelled; without the dedicated
    // pulse thread nobody could receive the unblock pulses and both clients would stay
    // REPLY-blocked until the handlers' 5 s cap. Each client therefore checks its OWN
    // elapsed time against its timeout: returning late means the pulses were not served.
    constexpr auto kTimeout = milliseconds(50);
    constexpr auto kSlack = milliseconds(450); // scheduling noise, sanitizers, QNX ticks
    qmp_client_t c1;
    qmp_client_t c2;
    connect(c1);
    connect(c2);
    int rc1 = -1;
    int rc2 = -1;
    Clock::duration dt1{};
    Clock::duration dt2{};
    auto timed_call = [&](qmp_client_t &c, int &rc, Clock::duration &dt) {
        const auto t0 = Clock::now();
        rc = call(c, kMSlow, static_cast<uint64_t>(kTimeout.count()) * kMs);
        dt = Clock::now() - t0;
    };
    std::thread t1([&] { timed_call(c1, rc1, dt1); });
    std::thread t2([&] { timed_call(c2, rc2, dt2); });
    t1.join();
    t2.join();
    EXPECT_EQ(rc1, ETIMEDOUT);
    EXPECT_EQ(rc2, ETIMEDOUT);
    for (const auto dt : {dt1, dt2}) {
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(dt).count();
        // Lower bound with 1 ms of clock-granularity slack: the call must not give up early.
        EXPECT_GE(us, std::chrono::microseconds(kTimeout - milliseconds(1)).count());
        EXPECT_LT(us, std::chrono::microseconds(kTimeout + kSlack).count());
    }
    // Both handlers poll qmp_req_canceled() every millisecond, so they exit promptly and
    // TearDown does not wait for the cap.
    EXPECT_TRUE(wait_until([&] { return model_.slow_canceled.load() >= 2; }));
    EXPECT_EQ(model_.slow_canceled.load(), 2);
    EXPECT_EQ(model_.slow_completed.load(), 0);
    qmp_client_close(&c1);
    qmp_client_close(&c2);
}

TEST_F(Cancel, StaleUnblockPulseAfterACompletedCallIsDropped) {
    qmp_client_t c;
    connect(c);
    int32_t out = 0;
    EXPECT_EQ(call(c, kMQuick, 0, 1, &out), EOK);
    EXPECT_EQ(out, 2);
    // A pulse for the already-replied rcvid arrives late. MsgInfo (fake: slot state and
    // generation) no longer reports an unblock request, so it must be dropped.
    qmp_server_on_unblock(impl(), model_.last_rcvid.load());
    qmp_server_on_unblock(impl(), (99ll << 16) | 7); // never-issued rcvid
    EXPECT_EQ(call(c, kMQuick, 0, 5, &out), EOK);    // the server is unaffected
    EXPECT_EQ(out, 6);
    EXPECT_EQ(model_.cancels.load(), 0);
    EXPECT_EQ(model_.slow_canceled.load(), 0);
    qmp_client_close(&c);
}

TEST_F(Cancel, SignalUnblockReturnsECANCELEDNotETIMEDOUT) {
    // No client timeout: the unblock comes from a "signal" injected into the fake transport.
    // The server replies ECANCELED; with no expired deadline the client must NOT map it.
    qmp_client_t c;
    connect(c);
    int rc = -1;
    std::thread t([&] { rc = call(c, kMSlow, 0); });
    (void)wait_until([&] { return model_.slow_running.load() != 0; });
    ASSERT_EQ(model_.slow_running.load(), 1);
    ASSERT_EQ(qmp_transport_fake_unblock_inject(impl()->path), EOK);
    t.join();
    EXPECT_EQ(rc, ECANCELED);
    (void)wait_until([&] { return model_.slow_canceled.load() != 0; });
    EXPECT_EQ(model_.slow_canceled.load(), 1);
    qmp_client_close(&c);
}

TEST_F(Cancel, DisconnectDuringARunningHandlerDiscardsTheLateResult) {
    // Block never polls: the client times out (in-flight cancel) and disconnects while the
    // handler is still running. When it finally returns, the result must be discarded and
    // the server must keep serving other clients.
    qmp_client_t a;
    connect(a);
    EXPECT_EQ(call(a, kMBlock, 50 * kMs), ETIMEDOUT);
    EXPECT_EQ(model_.block_running.load(), 1);
    EXPECT_EQ(model_.block_completed.load(), 0); // still inside the handler
    qmp_client_close(&a);
    (void)wait_until([&] { return model_.clients_gone.load() != 0; });
    EXPECT_GE(model_.clients_gone.load(), 1);
    model_.release = true;
    (void)wait_until([&] { return model_.block_completed.load() != 0; });
    EXPECT_EQ(model_.block_completed.load(), 1);
    qmp_client_t b;
    connect(b);
    int32_t out = 0;
    EXPECT_EQ(call(b, kMQuick, 0, 20, &out), EOK); // no stray reply corrupted this slot
    EXPECT_EQ(out, 21);
    qmp_client_close(&b);
}

} // namespace
