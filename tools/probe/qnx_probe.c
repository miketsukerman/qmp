/* SPDX-License-Identifier: Apache-2.0 */
/*
 * qmp_qnx_probe: checks the QNX 8.0 kernel facts that libqmp's native transport is built
 * on, directly against the kernel API and without libqmp, so that a failure points at the
 * assumption and not at our code. Every check is tied to an entry in docs/VERIFY.md and
 * prints one line:
 *
 *   V-QNX-012 PASS  2 send IOVs arrive in one MsgReceivev: msglen=64 ...
 *
 * PASS means the kernel behaves as docs/VERIFY.md now records it, which for V-QNX-009 and
 * V-QNX-010 is the measured behaviour that refuted the original assumption. FAIL means
 * the behaviour changed and the design notes need another look. INFO lines report a
 * measured value. Exit code: 0 when nothing FAILed, 1 otherwise.
 *
 * Usage: qmp_qnx_probe            run every check
 *        qmp_qnx_probe child ...  internal: helper process for the death scenarios
 *
 * QNX only. Run it on the target with scripts/target-run.sh tools/probe/qmp_qnx_probe.
 */
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/dispatch.h>
#include <sys/iomsg.h>
#include <sys/neutrino.h>
#include <sys/syspage.h>
#include <sys/wait.h>

#define PULSE_EVENT (_PULSE_CODE_MINAVAIL + 5)

static int g_fails;
static const char *g_self;

static void report(const char *id, const char *verdict, const char *fmt, ...) {
    char buf[400];
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    printf("%s %-4s  %s\n", id, verdict, buf);
    (void)fflush(stdout);
    if (strcmp(verdict, "FAIL") == 0) {
        g_fails++;
    }
}
#define VERDICT(ok_) ((ok_) ? "PASS" : "FAIL")

static uint64_t now_ns(void) {
    struct timespec ts;
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void msleep(unsigned ms) {
    struct timespec ts = {ms / 1000U, (long)(ms % 1000U) * 1000000L};
    (void)nanosleep(&ts, NULL);
}

static int my_prio(void) {
    struct sched_param sp;
    int policy = 0;
    return pthread_getschedparam(pthread_self(), &policy, &sp) == 0 ? sp.sched_priority : -1;
}

static void set_prio(int prio) {
    struct sched_param sp;
    memset(&sp, 0, sizeof(sp));
    sp.sched_priority = prio;
    (void)pthread_setschedparam(pthread_self(), SCHED_RR, &sp);
}

/* Arm a kernel timeout for the next blocking receive so a wrong assumption cannot hang
 * the probe. */
static void arm_receive_timeout(unsigned ms) {
    uint64_t ns = (uint64_t)ms * 1000000ULL;
    (void)TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_RECEIVE, NULL, &ns, NULL);
}

/* ---- V-QNX-001 ---------------------------------------------------------------------- */

static void chk_io_max(void) {
    report("V-QNX-001", VERDICT(_IO_MAX == 0x1FF),
           "_IO_MAX = 0x%X, so the qmp message type 0x%X "
           "is above every resource-manager message",
           (unsigned)_IO_MAX, (unsigned)_IO_MAX + 0x100U);
}

/* ---- V-QNX-012 / V-QNX-014 / V-QNX-018 ----------------------------------------------- */

typedef struct call_arg {
    int coid;
    int prio; /* 0 = leave alone */
    unsigned delay_ms;
    long result;
    int err;
    uint8_t rep_a[24];
    uint8_t rep_b[8];
} call_arg_t;

static void *iov_caller(void *arg) {
    call_arg_t *ca = (call_arg_t *)arg;
    if (ca->prio > 0) {
        set_prio(ca->prio);
    }
    if (ca->delay_ms > 0) {
        msleep(ca->delay_ms);
    }
    uint8_t a[24];
    uint8_t b[40];
    memset(a, 0xA1, sizeof(a));
    memset(b, 0xB2, sizeof(b));
    a[0] = (uint8_t)ca->prio;
    iov_t siov[2];
    iov_t riov[2];
    SETIOV(&siov[0], a, sizeof(a));
    SETIOV(&siov[1], b, sizeof(b));
    SETIOV(&riov[0], ca->rep_a, sizeof(ca->rep_a));
    SETIOV(&riov[1], ca->rep_b, sizeof(ca->rep_b));
    ca->result = MsgSendv(ca->coid, siov, 2, riov, 2);
    ca->err = ca->result == -1 ? errno : 0;
    return NULL;
}

static void *reply_from_other_thread(void *arg) {
    rcvid_t rcvid = *(rcvid_t *)arg;
    uint8_t rep[32];
    memset(rep, 0x5A, sizeof(rep));
    intptr_t rc = MsgReply(rcvid, 7, rep, sizeof(rep));
    return (void *)(rc == -1 ? (intptr_t)errno : (intptr_t)0);
}

static void chk_iov_creds_crossthread(void) {
    int chid = ChannelCreate(0);
    int coid = ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);
    call_arg_t ca;
    memset(&ca, 0, sizeof(ca));
    ca.coid = coid;
    pthread_t tid;
    (void)pthread_create(&tid, NULL, iov_caller, &ca);

    uint8_t buf[128];
    iov_t riov[1];
    SETIOV(&riov[0], buf, sizeof(buf));
    struct _msg_info info;
    memset(&info, 0, sizeof(info));
    arm_receive_timeout(3000);
    rcvid_t rcvid = MsgReceivev(chid, riov, 1, &info);
    if (rcvid <= 0) {
        report("V-QNX-012", "FAIL", "MsgReceivev returned %ld errno=%d", (long)rcvid, errno);
        return;
    }
    int whole = info.msglen == 64 && info.srcmsglen == 64 && buf[1] == 0xA1 && buf[63] == 0xB2;
    report("V-QNX-012", VERDICT(whole && info.dstmsglen == 32),
           "2 send IOVs (24+40) arrive in one MsgReceivev: msglen=%ld srcmsglen=%ld; the "
           "client's 2 reply IOVs show as dstmsglen=%ld",
           (long)info.msglen, (long)info.srcmsglen, (long)info.dstmsglen);

    struct _client_info *ci = NULL;
    int rc = ConnectClientInfoExt(info.scoid, &ci, _NTO_CLIENTINFO_GETTYPEID);
    if (rc == -1 || ci == NULL) {
        report("V-QNX-014", "FAIL", "ConnectClientInfoExt failed errno=%d", errno);
    } else {
        int ok = ci->pid == getpid() && ci->cred.euid == geteuid() && ci->cred.egid == getegid();
        report("V-QNX-014", VERDICT(ok),
               "ConnectClientInfoExt(scoid, GETTYPEID): pid=%d euid=%d egid=%d ngroups=%u; "
               "_msg_info.type_id=%u; ClientInfoExtFree -> %d",
               (int)ci->pid, (int)ci->cred.euid, (int)ci->cred.egid, (unsigned)ci->cred.ngroups,
               (unsigned)info.type_id, ClientInfoExtFree(&ci));
    }

    /* V-QNX-018: the rcvid is valid on any thread of the server process. */
    pthread_t rt;
    void *rret = NULL;
    (void)pthread_create(&rt, NULL, reply_from_other_thread, &rcvid);
    (void)pthread_join(rt, &rret);
    (void)pthread_join(tid, NULL);
    report("V-QNX-018", VERDICT((intptr_t)rret == 0 && ca.result == 7 && ca.rep_a[0] == 0x5A),
           "request received on one thread, MsgReply from another: reply errno=%ld, client got "
           "status=%ld and the payload",
           (long)(intptr_t)rret, ca.result);
    (void)ConnectDetach(coid);
    (void)ChannelDestroy(chid);
}

/* ---- V-QNX-007 / V-QNX-008 / V-QNX-045 / V-QNX-009 ----------------------------------- */

typedef struct ev_srv {
    int chid;
    int prio;
    int deliver_errno[3];
    int unregistered_errno;
    int raw_coid_for_unregistered;
} ev_srv_t;

static void *event_server(void *arg) {
    ev_srv_t *es = (ev_srv_t *)arg;
    set_prio(es->prio);
    struct sigevent ev;
    struct _msg_info info;
    arm_receive_timeout(3000);
    rcvid_t rcvid = MsgReceive(es->chid, &ev, sizeof(ev), &info);
    if (rcvid <= 0) {
        return NULL;
    }
    (void)MsgReply(rcvid, 0, NULL, 0); /* the rcvid stays usable for MsgDeliverEvent */
    /* Receiving from the priority-10 client lowered this thread to 10 (a receiver takes
     * the sender's priority in both directions and keeps it, V-QNX-011): go back to 30. */
    set_prio(es->prio);
    for (int i = 0; i < 3; i++) {
        struct sigevent e = ev;
        e.sigev_value.sival_ptr = (void *)(intptr_t)(i + 1);
        msleep(100);
        es->deliver_errno[i] = MsgDeliverEvent(rcvid, &e) == -1 ? errno : 0;
    }
    /* An event the client never registered must be refused. */
    struct sigevent plain;
    SIGEV_PULSE_INIT(&plain, es->raw_coid_for_unregistered, 10, PULSE_EVENT, 99);
    es->unregistered_errno = MsgDeliverEvent(rcvid, &plain) == -1 ? errno : 0;
    return NULL;
}

static void chk_events(void) {
    static ev_srv_t es;
    memset(&es, 0, sizeof(es));
    es.chid = ChannelCreate(0);
    es.prio = 30;
    int coid = ConnectAttach(0, 0, es.chid, _NTO_SIDE_CHANNEL, 0);
    int nchid = ChannelCreate(0);
    int ncoid = ConnectAttach(0, 0, nchid, _NTO_SIDE_CHANNEL, 0);
    es.raw_coid_for_unregistered = ncoid;
    set_prio(10);
    pthread_t tid;
    (void)pthread_create(&tid, NULL, event_server, &es);

    struct sigevent ev;
    SIGEV_PULSE_INIT(&ev, ncoid, SIGEV_PULSE_PRIO_INHERIT, PULSE_EVENT, 0);
    SIGEV_MAKE_UPDATEABLE(&ev);
    /* Three distinct client priorities tell where an inherited pulse priority comes from:
     * 20 while registering, 15 while sending the subscription, 10 while waiting. */
    set_prio(20);
    if (MsgRegisterEvent(&ev, coid) == -1) {
        report("V-QNX-007", "FAIL", "MsgRegisterEvent failed errno=%d", errno);
        return;
    }
    set_prio(15);
    if (MsgSend(coid, &ev, sizeof(ev), NULL, 0) == -1) {
        report("V-QNX-007", "FAIL", "sending the registered event failed errno=%d", errno);
        return;
    }
    set_prio(10);
    long values[3] = {-1, -1, -1};
    int prio_after[3] = {-1, -1, -1};
    int got = 0;
    for (int i = 0; i < 3; i++) {
        struct _pulse p;
        arm_receive_timeout(3000);
        if (MsgReceivePulse(nchid, &p, sizeof(p), NULL) == -1) {
            break;
        }
        values[i] = (long)(intptr_t)p.value.sival_ptr;
        prio_after[i] = my_prio();
        set_prio(10);
        got += p.code == PULSE_EVENT;
    }
    (void)pthread_join(tid, NULL);
    int delivered =
        es.deliver_errno[0] == 0 && es.deliver_errno[1] == 0 && es.deliver_errno[2] == 0;
    report("V-QNX-007", VERDICT(got == 3 && delivered && es.unregistered_errno != 0),
           "the server delivers the client's registered event from the bytes it was sent "
           "(3 pulses received, errno %d/%d/%d); an unregistered event is refused with errno=%d",
           es.deliver_errno[0], es.deliver_errno[1], es.deliver_errno[2], es.unregistered_errno);
    int updated = values[0] == 1 && values[1] == 2 && values[2] == 3;
    report("V-QNX-008", VERDICT(updated),
           "SIGEV_MAKE_UPDATEABLE: pulse values set by the server per delivery = %ld,%ld,%ld",
           values[0], values[1], values[2]);
    report("V-QNX-045", VERDICT(updated && sizeof(((struct _pulse *)0)->value) == 8),
           "the value travels in sival_ptr; pulse value size = %zu bytes",
           sizeof(((struct _pulse *)0)->value));
    report("V-QNX-009", VERDICT(prio_after[0] == 10 && prio_after[1] == 10 && prio_after[2] == 10),
           "SIGEV_PULSE_PRIO_INHERIT: client registered the event at 20, subscribed at 15 and "
           "waits at 10; server delivers at 30 -> receiver priority after MsgReceivePulse = "
           "%d,%d,%d (the receiver keeps its own priority; nobody's is propagated)",
           prio_after[0], prio_after[1], prio_after[2]);
    /* Control: pulses with an explicit priority of 30, taken with both receive calls. */
    set_prio(10);
    struct _pulse p2;
    int explicit_pulse = -1;
    int explicit_msg = -1;
    if (MsgSendPulse(ncoid, 30, PULSE_EVENT, 1) == 0) {
        arm_receive_timeout(1000);
        if (MsgReceivePulse(nchid, &p2, sizeof(p2), NULL) == 0) {
            explicit_pulse = my_prio();
        }
    }
    set_prio(10);
    if (MsgSendPulse(ncoid, 30, PULSE_EVENT, 2) == 0) {
        arm_receive_timeout(1000);
        if (MsgReceive(nchid, &p2, sizeof(p2), NULL) == 0) {
            explicit_msg = my_prio();
        }
    }
    report("V-QNX-009", VERDICT(explicit_pulse == 30 && explicit_msg == 30),
           "control with MsgSendPulse(priority 30) to a receiver at 10: priority after "
           "MsgReceivePulse = %d, after MsgReceive = %d (an explicit pulse priority is taken)",
           explicit_pulse, explicit_msg);
    set_prio(10);
    (void)ConnectDetach(ncoid);
    (void)ChannelDestroy(nchid);
    (void)ConnectDetach(coid);
    (void)ChannelDestroy(es.chid);
}

/* ---- V-QNX-010: several senders, the worker runs at the highest pending one ----------- */

static void chk_highest_pending(void) {
    int chid = ChannelCreate(0);
    int coid = ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);
    static call_arg_t ca[3];
    static const int prios[3] = {8, 20, 30};
    pthread_t tids[3];
    memset(ca, 0, sizeof(ca));
    set_prio(10);
    for (int i = 0; i < 3; i++) {
        ca[i].coid = coid;
        ca[i].prio = prios[i];
        ca[i].delay_ms = i == 0 ? 0 : 150; /* 8 sends first, 20 and 30 queue up behind it */
        (void)pthread_create(&tids[i], NULL, iov_caller, &ca[i]);
    }
    uint8_t buf[64];
    int order[3] = {0, 0, 0};
    int prio_at_receive = -1;
    int prio_with_pending = -1;
    for (int i = 0; i < 3; i++) {
        struct _msg_info info;
        arm_receive_timeout(3000);
        rcvid_t rcvid = MsgReceive(chid, buf, sizeof(buf), &info);
        if (rcvid <= 0) {
            report("V-QNX-010", "FAIL", "MsgReceive %d failed errno=%d", i, errno);
            return;
        }
        order[i] = buf[0];
        if (i == 0) {
            prio_at_receive = my_prio();
            /* Stay RUNNING while the other two become SEND-blocked on this channel. */
            uint64_t until = now_ns() + 400000000ULL;
            while (now_ns() < until) {
                int p = my_prio();
                if (p > prio_with_pending) {
                    prio_with_pending = p;
                }
            }
        }
        (void)MsgReply(rcvid, 0, buf, 32);
    }
    for (int i = 0; i < 3; i++) {
        (void)pthread_join(tids[i], NULL);
    }
    report("V-QNX-010", VERDICT(prio_at_receive == 8 && prio_with_pending == 8 && order[1] == 30),
           "worker at base 10 serving a prio-8 sender: priority %d at receive, highest seen "
           "while senders at 20 and 30 are SEND-blocked behind it: %d (a busy worker is NOT "
           "boosted by queued senders); served in order %d,%d,%d",
           prio_at_receive, prio_with_pending, order[0], order[1], order[2]);
    set_prio(10);
    (void)ConnectDetach(coid);
    (void)ChannelDestroy(chid);
}

/* ---- V-QNX-016 / V-QNX-030 ------------------------------------------------------------ */

static void chk_clock(void) {
    uint64_t cps = SYSPAGE_ENTRY(qtime)->cycles_per_sec;
    unsigned ncpu = _syspage_ptr->num_cpu;
    long worst_ppm = 0;
    int monotonic = 1;
    uint64_t prev = ClockCycles();
    for (unsigned cpu = 0; cpu < ncpu && cpu < 32; cpu++) {
        if (ThreadCtl(_NTO_TCTL_RUNMASK, (void *)(uintptr_t)(1U << cpu)) == -1) {
            report("V-QNX-016", "FAIL", "cannot pin to cpu %u errno=%d", cpu, errno);
            return;
        }
        uint64_t c0 = ClockCycles();
        uint64_t t0 = now_ns();
        monotonic = monotonic && c0 >= prev;
        while (now_ns() - t0 < 250000000ULL) {
        }
        uint64_t c1 = ClockCycles();
        uint64_t t1 = now_ns();
        prev = c1;
        double expect = (double)(t1 - t0) * (double)cps / 1e9;
        long ppm = (long)(((double)(c1 - c0) - expect) / expect * 1e6);
        if (labs(ppm) > labs(worst_ppm)) {
            worst_ppm = ppm;
        }
    }
    (void)ThreadCtl(_NTO_TCTL_RUNMASK, (void *)(uintptr_t)((1U << ncpu) - 1U));
    report("V-QNX-030", "INFO", "cycles_per_sec = %" PRIu64 " (resolution %.1f ns), %u cpus", cps,
           1e9 / (double)cps, ncpu);
    report("V-QNX-016", VERDICT(monotonic && labs(worst_ppm) < 500),
           "ClockCycles against CLOCK_MONOTONIC over 250 ms pinned to each cpu: worst deviation "
           "%ld ppm, monotonic across cpus=%d",
           worst_ppm, monotonic);
}

/* ---- V-QNX-037: _NTO_MI_UNBLOCK_REQ ---------------------------------------------------- */

static void *timeout_caller(void *arg) {
    call_arg_t *ca = (call_arg_t *)arg;
    uint64_t ns = 200000000ULL;
    uint8_t msg[8] = {0};
    (void)TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_SEND | _NTO_TIMEOUT_REPLY, NULL, &ns, NULL);
    ca->result = MsgSend(ca->coid, msg, sizeof(msg), NULL, 0);
    ca->err = ca->result == -1 ? errno : 0;
    return NULL;
}

static void chk_unblock(void) {
    int chid = ChannelCreate(_NTO_CHF_UNBLOCK);
    int coid = ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);
    static call_arg_t ca;
    memset(&ca, 0, sizeof(ca));
    ca.coid = coid;
    pthread_t tid;
    (void)pthread_create(&tid, NULL, timeout_caller, &ca);
    uint8_t buf[16];
    struct _msg_info mi;
    arm_receive_timeout(3000);
    rcvid_t rcvid = MsgReceive(chid, buf, sizeof(buf), NULL);
    int before = MsgInfo(rcvid, &mi) == 0 ? (mi.flags & _NTO_MI_UNBLOCK_REQ) != 0 : -1;
    struct _pulse p;
    memset(&p, 0, sizeof(p));
    arm_receive_timeout(3000);
    int prc = MsgReceivePulse(chid, &p, sizeof(p), NULL);
    int during = MsgInfo(rcvid, &mi) == 0 ? (mi.flags & _NTO_MI_UNBLOCK_REQ) != 0 : -1;
    (void)MsgError(rcvid, ETIMEDOUT);
    (void)pthread_join(tid, NULL);
    int after_rc = MsgInfo(rcvid, &mi);
    int after_errno = after_rc == -1 ? errno : 0;
    int ok = prc == 0 && before == 0 && during == 1 && after_rc == -1 &&
             (rcvid_t)p.value.sival_long == rcvid && ca.err == ETIMEDOUT;
    report("V-QNX-037", VERDICT(ok),
           "client timeout on a REPLY-blocked call: pulse code %d with value==rcvid:%d; "
           "_NTO_MI_UNBLOCK_REQ before=%d, after the pulse=%d; MsgInfo after MsgError -> %d "
           "errno=%d; client errno=%d",
           (int)p.code, (rcvid_t)p.value.sival_long == rcvid, before, during, after_rc, after_errno,
           ca.err);
    (void)ConnectDetach(coid);
    (void)ChannelDestroy(chid);
}

/* ---- V-QNX-039 / V-QNX-040 ------------------------------------------------------------- */

static void *name_opener(void *arg) {
    return (void *)(intptr_t)name_open((const char *)arg, 0);
}

static void chk_name_attach(void) {
    static const char *name = "qmp/probe-0000/v0";
    int chid = ChannelCreate(_NTO_CHF_UNBLOCK | _NTO_CHF_DISCONNECT | _NTO_CHF_COID_DISCONNECT);
    dispatch_t *dpp = dispatch_create_channel(chid, 0);
    name_attach_t *att = dpp != NULL ? name_attach(dpp, name, 0) : NULL;
    if (att == NULL) {
        report("V-QNX-039", "FAIL", "name_attach over our channel failed errno=%d", errno);
        return;
    }
    pthread_t opener;
    (void)pthread_create(&opener, NULL, name_opener, (void *)(uintptr_t)name);
    uint8_t cbuf[256];
    arm_receive_timeout(3000);
    rcvid_t crcvid = MsgReceive(chid, cbuf, sizeof(cbuf), NULL);
    if (crcvid > 0) {
        (void)MsgReply(crcvid, EOK, NULL, 0); /* name_open's _IO_CONNECT */
    }
    void *oret = NULL;
    (void)pthread_join(opener, &oret);
    int coid = (int)(intptr_t)oret;
    report("V-QNX-039", VERDICT(att->chid == chid && coid != -1),
           "name_attach(dispatch_create_channel(chid)) keeps our channel: attach->chid=%d, "
           "ours=%d, name_open -> %d",
           att->chid, chid, coid);
    if (coid != -1) {
        (void)name_close(coid);
    }
    int drc = name_detach(att, 0);
    int destroy_rc = ChannelDestroy(chid);
    int destroy_errno = destroy_rc == -1 ? errno : 0;
    int reopen = name_open(name, 0);
    int reopen_errno = reopen == -1 ? errno : 0;
    report("V-QNX-040", VERDICT(drc == 0 && destroy_rc == -1 && reopen == -1),
           "name_detach -> %d; a following ChannelDestroy(chid) -> %d errno=%d (the channel is "
           "already gone); name_open afterwards -> %d errno=%d",
           drc, destroy_rc, destroy_errno, reopen, reopen_errno);
}

/* ---- child-process scenarios: V-QNX-004 / V-QNX-024 / V-QNX-005 ------------------------ */

static pid_t spawn_child(const char *mode, int chid) {
    char pidbuf[24];
    char chidbuf[24];
    (void)snprintf(pidbuf, sizeof(pidbuf), "%d", (int)getpid());
    (void)snprintf(chidbuf, sizeof(chidbuf), "%d", chid);
    char *argv[] = {(char *)g_self, "child", (char *)mode, pidbuf, chidbuf, NULL};
    extern char **environ;
    pid_t pid = -1;
    if (posix_spawn(&pid, g_self, NULL, NULL, argv, environ) != 0) {
        return -1;
    }
    return pid;
}

static int child_main(const char *mode, pid_t ppid, int pchid) {
    if (strcmp(mode, "two-conn-block") == 0) {
        int c1 = ConnectAttach(0, ppid, pchid, _NTO_SIDE_CHANNEL, 0);
        int c2 = ConnectAttach(0, ppid, pchid, _NTO_SIDE_CHANNEL, 0);
        if (c1 == -1 || c2 == -1) {
            return 3;
        }
        uint8_t msg[8] = {1};
        (void)MsgSend(c1, msg, sizeof(msg), NULL, 0); /* the parent never replies in time */
        return 0;
    }
    if (strcmp(mode, "serve") == 0) {
        int chid = ChannelCreate(0);
        int c = ConnectAttach(0, ppid, pchid, _NTO_SIDE_CHANNEL, 0);
        if (chid == -1 || c == -1 || MsgSend(c, &chid, sizeof(chid), NULL, 0) == -1) {
            return 3;
        }
        for (;;) {
            uint8_t buf[16];
            rcvid_t rcvid = MsgReceive(chid, buf, sizeof(buf), NULL);
            if (rcvid > 0) {
                (void)MsgReply(rcvid, 0, NULL, 0);
            }
        }
    }
    return 2;
}

static void chk_client_death(void) {
    /* No _NTO_CHF_UNBLOCK here: with it a killed REPLY-blocked client stays until the
     * server answers (that path is V-QNX-037 and the smoke scenarios). */
    int chid = ChannelCreate(_NTO_CHF_DISCONNECT);
    pid_t child = spawn_child("two-conn-block", chid);
    if (child == -1) {
        report("V-QNX-004", "FAIL", "cannot spawn the helper process errno=%d", errno);
        return;
    }
    uint8_t buf[16];
    struct _msg_info info;
    memset(&info, 0, sizeof(info));
    arm_receive_timeout(5000);
    rcvid_t rcvid = MsgReceive(chid, buf, sizeof(buf), &info);
    if (rcvid <= 0) {
        report("V-QNX-004", "FAIL", "no message from the helper errno=%d", errno);
        (void)kill(child, SIGKILL);
        return;
    }
    (void)kill(child, SIGKILL);
    int status = 0;
    (void)waitpid(child, &status, 0);
    int disconnects = 0;
    int scoid_match = 0;
    int others = 0;
    for (;;) {
        struct _pulse p;
        arm_receive_timeout(500);
        if (MsgReceivePulse(chid, &p, sizeof(p), NULL) == -1) {
            break;
        }
        if (p.code == _PULSE_CODE_DISCONNECT) {
            disconnects++;
            scoid_match = p.scoid == info.scoid;
        } else {
            others++;
        }
    }
    int reply_rc = (int)MsgReply(rcvid, 0, NULL, 0);
    int reply_errno = reply_rc == -1 ? errno : 0;
    int detach_rc = ConnectDetach(info.scoid);
    report("V-QNX-004", VERDICT(disconnects == 1 && scoid_match && detach_rc == 0),
           "process with 2 connections killed: %d _PULSE_CODE_DISCONNECT (other pulses: %d), "
           "pulse scoid == message scoid: %d, ConnectDetach(scoid) -> %d",
           disconnects, others, scoid_match, detach_rc);
    report("V-QNX-024", VERDICT(reply_rc == -1 && reply_errno == ESRCH),
           "MsgReply to the rcvid of a client that already died -> %d errno=%d (%s)", reply_rc,
           reply_errno, strerror(reply_errno));
    (void)ChannelDestroy(chid);
}

static void chk_server_death(void) {
    int chid = ChannelCreate(_NTO_CHF_COID_DISCONNECT);
    pid_t child = spawn_child("serve", chid);
    if (child == -1) {
        report("V-QNX-005", "FAIL", "cannot spawn the helper process errno=%d", errno);
        return;
    }
    int child_chid = -1;
    arm_receive_timeout(5000);
    rcvid_t rcvid = MsgReceive(chid, &child_chid, sizeof(child_chid), NULL);
    if (rcvid <= 0) {
        report("V-QNX-005", "FAIL", "no message from the helper errno=%d", errno);
        (void)kill(child, SIGKILL);
        return;
    }
    (void)MsgReply(rcvid, 0, NULL, 0);
    int coid = ConnectAttach(0, child, child_chid, _NTO_SIDE_CHANNEL, 0);
    uint8_t ping[4] = {0};
    long alive = MsgSend(coid, ping, sizeof(ping), NULL, 0);
    (void)kill(child, SIGKILL);
    int status = 0;
    (void)waitpid(child, &status, 0);
    int death = 0;
    int value_is_coid = 0;
    for (;;) {
        struct _pulse p;
        arm_receive_timeout(500);
        if (MsgReceivePulse(chid, &p, sizeof(p), NULL) == -1) {
            break;
        }
        if (p.code == _PULSE_CODE_COIDDEATH) {
            death++;
            value_is_coid = p.value.sival_int == coid;
        }
    }
    long after = MsgSend(coid, ping, sizeof(ping), NULL, 0);
    int after_errno = after == -1 ? errno : 0;
    report("V-QNX-005", VERDICT(alive == 0 && death == 1 && value_is_coid && after == -1),
           "server process killed: %d _PULSE_CODE_COIDDEATH on the _NTO_CHF_COID_DISCONNECT "
           "channel, value == coid: %d; MsgSend on the dead coid -> %ld errno=%d (%s)",
           death, value_is_coid, after, after_errno, strerror(after_errno));
    (void)ConnectDetach(coid);
    (void)ChannelDestroy(chid);
}

int main(int argc, char **argv) {
    g_self = argv[0];
    if (argc == 5 && strcmp(argv[1], "child") == 0) {
        return child_main(argv[2], (pid_t)atoi(argv[3]), atoi(argv[4]));
    }
    if (argc != 1) {
        fprintf(stderr, "usage: %s\n", argv[0]);
        return 2;
    }
    printf("qmp_qnx_probe: pid=%d\n", (int)getpid());
    chk_io_max();
    /* The child scenarios spawn processes: run them before any other thread exists. */
    chk_client_death();
    chk_server_death();
    chk_iov_creds_crossthread();
    chk_events();
    chk_highest_pending();
    chk_unblock();
    chk_name_attach();
    chk_clock();
    printf("%s (%d failed)\n", g_fails == 0 ? "PROBE PASSED" : "PROBE FAILED", g_fails);
    return g_fails == 0 ? 0 : 1;
}
