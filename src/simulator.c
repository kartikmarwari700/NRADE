/*
 * M2: Node (Client) Simulator (owner: Ishita)
 * One POSIX thread per network node; random or scripted requests.
 *
 * Script file format:
 *   <node> request v1 ... vM
 *   <node> release v1 ... vM
 *   <node> sleep <milliseconds>
 */
#include "nrade.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    Monitor         *mon;
    const SimConfig *cfg;
    int              id;
    unsigned         rng;
} Worker;

/* Small per-thread random generator (xorshift); no shared state. */
static unsigned next_rand(unsigned *x)
{
    *x ^= *x << 13;
    *x ^= *x >> 17;
    *x ^= *x << 5;
    return *x;
}

static void add_work(Monitor *mon, int id, int units)
{
    pthread_mutex_lock(&mon->lock);
    mon->sys->node[id].cpu_time += units;
    pthread_mutex_unlock(&mon->lock);
}

/* After pre-emption: wait a little so that the other nodes can finish first. */
static void back_off(Worker *w)
{
    pthread_mutex_lock(&w->mon->lock);
    int times = w->mon->sys->node[w->id].victim_count;
    pthread_mutex_unlock(&w->mon->lock);
    if (times > 10) times = 10;
    usleep((useconds_t)(1000u * (1 + next_rand(&w->rng) % 5) * (unsigned)times));
}

static void *random_node(void *arg)
{
    Worker *w = arg;
    Monitor *mon = w->mon;
    System *s = mon->sys;
    int id = w->id;
    int op = 0;                             /* checkpoint is operation 0 */

    sem_wait(&mon->start_gate);
    while (op < w->cfg->ops_per_node) {
        int need[MAX_RES], held[MAX_RES], v[MAX_RES] = { 0 };

        pthread_mutex_lock(&mon->lock);     /* take a consistent copy */
        memcpy(need, s->need[id], sizeof need);
        memcpy(held, s->alloc[id], sizeof held);
        pthread_mutex_unlock(&mon->lock);

        bool can_request = !vec_is_zero(need, s->m);
        bool can_release = !vec_is_zero(held, s->m);
        if (!can_request && !can_release) break;       /* claim is zero */

        if (can_request && (!can_release || next_rand(&w->rng) % 100 < 65)) {
            while (vec_is_zero(v, s->m))
                for (int j = 0; j < s->m; j++)
                    v[j] = (int)(next_rand(&w->rng) % (unsigned)(need[j] + 1));
            ReqResult r = monitor_request(mon, id, v);
            if (r == REQ_TERMINATED) return NULL;
            if (r == REQ_PREEMPTED) {       /* roll back to checkpoint */
                op = 0;
                back_off(w);
                continue;
            }
        } else {
            while (vec_is_zero(v, s->m))
                for (int j = 0; j < s->m; j++)
                    v[j] = (int)(next_rand(&w->rng) % (unsigned)(held[j] + 1));
            monitor_release(mon, id, v);
        }
        usleep((useconds_t)(200 + next_rand(&w->rng) % 1500));  /* "transmit" */
        add_work(mon, id, 1);
        op++;
    }
    monitor_finish_node(mon, id);
    return NULL;
}

static void *scripted_node(void *arg)
{
    Worker *w = arg;
    Monitor *mon = w->mon;
    const Script *sc = w->cfg->script;
    int k = 0;                              /* checkpoint is line 0 */

    sem_wait(&mon->start_gate);
    while (k < sc->count) {
        const ScriptStep *st = &sc->step[k];
        if (st->node != w->id) { k++; continue; }

        if (st->op == 's') {
            usleep((useconds_t)st->ms * 1000);
            add_work(mon, w->id, st->ms);
        } else if (st->op == 'l') {
            monitor_release(mon, w->id, st->amount);
        } else {
            ReqResult r = monitor_request(mon, w->id, st->amount);
            if (r == REQ_TERMINATED) return NULL;
            if (r == REQ_PREEMPTED) {       /* roll back to checkpoint */
                k = 0;
                back_off(w);
                continue;
            }
        }
        k++;
    }
    monitor_finish_node(mon, w->id);
    return NULL;
}

int script_load(Script *sc, const System *s, const char *path, char *err, size_t errlen)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        snprintf(err, errlen, "cannot open '%s'", path);
        return -1;
    }
    char line[512];
    int lineno = 0;
    sc->count = 0;
    while (fgets(line, sizeof line, f)) {
        lineno++;
        char *hash = strchr(line, '#');
        if (hash) *hash = '\0';
        char *name = strtok(line, " \t\r\n");
        if (!name) continue;
        char *op = strtok(NULL, " \t\r\n");
        int id = system_find_node(s, name);
        if (id < 0 || !op || sc->count >= MAX_SCRIPT) goto bad;

        ScriptStep *st = &sc->step[sc->count];
        memset(st, 0, sizeof *st);
        st->node = id;
        int values = 0;
        if (!strcmp(op, "sleep"))        { st->op = 's'; values = 1; }
        else if (!strcmp(op, "request")) { st->op = 'q'; values = s->m; }
        else if (!strcmp(op, "release")) { st->op = 'l'; values = s->m; }
        else goto bad;

        for (int j = 0; j < values; j++) {
            char *tok = strtok(NULL, " \t\r\n"), *end;
            if (!tok) goto bad;
            long x = strtol(tok, &end, 10);
            if (*end || x < 0 || x > 100000) goto bad;
            if (st->op == 's') st->ms = (int)x;
            else st->amount[j] = (int)x;
        }
        if (strtok(NULL, " \t\r\n")) goto bad;
        sc->count++;
    }
    fclose(f);
    return 0;
bad:
    fclose(f);
    snprintf(err, errlen, "%s: invalid line %d", path, lineno);
    return -1;
}

/* Runs one complete simulation on a private copy of the topology. */
int simulate(const System *base, const SimConfig *cfg, Stats *out)
{
    static System sys;
    static Monitor mon;
    pthread_t threads[MAX_NODES], detector;
    Worker workers[MAX_NODES];
    int started = 0;

    sys = *base;
    system_reset_allocation(&sys);
    monitor_init(&mon, &sys, cfg->mode, cfg->policy, cfg->interval_ms);

    bool use_detector = cfg->mode == MODE_DETECTION;
    if (use_detector && pthread_create(&detector, NULL, detector_thread, &mon)) {
        monitor_destroy(&mon);
        return -1;
    }
    for (int i = 0; i < sys.n; i++) {
        workers[i] = (Worker){ &mon, cfg, i,
                               cfg->seed * 2654435761u + (unsigned)i * 40503u + 1u };
        if (workers[i].rng == 0) workers[i].rng = 1;
        if (pthread_create(&threads[i], NULL,
                           cfg->script ? scripted_node : random_node, &workers[i]))
            break;
        started++;
    }
    double t0 = now_ms();
    for (int i = 0; i < started; i++)
        sem_post(&mon.start_gate);              /* open the gate */
    for (int i = 0; i < started; i++)
        pthread_join(threads[i], NULL);
    mon.stats.elapsed_ms = now_ms() - t0;

    if (use_detector) {
        pthread_mutex_lock(&mon.lock);
        mon.stop_detector = 1;
        pthread_mutex_unlock(&mon.lock);
        sem_post(&mon.detector_wakeup);
        pthread_join(detector, NULL);
    }
    if (!system_invariant_ok(&sys)) mon.stats.invariant_violations++;
    save_snapshot(&sys, cfg->mode == MODE_AVOIDANCE ? "end of avoidance run"
                                                    : "end of detection run");
    *out = mon.stats;
    monitor_destroy(&mon);
    return started == base->n ? 0 : -1;
}
