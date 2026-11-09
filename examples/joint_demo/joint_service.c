/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Joint actuator service (C). Codelab chapter 01.
 *
 * Handlers run on a libqmp worker thread at the caller's priority. No heap, no blocking
 * I/O after init.
 */
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "joint.qmp.h"

typedef struct joint_model {
    float target_rad;
    float position_rad;
    uint64_t seq;
} joint_model_t;

static int on_set_target(void *ctx, qmp_req_t *req, const demo_joint_v1_SetTargetRequest *in,
                         demo_joint_v1_SetTargetReply *out, int32_t *status) {
    joint_model_t *m = (joint_model_t *)ctx;
    (void)req;
    if (in->max_velocity <= 0.0f) {
        *status = QMP_UCODE_INVALID_ARGUMENT;
        return QMP_HANDLED;
    }
    m->target_rad = in->position_rad;
    m->position_rad = in->position_rad; /* ideal actuator: arrives instantly */
    m->seq++;
    out->result = 0;
    return QMP_HANDLED;
}

static int on_get_state(void *ctx, qmp_req_t *req, demo_joint_v1_JointState *out, int32_t *status) {
    joint_model_t *m = (joint_model_t *)ctx;
    (void)req;
    (void)status;
    out->seq = m->seq;
    out->timestamp_ns = 0; /* ClockCycles() on target (W2) */
    out->position_rad = m->position_rad;
    out->velocity = 0.0f;
    static const char mode[] = "hold";
    out->mode.len = (uint16_t)(sizeof(mode) - 1);
    memcpy(out->mode.data, mode, sizeof(mode) - 1);
    return QMP_HANDLED;
}

#define NTHREADS 2
static uint64_t g_workbuf[QMP_SERVER_WORKBUF_SIZE(DEMO_JOINT_V1_JOINT_MAX_MSG_SIZE, NTHREADS) / 8];

#if !defined(__QNX__)
static void *selftest_client(void *arg) {
    qmp_server_t *server = (qmp_server_t *)arg;
    qmp_client_t c;
    int rc = qmp_client_init(&c, NULL);
    if (rc == EOK) {
        rc = demo_joint_v1_Joint_connect(&c, 0, NULL);
    }
    demo_joint_v1_SetTargetRequest req = {0.5f, 1.0f};
    demo_joint_v1_SetTargetReply rep;
    demo_joint_v1_JointState st;
    int32_t status = 0;
    if (rc == EOK) {
        rc = demo_joint_v1_Joint_SetTarget(&c, &req, &rep, &status);
    }
    if (rc == EOK && status == 0) {
        rc = demo_joint_v1_Joint_GetState(&c, &st, &status);
    }
    if (rc == EOK && status == 0) {
        printf("selftest: seq=%llu position=%.2f mode=%.*s\n", (unsigned long long)st.seq,
               (double)st.position_rad, (int)st.mode.len, st.mode.data);
    } else {
        fprintf(stderr, "selftest failed: rc=%d status=%d\n", rc, status);
    }
    (void)qmp_client_close(&c);
    (void)qmp_server_stop(server);
    return (void *)(intptr_t)(rc == EOK && status == 0 ? 0 : 1);
}
#endif

int main(int argc, char **argv) {
    joint_model_t model = {0.0f, 0.0f, 0};
    demo_joint_v1_Joint_ops ops;
    memset(&ops, 0, sizeof(ops));
    ops.SetTarget = on_set_target;
    ops.GetState = on_get_state;

    qmp_server_t server;
    qmp_server_cfg_t cfg = {NULL, NTHREADS, g_workbuf, sizeof(g_workbuf)};
    int rc = qmp_server_init(&server, &cfg);
    if (rc == EOK) {
        rc = demo_joint_v1_Joint_register(&server, 0, &ops, &model);
    }
    if (rc != EOK) {
        fprintf(stderr, "joint_service: register failed: %s\n", strerror(rc));
        return 1;
    }
    printf("joint_service: %s listening on %s\n", DEMO_JOINT_V1_JOINT_SERVICE_NAME,
           DEMO_JOINT_V1_JOINT_QNX_PATH);

    int result = 0;
#if !defined(__QNX__)
    pthread_t t;
    int selftest = argc > 1 && strcmp(argv[1], "--selftest") == 0;
    if (selftest && pthread_create(&t, NULL, selftest_client, &server) != 0) {
        return 1;
    }
#else
    (void)argc;
    (void)argv;
#endif
    rc = qmp_server_run(&server);
#if !defined(__QNX__)
    if (selftest) {
        void *ret = NULL;
        (void)pthread_join(t, &ret);
        result = (int)(intptr_t)ret;
    }
#endif
    (void)qmp_server_close(&server);
    return rc == EOK ? result : 1;
}
