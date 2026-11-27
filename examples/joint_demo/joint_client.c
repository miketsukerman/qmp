/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Standalone Joint client: the counterpart of joint_server.c for two-process experiments
 * on a QNX target (pidin, kill -9, priority inheritance). Orchestrated by
 * scripts/target-smoke.sh; every mode can also be run by hand in a second SSH session.
 *
 * Usage: qmp_joint_client [--timeout MS] [--expect ERRNO] [--transport NAME] <mode> [args]
 *   once                   one SetTarget + GetState round trip, print the state
 *   loop N [WARMUP]        WARMUP uncounted SetTarget calls (default 100), then N timed
 *                          ones; print min/median/p99/max latency in microseconds
 *   stop                   one-way Stop: returns once the server accepted it, then poll
 *                          GetState until the handler (which runs after the caller was
 *                          released) has switched the mode to "stopped"
 *   slow MS sleep|spin     one Burn call that keeps a server worker busy for MS ms
 *   late MS                connect, sleep MS ms (the harness kills the server meanwhile),
 *                          then make one GetState call and report its errno
 *   reconnect [MAX_S]      keep calling GetState; report every failure with its errno,
 *                          reconnect with backoff, exit 0 once the server generation
 *                          changed and a call succeeded (give up after MAX_S s, def. 60)
 *   subscribe [state|log] [N] [MAX_S] [SLOW_MS]
 *                          subscribe to a topic — state = StateChanged (LATEST), log =
 *                          StateLog (QUEUE depth 8) — and print one line per update
 *                          (receive timestamp, topic seq, dropped count, payload); PASS
 *                          after N updates (default 10) within MAX_S s (default 30).
 *                          SLOW_MS > 0 sleeps between polls to emulate a slow subscriber:
 *                          LATEST then skips to the newest value, QUEUE replays the
 *                          backlog; both report the dropped count instead of hiding loss.
 *                          If the server dies mid-stream, reconnect; the subscription is
 *                          re-established automatically on the new session (ADR-003).
 *
 *   --timeout MS           per-call timeout (default: 5000; 0 = block forever)
 *   --expect ERRNO         the mode is expected to fail with this errno name (ETIMEDOUT,
 *                          ESRCH, ...): print PASS and exit 0 when the failing call
 *                          returns exactly that, FAIL and exit 1 on success or any other
 *                          error. Without it, success is PASS.
 *   --transport NAME       qnx | uds (default: qnx on QNX, uds on the host); must match
 *                          the server. uds is the AF_UNIX bench/test transport (ADR-004)
 *                          and has NO priority inheritance.
 *
 * Output: the last stdout line is "PASS <mode>" or "FAIL <mode>"; with --expect it also
 * names the expected and the actual outcome.
 * Exit code: 0 on PASS, 1 on FAIL, 2 on usage errors.
 */
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "joint.qmp.h"

#define LOOP_MAX 1000000

/* Mode outcome: EOK, the errno of the call that failed, or one of these. */
#define OUTCOME_STATUS (-1) /* the call returned EOK with a non-zero application status */
#define OUTCOME_USAGE  (-2) /* bad mode arguments: exit 2, no PASS/FAIL line             */

/* errno names accepted by --expect (and used to print outcomes). */
static const struct {
    const char *name;
    int value;
} g_errnos[] = {
    {"EOK", EOK},
    {"ETIMEDOUT", ETIMEDOUT},
    {"ESRCH", ESRCH},
    {"ENOTCONN", ENOTCONN},
    {"ECANCELED", ECANCELED},
    {"EBADF", EBADF},
    {"EPROTO", EPROTO},
    {"EINVAL", EINVAL},
    {"ENOTSUP", ENOTSUP},
    {"EACCES", EACCES},
    {"EBUSY", EBUSY},
    {"ENOENT", ENOENT},
    {"ECONNREFUSED", ECONNREFUSED},
};

static int errno_from_name(const char *name, int *out) {
    for (size_t k = 0; k < sizeof(g_errnos) / sizeof(g_errnos[0]); k++) {
        if (strcmp(g_errnos[k].name, name) == 0) {
            *out = g_errnos[k].value;
            return 0;
        }
    }
    return -1;
}

static const char *outcome_name(int outcome) {
    if (outcome == OUTCOME_STATUS) {
        return "app-status";
    }
    for (size_t k = 0; k < sizeof(g_errnos) / sizeof(g_errnos[0]); k++) {
        if (g_errnos[k].value == outcome) {
            return g_errnos[k].name;
        }
    }
    return strerror(outcome);
}

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
                     "[%" PRIu64 ".%06" PRIu64 "] joint_client: ", (uint64_t)(t / 1000000000ULL),
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

static void msleep(uint32_t ms) {
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
    (void)nanosleep(&ts, NULL);
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

static int mode_once(qmp_client_t *c) {
    demo_joint_v1_SetTargetRequest req = {0.5f, 1.0f};
    demo_joint_v1_SetTargetReply rep;
    demo_joint_v1_JointState st;
    int32_t status = 0;
    int rc = demo_joint_v1_Joint_SetTarget(c, &req, &rep, &status);
    if (rc != EOK || status != 0) {
        LOGF("SetTarget failed rc=%d (%s) status=%" PRId32, rc, strerror(rc), status);
        return rc != EOK ? rc : OUTCOME_STATUS;
    }
    rc = demo_joint_v1_Joint_GetState(c, &st, &status);
    if (rc != EOK || status != 0) {
        LOGF("GetState failed rc=%d (%s) status=%" PRId32, rc, strerror(rc), status);
        return rc != EOK ? rc : OUTCOME_STATUS;
    }
    printf("state: seq=%" PRIu64 " position=%.2f mode=%.*s\n", st.seq, (double)st.position_rad,
           (int)st.mode.len, st.mode.data);
    return 0;
}

/* Latency in microseconds with one decimal, from nanoseconds. */
#define US_FMT       "%" PRIu64 ".%" PRIu64 "us"
#define US_ARGS(ns_) (uint64_t)((ns_) / 1000U), (uint64_t)(((ns_) % 1000U) / 100U)

/* One-way Stop. EOK from the stub only means "accepted": the handler runs afterwards, so
 * its effect is observed by polling GetState (bounded to about one second). */
static int mode_stop(qmp_client_t *c) {
    uint64_t t0 = now_ns();
    int rc = demo_joint_v1_Joint_Stop(c);
    uint64_t dt = now_ns() - t0;
    if (rc != EOK) {
        LOGF("Stop failed rc=%d (%s)", rc, strerror(rc));
        return rc;
    }
    for (int tries = 0; tries < 100; tries++) {
        demo_joint_v1_JointState st;
        int32_t status = 0;
        rc = demo_joint_v1_Joint_GetState(c, &st, &status);
        if (rc != EOK || status != 0) {
            LOGF("GetState failed rc=%d (%s) status=%" PRId32, rc, strerror(rc), status);
            return rc != EOK ? rc : OUTCOME_STATUS;
        }
        if (st.mode.len == 7 && memcmp(st.mode.data, "stopped", 7) == 0) {
            printf("stop: accepted in " US_FMT ", mode=stopped after %d GetState poll(s)\n",
                   US_ARGS(dt), tries + 1);
            return EOK;
        }
        msleep(10);
    }
    LOGF("stop: accepted, but the mode never became \"stopped\"");
    return ETIMEDOUT;
}

static int mode_loop(qmp_client_t *c, long n, long warmup) {
    if (n < 1 || n > LOOP_MAX || warmup < 0 || warmup > LOOP_MAX) {
        fprintf(stderr, "joint_client: loop count and warm-up must be 1..%d and 0..%d\n", LOOP_MAX,
                LOOP_MAX);
        return OUTCOME_USAGE;
    }
    uint64_t *lat = (uint64_t *)malloc((size_t)n * sizeof(*lat)); /* before any call */
    if (lat == NULL) {
        return ENOMEM;
    }
    demo_joint_v1_SetTargetRequest req = {0.1f, 1.0f};
    demo_joint_v1_SetTargetReply rep;
    int32_t status = 0;
    /* The warm-up faults in code and stack pages and settles caches on both sides; it is
     * not timed. */
    for (long i = -warmup; i < n; i++) {
        req.position_rad = (float)i;
        uint64_t t0 = now_ns();
        int rc = demo_joint_v1_Joint_SetTarget(c, &req, &rep, &status);
        uint64_t dt = now_ns() - t0;
        if (rc != EOK || status != 0) {
            LOGF("call %ld failed rc=%d (%s) status=%" PRId32, i, rc, strerror(rc), status);
            free(lat);
            return rc != EOK ? rc : OUTCOME_STATUS;
        }
        if (i >= 0) {
            lat[i] = dt;
        }
    }
    qsort(lat, (size_t)n, sizeof(*lat), cmp_u64);
    /* Nearest-rank percentiles: the smallest sample with at least p% of samples <= it. */
    size_t p50 = ((size_t)n * 50U + 99U) / 100U - 1U;
    size_t p99 = ((size_t)n * 99U + 99U) / 100U - 1U;
    printf("loop: n=%ld warmup=%ld min=" US_FMT " median=" US_FMT " p99=" US_FMT " max=" US_FMT
           "\n",
           n, warmup, US_ARGS(lat[0]), US_ARGS(lat[p50]), US_ARGS(lat[p99]), US_ARGS(lat[n - 1]));
    free(lat);
    return EOK;
}

static int mode_slow(qmp_client_t *c, long ms, const char *variant) {
    int spin;
    if (strcmp(variant, "spin") == 0) {
        spin = 1;
    } else if (strcmp(variant, "sleep") == 0) {
        spin = 0;
    } else {
        fprintf(stderr, "joint_client: slow variant must be sleep or spin\n");
        return OUTCOME_USAGE;
    }
    if (ms < 0 || ms > 600000) {
        fprintf(stderr, "joint_client: slow duration must be 0..600000 ms\n");
        return OUTCOME_USAGE;
    }
    demo_joint_v1_BurnRequest req = {(uint32_t)ms, (uint8_t)spin, {0}};
    demo_joint_v1_BurnReply rep = {0};
    int32_t status = 0;
    LOGF("slow: calling Burn duration_ms=%ld variant=%s", ms, variant);
    uint64_t t0 = now_ns();
    int rc = demo_joint_v1_Joint_Burn(c, &req, &rep, &status);
    uint64_t dt = now_ns() - t0;
    if (rc != EOK || status != 0) {
        LOGF("slow: Burn failed rc=%d (%s) status=%" PRId32 " elapsed_ms=%" PRIu64, rc,
             strerror(rc), status, dt / 1000000U);
        return rc != EOK ? rc : OUTCOME_STATUS;
    }
    printf("slow: done rc=0 elapsed_ms=%" PRIu64 " iterations=%" PRIu64 "\n", dt / 1000000U,
           rep.iterations);
    return EOK;
}

static int mode_late(qmp_client_t *c, long ms) {
    if (ms < 0 || ms > 600000) {
        fprintf(stderr, "joint_client: late delay must be 0..600000 ms\n");
        return OUTCOME_USAGE;
    }
    LOGF("late: connected, sleeping %ld ms before the call", ms);
    msleep((uint32_t)ms);
    demo_joint_v1_JointState st;
    int32_t status = 0;
    uint64_t t0 = now_ns();
    int rc = demo_joint_v1_Joint_GetState(c, &st, &status);
    uint64_t dt = now_ns() - t0;
    LOGF("late: GetState -> rc=%d (%s) status=%" PRId32 " elapsed_us=%" PRIu64 " connected=%d", rc,
         strerror(rc), status, dt / 1000U, qmp_client_is_connected(c));
    if (rc != EOK) {
        return rc;
    }
    return status != 0 ? OUTCOME_STATUS : EOK;
}

static int mode_reconnect(qmp_client_t *c, long max_s) {
    uint32_t gen0 = qmp_client_server_generation(c);
    LOGF("reconnect: initial generation=%" PRIu32, gen0);
    const uint64_t deadline = now_ns() + (uint64_t)max_s * 1000000000ULL;
    while (now_ns() < deadline) {
        demo_joint_v1_JointState st;
        int32_t status = 0;
        int rc = demo_joint_v1_Joint_GetState(c, &st, &status);
        if (rc == EOK && status == 0) {
            uint32_t gen = qmp_client_server_generation(c);
            if (gen != gen0) {
                printf("reconnect: generation changed %" PRIu32 " -> %" PRIu32 "\n", gen0, gen);
                return EOK;
            }
            msleep(200);
            continue;
        }
        LOGF("reconnect: call failed rc=%d (%s) status=%" PRId32, rc, strerror(rc), status);
        if (!qmp_client_is_connected(c)) {
            rc = qmp_client_reconnect(c);
            LOGF("reconnect: qmp_client_reconnect -> %d (%s)", rc, strerror(rc));
        } else {
            msleep(200);
        }
    }
    LOGF("reconnect: gave up after %ld s without a generation change", max_s);
    return ETIMEDOUT;
}

/* ---- subscribe: topic updates line by line (ADR-003 notify-then-pull) ----------------- */

typedef struct sub_stats {
    long updates;
    uint64_t dropped_total;
} sub_stats_t;

/* Runs on the qmp_client_poll() thread (= main). One stdout line per update. */
static void on_state_update(void *ctx, const demo_joint_v1_JointState *st, uint64_t seq,
                            uint64_t dropped) {
    sub_stats_t *ss = (sub_stats_t *)ctx;
    uint64_t t = now_ns();
    ss->updates++;
    ss->dropped_total += dropped;
    printf("update: recv=%" PRIu64 ".%06" PRIu64 " seq=%" PRIu64 " dropped=%" PRIu64
           " state_seq=%" PRIu64 " position=%.2f\n",
           (uint64_t)(t / 1000000000ULL), (uint64_t)((t % 1000000000ULL) / 1000ULL), seq, dropped,
           st->seq, (double)st->position_rad);
}

static int mode_subscribe(qmp_client_t *c, const char *topic, long want, long max_s, long slow_ms) {
    int use_log;
    if (strcmp(topic, "state") == 0) {
        use_log = 0;
    } else if (strcmp(topic, "log") == 0) {
        use_log = 1;
    } else {
        fprintf(stderr, "joint_client: subscribe topic must be state or log\n");
        return OUTCOME_USAGE;
    }
    if (want < 1 || want > LOOP_MAX || max_s < 1 || max_s > 3600 || slow_ms < 0 ||
        slow_ms > 60000) {
        fprintf(stderr, "joint_client: subscribe wants 1..%d updates within 1..3600 s\n", LOOP_MAX);
        return OUTCOME_USAGE;
    }
    /* Must outlive the subscription (caller-provided storage, no heap). */
    static demo_joint_v1_Joint_StateChanged_sub_t sub_state;
    static demo_joint_v1_Joint_StateLog_sub_t sub_log;
    sub_stats_t ss = {0, 0};
    int rc = use_log
                 ? demo_joint_v1_Joint_subscribe_StateLog(c, &sub_log, on_state_update, &ss)
                 : demo_joint_v1_Joint_subscribe_StateChanged(c, &sub_state, on_state_update, &ss);
    if (rc != EOK) {
        LOGF("subscribe: failed rc=%d (%s)", rc, strerror(rc));
        return rc;
    }
    LOGF("subscribe: topic=%s(0x%04X) want=%ld slow_ms=%ld",
         use_log ? "StateLog/QUEUE" : "StateChanged/LATEST",
         use_log ? DEMO_JOINT_V1_JOINT_T_STATELOG : DEMO_JOINT_V1_JOINT_T_STATECHANGED, want,
         slow_ms);
    const uint64_t deadline = now_ns() + (uint64_t)max_s * 1000000000ULL;
    while (ss.updates < want && now_ns() < deadline) {
        rc = qmp_client_poll(c, 1000000000ULL); /* 1 s slices to honour the deadline */
        if (rc == EOK || rc == ETIMEDOUT) {
            if (rc == EOK && slow_ms > 0) {
                msleep((uint32_t)slow_ms); /* emulate a slow subscriber */
            }
            continue;
        }
        if (rc == ESRCH) {
            /* Server died mid-stream: reconnect re-handshakes and resubscribes. */
            LOGF("subscribe: server gone, reconnecting");
            rc = qmp_client_reconnect(c);
            LOGF("subscribe: qmp_client_reconnect -> %d (%s) generation=%" PRIu32, rc, strerror(rc),
                 qmp_client_server_generation(c));
            if (rc != EOK && rc != EAGAIN) {
                return rc;
            }
            continue;
        }
        LOGF("subscribe: poll failed rc=%d (%s)", rc, strerror(rc));
        return rc;
    }
    printf("subscribe: updates=%ld dropped_total=%" PRIu64 "\n", ss.updates, ss.dropped_total);
    return ss.updates >= want ? EOK : ETIMEDOUT;
}

static int usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s [--timeout MS] [--expect ERRNO] [--transport qnx|uds] once |"
            " stop | loop N [WARMUP] |"
            " slow MS sleep|spin | late MS | reconnect [MAX_S] |"
            " subscribe [state|log] [N] [MAX_S] [SLOW_MS]\n",
            argv0);
    return 2;
}

int main(int argc, char **argv) {
    long timeout_ms = 5000;
    const char *tr_name = NULL;
    const char *expect_name = NULL;
    int expect = EOK;
    int i = 1;
    for (; i < argc && strncmp(argv[i], "--", 2) == 0; i++) {
        if (strcmp(argv[i], "--timeout") == 0 && i + 1 < argc) {
            timeout_ms = atol(argv[++i]);
        } else if (strcmp(argv[i], "--expect") == 0 && i + 1 < argc) {
            expect_name = argv[++i];
            if (errno_from_name(expect_name, &expect) != 0) {
                fprintf(stderr, "joint_client: --expect: unknown errno name \"%s\"\n", expect_name);
                return 2;
            }
        } else if (strcmp(argv[i], "--transport") == 0 && i + 1 < argc) {
            tr_name = argv[++i];
        } else {
            return usage(argv[0]);
        }
    }
    if (i >= argc) {
        return usage(argv[0]);
    }
    const char *mode = argv[i++];

    const qmp_transport_t *tr =
        tr_name != NULL ? qmp_transport_by_name(tr_name) : qmp_transport_default();
    if (tr == NULL || strcmp(tr->name, "fake") == 0) {
        fprintf(stderr, "joint_client: transport \"%s\" cannot reach a server in another process\n",
                tr_name != NULL ? tr_name : "fake");
        return 2;
    }

    qmp_client_t c;
    qmp_client_cfg_t cfg = {.transport = tr,
                            .default_timeout_ns = (uint64_t)timeout_ms * 1000000ULL,
                            .reconnect_initial_ms = 50,
                            .reconnect_max_ms = 1000,
                            .reconnect_attempts = 20};
    int rc = demo_joint_v1_Joint_connect(&c, 0, &cfg);
    if (rc != EOK) {
        fprintf(stderr, "joint_client: connect to %s failed: %d (%s)\n",
                DEMO_JOINT_V1_JOINT_QNX_PATH, rc, strerror(rc));
        return 1;
    }
    LOGF("connected pid=%d version=%s build=%s service=%s transport=%s generation=%" PRIu32
         " timeout_ms=%ld",
         (int)getpid(), qmp_build_version(), qmp_build_id(), DEMO_JOINT_V1_JOINT_SERVICE_NAME,
         tr->name, qmp_client_server_generation(&c), timeout_ms);

    int outcome;
    if (strcmp(mode, "once") == 0) {
        outcome = mode_once(&c);
    } else if (strcmp(mode, "stop") == 0) {
        outcome = mode_stop(&c);
    } else if (strcmp(mode, "loop") == 0 && i < argc) {
        outcome = mode_loop(&c, atol(argv[i]), i + 1 < argc ? atol(argv[i + 1]) : 100);
    } else if (strcmp(mode, "slow") == 0 && i + 1 < argc) {
        outcome = mode_slow(&c, atol(argv[i]), argv[i + 1]);
    } else if (strcmp(mode, "late") == 0 && i < argc) {
        outcome = mode_late(&c, atol(argv[i]));
    } else if (strcmp(mode, "reconnect") == 0) {
        outcome = mode_reconnect(&c, i < argc ? atol(argv[i]) : 60);
    } else if (strcmp(mode, "subscribe") == 0) {
        outcome = mode_subscribe(
            &c, i < argc ? argv[i] : "state", i + 1 < argc ? atol(argv[i + 1]) : 10,
            i + 2 < argc ? atol(argv[i + 2]) : 30, i + 3 < argc ? atol(argv[i + 3]) : 0);
    } else {
        outcome = OUTCOME_USAGE;
    }
    (void)qmp_client_close(&c);
    if (outcome == OUTCOME_USAGE) {
        return usage(argv[0]);
    }
    int pass = outcome == expect;
    if (expect_name != NULL) {
        printf("%s %s (expected %s, got %s)\n", pass ? "PASS" : "FAIL", mode, expect_name,
               outcome_name(outcome));
    } else {
        printf("%s %s\n", pass ? "PASS" : "FAIL", mode);
    }
    return pass ? 0 : 1;
}
