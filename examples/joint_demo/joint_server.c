/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Standalone Joint server (evolved from the W1 joint_service self-test).
 *
 * Registers one instance of demo.joint.v1.Joint over the default transport (native QNX
 * message passing on target) and serves until SIGTERM/SIGINT. Implements SetTarget and
 * GetState trivially plus Burn, a slow method that keeps the handler busy (sleeping or
 * CPU-spinning) so real kernel behaviour — REPLY-blocked clients, priority inheritance —
 * can be observed with pidin. Companion client: joint_client.c; orchestration:
 * scripts/target-smoke.sh.
 *
 * Usage: qmp_joint_server [--threads N] [--verbose] [--fake] [--fixed-priority]
 *                         [--selftest]
 *   --threads N        worker threads, 1..QMP_SERVER_MAX_THREADS (default 2)
 *   --verbose          log each call and reply (off by default: no fast-path logging)
 *   --fake             allow the in-process fake transport (host); otherwise the server
 *                      refuses to start when the default transport is not "qnx"
 *   --fixed-priority   requested negative control for priority inheritance. NOT
 *                      SUPPORTED: the qmp QNX transport always creates the channel with
 *                      priority inheritance (never _NTO_CHF_FIXED_PRIORITY) and exposes
 *                      no channel-flags knob. The server reports this and exits 2.
 *   --selftest         host only: run an in-process client once and exit (ctest)
 *
 * Handlers run on a libqmp worker at the caller's inherited priority; per-call logging is
 * only enabled with --verbose and uses one fprintf per event.
 */
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "joint.qmp.h"

#define MAX_BURN_MS 600000 /* cap Burn at 10 minutes */

static uint64_t now_ns(void) {
    struct timespec ts;
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* One line per event on stderr, prefixed with the monotonic clock in seconds. */
static void log_event(const char *fmt, ...) {
    uint64_t t = now_ns();
    fprintf(stderr, "[%" PRIu64 ".%06" PRIu64 "] joint_server: ", (uint64_t)(t / 1000000000ULL),
            (uint64_t)((t % 1000000000ULL) / 1000ULL));
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}
#define LOGF log_event

typedef struct srv_state {
    float target_rad;
    float position_rad;
    uint64_t seq;
    int verbose;
} srv_state_t;

static int thread_priority(void) {
    struct sched_param sp;
    int policy = 0;
    /* VERIFY(qnx8): V-QNX-036 — does pthread_getschedparam report the boosted (inherited)
     * priority while handling a message, or only the assigned one? pidin is the ground
     * truth in scripts/target-smoke.sh scenario 5. */
    if (pthread_getschedparam(pthread_self(), &policy, &sp) != 0) {
        return -1;
    }
    return sp.sched_priority;
}

static void log_call(const srv_state_t *sv, const char *method, qmp_req_t *req) {
    if (!sv->verbose) {
        return;
    }
    qmp_req_info_t info;
    memset(&info, 0, sizeof(info));
    (void)qmp_req_get_info(req, &info);
    LOGF("recv %s client_pid=%" PRId32 " caller_prio=%" PRId32 " thread_prio=%d", method, info.pid,
         info.priority, thread_priority());
}

static void log_reply(const srv_state_t *sv, const char *method, int32_t status) {
    if (sv->verbose) {
        LOGF("reply %s status=%" PRId32, method, status);
    }
}

static int on_set_target(void *ctx, qmp_req_t *req, const demo_joint_v1_SetTargetRequest *in,
                         demo_joint_v1_SetTargetReply *out, int32_t *status) {
    srv_state_t *sv = (srv_state_t *)ctx;
    log_call(sv, "SetTarget", req);
    if (in->max_velocity <= 0.0f) {
        *status = QMP_UCODE_INVALID_ARGUMENT;
    } else {
        sv->target_rad = in->position_rad;
        sv->position_rad = in->position_rad; /* ideal actuator: arrives instantly */
        sv->seq++;
        out->result = 0;
        *status = 0;
    }
    log_reply(sv, "SetTarget", *status);
    return QMP_HANDLED;
}

static int on_get_state(void *ctx, qmp_req_t *req, demo_joint_v1_JointState *out, int32_t *status) {
    srv_state_t *sv = (srv_state_t *)ctx;
    log_call(sv, "GetState", req);
    out->seq = sv->seq;
    out->timestamp_ns = now_ns();
    out->position_rad = sv->position_rad;
    out->velocity = 0.0f;
    static const char mode[] = "hold";
    out->mode.len = (uint16_t)(sizeof(mode) - 1);
    memcpy(out->mode.data, mode, sizeof(mode) - 1);
    *status = 0;
    log_reply(sv, "GetState", *status);
    return QMP_HANDLED;
}

static int on_burn(void *ctx, qmp_req_t *req, const demo_joint_v1_BurnRequest *in,
                   demo_joint_v1_BurnReply *out, int32_t *status) {
    srv_state_t *sv = (srv_state_t *)ctx;
    log_call(sv, in->spin ? "Burn(spin)" : "Burn(sleep)", req);
    if (in->duration_ms > MAX_BURN_MS) {
        *status = QMP_UCODE_INVALID_ARGUMENT;
        log_reply(sv, "Burn", *status);
        return QMP_HANDLED;
    }
    uint64_t iterations = 0;
    if (in->spin) {
        /* Busy-burn the CPU: this is what makes priority inheritance observable. */
        const uint64_t deadline = now_ns() + (uint64_t)in->duration_ms * 1000000ULL;
        volatile uint32_t sink = 0;
        while (now_ns() < deadline) {
            for (int i = 0; i < 10000; i++) {
                sink += (uint32_t)i;
            }
            iterations++;
        }
    } else {
        struct timespec ts = {in->duration_ms / 1000, (long)(in->duration_ms % 1000) * 1000000L};
        (void)nanosleep(&ts, NULL);
    }
    out->iterations = iterations;
    *status = 0;
    if (sv->verbose) {
        LOGF("burn done duration_ms=%" PRIu32 " spin=%u iterations=%" PRIu64 " thread_prio=%d",
             in->duration_ms, (unsigned)in->spin, iterations, thread_priority());
    }
    log_reply(sv, "Burn", *status);
    return QMP_HANDLED;
}

static void on_client_gone(void *ctx, int32_t pid) {
    (void)ctx;
    LOGF("client disconnected pid=%" PRId32, pid);
}

/* ---- shutdown: a dedicated thread sigwait()s for SIGINT/SIGTERM ---------------------- */

typedef struct sig_ctx {
    qmp_server_t *server;
    sigset_t set;
} sig_ctx_t;

static void *signal_thread(void *arg) {
    sig_ctx_t *sc = (sig_ctx_t *)arg;
    int sig = 0;
    if (sigwait(&sc->set, &sig) == 0) {
        LOGF("shutdown signal=%d", sig);
        (void)qmp_server_stop(sc->server);
    }
    return NULL;
}

/* ---- host self-test (fake transport, in-process client) ------------------------------ */

#if !defined(__QNX__)
static void *selftest_client(void *arg) {
    qmp_server_t *server = (qmp_server_t *)arg;
    qmp_client_t c;
    int32_t status = 0;
    int rc = demo_joint_v1_Joint_connect(&c, 0, NULL);
    demo_joint_v1_SetTargetRequest req = {0.5f, 1.0f};
    demo_joint_v1_SetTargetReply rep;
    demo_joint_v1_JointState st;
    demo_joint_v1_BurnRequest breq = {10, 1, {0}};
    demo_joint_v1_BurnReply brep;
    if (rc == EOK) {
        rc = demo_joint_v1_Joint_SetTarget(&c, &req, &rep, &status);
    }
    if (rc == EOK && status == 0) {
        rc = demo_joint_v1_Joint_GetState(&c, &st, &status);
    }
    if (rc == EOK && status == 0) {
        rc = demo_joint_v1_Joint_Burn(&c, &breq, &brep, &status);
    }
    if (rc == EOK && status == 0) {
        printf("selftest: seq=%" PRIu64 " position=%.2f mode=%.*s burn_iters=%" PRIu64 "\n", st.seq,
               (double)st.position_rad, (int)st.mode.len, st.mode.data, brep.iterations);
    } else {
        fprintf(stderr, "selftest failed: rc=%d status=%" PRId32 "\n", rc, status);
    }
    (void)qmp_client_close(&c);
    (void)qmp_server_stop(server);
    return (void *)(intptr_t)(rc == EOK && status == 0 ? 0 : 1);
}
#endif

/* ---- main ----------------------------------------------------------------------------- */

static uint64_t
    g_workbuf[QMP_SERVER_WORKBUF_SIZE(DEMO_JOINT_V1_JOINT_MAX_MSG_SIZE, QMP_SERVER_MAX_THREADS) /
              8];

static int usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s [--threads N] [--verbose] [--fake] [--fixed-priority] [--selftest]\n",
            argv0);
    return 2;
}

int main(int argc, char **argv) {
    int nthreads = 2;
    int verbose = 0;
    int use_fake = 0;
    int selftest = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc) {
            nthreads = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--verbose") == 0) {
            verbose = 1;
        } else if (strcmp(argv[i], "--fake") == 0) {
            use_fake = 1;
        } else if (strcmp(argv[i], "--selftest") == 0) {
            selftest = 1;
            use_fake = 1; /* the host default transport is the fake */
        } else if (strcmp(argv[i], "--fixed-priority") == 0) {
            fprintf(stderr,
                    "joint_server: --fixed-priority is not supported: the qmp QNX transport\n"
                    "always creates the channel with priority inheritance (never\n"
                    "_NTO_CHF_FIXED_PRIORITY) and has no channel-flags option. A runtime\n"
                    "change is required for this negative control.\n");
            return 2;
        } else {
            return usage(argv[0]);
        }
    }
    if (nthreads < 1 || nthreads > QMP_SERVER_MAX_THREADS) {
        fprintf(stderr, "joint_server: --threads must be 1..%d\n", QMP_SERVER_MAX_THREADS);
        return 2;
    }

    const qmp_transport_t *tr = use_fake ? qmp_transport_fake() : qmp_transport_default();
    if (!use_fake && strcmp(tr->name, "qnx") != 0) {
        fprintf(stderr,
                "joint_server: default transport is \"%s\", not the native QNX transport;\n"
                "refusing to start (pass --fake to use the in-process fake on the host)\n",
                tr->name);
        return 2;
    }

    /* Block SIGINT/SIGTERM before any thread exists so only signal_thread sees them. */
    static sig_ctx_t sc;
    sigemptyset(&sc.set);
    sigaddset(&sc.set, SIGINT);
    sigaddset(&sc.set, SIGTERM);
    (void)pthread_sigmask(SIG_BLOCK, &sc.set, NULL);

    srv_state_t state = {0.0f, 0.0f, 0, verbose};
    demo_joint_v1_Joint_ops ops;
    memset(&ops, 0, sizeof(ops));
    ops.SetTarget = on_set_target;
    ops.GetState = on_get_state;
    ops.Burn = on_burn;

    qmp_server_t server;
    qmp_server_cfg_t cfg = {.transport = tr,
                            .nthreads = nthreads,
                            .workbuf = g_workbuf,
                            .workbuf_size = sizeof(g_workbuf),
                            .on_client_gone = on_client_gone};
    int rc = qmp_server_init(&server, &cfg);
    if (rc == EOK) {
        rc = demo_joint_v1_Joint_register(&server, 0, &ops, &state);
    }
    if (rc != EOK) {
        fprintf(stderr, "joint_server: register failed: %d (%s)\n", rc, strerror(rc));
        return 1;
    }

    sc.server = &server;
    pthread_t sig_thread;
    if (pthread_create(&sig_thread, NULL, signal_thread, &sc) != 0) {
        fprintf(stderr, "joint_server: cannot create signal thread\n");
        return 1;
    }

    LOGF("started pid=%d service=%s path=%s transport=%s threads=%d generation=%" PRIu32,
         (int)getpid(), DEMO_JOINT_V1_JOINT_SERVICE_NAME, DEMO_JOINT_V1_JOINT_QNX_PATH, tr->name,
         nthreads, qmp_server_generation(&server));

    int result = 0;
#if !defined(__QNX__)
    pthread_t st_thread;
    if (selftest && pthread_create(&st_thread, NULL, selftest_client, &server) != 0) {
        return 1;
    }
#else
    if (selftest) {
        fprintf(stderr, "joint_server: --selftest is host-only\n");
        return 2;
    }
#endif
    rc = qmp_server_run(&server); /* until a signal (or the selftest) stops it */
#if !defined(__QNX__)
    if (selftest) {
        void *ret = NULL;
        (void)pthread_join(st_thread, &ret);
        result = (int)(intptr_t)ret;
        /* Unblock the signal thread so the process can exit. */
        (void)pthread_kill(sig_thread, SIGTERM);
    }
#endif
    (void)pthread_join(sig_thread, NULL);
    (void)qmp_server_close(&server);
    LOGF("stopped");
    return rc == EOK ? result : 1;
}
