/* SPDX-License-Identifier: Apache-2.0 */
/* Server lifecycle and worker threads (DESIGN §5.1). */
#include <sched.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>

#include "internal.h"

static qmp_server_impl_t *impl(qmp_server_t *s) {
    return (qmp_server_impl_t *)(void *)s;
}

int qmp_server_init(qmp_server_t *s, const qmp_server_cfg_t *cfg) {
    if (s == NULL || cfg == NULL) {
        return EINVAL;
    }
    if (cfg->nthreads < 0 || cfg->nthreads > QMP_SERVER_MAX_THREADS) {
        return EINVAL;
    }
    if (cfg->workbuf == NULL || cfg->workbuf_size == 0 || ((uintptr_t)cfg->workbuf & 7U) != 0) {
        return EINVAL;
    }
    qmp_server_impl_t *si = impl(s);
    memset(si, 0, sizeof(*si));
    si->tr = cfg->transport != NULL ? cfg->transport : qmp_transport_default();
    if (si->tr == NULL) {
        return EINVAL;
    }
    si->nthreads = (uint8_t)(cfg->nthreads == 0 ? 4 : cfg->nthreads);
    si->workbuf = (uint8_t *)cfg->workbuf;
    si->workbuf_size = cfg->workbuf_size;
    if (cfg->deferred_pool != NULL) {
        if (cfg->deferred_count == 0 || ((uintptr_t)cfg->deferred_pool & 7U) != 0) {
            return EINVAL;
        }
        memset(cfg->deferred_pool, 0, cfg->deferred_count * sizeof(qmp_req_t));
        si->pool = (qmp_req_impl_t *)(void *)cfg->deferred_pool;
        si->pool_count = cfg->deferred_count;
    }
    si->acl_check = cfg->acl_check;
    si->on_cancel = cfg->on_cancel;
    si->on_client_gone = cfg->on_client_gone;
    si->listen_flags = cfg->listen_flags;
    si->verbose = cfg->verbose != 0;
    if (cfg->topicbuf != NULL) {
        if (cfg->topicbuf_size == 0 || ((uintptr_t)cfg->topicbuf & 7U) != 0) {
            return EINVAL;
        }
        si->topicbuf = (uint8_t *)cfg->topicbuf;
        si->topicbuf_size = cfg->topicbuf_size;
    }
    int mrc = pthread_mutex_init(&si->pool_mu, NULL);
    if (mrc != 0) {
        return mrc;
    }
    mrc = pthread_mutex_init(&si->topic_mu, NULL);
    if (mrc != 0) {
        return mrc;
    }
    /* Generation changes on every restart so clients can detect it (DESIGN §3.6). Low
     * bits of the monotonic clock in ms mixed with the object address and a process-wide
     * counter (back-to-back restarts land in the same millisecond); never 0. */
    static atomic_uint g_gen_seq;
    struct timespec ts;
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    uint32_t gen = (uint32_t)ts.tv_sec * 1000U + (uint32_t)(ts.tv_nsec / 1000000);
    gen ^= (uint32_t)(uintptr_t)si;
    gen += (atomic_fetch_add(&g_gen_seq, 1U) + 1U) << 20;
    si->generation = gen != 0 ? gen : 1U;
    return EOK;
}

int qmp_server_register(qmp_server_t *s, const qmp_service_desc_t *desc, uint16_t instance_id,
                        const void *ops, void *ctx) {
    if (s == NULL || desc == NULL || ops == NULL || desc->methods == NULL) {
        return EINVAL;
    }
    qmp_server_impl_t *si = impl(s);
    if (si->registered) {
        return EEXIST;
    }
    for (size_t i = 1; i < desc->nmethods; i++) {
        if (desc->methods[i - 1].id >= desc->methods[i].id) {
            return EINVAL; /* generated tables are sorted; reject hand-written junk */
        }
    }
    uint32_t need = desc->max_msg_size;
    if (need < QMP_HDR_SIZE + sizeof(qmp_hello_reply_t)) {
        need = QMP_HDR_SIZE + sizeof(qmp_hello_reply_t);
    }
    for (size_t i = 0; i < desc->ntopics; i++) {
        /* PULL replies carry qmp_pull_reply_t in front of the sample. */
        uint32_t m =
            QMP_HDR_SIZE + (uint32_t)sizeof(qmp_pull_reply_t) + desc->topics[i].sample_size;
        if (need < m) {
            need = m;
        }
    }
    if (desc->ntopics > 0 && need < QMP_HDR_SIZE + sizeof(qmp_subscribe_t)) {
        need = QMP_HDR_SIZE + sizeof(qmp_subscribe_t);
    }
    need = (need + 7U) & ~7U;
    if (si->workbuf_size < QMP_SERVER_WORKBUF_SIZE(need, si->nthreads)) {
        return EMSGSIZE;
    }
    si->desc = desc;
    int rc = qmp_topics_init(si);
    if (rc != EOK) {
        si->desc = NULL;
        return rc;
    }
    rc = qmp_service_path(si->path, sizeof(si->path), desc->service_id, instance_id,
                          desc->ver_major);
    if (rc != EOK) {
        return rc;
    }
    rc = si->tr->listen(&si->ep, si->path, si->listen_flags);
    if (rc != EOK) {
        return rc;
    }
    si->desc = desc;
    si->ops = ops;
    si->ctx = ctx;
    si->instance_id = instance_id;
    si->max_msg_size = need;
    si->registered = 1;
    si->listening = 1;
    return EOK;
}

void qmp_worker_restore_priority(const qmp_inflight_t *infl) {
    if (infl == NULL || infl->base_prio == 0) {
        return;
    }
    struct sched_param sp;
    int policy = 0;
    if (pthread_getschedparam(pthread_self(), &policy, &sp) != 0 ||
        sp.sched_priority == (int)infl->base_prio) {
        return;
    }
    memset(&sp, 0, sizeof(sp));
    sp.sched_priority = (int)infl->base_prio;
    /* Best effort: lowering is always allowed; raising back (a caller below the base) can
     * be refused without privileges, and then the handler simply runs at that priority. */
    (void)pthread_setschedparam(pthread_self(), (int)infl->base_policy, &sp);
}

typedef struct worker_arg {
    qmp_server_impl_t *s;
    unsigned index;
} worker_arg_t;

static void worker_loop(qmp_server_impl_t *s, unsigned index) {
    uint8_t *rcv = s->workbuf + (size_t)index * 2U * s->max_msg_size;
    uint8_t *rep = rcv + s->max_msg_size;
    qmp_inflight_t *infl = &s->inflight[index];
    struct sched_param sp;
    int policy = 0;
    infl->base_prio = 0;
    if (pthread_getschedparam(pthread_self(), &policy, &sp) == 0 && sp.sched_priority > 0 &&
        sp.sched_priority <= UINT8_MAX && policy >= 0 && policy <= UINT8_MAX) {
        infl->base_prio = (uint8_t)sp.sched_priority;
        infl->base_policy = (uint8_t)policy;
    }
    for (;;) {
        qmp_rcv_info_t info;
        memset(&info, 0, sizeof(info));
        int rc = s->tr->receive(&s->ep, rcv, s->max_msg_size, &info);
        if (rc == ECANCELED) {
            return;
        }
        if (rc != EOK) {
            qmp_log("receive failed: %d", rc);
            continue;
        }
        if (info.kind == QMP_RCV_MESSAGE) {
            qmp_server_dispatch(s, &info, rcv, info.msglen, rep, s->max_msg_size, infl);
        } else if (info.kind == QMP_RCV_UNBLOCK) {
            qmp_server_on_unblock(s, info.rcvid);
        } else if (info.kind == QMP_RCV_DISCONNECT) {
            qmp_server_on_disconnect(s, info.scoid, info.pid);
        } /* QMP_RCV_PULSE and unknown kinds: ignore */
    }
}

static void *worker_main(void *arg) {
    worker_arg_t *wa = (worker_arg_t *)arg;
    qmp_server_impl_t *s = wa->s;
    unsigned index = wa->index;
    worker_loop(s, index);
    return NULL;
}

/* Dedicated pulse thread: receives only lifecycle pulses (QNX: MsgReceivePulse) so
 * UNBLOCK/DISCONNECT are served even when every worker is busy inside a handler
 * (V-QNX-038). Runs at the caller's scheduling attributes, like the workers. */
static void *pulse_main(void *arg) {
    qmp_server_impl_t *s = (qmp_server_impl_t *)arg;
    for (;;) {
        qmp_rcv_info_t info;
        memset(&info, 0, sizeof(info));
        int rc = s->tr->receive_pulse(&s->ep, &info);
        if (rc == ECANCELED) {
            return NULL;
        }
        if (rc != EOK) {
            qmp_log("receive_pulse failed: %d", rc);
            continue;
        }
        if (info.kind == QMP_RCV_UNBLOCK) {
            qmp_server_on_unblock(s, info.rcvid);
        } else if (info.kind == QMP_RCV_DISCONNECT) {
            qmp_server_on_disconnect(s, info.scoid, info.pid);
        } /* other pulses: ignore */
    }
}

int qmp_server_run(qmp_server_t *s) {
    if (s == NULL) {
        return EINVAL;
    }
    qmp_server_impl_t *si = impl(s);
    if (!si->registered) {
        return ENOTCONN;
    }
    if (si->running) {
        return EBUSY;
    }
    si->running = 1;
    si->has_pulse_thr = 0;
    if (si->tr->receive_pulse != NULL) {
        if (pthread_create(&si->pulse_thread, NULL, pulse_main, si) == 0) {
            si->has_pulse_thr = 1;
        } else {
            qmp_log("pulse thread create failed; pulses are served by idle workers only");
        }
    }
    worker_arg_t args[QMP_SERVER_MAX_THREADS];
    unsigned started = 0;
    for (unsigned i = 1; i < si->nthreads; i++) {
        args[i].s = si;
        args[i].index = i;
        /* Workers inherit the caller's scheduling attributes (pthread default). */
        int rc = pthread_create(&si->workers[i], NULL, worker_main, &args[i]);
        if (rc != 0) {
            qmp_log("pthread_create failed: %d", rc);
            break;
        }
        started = i;
    }
    worker_loop(si, 0);
    for (unsigned i = 1; i <= started; i++) {
        (void)pthread_join(si->workers[i], NULL);
    }
    if (si->has_pulse_thr) {
        (void)pthread_join(si->pulse_thread, NULL);
        si->has_pulse_thr = 0;
    }
    si->running = 0;
    return EOK;
}

int qmp_server_stop(qmp_server_t *s) {
    if (s == NULL) {
        return EINVAL;
    }
    qmp_server_impl_t *si = impl(s);
    if (!si->listening) {
        return ENOTCONN;
    }
    return si->tr->unblock(&si->ep);
}

int qmp_server_close(qmp_server_t *s) {
    if (s == NULL) {
        return EINVAL;
    }
    qmp_server_impl_t *si = impl(s);
    if (si->running) {
        return EBUSY;
    }
    int rc = EOK;
    if (si->listening) {
        rc = si->tr->close(&si->ep);
        si->listening = 0;
    }
    si->registered = 0;
    return rc;
}

uint32_t qmp_server_generation(const qmp_server_t *s) {
    return s != NULL ? ((const qmp_server_impl_t *)(const void *)s)->generation : 0;
}
