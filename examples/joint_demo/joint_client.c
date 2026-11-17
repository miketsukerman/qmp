/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Standalone Joint client: the counterpart of joint_server.c for two-process experiments
 * on a QNX target (pidin, kill -9, priority inheritance). Orchestrated by
 * scripts/target-smoke.sh; every mode can also be run by hand in a second SSH session.
 *
 * Usage: qmp_joint_client [--timeout MS] [--expect ERRNO] [--fake] <mode> [args]
 *   once                   one SetTarget + GetState round trip, print the state
 *   loop N [WARMUP]        WARMUP uncounted SetTarget calls (default 100), then N timed
 *                          ones; print min/median/p99/max latency in microseconds
 *   slow MS sleep|spin     one Burn call that keeps a server worker busy for MS ms
 *   late MS                connect, sleep MS ms (the harness kills the server meanwhile),
 *                          then make one GetState call and report its errno
 *   reconnect [MAX_S]      keep calling GetState; report every failure with its errno,
 *                          reconnect with backoff, exit 0 once the server generation
 *                          changed and a call succeeded (give up after MAX_S s, def. 60)
 *
 *   --timeout MS           per-call timeout (default: 5000; 0 = block forever)
 *   --expect ERRNO         the mode is expected to fail with this errno name (ETIMEDOUT,
 *                          ESRCH, ...): print PASS and exit 0 when the failing call
 *                          returns exactly that, FAIL and exit 1 on success or any other
 *                          error. Without it, success is PASS.
 *   --fake                 allow the in-process fake transport (host); otherwise the
 *                          client refuses to start when the transport is not "qnx"
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
#include "qmp_build_id.h"

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
    (void)write(STDERR_FILENO, buf, off);
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

static int usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s [--timeout MS] [--expect ERRNO] [--fake] once | loop N [WARMUP] |"
            " slow MS sleep|spin | late MS | reconnect [MAX_S]\n",
            argv0);
    return 2;
}

int main(int argc, char **argv) {
    long timeout_ms = 5000;
    int use_fake = 0;
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
        } else if (strcmp(argv[i], "--fake") == 0) {
            use_fake = 1;
        } else {
            return usage(argv[0]);
        }
    }
    if (i >= argc) {
        return usage(argv[0]);
    }
    const char *mode = argv[i++];

    const qmp_transport_t *tr = use_fake ? qmp_transport_fake() : qmp_transport_default();
    if (!use_fake && strcmp(tr->name, "qnx") != 0) {
        fprintf(stderr,
                "joint_client: default transport is \"%s\", not the native QNX transport;\n"
                "refusing to run (pass --fake to use the in-process fake on the host)\n",
                tr->name);
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
    LOGF("connected pid=%d build=%s service=%s transport=%s generation=%" PRIu32 " timeout_ms=%ld",
         (int)getpid(), QMP_BUILD_ID, DEMO_JOINT_V1_JOINT_SERVICE_NAME, tr->name,
         qmp_client_server_generation(&c), timeout_ms);

    int outcome;
    if (strcmp(mode, "once") == 0) {
        outcome = mode_once(&c);
    } else if (strcmp(mode, "loop") == 0 && i < argc) {
        outcome = mode_loop(&c, atol(argv[i]), i + 1 < argc ? atol(argv[i + 1]) : 100);
    } else if (strcmp(mode, "slow") == 0 && i + 1 < argc) {
        outcome = mode_slow(&c, atol(argv[i]), argv[i + 1]);
    } else if (strcmp(mode, "late") == 0 && i < argc) {
        outcome = mode_late(&c, atol(argv[i]));
    } else if (strcmp(mode, "reconnect") == 0) {
        outcome = mode_reconnect(&c, i < argc ? atol(argv[i]) : 60);
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
