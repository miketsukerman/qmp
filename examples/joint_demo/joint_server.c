/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Standalone Joint server (evolved from the W1 joint_service self-test).
 *
 * Registers one instance of demo.joint.v1.Joint over the default transport (native QNX
 * message passing on target) and serves until SIGTERM/SIGINT. Implements SetTarget and
 * GetState trivially, the one-way Stop (mode becomes "stopped" until the next SetTarget;
 * its handler runs after the caller was released) plus Burn, a slow method that keeps the handler
 * busy (sleeping or CPU-spinning) so real kernel behaviour — REPLY-blocked clients, priority
 * inheritance — can be observed with pidin. Companion client: joint_client.c; orchestration:
 * scripts/target-smoke.sh.
 *
 * Usage: qmp_joint_server [--threads N] [--verbose] [--transport NAME] [--fixed-priority]
 *                         [--publish-ms N] [--selftest]
 *   --threads N        worker threads, 1..QMP_SERVER_MAX_THREADS (default 2)
 *   --publish-ms N     publish the StateChanged (LATEST) and StateLog (QUEUE depth 8)
 *                      topics every N ms (1..60000; off by default; 2 = 500 Hz, paced by
 *                      absolute deadlines so the average rate is exact). Subscribers are
 *                      notified with MsgDeliverEvent and pull the sample (ADR-003); a
 *                      slow or dead subscriber never blocks this.
 *   --verbose          log each call and reply (off by default: no fast-path logging)
 *   --transport NAME   qnx | uds | fake (default: qnx on QNX, uds on the host). uds is
 *                      the AF_UNIX bench/test transport (ADR-004): it works across
 *                      processes on the host and on QNX but has NO priority inheritance.
 *                      fake is in-process only, so it is refused except for --selftest.
 *   --fake             same as --transport fake
 *   --fixed-priority   negative control for priority inheritance: the channel is created
 *                      with _NTO_CHF_FIXED_PRIORITY (QMP_LISTEN_FIXED_PRIORITY), so
 *                      handler threads keep their own priority instead of inheriting the
 *                      caller's (V-QNX-041). Never use this outside the experiment.
 *   --selftest         host only: run an in-process client once and exit (ctest)
 *
 * Handlers run on a libqmp worker at the caller's inherited priority; per-call logging is
 * only enabled with --verbose. Each log line is written with a single write() so lines
 * from different threads cannot interleave.
 */
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "joint.qmp.h"

#define MAX_BURN_MS  600000 /* cap Burn at 10 minutes                                   */
#define BURN_POLL_MS 50     /* how often a running Burn polls qmp_req_canceled()        */

static uint64_t now_ns(void) {
    struct timespec ts;
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* One line per event on stderr, prefixed with the monotonic clock in seconds. The line is
 * assembled in one buffer and written with a single write() so concurrent threads cannot
 * interleave their output. */
static void log_event(const char *fmt, ...) {
    char buf[256];
    uint64_t t = now_ns();
    int n = snprintf(buf, sizeof(buf),
                     "[%" PRIu64 ".%06" PRIu64 "] joint_server: ", (uint64_t)(t / 1000000000ULL),
                     (uint64_t)((t % 1000000000ULL) / 1000ULL));
    if (n < 0) {
        return;
    }
    size_t off = (size_t)n < sizeof(buf) - 1 ? (size_t)n : sizeof(buf) - 1;
    va_list ap;
    va_start(ap, fmt);
    int m = vsnprintf(buf + off, sizeof(buf) - off, fmt, ap);
    va_end(ap);
    if (m > 0) {
        off += (size_t)m < sizeof(buf) - off ? (size_t)m : sizeof(buf) - 1 - off;
    }
    buf[off++] = '\n';
    /* Best effort; a (void) cast does not silence glibc's warn_unused_result. */
    ssize_t written = write(STDERR_FILENO, buf, off);
    (void)written;
}
#define LOGF log_event

typedef struct srv_state {
    float target_rad;
    float position_rad;
    uint64_t seq;
    int verbose;
    int stopped; /* set by the one-way Stop, cleared by SetTarget */
    /* Handlers run on several workers and the publisher is a thread of its own: every
     * access to the fields above takes this lock. It is never held across a reply or a
     * publish. */
    pthread_mutex_t mu;
} srv_state_t;

/* Caller holds sv->mu. */
static void fill_mode(const srv_state_t *sv, demo_joint_v1_JointState *st) {
    const char *mode = sv->stopped ? "stopped" : "hold";
    st->mode.len = (uint16_t)strlen(mode);
    memcpy(st->mode.data, mode, st->mode.len);
}

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
        (void)pthread_mutex_lock(&sv->mu);
        sv->target_rad = in->position_rad;
        sv->position_rad = in->position_rad; /* ideal actuator: arrives instantly */
        sv->stopped = 0;
        sv->seq++;
        (void)pthread_mutex_unlock(&sv->mu);
        out->result = 0;
        *status = 0;
    }
    log_reply(sv, "SetTarget", *status);
    return QMP_HANDLED;
}

static int on_get_state(void *ctx, qmp_req_t *req, demo_joint_v1_JointState *out, int32_t *status) {
    srv_state_t *sv = (srv_state_t *)ctx;
    log_call(sv, "GetState", req);
    (void)pthread_mutex_lock(&sv->mu);
    out->seq = sv->seq;
    out->timestamp_ns = now_ns();
    out->position_rad = sv->position_rad;
    out->velocity = 0.0f;
    fill_mode(sv, out);
    (void)pthread_mutex_unlock(&sv->mu);
    *status = 0;
    log_reply(sv, "GetState", *status);
    return QMP_HANDLED;
}

/* One-way (DESIGN §3.4): the client was released before this runs, so the worker is back
 * at its base priority and there is nothing to reply. Logged unconditionally: a stop is a
 * state change, not fast-path traffic. */
static int on_stop(void *ctx, const qmp_req_info_t *info) {
    srv_state_t *sv = (srv_state_t *)ctx;
    (void)pthread_mutex_lock(&sv->mu);
    sv->target_rad = sv->position_rad;
    sv->stopped = 1;
    (void)pthread_mutex_unlock(&sv->mu);
    LOGF("recv Stop (one-way) client_pid=%" PRId32 " caller_prio=%" PRId32 " thread_prio=%d",
         info->pid, info->priority, thread_priority());
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
    int canceled = 0;
    const uint64_t t0 = now_ns();
    const uint64_t deadline = t0 + (uint64_t)in->duration_ms * 1000000ULL;
    if (in->spin) {
        /* Busy-burn the CPU: this is what makes priority inheritance observable. Poll
         * qmp_req_canceled() about every BURN_POLL_MS so a client timeout, signal or
         * disconnect aborts the burn instead of wasting the rest of the budget. */
        volatile uint32_t sink = 0;
        uint64_t next_poll = t0 + (uint64_t)BURN_POLL_MS * 1000000ULL;
        while (now_ns() < deadline) {
            for (int i = 0; i < 10000; i++) {
                sink += (uint32_t)i;
            }
            iterations++;
            if (now_ns() >= next_poll) {
                if (qmp_req_canceled(req)) {
                    canceled = 1;
                    break;
                }
                next_poll += (uint64_t)BURN_POLL_MS * 1000000ULL;
            }
        }
    } else {
        /* Sleep in BURN_POLL_MS chunks so cancellation is noticed at the same rate. */
        while (now_ns() < deadline) {
            uint64_t left_ns = deadline - now_ns();
            uint64_t chunk_ns = (uint64_t)BURN_POLL_MS * 1000000ULL;
            if (chunk_ns > left_ns) {
                chunk_ns = left_ns;
            }
            struct timespec ts = {(time_t)(chunk_ns / 1000000000ULL),
                                  (long)(chunk_ns % 1000000000ULL)};
            (void)nanosleep(&ts, NULL);
            if (qmp_req_canceled(req)) {
                canceled = 1;
                break;
            }
        }
    }
    if (canceled) {
        /* The runtime already cancelled the rcvid; our result is discarded, no reply. */
        LOGF("burn canceled after %" PRIu64 " of %" PRIu32 " ms spin=%u iterations=%" PRIu64,
             (now_ns() - t0) / 1000000ULL, in->duration_ms, (unsigned)in->spin, iterations);
        return QMP_HANDLED;
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

/* ---- topic publisher: StateChanged every --publish-ms milliseconds ------------------- */

typedef struct pub_ctx {
    qmp_server_t *server;
    srv_state_t *state;
    uint32_t period_ms;
    atomic_int stop;
} pub_ctx_t;

static void *publisher_thread(void *arg) {
    pub_ctx_t *pc = (pub_ctx_t *)arg;
    /* Absolute deadlines: a relative sleep of N ms is rounded up by the timer tick on top of
     * the time the publish took, which turned --publish-ms 2 into one update every 3 ms
     * (333 Hz instead of 500 Hz) on the RPi5. Sleeping until t0 + k * period keeps the
     * average rate exact; a tick of jitter on single updates remains. */
    const uint64_t period_ns = (uint64_t)pc->period_ms * 1000000ULL;
    uint64_t next = now_ns();
    while (!atomic_load(&pc->stop)) {
        demo_joint_v1_JointState st;
        memset(&st, 0, sizeof(st));
        (void)pthread_mutex_lock(&pc->state->mu);
        st.seq = ++pc->state->seq;
        st.timestamp_ns = now_ns();
        st.position_rad = pc->state->position_rad;
        st.velocity = 0.0f;
        fill_mode(pc->state, &st);
        (void)pthread_mutex_unlock(&pc->state->mu);
        int rc = demo_joint_v1_Joint_publish_StateChanged(pc->server, &st);
        if (rc == EOK) {
            rc = demo_joint_v1_Joint_publish_StateLog(pc->server, &st);
        }
        if (rc != EOK) {
            LOGF("publish failed rc=%d (%s); publisher stops", rc, strerror(rc));
            return NULL;
        }
        if (pc->state->verbose) {
            LOGF("published seq=%" PRIu64, st.seq);
        }
        next += period_ns;
        uint64_t now = now_ns();
        if (now > next + period_ns) {
            next = now; /* fell behind (preempted for long): do not burst to catch up */
        }
        struct timespec until = {(time_t)(next / 1000000000ULL), (long)(next % 1000000000ULL)};
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &until, NULL) == EINTR) {
        }
    }
    return NULL;
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

/* ---- host self-test (in-process client on the server's transport; fake by default) --- */

#if !defined(__QNX__)
static const qmp_transport_t *g_selftest_tr; /* set once in main before the thread starts */

static void *selftest_client(void *arg) {
    qmp_server_t *server = (qmp_server_t *)arg;
    qmp_client_t c;
    int32_t status = 0;
    qmp_client_cfg_t ccfg = {.transport = g_selftest_tr};
    int rc = demo_joint_v1_Joint_connect(&c, 0, &ccfg);
    demo_joint_v1_SetTargetRequest req = {0.5f, 1.0f};
    demo_joint_v1_SetTargetReply rep;
    demo_joint_v1_JointState st;
    demo_joint_v1_BurnRequest breq = {10, 1, {0}};
    demo_joint_v1_BurnReply brep;
    if (rc == EOK) {
        rc = demo_joint_v1_Joint_SetTarget(&c, &req, &rep, &status);
    }
    if (rc == EOK && status == 0) {
        rc = demo_joint_v1_Joint_Stop(&c);
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
static uint64_t g_topicbuf[DEMO_JOINT_V1_JOINT_TOPIC_STORAGE_SIZE / 8 + 1];

static int usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s [--threads N] [--verbose] [--transport qnx|uds|fake] [--fixed-priority] "
            "[--publish-ms N] "
            "[--selftest]\n",
            argv0);
    return 2;
}

int main(int argc, char **argv) {
    int nthreads = 2;
    int verbose = 0;
    const char *tr_name = NULL;
    int selftest = 0;
    int fixed_priority = 0;
    long publish_ms = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc) {
            nthreads = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--publish-ms") == 0 && i + 1 < argc) {
            publish_ms = atol(argv[++i]);
        } else if (strcmp(argv[i], "--verbose") == 0) {
            verbose = 1;
        } else if (strcmp(argv[i], "--transport") == 0 && i + 1 < argc) {
            tr_name = argv[++i];
        } else if (strcmp(argv[i], "--fake") == 0) {
            tr_name = "fake";
        } else if (strcmp(argv[i], "--selftest") == 0) {
            selftest = 1;
        } else if (strcmp(argv[i], "--fixed-priority") == 0) {
            fixed_priority = 1;
        } else {
            return usage(argv[0]);
        }
    }
    if (nthreads < 1 || nthreads > QMP_SERVER_MAX_THREADS) {
        fprintf(stderr, "joint_server: --threads must be 1..%d\n", QMP_SERVER_MAX_THREADS);
        return 2;
    }
    if (publish_ms < 0 || publish_ms > 60000) {
        fprintf(stderr, "joint_server: --publish-ms must be 1..60000\n");
        return 2;
    }

    if (selftest && tr_name == NULL) {
        tr_name = "fake"; /* client and server share this process */
    }
    const qmp_transport_t *tr =
        tr_name != NULL ? qmp_transport_by_name(tr_name) : qmp_transport_default();
    if (tr == NULL) {
        fprintf(stderr, "joint_server: transport \"%s\" is not available in this build\n", tr_name);
        return 2;
    }
    if (!selftest && strcmp(tr->name, "fake") == 0) {
        fprintf(stderr, "joint_server: the fake transport is in-process only; no client in\n"
                        "another process could reach this server (use --selftest)\n");
        return 2;
    }
    if (strcmp(tr->name, "uds") == 0) {
        fprintf(stderr, "joint_server: transport=uds is for benchmarks and host tests only: "
                        "NO priority inheritance (ADR-004)\n");
    }

    /* Block SIGINT/SIGTERM before any thread exists so only signal_thread sees them. */
    static sig_ctx_t sc;
    sigemptyset(&sc.set);
    sigaddset(&sc.set, SIGINT);
    sigaddset(&sc.set, SIGTERM);
    (void)pthread_sigmask(SIG_BLOCK, &sc.set, NULL);

    static srv_state_t state; /* shared with the workers and the publisher */
    state.verbose = verbose;
    (void)pthread_mutex_init(&state.mu, NULL);
    demo_joint_v1_Joint_ops ops;
    memset(&ops, 0, sizeof(ops));
    ops.SetTarget = on_set_target;
    ops.Stop = on_stop;
    ops.GetState = on_get_state;
    ops.Burn = on_burn;

    static qmp_server_t server; /* outlives main's frame: the signal thread holds it */
    qmp_server_cfg_t cfg = {.transport = tr,
                            .nthreads = nthreads,
                            .listen_flags = fixed_priority ? QMP_LISTEN_FIXED_PRIORITY : 0,
                            .verbose = verbose,
                            .workbuf = g_workbuf,
                            .workbuf_size = sizeof(g_workbuf),
                            .on_client_gone = on_client_gone,
                            .topicbuf = g_topicbuf,
                            .topicbuf_size = sizeof(g_topicbuf)};
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

    LOGF("started pid=%d version=%s build=%s service=%s path=%s transport=%s threads=%d "
         "fixed_priority=%d "
         "publish_ms=%ld generation=%" PRIu32,
         (int)getpid(), qmp_build_version(), qmp_build_id(), DEMO_JOINT_V1_JOINT_SERVICE_NAME,
         DEMO_JOINT_V1_JOINT_QNX_PATH, tr->name, nthreads, fixed_priority, publish_ms,
         qmp_server_generation(&server));

    static pub_ctx_t pc;
    pthread_t pub_thread;
    int publishing = 0;
    if (publish_ms > 0) {
        pc.server = &server;
        pc.state = &state;
        pc.period_ms = (uint32_t)publish_ms;
        if (pthread_create(&pub_thread, NULL, publisher_thread, &pc) != 0) {
            fprintf(stderr, "joint_server: cannot create publisher thread\n");
            return 1;
        }
        publishing = 1;
    }

    int result = 0;
#if !defined(__QNX__)
    pthread_t st_thread;
    g_selftest_tr = tr;
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
    if (publishing) {
        atomic_store(&pc.stop, 1);
        (void)pthread_join(pub_thread, NULL);
    }
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
