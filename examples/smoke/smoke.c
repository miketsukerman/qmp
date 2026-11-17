/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Target smoke tests for the native QNX transport: the scenarios the in-process fake
 * cannot exercise (two processes, kill -9, kernel timeouts, reconnect).
 *
 * One binary, two roles:
 *   qmp_smoke server                     run the smoke service until killed
 *   qmp_smoke <scenario>                 run one client scenario, exit 0 on PASS
 *
 * Scenarios (see scripts/target-smoke.sh for the full orchestration):
 *   timeout     call a method that never replies with a 200 ms timeout -> ETIMEDOUT,
 *               then verify the server still answers (the parked request was cancelled)
 *   block       block in a 60 s call; the harness kills this process with -9 and then
 *               checks the server survived the disconnect pulse
 *   echo        one echo round trip (post-kill health check)
 *   reconnect   call a slow method while the harness kills the server; expect ESRCH or
 *               ETIMEDOUT, reconnect with backoff to the restarted server, expect a new
 *               generation and a working echo
 *   race        deferred reply racing a client timeout, both orderings
 *   saturate    two Slow calls occupy both server workers; a third client's call with a
 *               1000 ms timeout must stay SEND-blocked and return ETIMEDOUT at ~1000 ms
 *               (the kernel timeout, not the server, ends it); then the two slow calls
 *               complete and the server still answers
 */
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <qmp/qmp_client.h>
#include <qmp/qmp_server.h>
#include <qmp/qmp_transport.h>

#define SMOKE_SVC    0x0505U
#define SMOKE_M_ECHO 0x0001U
#define SMOKE_M_SLOW 0x0002U /* replies after req.delay_ms */
#define SMOKE_M_PARK 0x0003U /* defers and never replies   */
#define SMOKE_M_RACE 0x0004U /* defers; replies after req.delay_ms from a helper */

typedef struct smoke_req {
    int32_t value;
    int32_t delay_ms;
} smoke_req_t;

typedef struct smoke_rep {
    int32_t value;
    int32_t _pad0;
} smoke_rep_t;

static void msleep(int32_t ms) {
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
    (void)nanosleep(&ts, NULL);
}

/* ---- server ---------------------------------------------------------------- */

typedef struct race_box {
    pthread_mutex_t mu;
    qmp_req_t *req;
    int32_t value;
    int32_t delay_ms;
} race_box_t;

static race_box_t g_race = {PTHREAD_MUTEX_INITIALIZER, NULL, 0, 0};

static int h_echo(const void *ops, void *ctx, qmp_req_t *req, const void *in, void *out,
                  int32_t *status) {
    (void)ops;
    (void)ctx;
    (void)req;
    ((smoke_rep_t *)out)->value = ((const smoke_req_t *)in)->value + 1;
    *status = 0;
    return QMP_HANDLED;
}

static int h_slow(const void *ops, void *ctx, qmp_req_t *req, const void *in, void *out,
                  int32_t *status) {
    (void)ops;
    (void)ctx;
    (void)req;
    const smoke_req_t *r = (const smoke_req_t *)in;
    msleep(r->delay_ms);
    ((smoke_rep_t *)out)->value = r->value + 1;
    *status = 0;
    return QMP_HANDLED;
}

static int h_park(const void *ops, void *ctx, qmp_req_t *req, const void *in, void *out,
                  int32_t *status) {
    (void)ops;
    (void)ctx;
    (void)req;
    (void)in;
    (void)out;
    (void)status;
    return QMP_DEFERRED; /* never replied; cancelled by unblock or disconnect */
}

static int h_race(const void *ops, void *ctx, qmp_req_t *req, const void *in, void *out,
                  int32_t *status) {
    (void)ops;
    (void)ctx;
    (void)out;
    (void)status;
    const smoke_req_t *r = (const smoke_req_t *)in;
    (void)pthread_mutex_lock(&g_race.mu);
    g_race.req = req;
    g_race.value = r->value;
    g_race.delay_ms = r->delay_ms;
    (void)pthread_mutex_unlock(&g_race.mu);
    return QMP_DEFERRED;
}

static void *race_completer(void *arg) {
    (void)arg;
    for (;;) {
        (void)pthread_mutex_lock(&g_race.mu);
        qmp_req_t *req = g_race.req;
        int32_t value = g_race.value;
        int32_t delay = g_race.delay_ms;
        g_race.req = NULL;
        (void)pthread_mutex_unlock(&g_race.mu);
        if (req == NULL) {
            msleep(5);
            continue;
        }
        msleep(delay);
        smoke_rep_t rep = {value + 1, 0};
        int rc = qmp_req_reply(req, &rep, sizeof(rep), 0);
        /* EINVAL: the client timeout cancelled the request first. Both are fine. */
        fprintf(stderr, "smoke: race reply after %" PRId32 " ms -> %d\n", delay, rc);
    }
    return NULL;
}

static void on_cancel(void *ctx, qmp_req_t *req) {
    (void)ctx;
    (void)pthread_mutex_lock(&g_race.mu);
    if (g_race.req == req) {
        g_race.req = NULL; /* do not reply a request that is being cancelled */
    }
    (void)pthread_mutex_unlock(&g_race.mu);
    fprintf(stderr, "smoke: request cancelled\n");
}

static void on_client_gone(void *ctx, int32_t pid) {
    (void)ctx;
    fprintf(stderr, "smoke: client %" PRId32 " gone\n", pid);
}

static const qmp_method_desc_t g_methods[] = {
    {SMOKE_M_ECHO, 0, sizeof(smoke_req_t), sizeof(smoke_rep_t), 0, NULL, h_echo, "Echo"},
    {SMOKE_M_SLOW, 0, sizeof(smoke_req_t), sizeof(smoke_rep_t), 0, NULL, h_slow, "Slow"},
    {SMOKE_M_PARK, 0, sizeof(smoke_req_t), sizeof(smoke_rep_t), 0, NULL, h_park, "Park"},
    {SMOKE_M_RACE, 0, sizeof(smoke_req_t), sizeof(smoke_rep_t), 0, NULL, h_race, "Race"},
};

static const qmp_service_desc_t g_desc = {SMOKE_SVC,      1,         0, QMP_HDR_SIZE + 16,
                                          "qmp.smoke.v1", g_methods, 4};

#define SMOKE_THREADS 2
static uint64_t g_workbuf[QMP_SERVER_WORKBUF_SIZE(QMP_HDR_SIZE + 16, SMOKE_THREADS) / 8 + 1];
static qmp_req_t g_pool[4];
static int g_dummy_ops;

static int run_server(void) {
    qmp_server_t server;
    qmp_server_cfg_t cfg = {.transport = NULL, /* native transport on QNX */
                            .nthreads = SMOKE_THREADS,
                            .workbuf = g_workbuf,
                            .workbuf_size = sizeof(g_workbuf),
                            .deferred_pool = g_pool,
                            .deferred_count = 4,
                            .on_cancel = on_cancel,
                            .on_client_gone = on_client_gone};
    int rc = qmp_server_init(&server, &cfg);
    if (rc == EOK) {
        rc = qmp_server_register(&server, &g_desc, 0, &g_dummy_ops, NULL);
    }
    if (rc != EOK) {
        fprintf(stderr, "smoke: server setup failed: %d (%s)\n", rc, strerror(rc));
        return 1;
    }
    pthread_t helper;
    (void)pthread_create(&helper, NULL, race_completer, NULL);
    printf("READY pid=%d generation=%" PRIu32 "\n", (int)getpid(), qmp_server_generation(&server));
    (void)fflush(stdout);
    (void)qmp_server_run(&server); /* until killed */
    return 0;
}

/* ---- client scenarios ------------------------------------------------------ */

#define CHECK(what, cond)                                                                          \
    do {                                                                                           \
        if (cond) {                                                                                \
            printf("ok   - %s\n", what);                                                           \
        } else {                                                                                   \
            printf("FAIL - %s\n", what);                                                           \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

static int smoke_connect(qmp_client_t *c) {
    qmp_client_cfg_t cfg = {.transport = NULL,
                            .default_timeout_ns = 5ULL * 1000 * 1000 * 1000,
                            .reconnect_initial_ms = 50,
                            .reconnect_max_ms = 1000,
                            .reconnect_attempts = 20};
    int rc = qmp_client_init(c, &cfg);
    if (rc != EOK) {
        return rc;
    }
    return qmp_client_connect(c, SMOKE_SVC, 0, 1, 0);
}

static int call_echo(qmp_client_t *c, const qmp_call_opts_t *opts) {
    smoke_req_t in = {41, 0};
    smoke_rep_t out = {0, 0};
    int32_t st = 0;
    int rc = qmp_client_call(c, SMOKE_M_ECHO, &in, sizeof(in), &out, sizeof(out), &st, opts);
    if (rc != EOK) {
        return rc;
    }
    return (st == 0 && out.value == 42) ? EOK : EPROTO;
}

static int sc_echo(void) {
    qmp_client_t c;
    CHECK("connect", smoke_connect(&c) == EOK);
    CHECK("echo round trip", call_echo(&c, NULL) == EOK);
    (void)qmp_client_close(&c);
    return 0;
}

static int sc_timeout(void) {
    qmp_client_t c;
    CHECK("connect", smoke_connect(&c) == EOK);
    smoke_req_t in = {1, 0};
    smoke_rep_t out = {0, 0};
    int32_t st = 0;
    qmp_call_opts_t opts = {.timeout_ns = 200ULL * 1000 * 1000};
    int rc = qmp_client_call(&c, SMOKE_M_PARK, &in, sizeof(in), &out, sizeof(out), &st, &opts);
    printf("# park call returned %d (%s)\n", rc, strerror(rc));
    CHECK("never-replied call times out", rc == ETIMEDOUT);
    CHECK("server healthy after cancel", call_echo(&c, NULL) == EOK);
    (void)qmp_client_close(&c);
    return 0;
}

static int sc_block(void) {
    qmp_client_t c;
    if (smoke_connect(&c) != EOK) {
        return 1;
    }
    printf("BLOCKING pid=%d\n", (int)getpid());
    (void)fflush(stdout);
    smoke_req_t in = {1, 60000};
    smoke_rep_t out = {0, 0};
    int32_t st = 0;
    qmp_call_opts_t opts = {.timeout_ns = 0}; /* block forever; harness kills us */
    (void)qmp_client_call(&c, SMOKE_M_SLOW, &in, sizeof(in), &out, sizeof(out), &st, &opts);
    return 1; /* we are supposed to die before this returns */
}

static int sc_reconnect(void) {
    qmp_client_t c;
    CHECK("connect", smoke_connect(&c) == EOK);
    uint32_t gen1 = qmp_client_server_generation(&c);
    printf("# generation before: %" PRIu32 "\n", gen1);
    smoke_req_t in = {1, 10000};
    smoke_rep_t out = {0, 0};
    int32_t st = 0;
    qmp_call_opts_t opts = {.timeout_ns = 8ULL * 1000 * 1000 * 1000};
    /* The harness kills the server while we are REPLY-blocked here. */
    int rc = qmp_client_call(&c, SMOKE_M_SLOW, &in, sizeof(in), &out, sizeof(out), &st, &opts);
    printf("# call during server kill returned %d (%s)\n", rc, strerror(rc));
    CHECK("call fails when the server dies", rc == ESRCH || rc == EBADF || rc == ETIMEDOUT);
    if (rc == ETIMEDOUT) {
        /* Timed out before noticing the death: the next call surfaces it. */
        rc = call_echo(&c, NULL);
        printf("# follow-up call returned %d (%s)\n", rc, strerror(rc));
        CHECK("follow-up fails on the dead server", rc == ESRCH || rc == EBADF);
    }
    CHECK("client marked disconnected", !qmp_client_is_connected(&c));
    rc = qmp_client_reconnect(&c);
    printf("# reconnect returned %d (%s)\n", rc, strerror(rc));
    CHECK("reconnect to the restarted server", rc == EOK);
    uint32_t gen2 = qmp_client_server_generation(&c);
    printf("# generation after: %" PRIu32 "\n", gen2);
    CHECK("generation changed after restart", gen2 != gen1);
    CHECK("echo on the new session", call_echo(&c, NULL) == EOK);
    (void)qmp_client_close(&c);
    return 0;
}

/* One Slow call on its own connection, run on a helper thread by sc_saturate. */
typedef struct slow_job {
    qmp_client_t *c;
    int32_t delay_ms;
    int rc;
    int32_t value;
} slow_job_t;

static void *slow_job_main(void *arg) {
    slow_job_t *j = (slow_job_t *)arg;
    smoke_req_t in = {7, j->delay_ms};
    smoke_rep_t out = {0, 0};
    int32_t st = 0;
    qmp_call_opts_t opts = {.timeout_ns = 10ULL * 1000 * 1000 * 1000};
    j->rc = qmp_client_call(j->c, SMOKE_M_SLOW, &in, sizeof(in), &out, sizeof(out), &st, &opts);
    if (j->rc == EOK && st != 0) {
        j->rc = EPROTO;
    }
    j->value = out.value;
    return NULL;
}

static uint64_t mono_ms(void) {
    struct timespec ts;
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000U + (uint64_t)ts.tv_nsec / 1000000U;
}

static int sc_saturate(void) {
    /* Connect all three first: the HELLO handshake itself needs a free worker. */
    qmp_client_t busy[SMOKE_THREADS];
    qmp_client_t third;
    for (int k = 0; k < SMOKE_THREADS; k++) {
        CHECK("connect busy client", smoke_connect(&busy[k]) == EOK);
    }
    CHECK("connect third client", smoke_connect(&third) == EOK);

    slow_job_t jobs[SMOKE_THREADS];
    pthread_t th[SMOKE_THREADS];
    for (int k = 0; k < SMOKE_THREADS; k++) {
        jobs[k] = (slow_job_t){&busy[k], 3000, -1, 0};
        CHECK("start slow call", pthread_create(&th[k], NULL, slow_job_main, &jobs[k]) == 0);
    }
    msleep(300); /* both workers are inside h_slow now */

    const uint64_t timeout_ms = 1000;
    qmp_call_opts_t opts = {.timeout_ns = timeout_ms * 1000 * 1000};
    uint64_t t0 = mono_ms();
    int rc = call_echo(&third, &opts);
    uint64_t dt = mono_ms() - t0;
    printf("# third call with every worker busy returned %d (%s) after %" PRIu64 " ms\n", rc,
           strerror(rc), dt);
    CHECK("third call times out", rc == ETIMEDOUT);
    /* Lower bound: a few ms of clock slack. Upper bound: well before the slow calls end
     * (~3300 ms), so it was the timeout and not a freed worker that ended the call. */
    CHECK("third call returned at ~1000 ms", dt + 5 >= timeout_ms && dt < timeout_ms + 250);

    for (int k = 0; k < SMOKE_THREADS; k++) {
        (void)pthread_join(th[k], NULL);
        printf("# slow call %d returned %d (%s) v=%" PRId32 "\n", k, jobs[k].rc,
               strerror(jobs[k].rc), jobs[k].value);
        CHECK("slow call completes", jobs[k].rc == EOK && jobs[k].value == 8);
    }
    CHECK("server healthy afterwards", call_echo(&third, NULL) == EOK);
    for (int k = 0; k < SMOKE_THREADS; k++) {
        (void)qmp_client_close(&busy[k]);
    }
    (void)qmp_client_close(&third);
    return 0;
}

static int sc_race(void) {
    qmp_client_t c;
    CHECK("connect", smoke_connect(&c) == EOK);
    int32_t st = 0;
    smoke_rep_t out = {0, 0};
    /* 1: the timeout wins; the late qmp_req_reply must hit a cancelled request. */
    smoke_req_t in = {1, 300};
    qmp_call_opts_t opts = {.timeout_ns = 100ULL * 1000 * 1000};
    int rc = qmp_client_call(&c, SMOKE_M_RACE, &in, sizeof(in), &out, sizeof(out), &st, &opts);
    printf("# race (timeout first) returned %d (%s)\n", rc, strerror(rc));
    CHECK("timeout wins the race", rc == ETIMEDOUT);
    msleep(400); /* let the completer fire into the cancelled slot */
    CHECK("server survives the late reply", call_echo(&c, NULL) == EOK);
    /* 2: the reply wins. */
    in.delay_ms = 50;
    opts.timeout_ns = 2ULL * 1000 * 1000 * 1000;
    out.value = 0;
    rc = qmp_client_call(&c, SMOKE_M_RACE, &in, sizeof(in), &out, sizeof(out), &st, &opts);
    printf("# race (reply first) returned %d st=%" PRId32 " v=%" PRId32 "\n", rc, st, out.value);
    CHECK("deferred reply wins the race", rc == EOK && st == 0 && out.value == 2);
    (void)qmp_client_close(&c);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s server|echo|timeout|block|reconnect|race|saturate\n", argv[0]);
        return 2;
    }
    const char *mode = argv[1];
    int rc;
    if (strcmp(mode, "server") == 0) {
        rc = run_server();
    } else if (strcmp(mode, "echo") == 0) {
        rc = sc_echo();
    } else if (strcmp(mode, "timeout") == 0) {
        rc = sc_timeout();
    } else if (strcmp(mode, "block") == 0) {
        rc = sc_block();
    } else if (strcmp(mode, "reconnect") == 0) {
        rc = sc_reconnect();
    } else if (strcmp(mode, "race") == 0) {
        rc = sc_race();
    } else if (strcmp(mode, "saturate") == 0) {
        rc = sc_saturate();
    } else {
        fprintf(stderr, "unknown scenario: %s\n", mode);
        return 2;
    }
    if (strcmp(mode, "server") != 0 && strcmp(mode, "block") != 0) {
        printf("%s %s\n", rc == 0 ? "PASS" : "FAIL", mode);
    }
    return rc;
}
