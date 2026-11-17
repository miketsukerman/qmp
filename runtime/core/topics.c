/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Server-side topics (DESIGN §3.5, ADR-003 notify-then-pull).
 *
 * All state lives in the caller-provided topicbuf: a bounded subscription table shared
 * by every topic, one ring header per topic and the preallocated ring slots. topic_mu
 * guards the table and the rings; it is NEVER held across the transport deliver() call
 * (a kernel call that must not run under a lock) — publish snapshots the subscribers
 * onto the stack first. No heap and no logging on the publish or pull path.
 */
#include <string.h>

#include "internal.h"

/* topicbuf layout: [sub table][ring header array][topic 0 slots][topic 1 slots]... */
int qmp_topics_init(qmp_server_impl_t *s) {
    const qmp_service_desc_t *d = s->desc;
    if (d->ntopics == 0) {
        return EOK; /* no topics: topicbuf may be NULL */
    }
    if (d->topics == NULL) {
        return EINVAL;
    }
    if (s->topicbuf == NULL) {
        return EOK; /* topics declared but not configured: SUBSCRIBE/publish get ENOTSUP */
    }
    if (((uintptr_t)s->topicbuf & 7U) != 0) {
        return EINVAL;
    }
    size_t need = QMP_SERVER_SUBTABLE_SIZE + d->ntopics * QMP_TOPIC_HEADER_SIZE;
    for (size_t i = 0; i < d->ntopics; i++) {
        const qmp_topic_desc_t *t = &d->topics[i];
        if (t->id < 0x8000U || (i > 0 && d->topics[i - 1].id >= t->id)) {
            return EINVAL; /* sorted, topic range */
        }
        if (t->mode == QMP_TOPIC_QUEUE ? t->depth == 0 : t->mode != QMP_TOPIC_LATEST) {
            return EINVAL;
        }
        uint32_t nslots = t->mode == QMP_TOPIC_QUEUE ? t->depth : 1U;
        need += (size_t)nslots * QMP_TOPIC_SLOT_SIZE(t->sample_size);
    }
    if (s->topicbuf_size < need) {
        return EMSGSIZE;
    }
    memset(s->topicbuf, 0, need);
    s->subs = (qmp_sub_entry_t *)(void *)s->topicbuf;
    /* QMP_SUB_ENTRY_SIZE-strided table: index with a byte offset, not the struct size. */
    uint8_t *p = s->topicbuf + QMP_SERVER_SUBTABLE_SIZE;
    s->tstates = (qmp_topic_state_t *)(void *)p;
    uint8_t *slots = p + d->ntopics * QMP_TOPIC_HEADER_SIZE;
    for (size_t i = 0; i < d->ntopics; i++) {
        const qmp_topic_desc_t *t = &d->topics[i];
        qmp_topic_state_t *ts = &s->tstates[i];
        ts->next_seq = 1;
        ts->nslots = t->mode == QMP_TOPIC_QUEUE ? t->depth : 1U;
        ts->slot_stride = (uint32_t)QMP_TOPIC_SLOT_SIZE(t->sample_size);
        ts->slots = slots;
        slots += (size_t)ts->nslots * ts->slot_stride;
    }
    return EOK;
}

static qmp_sub_entry_t *sub_at(const qmp_server_impl_t *s, unsigned i) {
    return (qmp_sub_entry_t *)(void *)((uint8_t *)s->subs + (size_t)i * QMP_SUB_ENTRY_SIZE);
}

int qmp_topic_find(const qmp_server_impl_t *s, uint16_t topic_id, uint32_t *idx) {
    if (s->subs == NULL) {
        return ENOTSUP; /* no topic storage configured */
    }
    for (size_t i = 0; i < s->desc->ntopics; i++) {
        if (s->desc->topics[i].id == topic_id) {
            *idx = (uint32_t)i;
            return EOK;
        }
    }
    return ENOENT;
}

int qmp_topic_sub_add(qmp_server_impl_t *s, uint32_t idx, const qmp_rcv_info_t *info,
                      const qmp_subscribe_t *req, qmp_subscribe_reply_t *rep) {
    const qmp_topic_desc_t *t = &s->desc->topics[idx];
    if (req->event_len > QMP_EVENT_BLOB_MAX) {
        return EINVAL;
    }
    int rc = EAGAIN; /* table full: bounded by design, an explicit error */
    (void)pthread_mutex_lock(&s->topic_mu);
    for (unsigned i = 0; i < QMP_SERVER_MAX_SUBS; i++) {
        qmp_sub_entry_t *e = sub_at(s, i);
        if (e->sub_id != 0) {
            continue;
        }
        if (++s->sub_id_seq == 0) {
            ++s->sub_id_seq;
        }
        e->sub_id = s->sub_id_seq;
        e->rcvid = info->rcvid;
        e->scoid = info->scoid;
        e->pid = info->pid;
        e->topic_idx = (uint16_t)idx;
        e->event_len = req->event_len;
        memcpy(e->event, req->event, req->event_len);
        rep->sub_id = e->sub_id;
        rep->mode = t->mode;
        rep->depth = t->mode == QMP_TOPIC_QUEUE ? t->depth : 1U;
        rep->seq = s->tstates[idx].next_seq - 1;
        rc = EOK;
        break;
    }
    (void)pthread_mutex_unlock(&s->topic_mu);
    return rc;
}

int qmp_topic_sub_remove(qmp_server_impl_t *s, uint32_t sub_id, int32_t scoid) {
    if (s->subs == NULL || sub_id == 0) {
        return ENOENT;
    }
    int rc = ENOENT;
    (void)pthread_mutex_lock(&s->topic_mu);
    for (unsigned i = 0; i < QMP_SERVER_MAX_SUBS; i++) {
        qmp_sub_entry_t *e = sub_at(s, i);
        if (e->sub_id == sub_id && e->scoid == scoid) {
            memset(e, 0, sizeof(*e));
            rc = EOK;
            break;
        }
    }
    (void)pthread_mutex_unlock(&s->topic_mu);
    return rc;
}

void qmp_topic_drop_scoid(qmp_server_impl_t *s, int32_t scoid) {
    if (s->subs == NULL) {
        return;
    }
    (void)pthread_mutex_lock(&s->topic_mu);
    for (unsigned i = 0; i < QMP_SERVER_MAX_SUBS; i++) {
        qmp_sub_entry_t *e = sub_at(s, i);
        if (e->sub_id != 0 && e->scoid == scoid) {
            memset(e, 0, sizeof(*e));
        }
    }
    (void)pthread_mutex_unlock(&s->topic_mu);
}

int qmp_topic_pull(qmp_server_impl_t *s, uint32_t idx, uint64_t after_seq, qmp_pull_reply_t *rep,
                   void *sample_out) {
    const qmp_topic_desc_t *t = &s->desc->topics[idx];
    qmp_topic_state_t *ts = &s->tstates[idx];
    memset(rep, 0, sizeof(*rep));
    (void)pthread_mutex_lock(&s->topic_mu);
    uint64_t newest = ts->next_seq - 1;
    if (after_seq >= newest) {
        /* Nothing new. after_seq beyond newest can only mean a stale cursor from a
         * previous server generation; report no data, the client resets on reconnect. */
        (void)pthread_mutex_unlock(&s->topic_mu);
        return EOK;
    }
    uint64_t oldest = newest >= ts->nslots ? newest - ts->nslots + 1 : 1;
    uint64_t want = after_seq + 1;
    if (want < oldest) {
        want = oldest; /* lost updates are counted, never hidden */
    }
    uint8_t *slot = ts->slots + (size_t)((want - 1) % ts->nslots) * ts->slot_stride;
    rep->seq = want;
    rep->dropped = want - after_seq - 1;
    rep->remaining = (uint32_t)(newest - want);
    rep->count = 1;
    memcpy(sample_out, slot + 8, t->sample_size);
    (void)pthread_mutex_unlock(&s->topic_mu);
    return EOK;
}

int qmp_server_publish(qmp_server_t *s_, uint16_t topic_id, const void *sample, size_t len) {
    if (s_ == NULL || (sample == NULL && len != 0)) {
        return EINVAL;
    }
    qmp_server_impl_t *s = (qmp_server_impl_t *)(void *)s_;
    if (!s->registered) {
        return ENOTCONN;
    }
    if (s->tr->deliver == NULL) {
        return ENOSYS; /* transport has no notification support */
    }
    uint32_t idx = 0;
    int rc = qmp_topic_find(s, topic_id, &idx);
    if (rc != EOK) {
        return rc;
    }
    const qmp_topic_desc_t *t = &s->desc->topics[idx];
    if (len != t->sample_size) {
        return EINVAL;
    }
    qmp_topic_state_t *ts = &s->tstates[idx];
    /* Snapshot of this topic's subscribers, taken under the lock; delivery runs outside
     * it so a slow kernel path can never serialize publishers against subscribers. */
    struct {
        int64_t rcvid;
        uint32_t sub_id;
        uint16_t event_len;
        uint8_t event[QMP_EVENT_BLOB_MAX];
    } snap[QMP_SERVER_MAX_SUBS];
    unsigned nsnap = 0;

    (void)pthread_mutex_lock(&s->topic_mu);
    uint64_t seq = ts->next_seq++;
    uint8_t *slot = ts->slots + (size_t)((seq - 1) % ts->nslots) * ts->slot_stride;
    memcpy(slot, &seq, sizeof(seq));
    if (len > 0) {
        memcpy(slot + 8, sample, len);
    }
    for (unsigned i = 0; i < QMP_SERVER_MAX_SUBS; i++) {
        const qmp_sub_entry_t *e = sub_at(s, i);
        if (e->sub_id != 0 && e->topic_idx == idx) {
            snap[nsnap].rcvid = e->rcvid;
            snap[nsnap].sub_id = e->sub_id;
            snap[nsnap].event_len = e->event_len;
            memcpy(snap[nsnap].event, e->event, e->event_len);
            nsnap++;
        }
    }
    (void)pthread_mutex_unlock(&s->topic_mu);

    for (unsigned i = 0; i < nsnap; i++) {
        /* Non-blocking event delivery. The value is advisory (pulses may coalesce or
         * fail to queue, V-QNX-044); the pull cursor is the source of truth. */
        rc = s->tr->deliver(&s->ep, snap[i].rcvid, snap[i].event, snap[i].event_len, (int64_t)seq);
        if (rc == ESRCH || rc == EACCES || rc == EBADF) {
            /* Dead or unregistered subscriber: drop it, the others are unaffected. */
            (void)pthread_mutex_lock(&s->topic_mu);
            for (unsigned j = 0; j < QMP_SERVER_MAX_SUBS; j++) {
                qmp_sub_entry_t *e = sub_at(s, j);
                if (e->sub_id == snap[i].sub_id) {
                    memset(e, 0, sizeof(*e));
                    break;
                }
            }
            (void)pthread_mutex_unlock(&s->topic_mu);
        }
    }
    return EOK;
}
