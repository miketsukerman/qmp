/* SPDX-License-Identifier: Apache-2.0 */
/* Server lifecycle and worker threads (DESIGN §5.1). */
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
    /* Generation changes on every restart so clients can detect it (DESIGN §3.6). Low
     * bits of the monotonic clock in ms mixed with the object address; never 0. */
    struct timespec ts;
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    uint32_t gen = (uint32_t)ts.tv_sec * 1000U + (uint32_t)(ts.tv_nsec / 1000000);
    gen ^= (uint32_t)(uintptr_t)si;
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
    need = (need + 7U) & ~7U;
    if (si->workbuf_size < QMP_SERVER_WORKBUF_SIZE(need, si->nthreads)) {
        return EMSGSIZE;
    }
    int rc = qmp_service_path(si->path, sizeof(si->path), desc->service_id, instance_id,
                              desc->ver_major);
    if (rc != EOK) {
        return rc;
    }
    rc = si->tr->listen(&si->ep, si->path);
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

typedef struct worker_arg {
    qmp_server_impl_t *s;
    unsigned index;
} worker_arg_t;

static void worker_loop(qmp_server_impl_t *s, unsigned index) {
    uint8_t *rcv = s->workbuf + (size_t)index * 2U * s->max_msg_size;
    uint8_t *rep = rcv + s->max_msg_size;
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
        if (info.kind != QMP_RCV_MESSAGE) {
            continue; /* pulses / lifecycle: W2 */
        }
        qmp_server_dispatch(s, &info, rcv, info.msglen, rep, s->max_msg_size);
    }
}

static void *worker_main(void *arg) {
    worker_arg_t *wa = (worker_arg_t *)arg;
    qmp_server_impl_t *s = wa->s;
    unsigned index = wa->index;
    worker_loop(s, index);
    return NULL;
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
