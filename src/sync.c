/*
 * M4: Synchronization Layer (owner: Priyani)
 * Monitor-style allocator: ONE mutex protects all shared state, blocked
 * nodes sleep on a condition variable and wait in a FIFO queue.
 *
 * Granting happens in one place only: monitor_process_queue_locked().
 * It walks the queue from the oldest request to the newest and grants every
 * request that can be granted. A strict "head first only" rule is not used,
 * because the head may be waiting for resources held by a node behind it.
 *
 * Starvation protection: if a waiter has been overtaken STARVE_LIMIT times,
 * nodes behind it that hold NOTHING must wait. Nodes that already hold
 * resources may still run, so they can finish and release them.
 */
#include "nrade.h"
#include <string.h>

void monitor_init(Monitor *mon, System *s, Mode mode, Policy policy, int interval_ms)
{
    memset(mon, 0, sizeof *mon);
    mon->sys = s;
    mon->mode = mode;
    mon->policy = policy;
    mon->interval_ms = interval_ms > 0 ? interval_ms : 10;
    pthread_mutex_init(&mon->lock, NULL);
    pthread_cond_init(&mon->changed, NULL);
    sem_init(&mon->detector_wakeup, 0, 0);
    sem_init(&mon->start_gate, 0, 0);
    /* requests that are already pending in the loaded system */
    for (int i = 0; i < s->n; i++) {
        if (!vec_is_zero(s->request[i], s->m)) {
            s->node[i].state = NODE_BLOCKED;
            mon->queue[mon->qlen++] = i;
            mon->verdict[i] = REQ_WAIT_UNAVAILABLE;
        }
    }
}

void monitor_destroy(Monitor *mon)
{
    pthread_mutex_destroy(&mon->lock);
    pthread_cond_destroy(&mon->changed);
    sem_destroy(&mon->detector_wakeup);
    sem_destroy(&mon->start_gate);
}

static void check_invariant(Monitor *mon)
{
    if (!system_invariant_ok(mon->sys)) {
        mon->stats.invariant_violations++;
        log_event(EV_ERROR, mon->sys, -1, NULL, "INVARIANT VIOLATED");
    }
}

static void remove_at(Monitor *mon, int pos)
{
    memmove(&mon->queue[pos], &mon->queue[pos + 1],
            (size_t)(mon->qlen - pos - 1) * sizeof mon->queue[0]);
    mon->qlen--;
}

void monitor_remove_waiter_locked(Monitor *mon, int node)
{
    for (int p = 0; p < mon->qlen; p++) {
        if (mon->queue[p] == node) {
            remove_at(mon, p);
            break;
        }
    }
    memset(mon->sys->request[node], 0, sizeof mon->sys->request[node]);
    mon->bypassed[node] = 0;
}

void monitor_process_queue_locked(Monitor *mon)
{
    System *s = mon->sys;
    bool granted_any = false;
    bool barrier = false;                   /* a starving node is ahead */
    int p = 0;

    while (p < mon->qlen) {
        int i = mon->queue[p];
        int req[MAX_RES], seq[MAX_NODES], len = 0;
        ReqResult r;

        memcpy(req, s->request[i], sizeof req);
        if (barrier && vec_is_zero(s->alloc[i], s->m))
            r = REQ_WAIT_FAIRNESS;
        else
            r = try_allocate(s, mon->mode, i, req, seq, &len);

        if (r == REQ_GRANTED) {
            for (int q = 0; q < p; q++)     /* everybody ahead was overtaken */
                mon->bypassed[mon->queue[q]]++;
            monitor_remove_waiter_locked(mon, i);
            s->node[i].state = NODE_READY;
            mon->verdict[i] = REQ_GRANTED;
            mon->stats.grants++;
            log_event(EV_GRANT, s, i, req, "granted");
            check_invariant(mon);
            granted_any = true;
            barrier = false;                /* re-evaluate from the start */
            p = 0;
            continue;
        }
        if (r == REQ_WAIT_UNSAFE && mon->verdict[i] != REQ_WAIT_UNSAFE)
            mon->stats.unsafe_denials++;
        mon->verdict[i] = r;
        if (mon->bypassed[i] >= STARVE_LIMIT)
            barrier = true;
        p++;
    }
    if (granted_any)
        pthread_cond_broadcast(&mon->changed);
}

static ReqResult submit_locked(Monitor *mon, int node, const int *req)
{
    System *s = mon->sys;
    ReqResult v = validate_request(s, node, req);

    if (v == REQ_GRANTED &&
        (s->node[node].state == NODE_BLOCKED || s->node[node].state == NODE_TERMINATED))
        v = ERR_NODE_BUSY;
    if (v != REQ_GRANTED) {
        mon->stats.errors++;
        log_event(EV_ERROR, s, v == ERR_BAD_NODE ? -1 : node, req, req_result_text(v));
        return v;
    }
    if (vec_is_zero(req, s->m))
        return REQ_GRANTED;

    mon->stats.requests++;
    log_event(EV_REQUEST, s, node, req, "request");
    memcpy(s->request[node], req, (size_t)s->m * sizeof *req);
    s->node[node].state = NODE_BLOCKED;
    mon->verdict[node] = REQ_WAIT_UNAVAILABLE;
    mon->bypassed[node] = 0;
    mon->queue[mon->qlen++] = node;
    monitor_process_queue_locked(mon);

    v = (ReqResult)mon->verdict[node];
    if (v != REQ_GRANTED) {
        mon->stats.blocks++;
        log_event(EV_BLOCK, s, node, req, req_result_text(v));
    }
    return v;
}

/* Non-blocking request, used by the interactive menu. */
ReqResult monitor_submit(Monitor *mon, int node, const int *req)
{
    pthread_mutex_lock(&mon->lock);
    ReqResult r = submit_locked(mon, node, req);
    pthread_mutex_unlock(&mon->lock);
    return r;
}

/* Blocking request, used by node threads. */
ReqResult monitor_request(Monitor *mon, int node, const int *req)
{
    pthread_mutex_lock(&mon->lock);
    ReqResult r = submit_locked(mon, node, req);

    if (r == REQ_WAIT_UNAVAILABLE || r == REQ_WAIT_UNSAFE || r == REQ_WAIT_FAIRNESS) {
        double start = now_ms();
        /* while loop protects against spurious wake-ups */
        while (mon->sys->node[node].state == NODE_BLOCKED)
            pthread_cond_wait(&mon->changed, &mon->lock);
        r = (ReqResult)mon->verdict[node];
        mon->stats.total_wait_ms += now_ms() - start;
        mon->stats.waits++;
    }
    pthread_mutex_unlock(&mon->lock);
    return r;
}

ReqResult monitor_release(Monitor *mon, int node, const int *rel)
{
    pthread_mutex_lock(&mon->lock);
    System *s = mon->sys;
    ReqResult r;

    if (node >= 0 && node < s->n && s->node[node].state == NODE_BLOCKED)
        r = ERR_NODE_BUSY;
    else
        r = release_resources(s, node, rel);

    if (r == REQ_GRANTED) {
        mon->stats.releases++;
        log_event(EV_RELEASE, s, node, rel, "release");
        check_invariant(mon);
        monitor_process_queue_locked(mon);  /* wake waiting nodes in FIFO order */
    } else {
        mon->stats.errors++;
        log_event(EV_ERROR, s, -1, NULL, req_result_text(r));
    }
    pthread_mutex_unlock(&mon->lock);
    return r;
}

/* A node has completed its job: give everything back. */
void monitor_finish_node(Monitor *mon, int node)
{
    pthread_mutex_lock(&mon->lock);
    System *s = mon->sys;
    if (!vec_is_zero(s->alloc[node], s->m)) {
        log_event(EV_RELEASE, s, node, s->alloc[node], "job finished, release all");
        release_all(s, node);
        mon->stats.releases++;
    }
    /* A finished node will never ask again, so its claim drops to zero. */
    memset(s->need[node], 0, sizeof s->need[node]);
    s->node[node].state = NODE_DONE;
    mon->stats.completed++;
    check_invariant(mon);
    monitor_process_queue_locked(mon);
    pthread_mutex_unlock(&mon->lock);
}
