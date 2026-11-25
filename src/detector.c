/*
 * M5: Deadlock Detection Module (owner: Kartik)
 * Wait-For Graph + DFS cycle detection, multi-instance detection algorithm,
 * and the periodic detector thread.
 */
#include "nrade.h"
#include <errno.h>
#include <string.h>
#include <time.h>

/*
 * Edge i -> j: node i waits for resource r (not enough free instances)
 * and node j holds at least one instance of r.
 */
void build_wfg(const System *s, int wfg[MAX_NODES][MAX_NODES])
{
    memset(wfg, 0, sizeof(int) * MAX_NODES * MAX_NODES);
    for (int i = 0; i < s->n; i++)
        for (int r = 0; r < s->m; r++) {
            if (s->request[i][r] <= s->available[r]) continue;
            for (int j = 0; j < s->n; j++)
                if (j != i && s->alloc[j][r] > 0)
                    wfg[i][j] = 1;
        }
}

/* DFS colours: 0 = not visited, 1 = on the current path, 2 = finished. */
static bool dfs(int n, int wfg[MAX_NODES][MAX_NODES], int u, int *colour,
                int *path, int depth, int *cycle, int *cycle_len)
{
    colour[u] = 1;
    path[depth] = u;
    for (int v = 0; v < n; v++) {
        if (!wfg[u][v]) continue;
        if (colour[v] == 1) {               /* back edge => cycle */
            int start = 0;
            while (path[start] != v) start++;
            *cycle_len = 0;
            for (int k = start; k <= depth; k++)
                cycle[(*cycle_len)++] = path[k];
            return true;
        }
        if (colour[v] == 0 &&
            dfs(n, wfg, v, colour, path, depth + 1, cycle, cycle_len))
            return true;
    }
    colour[u] = 2;
    return false;
}

bool find_cycle_dfs(int n, int wfg[MAX_NODES][MAX_NODES], int *cycle, int *cycle_len)
{
    int colour[MAX_NODES] = { 0 };
    int path[MAX_NODES];
    *cycle_len = 0;
    for (int u = 0; u < n; u++)
        if (colour[u] == 0 && dfs(n, wfg, u, colour, path, 0, cycle, cycle_len))
            return true;
    return false;
}

bool is_single_instance(const System *s)
{
    for (int j = 0; j < s->m; j++)
        if (s->total[j] != 1) return false;
    return true;
}

/*
 * Multi-instance detection algorithm.
 *   Work = Available; Finish[i] = true if node i holds nothing
 *   find i with !Finish[i] and Request[i] <= Work
 *   Work += Allocation[i], Finish[i] = true, repeat
 * Every node with Finish[i] == false is deadlocked.
 * It differs from the Safety algorithm: it uses Request, not Need.
 * Returns the number of deadlocked nodes.
 */
int detect_deadlock(const System *s, bool *deadlocked, int *order, int *order_len)
{
    int work[MAX_RES];
    bool finish[MAX_NODES];
    int len = 0, count = 0;
    bool progress = true;

    memcpy(work, s->available, sizeof work);
    for (int i = 0; i < s->n; i++)
        finish[i] = vec_is_zero(s->alloc[i], s->m);

    while (progress) {
        progress = false;
        for (int i = 0; i < s->n; i++) {
            if (!finish[i] && vec_leq(s->request[i], work, s->m)) {
                for (int j = 0; j < s->m; j++)
                    work[j] += s->alloc[i][j];
                finish[i] = true;
                if (order) order[len] = i;
                len++;
                progress = true;
            }
        }
    }
    for (int i = 0; i < s->n; i++) {
        if (deadlocked) deadlocked[i] = !finish[i];
        if (!finish[i]) count++;
    }
    if (order_len) *order_len = len;
    return count;
}

/* Appends text to a fixed buffer without ever overflowing it. */
static void append(char *buf, size_t size, const char *a, const char *b)
{
    size_t used = strlen(buf);
    if (used + 1 < size)
        snprintf(buf + used, size - used, "%s%s", a, b);
}

static void report_deadlock(Monitor *mon, const bool *dead, bool show)
{
    System *s = mon->sys;
    int wfg[MAX_NODES][MAX_NODES], cycle[MAX_NODES], clen = 0;
    char msg[2048] = "DEADLOCK among {";

    for (int i = 0; i < s->n; i++)
        if (dead[i])
            append(msg, sizeof msg, " ", s->node[i].name);
    append(msg, sizeof msg, " }", "");

    build_wfg(s, wfg);
    if (find_cycle_dfs(s->n, wfg, cycle, &clen)) {
        append(msg, sizeof msg, "  WFG cycle: ", "");
        for (int k = 0; k < clen; k++)
            append(msg, sizeof msg, s->node[cycle[k]].name, " -> ");
        append(msg, sizeof msg, s->node[cycle[0]].name, "");
    }
    log_event(EV_DEADLOCK, s, -1, NULL, msg);
    if (show) say(C_RED, "%s\n", msg);

    /* graphs of the first deadlock of a run (and of every shown one) */
    if (show || mon->stats.deadlocks == 1) {
        write_rag_dot(s, dead, OUTPUT_DIR "/rag_deadlock.dot");
        write_wfg_dot(s, dead, OUTPUT_DIR "/wfg_deadlock.dot");
        save_snapshot(s, msg);
    }
}

/*
 * Detect; while a deadlock exists remove the cheapest victim and check again.
 * Returns the number of victims. Caller must hold the monitor lock.
 */
int monitor_detect_and_recover_locked(Monitor *mon, bool show)
{
    System *s = mon->sys;
    bool dead[MAX_NODES];
    int victims = 0;

    mon->stats.detector_runs++;
    while (detect_deadlock(s, dead, NULL, NULL) > 0) {
        if (victims == 0) {
            mon->stats.deadlocks++;
            report_deadlock(mon, dead, show);
        }
        int v = select_victim(s, dead);
        if (v < 0) break;

        char msg[160];
        snprintf(msg, sizeof msg, "victim %s (cost %d) is %s", s->node[v].name,
                 victim_cost(s, v),
                 mon->policy == POLICY_PREEMPT ? "pre-empted and rolled back"
                                               : "terminated");
        log_event(EV_RECOVERY, s, v, s->alloc[v], msg);
        if (show) say(C_YELLOW, "Recovery: %s\n", msg);

        monitor_remove_waiter_locked(mon, v);
        apply_recovery(s, v, mon->policy);
        mon->verdict[v] = mon->policy == POLICY_PREEMPT ? REQ_PREEMPTED : REQ_TERMINATED;
        if (mon->policy == POLICY_TERMINATE) mon->stats.terminated++;
        mon->stats.victims++;
        victims++;
        if (!system_invariant_ok(s)) mon->stats.invariant_violations++;
        monitor_process_queue_locked(mon);
    }
    if (victims)
        pthread_cond_broadcast(&mon->changed);
    return victims;
}

/* Wakes every interval_ms, or at once when the semaphore is posted. */
void *detector_thread(void *arg)
{
    Monitor *mon = arg;
    for (;;) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += (long)(mon->interval_ms % 1000) * 1000000L;
        ts.tv_sec  += mon->interval_ms / 1000 + ts.tv_nsec / 1000000000L;
        ts.tv_nsec %= 1000000000L;
        while (sem_timedwait(&mon->detector_wakeup, &ts) == -1 && errno == EINTR)
            ;

        pthread_mutex_lock(&mon->lock);
        if (mon->stop_detector) {
            pthread_mutex_unlock(&mon->lock);
            break;
        }
        monitor_detect_and_recover_locked(mon, g_verbose);
        pthread_mutex_unlock(&mon->lock);
    }
    return NULL;
}
