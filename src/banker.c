/*
 * M3: Allocation & Avoidance Engine (owner: Priyani)
 * Request / release handling, Banker's Safety algorithm and
 * Banker's Resource-Request algorithm.
 * These functions do no locking: the caller (module M4) holds the mutex.
 */
#include "nrade.h"
#include <string.h>

bool vec_leq(const int *a, const int *b, int m)
{
    for (int j = 0; j < m; j++)
        if (a[j] > b[j]) return false;
    return true;
}

bool vec_is_zero(const int *a, int m)
{
    for (int j = 0; j < m; j++)
        if (a[j] != 0) return false;
    return true;
}

/*
 * Safety algorithm.
 *   Work = Available, Finish[i] = false
 *   find i with !Finish[i] and Need[i] <= Work
 *   Work += Allocation[i], Finish[i] = true, repeat
 * SAFE if every node can finish. seq receives the safe sequence.
 */
bool safety_check(const System *s, int *seq, int *seq_len)
{
    int work[MAX_RES];
    bool finish[MAX_NODES] = { false };
    int count = 0;
    bool progress = true;

    memcpy(work, s->available, sizeof work);
    while (progress) {
        progress = false;
        for (int i = 0; i < s->n; i++) {
            if (!finish[i] && vec_leq(s->need[i], work, s->m)) {
                for (int j = 0; j < s->m; j++)
                    work[j] += s->alloc[i][j];
                finish[i] = true;
                if (seq) seq[count] = i;
                count++;
                progress = true;
            }
        }
    }
    if (seq_len) *seq_len = count;
    return count == s->n;
}

/* NFR5: reject invalid requests before touching any matrix. */
ReqResult validate_request(const System *s, int node, const int *req)
{
    if (node < 0 || node >= s->n) return ERR_BAD_NODE;
    for (int j = 0; j < s->m; j++)
        if (req[j] < 0) return ERR_NEGATIVE;
    for (int j = 0; j < s->m; j++)
        if (req[j] > s->total[j]) return ERR_EXCEEDS_TOTAL;
    if (!vec_leq(req, s->need[node], s->m)) return ERR_EXCEEDS_NEED;
    return REQ_GRANTED;
}

static void move_resources(System *s, int node, const int *v, int sign)
{
    for (int j = 0; j < s->m; j++) {
        int amount = sign * v[j];
        s->available[j]   -= amount;
        s->alloc[node][j] += amount;
        s->need[node][j]  -= amount;
    }
}

/*
 * Resource-Request algorithm.
 *   1. Request <= Need            (else error)
 *   2. Request <= Available       (else wait)
 *   3. avoidance mode: pretend to allocate, run Safety;
 *      unsafe -> roll back and wait.
 */
ReqResult try_allocate(System *s, Mode mode, int node, const int *req,
                       int *seq, int *seq_len)
{
    ReqResult v = validate_request(s, node, req);
    if (v != REQ_GRANTED) return v;
    if (!vec_leq(req, s->available, s->m)) return REQ_WAIT_UNAVAILABLE;

    move_resources(s, node, req, +1);            /* pretend to allocate */
    if (mode == MODE_AVOIDANCE && !safety_check(s, seq, seq_len)) {
        move_resources(s, node, req, -1);        /* roll back */
        return REQ_WAIT_UNSAFE;
    }
    return REQ_GRANTED;
}

ReqResult release_resources(System *s, int node, const int *rel)
{
    if (node < 0 || node >= s->n) return ERR_BAD_NODE;
    for (int j = 0; j < s->m; j++)
        if (rel[j] < 0) return ERR_NEGATIVE;
    if (!vec_leq(rel, s->alloc[node], s->m)) return ERR_EXCEEDS_ALLOC;
    move_resources(s, node, rel, -1);
    return REQ_GRANTED;
}

void release_all(System *s, int node)
{
    int held[MAX_RES] = { 0 };
    memcpy(held, s->alloc[node], (size_t)s->m * sizeof held[0]);
    move_resources(s, node, held, -1);
}

const char *req_result_text(ReqResult r)
{
    switch (r) {
    case REQ_GRANTED:          return "GRANTED";
    case REQ_WAIT_UNAVAILABLE: return "WAIT: resources not available";
    case REQ_WAIT_UNSAFE:      return "DENIED: resulting state is UNSAFE, node waits";
    case REQ_WAIT_FAIRNESS:    return "WAIT: giving priority to a starving node";
    case REQ_PREEMPTED:        return "PREEMPTED: rolled back to checkpoint";
    case REQ_TERMINATED:       return "TERMINATED by recovery";
    case ERR_BAD_NODE:         return "ERROR: unknown node";
    case ERR_NEGATIVE:         return "ERROR: negative value";
    case ERR_EXCEEDS_NEED:     return "ERROR: request exceeds need";
    case ERR_EXCEEDS_TOTAL:    return "ERROR: request exceeds total instances";
    case ERR_EXCEEDS_ALLOC:    return "ERROR: release exceeds allocation";
    case ERR_NODE_BUSY:        return "ERROR: node is blocked or terminated";
    }
    return "?";
}
