// SPDX-License-Identifier: Apache-2.0
// End to end on the host: generated C client -> fake transport -> generated ops table -> reply.
#include <gtest/gtest.h>

#include <qmp/qmp_client.h>
#include <qmp/qmp_server.h>
#include <qmp/qmp_transport.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>

#include "joint.qmp.h"

namespace {

/* Handlers run concurrently on N worker threads: the model guards its own state. The lock
 * is released before the handler returns, i.e. never held across the reply. */
struct JointModel {
    std::mutex mu;
    float target = 0.0F;
    float position = 0.0F;
    uint64_t seq = 0;
    std::atomic<int> calls{0};
    int32_t next_status = 0; /* app error injection */
    int next_errno = 0;      /* transport error injection */
    bool oversize_mode = false;
};

int on_set_target(void *ctx, qmp_req_t *req, const demo_joint_v1_SetTargetRequest *in,
                  demo_joint_v1_SetTargetReply *out, int32_t *status) {
    auto *m = static_cast<JointModel *>(ctx);
    qmp_req_info_t info{};
    if (qmp_req_get_info(req, &info) != EOK) {
        return EIO;
    }
    m->calls++;
    if (m->next_errno != 0) {
        return m->next_errno;
    }
    if (m->next_status != 0) {
        *status = m->next_status;
        out->result = -1; /* must NOT reach the client */
        return QMP_HANDLED;
    }
    if (in->max_velocity <= 0.0F) {
        *status = 3; /* INVALID_ARGUMENT */
        return QMP_HANDLED;
    }
    {
        std::lock_guard<std::mutex> lock(m->mu);
        m->target = in->position_rad;
        m->position = in->position_rad;
        m->seq++;
    }
    out->result = static_cast<int32_t>(info.up_priority);
    return QMP_HANDLED;
}

int on_get_state(void *ctx, qmp_req_t *, demo_joint_v1_JointState *out, int32_t *) {
    auto *m = static_cast<JointModel *>(ctx);
    {
        std::lock_guard<std::mutex> lock(m->mu);
        out->seq = m->seq;
        out->position_rad = m->position;
    }
    out->timestamp_ns = 42;
    out->velocity = 0.0F;
    const char *mode = m->oversize_mode ? "this-mode-name-is-far-too-long" : "hold";
    size_t n = strlen(mode);
    out->mode.len = static_cast<uint16_t>(n);
    memcpy(out->mode.data, mode, n < sizeof(out->mode.data) ? n : sizeof(out->mode.data));
    return QMP_HANDLED;
}

class E2E : public ::testing::Test {
  protected:
    static constexpr int kThreads = 2;
    alignas(8) unsigned char workbuf_[QMP_SERVER_WORKBUF_SIZE(DEMO_JOINT_V1_JOINT_MAX_MSG_SIZE,
                                                              kThreads)]{};
    qmp_server_t server_{};
    JointModel model_;
    demo_joint_v1_Joint_ops ops_{};
    std::thread runner_;

    void SetUp() override {
        ops_.SetTarget = on_set_target;
        ops_.GetState = on_get_state;
        qmp_server_cfg_t cfg{qmp_transport_fake(), kThreads, workbuf_, sizeof(workbuf_)};
        ASSERT_EQ(qmp_server_init(&server_, &cfg), EOK);
        ASSERT_EQ(demo_joint_v1_Joint_register(&server_, 0, &ops_, &model_), EOK);
        runner_ = std::thread([this] { EXPECT_EQ(qmp_server_run(&server_), EOK); });
    }

    void TearDown() override {
        EXPECT_EQ(qmp_server_stop(&server_), EOK);
        runner_.join();
        EXPECT_EQ(qmp_server_close(&server_), EOK);
    }

    void connect(qmp_client_t &c, uint64_t timeout_ns = 0) {
        qmp_client_cfg_t ccfg{qmp_transport_fake(), timeout_ns};
        ASSERT_EQ(qmp_client_init(&c, &ccfg), EOK);
        ASSERT_EQ(demo_joint_v1_Joint_connect(&c, 0, &ccfg), EOK);
    }
};

TEST_F(E2E, HelloHandshakeReportsVersionAndGeneration) {
    qmp_client_t c;
    connect(c);
    EXPECT_TRUE(qmp_client_is_connected(&c));
    EXPECT_EQ(qmp_client_server_minor(&c), DEMO_JOINT_V1_JOINT_VER_MINOR);
    EXPECT_EQ(qmp_client_server_generation(&c), qmp_server_generation(&server_));
    EXPECT_EQ(qmp_client_close(&c), EOK);
    EXPECT_FALSE(qmp_client_is_connected(&c));
}

TEST_F(E2E, SyncMethodRoundTrip) {
    qmp_client_t c;
    connect(c);
    demo_joint_v1_SetTargetRequest req{1.5F, 2.0F};
    demo_joint_v1_SetTargetReply rep{};
    int32_t status = -1;
    ASSERT_EQ(demo_joint_v1_Joint_SetTarget(&c, &req, &rep, &status), EOK);
    EXPECT_EQ(status, 0);
    EXPECT_EQ(rep.result, 0);

    demo_joint_v1_JointState st{};
    ASSERT_EQ(demo_joint_v1_Joint_GetState(&c, &st, &status), EOK);
    EXPECT_EQ(status, 0);
    EXPECT_EQ(st.seq, 1u);
    EXPECT_FLOAT_EQ(st.position_rad, 1.5F);
    EXPECT_EQ(st.mode.len, 4);
    EXPECT_EQ(std::string(st.mode.data, st.mode.len), "hold");
    EXPECT_EQ(model_.calls.load(), 1);
    qmp_client_close(&c);
}

TEST_F(E2E, UpPriorityReachesHandler) {
    qmp_client_t c;
    connect(c);
    demo_joint_v1_SetTargetRequest req{0.0F, 1.0F};
    demo_joint_v1_SetTargetReply rep{};
    int32_t status = -1;
    qmp_call_opts_t opts{};
    opts.up_priority = 5;
    ASSERT_EQ(demo_joint_v1_Joint_SetTarget_ex(&c, &req, &rep, &status, &opts), EOK);
    EXPECT_EQ(rep.result, 5);
    qmp_client_close(&c);
}

TEST_F(E2E, AppErrorLeavesReplyUntouched) {
    qmp_client_t c;
    connect(c);
    demo_joint_v1_SetTargetRequest req{0.0F, 0.0F}; /* handler: INVALID_ARGUMENT */
    demo_joint_v1_SetTargetReply rep{};
    rep.result = 77;
    int32_t status = 0;
    ASSERT_EQ(demo_joint_v1_Joint_SetTarget(&c, &req, &rep, &status), EOK);
    EXPECT_EQ(status, 3);
    EXPECT_EQ(rep.result, 77);

    model_.next_status = 14; /* UNAVAILABLE, with garbage in out */
    req.max_velocity = 1.0F;
    ASSERT_EQ(demo_joint_v1_Joint_SetTarget(&c, &req, &rep, &status), EOK);
    EXPECT_EQ(status, 14);
    EXPECT_EQ(rep.result, 77);
    qmp_client_close(&c);
}

TEST_F(E2E, HandlerErrnoBecomesTransportError) {
    qmp_client_t c;
    connect(c);
    model_.next_errno = EACCES;
    demo_joint_v1_SetTargetRequest req{0.0F, 1.0F};
    demo_joint_v1_SetTargetReply rep{};
    int32_t status = 0;
    EXPECT_EQ(demo_joint_v1_Joint_SetTarget(&c, &req, &rep, &status), EACCES);
    qmp_client_close(&c);
}

TEST_F(E2E, UnknownMethodAndOnewayPlaceholder) {
    qmp_client_t c;
    connect(c);
    int32_t status = 0;
    EXPECT_EQ(qmp_client_call(&c, 0x0042, nullptr, 0, nullptr, 0, &status, nullptr), ENOENT);
    EXPECT_EQ(
        qmp_client_call(&c, DEMO_JOINT_V1_JOINT_M_STOP, nullptr, 0, nullptr, 0, &status, nullptr),
        ENOSYS);
    EXPECT_EQ(qmp_client_call(&c, DEMO_JOINT_V1_JOINT_T_STATECHANGED, nullptr, 0, nullptr, 0,
                              &status, nullptr),
              ENOENT);
    qmp_client_close(&c);
}

TEST_F(E2E, WrongPayloadSizeIsEINVAL) {
    qmp_client_t c;
    connect(c);
    int32_t status = 0;
    demo_joint_v1_SetTargetReply rep{};
    uint8_t tiny[4] = {0};
    EXPECT_EQ(qmp_client_call(&c, DEMO_JOINT_V1_JOINT_M_SETTARGET, tiny, sizeof(tiny), &rep,
                              sizeof(rep), &status, nullptr),
              EINVAL);
    qmp_client_close(&c);
}

TEST_F(E2E, OutOfRangeStringOnTheWireIsERANGE) {
    /* SetTargetRequest has no bounded fields, so drive the validator through a hand-built
     * table: register a second server whose only method takes JointState. */
    static int (*const take_state)(void *, qmp_req_t *, const demo_joint_v1_JointState *,
                                   demo_joint_v1_SetTargetReply *, int32_t *) =
        [](void *, qmp_req_t *, const demo_joint_v1_JointState *, demo_joint_v1_SetTargetReply *out,
           int32_t *) {
            out->result = 1;
            return QMP_HANDLED;
        };
    struct Ops {
        decltype(take_state) f;
    } ops{take_state};
    static const auto tramp = [](const void *o, void *ctx, qmp_req_t *r, const void *in, void *out,
                                 int32_t *st) {
        return static_cast<const Ops *>(o)->f(ctx, r,
                                              static_cast<const demo_joint_v1_JointState *>(in),
                                              static_cast<demo_joint_v1_SetTargetReply *>(out), st);
    };
    static const qmp_method_desc_t methods[] = {
        {1, 0, sizeof(demo_joint_v1_JointState), sizeof(demo_joint_v1_SetTargetReply), 0,
         demo_joint_v1_JointState_validate, tramp, "TakeState"}};
    static const qmp_service_desc_t desc = {0x0202,         1,       0, QMP_HDR_SIZE + 48,
                                            "test.Bounded", methods, 1};
    alignas(8) unsigned char wb[QMP_SERVER_WORKBUF_SIZE(72, 1)];
    qmp_server_t s2;
    qmp_server_cfg_t cfg{qmp_transport_fake(), 1, wb, sizeof(wb)};
    ASSERT_EQ(qmp_server_init(&s2, &cfg), EOK);
    ASSERT_EQ(qmp_server_register(&s2, &desc, 0, &ops, nullptr), EOK);
    std::thread t([&] { qmp_server_run(&s2); });

    qmp_client_t c;
    qmp_client_cfg_t ccfg{qmp_transport_fake(), 0};
    ASSERT_EQ(qmp_client_init(&c, &ccfg), EOK);
    ASSERT_EQ(qmp_client_connect(&c, 0x0202, 0, 1, 0), EOK);
    demo_joint_v1_JointState st{};
    demo_joint_v1_SetTargetReply rep{};
    int32_t status = 0;
    st.mode.len = 16;
    EXPECT_EQ(qmp_client_call(&c, 1, &st, sizeof(st), &rep, sizeof(rep), &status, nullptr), EOK);
    EXPECT_EQ(rep.result, 1);
    st.mode.len = 17;
    EXPECT_EQ(qmp_client_call(&c, 1, &st, sizeof(st), &rep, sizeof(rep), &status, nullptr), ERANGE);
    qmp_client_close(&c);
    qmp_server_stop(&s2);
    t.join();
    qmp_server_close(&s2);
}

TEST_F(E2E, MajorMismatchAtHelloIsENOTSUP) {
    qmp_client_t c;
    qmp_client_cfg_t ccfg{qmp_transport_fake(), 0};
    ASSERT_EQ(qmp_client_init(&c, &ccfg), EOK);
    /* The name encodes the major, so a v2 client never finds a v1 server: ENOENT. */
    EXPECT_EQ(qmp_client_connect(&c, DEMO_JOINT_V1_JOINT_SERVICE_ID, 0, 2, 0), ENOENT);
    /* Same name, lying HELLO payload: the server checks it and answers ENOTSUP. */
    ASSERT_EQ(qmp_client_connect(&c, DEMO_JOINT_V1_JOINT_SERVICE_ID, 0, 1, 0), EOK);
    qmp_hdr_t req_hdr;
    qmp_hdr_t rep_hdr;
    qmp_hello_t hello{DEMO_JOINT_V1_JOINT_SERVICE_ID, 0, 2, 0, {0, 0}};
    qmp_hello_reply_t hr{};
    qmp_hdr_init(&req_hdr, DEMO_JOINT_V1_JOINT_SERVICE_ID, 0, QMP_OP_HELLO, 2, 0, QMP_F_CONTROL,
                 sizeof(hello));
    qmp_iov_t siov[2] = {{&req_hdr, sizeof(req_hdr)}, {&hello, sizeof(hello)}};
    qmp_iov_t riov[2] = {{&rep_hdr, sizeof(rep_hdr)}, {&hr, sizeof(hr)}};
    size_t len = 0;
    qmp_conn_t conn;
    ASSERT_EQ(qmp_transport_fake()->connect(&conn, DEMO_JOINT_V1_JOINT_QNX_PATH), EOK);
    EXPECT_EQ(qmp_transport_fake()->call(&conn, siov, 2, riov, 2, 0, &len), ENOTSUP);
    qmp_client_close(&c);
}

TEST_F(E2E, MinorSkewIsAccepted) {
    qmp_client_t c;
    qmp_client_cfg_t ccfg{qmp_transport_fake(), 0};
    ASSERT_EQ(qmp_client_init(&c, &ccfg), EOK);
    ASSERT_EQ(qmp_client_connect(&c, DEMO_JOINT_V1_JOINT_SERVICE_ID, 0, 1, 7), EOK);
    EXPECT_EQ(qmp_client_server_minor(&c), 0);
    qmp_client_close(&c);
}

TEST_F(E2E, ConcurrentClientsAcrossWorkers) {
    constexpr int kClients = 4;
    constexpr int kIter = 200;
    std::vector<std::thread> ts;
    std::atomic<int> failures{0};
    for (int i = 0; i < kClients; i++) {
        ts.emplace_back([&, i] {
            qmp_client_t c;
            qmp_client_cfg_t ccfg{qmp_transport_fake(), 0};
            if (qmp_client_init(&c, &ccfg) != EOK ||
                demo_joint_v1_Joint_connect(&c, 0, &ccfg) != EOK) {
                failures++;
                return;
            }
            for (int k = 0; k < kIter; k++) {
                demo_joint_v1_SetTargetRequest req{static_cast<float>(i), 1.0F};
                demo_joint_v1_SetTargetReply rep{};
                int32_t status = -1;
                if (demo_joint_v1_Joint_SetTarget(&c, &req, &rep, &status) != EOK || status != 0) {
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
    EXPECT_EQ(model_.calls.load(), kClients * kIter);
}

TEST_F(E2E, ServerStopUnblocksAndClientSeesESRCH) {
    qmp_client_t c;
    connect(c);
    EXPECT_EQ(qmp_server_stop(&server_), EOK);
    runner_.join();
    EXPECT_EQ(qmp_server_close(&server_), EOK);
    demo_joint_v1_JointState st{};
    int32_t status = 0;
    EXPECT_EQ(demo_joint_v1_Joint_GetState(&c, &st, &status), ESRCH);
    qmp_client_close(&c);
    /* restart so TearDown has something to stop */
    qmp_server_cfg_t cfg{qmp_transport_fake(), kThreads, workbuf_, sizeof(workbuf_)};
    ASSERT_EQ(qmp_server_init(&server_, &cfg), EOK);
    ASSERT_EQ(demo_joint_v1_Joint_register(&server_, 0, &ops_, &model_), EOK);
    runner_ = std::thread([this] { qmp_server_run(&server_); });
}

TEST(E2ENoServer, ConnectToMissingServiceIsENOENT) {
    qmp_client_t c;
    qmp_client_cfg_t ccfg{qmp_transport_fake(), 0};
    ASSERT_EQ(qmp_client_init(&c, &ccfg), EOK);
    EXPECT_EQ(qmp_client_connect(&c, 0x0303, 0, 1, 0), ENOENT);
    int32_t status = 0;
    EXPECT_EQ(qmp_client_call(&c, 1, nullptr, 0, nullptr, 0, &status, nullptr), ENOTCONN);
}

TEST(E2ENoServer, TimeoutWhenNobodyReceives) {
    /* Listen but never run the loop: the fake has the request PENDING forever. */
    alignas(8) unsigned char wb[QMP_SERVER_WORKBUF_SIZE(72, 1)];
    qmp_server_t s;
    qmp_server_cfg_t cfg{qmp_transport_fake(), 1, wb, sizeof(wb)};
    demo_joint_v1_Joint_ops ops{};
    ASSERT_EQ(qmp_server_init(&s, &cfg), EOK);
    ASSERT_EQ(demo_joint_v1_Joint_register(&s, 0, &ops, nullptr), EOK);
    qmp_client_t c;
    qmp_client_cfg_t ccfg{qmp_transport_fake(), 20ull * 1000 * 1000};
    ASSERT_EQ(qmp_client_init(&c, &ccfg), EOK);
    auto t0 = std::chrono::steady_clock::now();
    EXPECT_EQ(demo_joint_v1_Joint_connect(&c, 0, &ccfg), ETIMEDOUT);
    auto dt = std::chrono::steady_clock::now() - t0;
    EXPECT_GE(dt, std::chrono::milliseconds(15));
    EXPECT_EQ(qmp_server_close(&s), EOK);
}

} // namespace
