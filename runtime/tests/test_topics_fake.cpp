// SPDX-License-Identifier: Apache-2.0
// Topics on the host (ADR-003 notify-then-pull): subscribe, publish, pull in LATEST and
// QUEUE mode, loss accounting, unsubscribe, disconnect cleanup and restart resubscribe.
#include <gtest/gtest.h>

#include <qmp/qmp_client.h>
#include <qmp/qmp_server.h>
#include <qmp/qmp_transport.h>

#include <atomic>
#include <cstring>
#include <thread>
#include <vector>

#include "joint.qmp.h"

namespace {

inline qmp_server_cfg_t server_cfg(const qmp_transport_t *tr, int nthreads, void *buf, size_t size,
                                   void *topicbuf, size_t topicbuf_size) {
    qmp_server_cfg_t cfg{};
    cfg.transport = tr;
    cfg.nthreads = nthreads;
    cfg.workbuf = buf;
    cfg.workbuf_size = size;
    cfg.topicbuf = topicbuf;
    cfg.topicbuf_size = topicbuf_size;
    return cfg;
}

inline qmp_client_cfg_t client_cfg(const qmp_transport_t *tr, uint64_t timeout_ns) {
    qmp_client_cfg_t cfg{};
    cfg.transport = tr;
    cfg.default_timeout_ns = timeout_ns;
    return cfg;
}

constexpr uint64_t kPollMs = 2000ull * 1000 * 1000; /* generous: CI boxes are slow */
constexpr uint64_t kShortMs = 50ull * 1000 * 1000;

struct Update {
    demo_joint_v1_JointState state;
    uint64_t seq;
    uint64_t dropped;
};

/* The callback runs on the qmp_client_poll() thread == the test thread: no locking. */
void record_state(void *ctx, const demo_joint_v1_JointState *sample, uint64_t seq,
                  uint64_t dropped) {
    auto *v = static_cast<std::vector<Update> *>(ctx);
    v->push_back({*sample, seq, dropped});
}

demo_joint_v1_JointState state_at(uint64_t n) {
    demo_joint_v1_JointState st{};
    st.seq = n;
    st.timestamp_ns = n * 1000U;
    st.position_rad = static_cast<float>(n);
    return st;
}

class TopicsLatest : public ::testing::Test {
  protected:
    static constexpr int kThreads = 2;
    alignas(8) unsigned char workbuf_[QMP_SERVER_WORKBUF_SIZE(DEMO_JOINT_V1_JOINT_MAX_MSG_SIZE,
                                                              kThreads)]{};
    alignas(8) unsigned char topicbuf_[DEMO_JOINT_V1_JOINT_TOPIC_STORAGE_SIZE]{};
    qmp_server_t server_{};
    demo_joint_v1_Joint_ops ops_{};
    std::thread runner_;

    void SetUp() override { start(); }

    void TearDown() override { stop(); }

    void start() {
        qmp_server_cfg_t cfg = server_cfg(qmp_transport_fake(), kThreads, workbuf_,
                                          sizeof(workbuf_), topicbuf_, sizeof(topicbuf_));
        ASSERT_EQ(qmp_server_init(&server_, &cfg), EOK);
        ASSERT_EQ(demo_joint_v1_Joint_register(&server_, 0, &ops_, nullptr), EOK);
        runner_ = std::thread([this] { EXPECT_EQ(qmp_server_run(&server_), EOK); });
    }

    void stop() {
        if (runner_.joinable()) {
            EXPECT_EQ(qmp_server_stop(&server_), EOK);
            runner_.join();
            EXPECT_EQ(qmp_server_close(&server_), EOK);
        }
    }

    void connect(qmp_client_t &c, uint64_t timeout_ns = 0) {
        qmp_client_cfg_t ccfg = client_cfg(qmp_transport_fake(), timeout_ns);
        ASSERT_EQ(qmp_client_init(&c, &ccfg), EOK);
        ASSERT_EQ(demo_joint_v1_Joint_connect(&c, 0, &ccfg), EOK);
    }
};

TEST_F(TopicsLatest, SubscribePublishPollDeliversTypedSample) {
    qmp_client_t c;
    connect(c);
    std::vector<Update> got;
    demo_joint_v1_Joint_StateChanged_sub_t sub{};
    ASSERT_EQ(demo_joint_v1_Joint_subscribe_StateChanged(&c, &sub, record_state, &got), EOK);

    demo_joint_v1_JointState st = state_at(7);
    ASSERT_EQ(demo_joint_v1_Joint_publish_StateChanged(&server_, &st), EOK);
    ASSERT_EQ(qmp_client_poll(&c, kPollMs), EOK);

    ASSERT_EQ(got.size(), 1u);
    EXPECT_EQ(got[0].seq, 1u);
    EXPECT_EQ(got[0].dropped, 0u);
    EXPECT_EQ(got[0].state.seq, 7u);
    EXPECT_EQ(got[0].state.position_rad, 7.0F);
    EXPECT_EQ(qmp_client_close(&c), EOK);
}

TEST_F(TopicsLatest, LatestModeOverwritesAndReportsTheLoss) {
    qmp_client_t c;
    connect(c);
    std::vector<Update> got;
    demo_joint_v1_Joint_StateChanged_sub_t sub{};
    ASSERT_EQ(demo_joint_v1_Joint_subscribe_StateChanged(&c, &sub, record_state, &got), EOK);

    for (uint64_t n = 1; n <= 3; n++) {
        demo_joint_v1_JointState st = state_at(n);
        ASSERT_EQ(demo_joint_v1_Joint_publish_StateChanged(&server_, &st), EOK);
    }
    ASSERT_EQ(qmp_client_poll(&c, kPollMs), EOK);

    ASSERT_EQ(got.size(), 1u); /* one slot: newest wins */
    EXPECT_EQ(got[0].seq, 3u);
    EXPECT_EQ(got[0].dropped, 2u); /* loss is reported, never hidden */
    EXPECT_EQ(got[0].state.seq, 3u);
    EXPECT_EQ(qmp_client_close(&c), EOK);
}

TEST_F(TopicsLatest, OnlyUpdatesAfterSubscribeAreDelivered) {
    qmp_client_t c;
    connect(c);
    demo_joint_v1_JointState st = state_at(1);
    ASSERT_EQ(demo_joint_v1_Joint_publish_StateChanged(&server_, &st), EOK);

    std::vector<Update> got;
    demo_joint_v1_Joint_StateChanged_sub_t sub{};
    ASSERT_EQ(demo_joint_v1_Joint_subscribe_StateChanged(&c, &sub, record_state, &got), EOK);
    st = state_at(2);
    ASSERT_EQ(demo_joint_v1_Joint_publish_StateChanged(&server_, &st), EOK);
    ASSERT_EQ(qmp_client_poll(&c, kPollMs), EOK);

    ASSERT_EQ(got.size(), 1u);
    EXPECT_EQ(got[0].seq, 2u);
    EXPECT_EQ(got[0].dropped, 0u); /* seq 1 predates the subscription: not a loss */
    EXPECT_EQ(qmp_client_close(&c), EOK);
}

TEST_F(TopicsLatest, PublishWithNoSubscribersSucceeds) {
    demo_joint_v1_JointState st = state_at(1);
    EXPECT_EQ(demo_joint_v1_Joint_publish_StateChanged(&server_, &st), EOK);
}

TEST_F(TopicsLatest, PollWithoutTrafficTimesOut) {
    qmp_client_t c;
    connect(c);
    std::vector<Update> got;
    demo_joint_v1_Joint_StateChanged_sub_t sub{};
    ASSERT_EQ(demo_joint_v1_Joint_subscribe_StateChanged(&c, &sub, record_state, &got), EOK);
    EXPECT_EQ(qmp_client_poll(&c, kShortMs), ETIMEDOUT);
    EXPECT_TRUE(got.empty());
    EXPECT_EQ(qmp_client_close(&c), EOK);
}

TEST_F(TopicsLatest, UnsubscribeStopsDelivery) {
    qmp_client_t c;
    connect(c);
    std::vector<Update> got;
    demo_joint_v1_Joint_StateChanged_sub_t sub{};
    ASSERT_EQ(demo_joint_v1_Joint_subscribe_StateChanged(&c, &sub, record_state, &got), EOK);
    ASSERT_EQ(demo_joint_v1_Joint_unsubscribe_StateChanged(&c, &sub), EOK);

    demo_joint_v1_JointState st = state_at(1);
    ASSERT_EQ(demo_joint_v1_Joint_publish_StateChanged(&server_, &st), EOK);
    EXPECT_EQ(qmp_client_poll(&c, kShortMs), EINVAL); /* no active subscription left */
    EXPECT_TRUE(got.empty());
    EXPECT_EQ(qmp_client_close(&c), EOK);
}

TEST_F(TopicsLatest, DisconnectedSubscriberIsDroppedOthersKeepReceiving) {
    qmp_client_t a;
    qmp_client_t b;
    connect(a);
    connect(b);
    std::vector<Update> got_a;
    std::vector<Update> got_b;
    demo_joint_v1_Joint_StateChanged_sub_t sub_a{};
    demo_joint_v1_Joint_StateChanged_sub_t sub_b{};
    ASSERT_EQ(demo_joint_v1_Joint_subscribe_StateChanged(&a, &sub_a, record_state, &got_a), EOK);
    ASSERT_EQ(demo_joint_v1_Joint_subscribe_StateChanged(&b, &sub_b, record_state, &got_b), EOK);

    ASSERT_EQ(qmp_client_close(&a), EOK); /* "kill": connection gone, no unsubscribe */

    demo_joint_v1_JointState st = state_at(5);
    ASSERT_EQ(demo_joint_v1_Joint_publish_StateChanged(&server_, &st), EOK);
    ASSERT_EQ(qmp_client_poll(&b, kPollMs), EOK);
    ASSERT_EQ(got_b.size(), 1u);
    EXPECT_EQ(got_b[0].state.seq, 5u);
    EXPECT_TRUE(got_a.empty());
    EXPECT_EQ(qmp_client_close(&b), EOK);
}

TEST_F(TopicsLatest, ServerRestartIsSeenAndResubscribeWorks) {
    qmp_client_t c;
    connect(c);
    std::vector<Update> got;
    demo_joint_v1_Joint_StateChanged_sub_t sub{};
    ASSERT_EQ(demo_joint_v1_Joint_subscribe_StateChanged(&c, &sub, record_state, &got), EOK);
    uint32_t gen0 = qmp_client_server_generation(&c);

    stop(); /* server dies with the subscription outstanding */
    EXPECT_EQ(qmp_client_poll(&c, kPollMs), ESRCH);
    EXPECT_FALSE(qmp_client_is_connected(&c));

    start(); /* new session: old sub ids and event handles are gone */
    ASSERT_EQ(qmp_client_reconnect(&c), EOK);
    EXPECT_NE(qmp_client_server_generation(&c), gen0);

    demo_joint_v1_JointState st = state_at(9);
    ASSERT_EQ(demo_joint_v1_Joint_publish_StateChanged(&server_, &st), EOK);
    ASSERT_EQ(qmp_client_poll(&c, kPollMs), EOK);
    ASSERT_EQ(got.size(), 1u);
    EXPECT_EQ(got[0].seq, 1u); /* fresh server, fresh sequence space */
    EXPECT_EQ(got[0].state.seq, 9u);
    EXPECT_EQ(qmp_client_close(&c), EOK);
}

/* ---- QUEUE mode on a hand-built service -------------------------------------- */

struct QSample {
    uint64_t a;
    uint64_t b;
};

constexpr uint16_t kQSvc = 0x0404;
constexpr uint16_t kQTopic = 0x8002U;
constexpr uint16_t kQDepth = 4;
constexpr uint16_t kLTopic = 0x8008U;

int q_noop(void *, qmp_req_t *, const void *, void *, int32_t *) {
    return 0;
}

int q_tramp(const void *, void *ctx, qmp_req_t *req, const void *in, void *out, int32_t *status) {
    return q_noop(ctx, req, in, out, status);
}

const qmp_method_desc_t kQMethods[] = {
    {1, 0, 0, 0, 0, nullptr, q_tramp, "Noop"},
};
const qmp_topic_desc_t kQTopics[] = {
    {kQTopic, QMP_TOPIC_QUEUE, kQDepth, 0, sizeof(QSample), 0, "QueueTopic"},
    {kLTopic, QMP_TOPIC_LATEST, 0, 0, sizeof(QSample), 0, "LatestTopic"},
};
const qmp_service_desc_t kQDesc = {
    kQSvc, 1, 0, QMP_HDR_SIZE + 72, "test.TopicsQueue", kQMethods, 1, kQTopics, 2, NULL, 0};

struct QUpdate {
    QSample s;
    uint64_t seq;
    uint64_t dropped;
};

void record_q(void *ctx, const void *sample, uint32_t len, uint64_t seq, uint64_t dropped) {
    auto *v = static_cast<std::vector<QUpdate> *>(ctx);
    QUpdate u{};
    ASSERT_EQ(len, sizeof(QSample));
    std::memcpy(&u.s, sample, sizeof(QSample));
    u.seq = seq;
    u.dropped = dropped;
    v->push_back(u);
}

class TopicsQueue : public ::testing::Test {
  protected:
    alignas(8) unsigned char workbuf_[QMP_SERVER_WORKBUF_SIZE(QMP_HDR_SIZE + 72, 1)]{};
    alignas(8) unsigned char topicbuf_[QMP_SERVER_SUBTABLE_SIZE +
                                       QMP_TOPIC_STATE_SIZE(sizeof(QSample), kQDepth) +
                                       QMP_TOPIC_STATE_SIZE(sizeof(QSample), 1)]{};
    qmp_server_t server_{};
    int dummy_ops_ = 0;
    std::thread runner_;

    void SetUp() override {
        qmp_server_cfg_t cfg = server_cfg(qmp_transport_fake(), 1, workbuf_, sizeof(workbuf_),
                                          topicbuf_, sizeof(topicbuf_));
        ASSERT_EQ(qmp_server_init(&server_, &cfg), EOK);
        ASSERT_EQ(qmp_server_register(&server_, &kQDesc, 0, &dummy_ops_, nullptr), EOK);
        runner_ = std::thread([this] { EXPECT_EQ(qmp_server_run(&server_), EOK); });
    }

    void TearDown() override {
        EXPECT_EQ(qmp_server_stop(&server_), EOK);
        runner_.join();
        EXPECT_EQ(qmp_server_close(&server_), EOK);
    }

    void connect(qmp_client_t &c) {
        qmp_client_cfg_t ccfg = client_cfg(qmp_transport_fake(), 0);
        ASSERT_EQ(qmp_client_init(&c, &ccfg), EOK);
        ASSERT_EQ(qmp_client_connect(&c, kQSvc, 0, 1, 0), EOK);
    }

    int publish_q(uint64_t n) {
        QSample s{n, n * 2};
        return qmp_server_publish(&server_, kQTopic, &s, sizeof(s));
    }
};

TEST_F(TopicsQueue, QueueDeliversEveryUpdateInOrder) {
    qmp_client_t c;
    connect(c);
    std::vector<QUpdate> got;
    alignas(8) QSample buf{};
    qmp_sub_t sub{};
    ASSERT_EQ(qmp_client_subscribe(&c, kQTopic, &buf, sizeof(buf), record_q, &got, &sub), EOK);

    for (uint64_t n = 1; n <= 3; n++) {
        ASSERT_EQ(publish_q(n), EOK);
    }
    ASSERT_EQ(qmp_client_poll(&c, kPollMs), EOK);

    ASSERT_EQ(got.size(), 3u);
    for (uint64_t n = 1; n <= 3; n++) {
        EXPECT_EQ(got[n - 1].seq, n);
        EXPECT_EQ(got[n - 1].dropped, 0u);
        EXPECT_EQ(got[n - 1].s.a, n);
        EXPECT_EQ(got[n - 1].s.b, 2 * n);
    }
    EXPECT_EQ(qmp_client_close(&c), EOK);
}

TEST_F(TopicsQueue, QueueOverflowDropsOldestAndReportsIt) {
    qmp_client_t c;
    connect(c);
    std::vector<QUpdate> got;
    alignas(8) QSample buf{};
    qmp_sub_t sub{};
    ASSERT_EQ(qmp_client_subscribe(&c, kQTopic, &buf, sizeof(buf), record_q, &got, &sub), EOK);

    for (uint64_t n = 1; n <= kQDepth + 2; n++) { /* 6 publishes, depth 4 */
        ASSERT_EQ(publish_q(n), EOK);
    }
    ASSERT_EQ(qmp_client_poll(&c, kPollMs), EOK);

    ASSERT_EQ(got.size(), static_cast<size_t>(kQDepth));
    EXPECT_EQ(got[0].seq, 3u);     /* 1 and 2 were pushed out */
    EXPECT_EQ(got[0].dropped, 2u); /* ...and the loss is reported */
    for (size_t i = 1; i < got.size(); i++) {
        EXPECT_EQ(got[i].seq, 3u + i);
        EXPECT_EQ(got[i].dropped, 0u);
    }
    EXPECT_EQ(qmp_client_close(&c), EOK);
}

TEST_F(TopicsQueue, SlowPollerCatchesUpAcrossPulseLoss) {
    /* The fake's notify queue is shallow on purpose: hammer it so pulses are lost, then
     * verify the pull cursor still accounts for every sequence number (ADR-003: the pulse
     * is a hint, the cursor is the truth). */
    qmp_client_t c;
    connect(c);
    std::vector<QUpdate> got;
    alignas(8) QSample buf{};
    qmp_sub_t sub{};
    ASSERT_EQ(qmp_client_subscribe(&c, kQTopic, &buf, sizeof(buf), record_q, &got, &sub), EOK);

    constexpr uint64_t kTotal = 40;
    for (uint64_t n = 1; n <= kTotal; n++) {
        ASSERT_EQ(publish_q(n), EOK);
    }
    while (qmp_client_poll(&c, kShortMs) == EOK) {
    }

    uint64_t accounted = 0;
    uint64_t last = 0;
    for (const QUpdate &u : got) {
        EXPECT_GT(u.seq, last);
        accounted += 1 + u.dropped;
        last = u.seq;
    }
    EXPECT_EQ(last, kTotal);
    EXPECT_EQ(accounted, kTotal);
    EXPECT_EQ(qmp_client_close(&c), EOK);
}

TEST_F(TopicsQueue, SubscriptionTableFullIsEAGAIN) {
    qmp_client_t a;
    qmp_client_t b;
    qmp_client_t d;
    connect(a);
    connect(b);
    connect(d);
    alignas(8) QSample bufs[QMP_SERVER_MAX_SUBS + 1]{};
    qmp_sub_t subs[QMP_SERVER_MAX_SUBS + 1]{};
    for (int i = 0; i < QMP_CLIENT_MAX_SUBS; i++) {
        ASSERT_EQ(qmp_client_subscribe(&a, kQTopic, &bufs[i], sizeof(QSample), record_q, nullptr,
                                       &subs[i]),
                  EOK);
        ASSERT_EQ(qmp_client_subscribe(&b, kQTopic, &bufs[8 + i], sizeof(QSample), record_q,
                                       nullptr, &subs[8 + i]),
                  EOK);
    }
    /* 16 server slots are taken: the bound is an explicit error, not a silent drop. */
    EXPECT_EQ(
        qmp_client_subscribe(&d, kQTopic, &bufs[16], sizeof(QSample), record_q, nullptr, &subs[16]),
        EAGAIN);
    ASSERT_EQ(qmp_client_close(&a), EOK); /* frees 8 server slots */
    EXPECT_EQ(
        qmp_client_subscribe(&d, kQTopic, &bufs[16], sizeof(QSample), record_q, nullptr, &subs[16]),
        EOK);
    EXPECT_EQ(qmp_client_close(&b), EOK);
    EXPECT_EQ(qmp_client_close(&d), EOK);
}

TEST_F(TopicsQueue, UnsubscribeRacesAPublisher) {
    qmp_client_t c;
    connect(c);
    std::atomic<bool> stop{false};
    std::thread pub([this, &stop] {
        uint64_t n = 0;
        while (!stop.load()) {
            EXPECT_EQ(publish_q(++n), EOK); /* must never fail or block on churn */
        }
    });
    for (int i = 0; i < 200; i++) {
        std::vector<QUpdate> got;
        alignas(8) QSample buf{};
        qmp_sub_t sub{};
        ASSERT_EQ(qmp_client_subscribe(&c, kQTopic, &buf, sizeof(buf), record_q, &got, &sub), EOK);
        (void)qmp_client_poll(&c, kShortMs);
        ASSERT_EQ(qmp_client_unsubscribe(&c, &sub), EOK);
    }
    stop.store(true);
    pub.join();
    EXPECT_EQ(qmp_client_close(&c), EOK);
}

/* A publisher faster than the pull round trip must not trap the subscriber inside
 * qmp_client_poll(). The callback publishes a fresh sample on every delivery, so
 * "remaining" never drops to zero; poll must stop at the sequence that was newest when
 * it started, and the next poll must still see the newer samples (their pulses are
 * pending). Before the fix this looped until the cap and the first poll saw 50. */
TEST_F(TopicsQueue, PollReturnsWhilePublisherOutpacesTheDrain) {
    qmp_client_t c;
    connect(c);
    struct Ctx {
        qmp_server_t *srv;
        std::vector<QUpdate> got;
        uint64_t next;
    } ctx{&server_, {}, 2};
    auto cb = [](void *p, const void *sample, uint32_t len, uint64_t seq, uint64_t dropped) {
        auto *x = static_cast<Ctx *>(p);
        record_q(&x->got, sample, len, seq, dropped);
        if (x->next < 50) {
            ++x->next;
            QSample s{x->next, x->next * 2};
            EXPECT_EQ(qmp_server_publish(x->srv, kQTopic, &s, sizeof(s)), EOK);
        }
    };
    alignas(8) QSample buf{};
    qmp_sub_t sub{};
    ASSERT_EQ(qmp_client_subscribe(&c, kQTopic, &buf, sizeof(buf), cb, &ctx, &sub), EOK);
    ASSERT_EQ(publish_q(1), EOK);
    ASSERT_EQ(publish_q(2), EOK);
    ASSERT_EQ(qmp_client_poll(&c, kShortMs), EOK);
    ASSERT_EQ(ctx.got.size(), 2U); /* seq 1..2 existed at the first pull; 3, 4 came later */
    EXPECT_EQ(ctx.got.back().s.a, 2U);
    ASSERT_EQ(qmp_client_poll(&c, kShortMs), EOK); /* the pulse for seq 3 is pending */
    ASSERT_GE(ctx.got.size(), 3U);
    EXPECT_EQ(ctx.got[2].s.a, 3U);
    EXPECT_EQ(qmp_client_unsubscribe(&c, &sub), EOK);
    EXPECT_EQ(qmp_client_close(&c), EOK);
}

TEST_F(TopicsQueue, UnknownTopicIsRejected) {
    qmp_client_t c;
    connect(c);
    alignas(8) QSample buf{};
    qmp_sub_t sub{};
    int32_t rc = qmp_client_subscribe(&c, 0x9999, &buf, sizeof(buf), record_q, nullptr, &sub);
    EXPECT_EQ(rc, ENOENT);
    EXPECT_EQ(qmp_server_publish(&server_, 0x9999, &buf, sizeof(buf)), ENOENT);
    EXPECT_EQ(qmp_client_close(&c), EOK);
}

} // namespace
