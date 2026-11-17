/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Standalone Joint client: the counterpart of joint_server.c for two-process experiments
 * on a QNX target (pidin, kill -9, priority inheritance). Orchestrated by
 * scripts/target-smoke.sh; every mode can also be run by hand in a second SSH session.
 *
 * Usage: qmp_joint_client [--timeout MS] [--fake] <mode> [args]
 *   once                   one SetTarget + GetState round trip, print the state
 *   loop N                 N SetTarget calls; print min/median/max latency
 *   slow MS sleep|spin     one Burn call that keeps a server worker busy for MS ms
 *   reconnect [MAX_S]      keep calling GetState; report every failure with its errno,
 *                          reconnect with backoff, exit 0 once the server generation
 *                          changed and a call succeeded (give up after MAX_S s, def. 60)
 *
 *   --timeout MS           per-call timeout (default: 5000; 0 = block forever)
 *   --fake                 allow the in-process fake transport (host); otherwise the
 *                          client refuses to start when the transport is not "qnx"
 *
 * Exit code: 0 on success, 1 on any unexpected error, 2 on usage errors.
 */
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
        return 1;
    }
    rc = demo_joint_v1_Joint_GetState(c, &st, &status);
    if (rc != EOK || status != 0) {
        LOGF("GetState failed rc=%d (%s) status=%" PRId32, rc, strerror(rc), status);
        return 1;
    }
    printf("state: seq=%" PRIu64 " position=%.2f mode=%.*s\n", st.seq, (double)st.position_rad,
           (int)st.mode.len, st.mode.data);
    return 0;
}

static int mode_loop(qmp_client_t *c, long n) {
    if (n < 1 || n > LOOP_MAX) {
        fprintf(stderr, "joint_client: loop count must be 1..%d\n", LOOP_MAX);
        return 2;
    }
    uint64_t *lat = (uint64_t *)malloc((size_t)n * sizeof(*lat)); /* before any call */
    if (lat == NULL) {
        return 1;
    }
    demo_joint_v1_SetTargetRequest req = {0.1f, 1.0f};
    demo_joint_v1_SetTargetReply rep;
    int32_t status = 0;
    for (long i = 0; i < n; i++) {
        req.position_rad = (float)i;
        uint64_t t0 = now_ns();
        int rc = demo_joint_v1_Joint_SetTarget(c, &req, &rep, &status);
        lat[i] = now_ns() - t0;
        if (rc != EOK || status != 0) {
            LOGF("call %ld failed rc=%d (%s) status=%" PRId32, i, rc, strerror(rc), status);
            free(lat);
            return 1;
        }
    }
    qsort(lat, (size_t)n, sizeof(*lat), cmp_u64);
    printf("loop: n=%ld min=%" PRIu64 "us median=%" PRIu64 "us max=%" PRIu64 "us\n", n,
           lat[0] / 1000U, lat[n / 2] / 1000U, lat[n - 1] / 1000U);
    free(lat);
    return 0;
}

static int mode_slow(qmp_client_t *c, long ms, const char *variant) {
    int spin;
    if (strcmp(variant, "spin") == 0) {
        spin = 1;
    } else if (strcmp(variant, "sleep") == 0) {
        spin = 0;
    } else {
        fprintf(stderr, "joint_client: slow variant must be sleep or spin\n");
        return 2;
    }
    if (ms < 0 || ms > 600000) {
        fprintf(stderr, "joint_client: slow duration must be 0..600000 ms\n");
        return 2;
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
        return 1;
    }
    printf("slow: done rc=0 elapsed_ms=%" PRIu64 " iterations=%" PRIu64 "\n", dt / 1000000U,
           rep.iterations);
    return 0;
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
                return 0;
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
    return 1;
}

static int usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s [--timeout MS] [--fake] once | loop N | slow MS sleep|spin |"
            " reconnect [MAX_S]\n",
            argv0);
    return 2;
}

int main(int argc, char **argv) {
    long timeout_ms = 5000;
    int use_fake = 0;
    int i = 1;
    for (; i < argc && strncmp(argv[i], "--", 2) == 0; i++) {
        if (strcmp(argv[i], "--timeout") == 0 && i + 1 < argc) {
            timeout_ms = atol(argv[++i]);
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

    if (strcmp(mode, "once") == 0) {
        rc = mode_once(&c);
    } else if (strcmp(mode, "loop") == 0 && i < argc) {
        rc = mode_loop(&c, atol(argv[i]));
    } else if (strcmp(mode, "slow") == 0 && i + 1 < argc) {
        rc = mode_slow(&c, atol(argv[i]), argv[i + 1]);
    } else if (strcmp(mode, "reconnect") == 0) {
        rc = mode_reconnect(&c, i < argc ? atol(argv[i]) : 60);
    } else {
        (void)qmp_client_close(&c);
        return usage(argv[0]);
    }
    (void)qmp_client_close(&c);
    printf("%s %s\n", rc == 0 ? "PASS" : "FAIL", mode);
    return rc;
}
