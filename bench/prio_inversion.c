/* SPDX-License-Identifier: Apache-2.0 */
/*
 * W4 priority-inversion benchmark: what happens to a high-priority caller when the
 * service it calls is a low-priority thread and a medium-priority thread wants the CPU.
 *
 *   server  priority 10   one libqmp worker serving demo.joint.v1.Joint.SetTarget
 *   hog     priority 15   spins BURST ms, sleeps IDLE ms, repeats (off with --hog-burst-ms 0)
 *   client  priority 30   one SetTarget about every PERIOD (jittered by +-50 % from a fixed
 *                         seed so the calls sample every phase of the hog), each timed
 *
 * All three are threads of this process and must share ONE cpu, otherwise the hog simply
 * runs next to the server and nothing is measured: start it with `on -C <cpu>` on QNX
 * (bench/run.sh does) or `taskset -c <cpu>` on Linux.
 *
 * With native QNX message passing the worker inherits the caller's priority for the
 * duration of the call (30 > 15), so the hog cannot delay it. Over the UDS transport, or
 * on a native channel created with --fixed-priority (_NTO_CHF_FIXED_PRIORITY, the negative
 * control), the worker stays at 10 and the call waits until the hog's burst is over: a
 * classic unbounded priority inversion, bounded here only by the burst length.
 *
 * Usage: qmp_bench_prio_inversion [--transport qnx|uds] [--fixed-priority] [--calls N]
 *            [--warmup N] [--period-us US] [--hog-burst-ms MS] [--hog-idle-ms MS]
 *            [--prio-server P] [--prio-hog P] [--prio-client P] [--label TEXT] [--dump FILE]
 *            [--no-rt]
 *   --no-rt       do not change thread priorities (functional check on a host without
 *                 realtime privileges; the numbers then mean nothing)
 *   --dump FILE   write every measured latency in nanoseconds, one per line, call order
 *
 * Output: one "result:" line with key=value pairs (latencies in microseconds). Exit code 0
 * when every call succeeded, 1 otherwise, 2 on usage errors.
 */
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "joint.qmp.h"

#define MAX_CALLS 1000000L

static uint64_t now_ns(void) {
    struct timespec ts;
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void sleep_until(uint64_t t_ns) {
    struct timespec ts = {(time_t)(t_ns / 1000000000ULL), (long)(t_ns % 1000000000ULL)};
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {
    }
}

static int g_no_rt; /* --no-rt: leave every thread's scheduling alone */

/* Fixed-priority round-robin scheduling for the calling thread. Returns 0 or an errno. */
static int set_self_priority(int prio) {
    if (g_no_rt) {
        return 0;
    }
    struct sched_param sp;
    memset(&sp, 0, sizeof(sp));
    sp.sched_priority = prio;
    return pthread_setschedparam(pthread_self(), SCHED_RR, &sp);
}

static int self_priority(void) {
    struct sched_param sp;
    int policy = 0;
    if (pthread_getschedparam(pthread_self(), &policy, &sp) != 0) {
        return -1;
    }
    return sp.sched_priority;
}

/* ---- server ---------------------------------------------------------------------------- */

typedef struct srv_ctx {
    qmp_server_t server;
    int prio;
    int prio_rc;
    volatile int handler_prio_max; /* highest priority a handler observed itself at */
    volatile int handler_prio_min;
    volatile uint64_t calls;
} srv_ctx_t;

static int on_set_target(void *ctx, qmp_req_t *req, const demo_joint_v1_SetTargetRequest *in,
                         demo_joint_v1_SetTargetReply *out, int32_t *status) {
    srv_ctx_t *sc = (srv_ctx_t *)ctx;
    (void)req;
    (void)in;
    int p = self_priority();
    if (p > sc->handler_prio_max) {
        sc->handler_prio_max = p;
    }
    if (p < sc->handler_prio_min) {
        sc->handler_prio_min = p;
    }
    sc->calls++;
    out->result = 0;
    *status = 0;
    return QMP_HANDLED;
}

static void *server_thread(void *arg) {
    srv_ctx_t *sc = (srv_ctx_t *)arg;
    sc->prio_rc = set_self_priority(sc->prio);
    (void)qmp_server_run(&sc->server); /* one worker: this thread */
    return NULL;
}

/* ---- hog ------------------------------------------------------------------------------- */

typedef struct hog_ctx {
    int prio;
    int prio_rc;
    uint32_t burst_ms;
    uint32_t idle_ms;
    volatile int stop;
    volatile uint64_t bursts;
} hog_ctx_t;

static void *hog_thread(void *arg) {
    hog_ctx_t *hc = (hog_ctx_t *)arg;
    hc->prio_rc = set_self_priority(hc->prio);
    volatile uint32_t sink = 0;
    while (!hc->stop) {
        uint64_t end = now_ns() + (uint64_t)hc->burst_ms * 1000000ULL;
        while (now_ns() < end && !hc->stop) {
            for (int i = 0; i < 1000; i++) {
                sink += (uint32_t)i;
            }
        }
        hc->bursts++;
        sleep_until(now_ns() + (uint64_t)hc->idle_ms * 1000000ULL);
    }
    return NULL;
}

/* ---- main (= client) ------------------------------------------------------------------- */

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

/* Nearest-rank percentile of a sorted array: smallest sample with >= p/1000 of them <= it. */
static uint64_t pct(const uint64_t *sorted, long n, unsigned permille) {
    size_t idx = ((size_t)n * permille + 999U) / 1000U;
    return sorted[idx == 0 ? 0 : idx - 1];
}

#define US_FMT       "%" PRIu64 ".%" PRIu64
#define US_ARGS(ns_) (uint64_t)((ns_) / 1000U), (uint64_t)(((ns_) % 1000U) / 100U)

static uint64_t g_workbuf[QMP_SERVER_WORKBUF_SIZE(DEMO_JOINT_V1_JOINT_MAX_MSG_SIZE, 1) / 8 + 1];
static uint64_t g_topicbuf[DEMO_JOINT_V1_JOINT_TOPIC_STORAGE_SIZE / 8 + 1];

static int usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s [--transport qnx|uds] [--fixed-priority] [--calls N] [--warmup N]\n"
            "          [--period-us US] [--hog-burst-ms MS] [--hog-idle-ms MS]\n"
            "          [--prio-server P] [--prio-hog P] [--prio-client P] [--label TEXT]\n"
            "          [--dump FILE] [--no-rt]\n",
            argv0);
    return 2;
}

int main(int argc, char **argv) {
    const char *tr_name = NULL;
    const char *label = NULL;
    const char *dump = NULL;
    int fixed_priority = 0;
    long calls = 2000;
    long warmup = 200;
    long period_us = 7000;
    long burst_ms = 20;
    long idle_ms = 5;
    long prio_server = 10;
    long prio_hog = 15;
    long prio_client = 30;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (strcmp(a, "--fixed-priority") == 0) {
            fixed_priority = 1;
            continue;
        }
        if (strcmp(a, "--no-rt") == 0) {
            g_no_rt = 1;
            continue;
        }
        if (v == NULL) {
            return usage(argv[0]);
        }
        i++;
        if (strcmp(a, "--transport") == 0) {
            tr_name = v;
        } else if (strcmp(a, "--label") == 0) {
            label = v;
        } else if (strcmp(a, "--dump") == 0) {
            dump = v;
        } else if (strcmp(a, "--calls") == 0) {
            calls = atol(v);
        } else if (strcmp(a, "--warmup") == 0) {
            warmup = atol(v);
        } else if (strcmp(a, "--period-us") == 0) {
            period_us = atol(v);
        } else if (strcmp(a, "--hog-burst-ms") == 0) {
            burst_ms = atol(v);
        } else if (strcmp(a, "--hog-idle-ms") == 0) {
            idle_ms = atol(v);
        } else if (strcmp(a, "--prio-server") == 0) {
            prio_server = atol(v);
        } else if (strcmp(a, "--prio-hog") == 0) {
            prio_hog = atol(v);
        } else if (strcmp(a, "--prio-client") == 0) {
            prio_client = atol(v);
        } else {
            return usage(argv[0]);
        }
    }
    if (calls < 1 || calls > MAX_CALLS || warmup < 0 || warmup > MAX_CALLS || period_us < 0 ||
        period_us > 10000000 || burst_ms < 0 || burst_ms > 10000 || idle_ms < 1 ||
        idle_ms > 10000 || prio_server < 1 || prio_hog < 1 || prio_client < 1 ||
        prio_server > 255 || prio_hog > 255 || prio_client > 255) {
        return usage(argv[0]);
    }
    const qmp_transport_t *tr =
        tr_name != NULL ? qmp_transport_by_name(tr_name) : qmp_transport_default();
    if (tr == NULL || strcmp(tr->name, "fake") == 0) {
        fprintf(stderr, "bench: transport \"%s\" is not usable here (qnx or uds)\n",
                tr_name != NULL ? tr_name : "default");
        return 2;
    }
    if (label == NULL) {
        label = tr->name;
    }
    uint64_t *lat = (uint64_t *)malloc((size_t)calls * sizeof(*lat)); /* before any call */
    if (lat == NULL) {
        return 1;
    }

    static srv_ctx_t sc;
    sc.prio = (int)prio_server;
    sc.handler_prio_min = 1 << 30;
    demo_joint_v1_Joint_ops ops;
    memset(&ops, 0, sizeof(ops));
    ops.SetTarget = on_set_target;
    qmp_server_cfg_t scfg;
    memset(&scfg, 0, sizeof(scfg));
    scfg.transport = tr;
    scfg.nthreads = 1;
    scfg.listen_flags = fixed_priority ? QMP_LISTEN_FIXED_PRIORITY : 0;
    scfg.workbuf = g_workbuf;
    scfg.workbuf_size = sizeof(g_workbuf);
    scfg.topicbuf = g_topicbuf;
    scfg.topicbuf_size = sizeof(g_topicbuf);
    int rc = qmp_server_init(&sc.server, &scfg);
    if (rc == EOK) {
        rc = demo_joint_v1_Joint_register(&sc.server, 0, &ops, &sc);
    }
    if (rc != EOK) {
        fprintf(stderr, "bench: server setup failed: %d (%s)\n", rc, strerror(rc));
        return 1;
    }
    pthread_t srv_tid;
    if (pthread_create(&srv_tid, NULL, server_thread, &sc) != 0) {
        fprintf(stderr, "bench: cannot create the server thread\n");
        return 1;
    }

    qmp_client_t c;
    qmp_client_cfg_t ccfg;
    memset(&ccfg, 0, sizeof(ccfg));
    ccfg.transport = tr;
    ccfg.default_timeout_ns = 30ULL * 1000000000ULL;
    rc = ENOENT;
    for (int tries = 0; tries < 200 && rc != EOK; tries++) { /* the server thread is starting */
        rc = demo_joint_v1_Joint_connect(&c, 0, &ccfg);
        if (rc != EOK) {
            sleep_until(now_ns() + 10000000ULL);
        }
    }
    if (rc != EOK) {
        fprintf(stderr, "bench: connect failed: %d (%s)\n", rc, strerror(rc));
        return 1;
    }

    static hog_ctx_t hc;
    hc.prio = (int)prio_hog;
    hc.burst_ms = (uint32_t)burst_ms;
    hc.idle_ms = (uint32_t)idle_ms;
    pthread_t hog_tid;
    int hog = burst_ms > 0;
    if (hog && pthread_create(&hog_tid, NULL, hog_thread, &hc) != 0) {
        fprintf(stderr, "bench: cannot create the hog thread\n");
        return 1;
    }
    int client_prio_rc = set_self_priority((int)prio_client);
    if (client_prio_rc != 0 || sc.prio_rc != 0 || (hog && hc.prio_rc != 0)) {
        fprintf(stderr, "bench: could not set a thread priority (client %d, server %d, hog %d)\n",
                client_prio_rc, sc.prio_rc, hc.prio_rc);
        return 1;
    }

    demo_joint_v1_SetTargetRequest req = {0.1f, 1.0f};
    demo_joint_v1_SetTargetReply rep;
    int32_t status = 0;
    int failed = 0;
    uint64_t next = now_ns();
    const uint64_t t_start = next;
    uint32_t rng = 0x9E3779B9U; /* fixed seed: every run sees the same call schedule */
    for (long i = -warmup; i < calls; i++) {
        /* Period jittered uniformly over [0.5, 1.5) x PERIOD (xorshift32), so the calls
         * sample every phase of the hog's cycle instead of locking onto one. */
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;
        uint64_t period_ns = (uint64_t)period_us * 1000ULL;
        next += period_ns / 2U + (period_ns != 0 ? (uint64_t)rng % period_ns : 0U);
        sleep_until(next);
        uint64_t t0 = now_ns();
        rc = demo_joint_v1_Joint_SetTarget(&c, &req, &rep, &status);
        uint64_t dt = now_ns() - t0;
        if (rc != EOK || status != 0) {
            fprintf(stderr, "bench: call %ld failed rc=%d (%s) status=%" PRId32 "\n", i, rc,
                    strerror(rc), status);
            failed = 1;
            break;
        }
        if (i >= 0) {
            lat[i] = dt;
        }
        if (now_ns() > next) {
            next = now_ns(); /* a late call must not cause a burst of catch-up calls */
        }
    }
    const uint64_t elapsed_ns = now_ns() - t_start;

    hc.stop = 1;
    if (hog) {
        (void)pthread_join(hog_tid, NULL);
    }
    (void)qmp_client_close(&c);
    (void)qmp_server_stop(&sc.server);
    (void)pthread_join(srv_tid, NULL);
    (void)qmp_server_close(&sc.server);
    if (failed) {
        free(lat);
        return 1;
    }

    if (dump != NULL) {
        FILE *f = fopen(dump, "w");
        if (f == NULL) {
            fprintf(stderr, "bench: cannot write %s: %s\n", dump, strerror(errno));
        } else {
            for (long i = 0; i < calls; i++) {
                fprintf(f, "%" PRIu64 "\n", lat[i]);
            }
            (void)fclose(f);
        }
    }
    qsort(lat, (size_t)calls, sizeof(*lat), cmp_u64);
    long over_1ms = 0;
    for (long i = 0; i < calls; i++) {
        over_1ms += lat[i] > 1000000ULL;
    }
    printf(
        "result: label=%s version=%s build=%s transport=%s fixed_priority=%d calls=%ld "
        "warmup=%ld period_us=%ld hog_burst_ms=%ld hog_idle_ms=%ld prio_server=%ld "
        "prio_hog=%ld prio_client=%ld handler_prio_min=%d handler_prio_max=%d "
        "hog_bursts=%" PRIu64 " elapsed_ms=%" PRIu64 " min_us=" US_FMT " p50_us=" US_FMT
        " p90_us=" US_FMT " p99_us=" US_FMT " p999_us=" US_FMT " max_us=" US_FMT " over_1ms=%ld\n",
        label, qmp_build_version(), qmp_build_id(), tr->name, fixed_priority, calls, warmup,
        period_us, burst_ms, idle_ms, prio_server, prio_hog, prio_client, sc.handler_prio_min,
        sc.handler_prio_max, hc.bursts, (uint64_t)(elapsed_ns / 1000000ULL), US_ARGS(lat[0]),
        US_ARGS(pct(lat, calls, 500)), US_ARGS(pct(lat, calls, 900)), US_ARGS(pct(lat, calls, 990)),
        US_ARGS(pct(lat, calls, 999)), US_ARGS(lat[calls - 1]), over_1ms);
    free(lat);
    return 0;
}
