/*
 * nrade.h - common header of the
 * Network Resource Allocation and Deadlock Detection Engine (NRADE).
 */
#ifndef NRADE_H
#define NRADE_H

#include <pthread.h>
#include <semaphore.h>
#include <stdbool.h>
#include <stdio.h>

#define MAX_NODES   50
#define MAX_RES     10
#define NAME_LEN    24
#define MAX_SCRIPT  2048
#define OUTPUT_DIR  "output"

/* A waiting request that is overtaken this many times is "starving". */
#define STARVE_LIMIT 25

/* Victim cost = W_PRIORITY*priority + W_HELD*held + W_WORK*work + W_VICTIM*times_victim */
#define W_PRIORITY  10
#define W_HELD       3
#define W_WORK       1
#define W_VICTIM    50

/* ------------------------------------------------------------------ types */

typedef enum { RES_LINK_BW, RES_BUFFER, RES_PORT, RES_CHANNEL } ResType;
typedef enum { NODE_READY, NODE_BLOCKED, NODE_DONE, NODE_TERMINATED } NodeState;
typedef enum { MODE_AVOIDANCE, MODE_DETECTION } Mode;
typedef enum { POLICY_PREEMPT, POLICY_TERMINATE } Policy;

typedef enum {
    REQ_GRANTED,          /* resources allocated                         */
    REQ_WAIT_UNAVAILABLE, /* not enough free instances                   */
    REQ_WAIT_UNSAFE,      /* Banker: granting would give an unsafe state */
    REQ_WAIT_FAIRNESS,    /* held back so that a starving node can run   */
    REQ_PREEMPTED,        /* chosen as victim: roll back and retry       */
    REQ_TERMINATED,       /* chosen as victim: killed                    */
    ERR_BAD_NODE,
    ERR_NEGATIVE,
    ERR_EXCEEDS_NEED,
    ERR_EXCEEDS_TOTAL,
    ERR_EXCEEDS_ALLOC,
    ERR_NODE_BUSY         /* node is blocked, terminated, ...            */
} ReqResult;

typedef enum {
    EV_REQUEST, EV_GRANT, EV_BLOCK, EV_RELEASE, EV_DEADLOCK, EV_RECOVERY, EV_ERROR, EV_INFO
} EventType;

typedef struct {
    int     id;
    ResType type;
    char    name[NAME_LEN];
    int     instances;
} Resource;

typedef struct {            /* node control block */
    int       id;
    char      name[NAME_LEN];
    int       priority;     /* larger number = more important            */
    NodeState state;
    int       cpu_time;     /* work done since the last checkpoint       */
    int       victim_count; /* how many times it was chosen as victim    */
    int       checkpoint;   /* script / operation index to roll back to  */
} Node;

typedef struct {
    int      n, m;                      /* number of nodes, resource types */
    Resource res[MAX_RES];
    Node     node[MAX_NODES];
    int      total[MAX_RES];
    int      available[MAX_RES];
    int      max[MAX_NODES][MAX_RES];
    int      alloc[MAX_NODES][MAX_RES];
    int      need[MAX_NODES][MAX_RES];
    int      request[MAX_NODES][MAX_RES]; /* pending (blocked) requests  */
} System;

typedef struct {
    long   requests, grants, blocks, releases, errors;
    long   unsafe_denials;
    long   deadlocks, victims, detector_runs;
    long   invariant_violations;
    int    completed, terminated;
    double total_wait_ms;
    long   waits;
    double elapsed_ms;
} Stats;

typedef struct {            /* the monitor-style allocator (module M4) */
    pthread_mutex_t lock;
    pthread_cond_t  changed;
    sem_t           detector_wakeup;    /* wakes the detector early      */
    sem_t           start_gate;         /* all node threads start together */
    System         *sys;
    Mode            mode;
    Policy          policy;
    int             queue[MAX_NODES];   /* FIFO wait queue of node ids   */
    int             qlen;
    int             verdict[MAX_NODES]; /* ReqResult seen by a waiter    */
    int             bypassed[MAX_NODES];
    int             stop_detector;
    int             interval_ms;
    Stats           stats;
} Monitor;

typedef struct { char op; int amount[MAX_RES]; int ms; int node; } ScriptStep;
typedef struct { int count; ScriptStep step[MAX_SCRIPT]; } Script;

typedef struct {
    Mode     mode;
    Policy   policy;
    int      ops_per_node;
    unsigned seed;
    int      interval_ms;
    const Script *script;   /* NULL = random request pattern */
} SimConfig;

/* ------------------------------------------- M1: topology & resources */
void system_init(System *s);
int  topology_load(System *s, const char *path, char *err, size_t errlen);
int  topology_from_menu(System *s, FILE *in);
int  system_finalize(System *s, char *err, size_t errlen);
void system_reset_allocation(System *s);
int  system_find_node(const System *s, const char *name);
bool system_invariant_ok(const System *s);
const char *res_type_name(ResType t);

/* ------------------------------------------- M3: allocation & avoidance */
bool      vec_leq(const int *a, const int *b, int m);
bool      vec_is_zero(const int *a, int m);
bool      safety_check(const System *s, int *seq, int *seq_len);
ReqResult validate_request(const System *s, int node, const int *req);
ReqResult try_allocate(System *s, Mode mode, int node, const int *req,
                       int *seq, int *seq_len);
ReqResult release_resources(System *s, int node, const int *rel);
void      release_all(System *s, int node);
const char *req_result_text(ReqResult r);

/* ------------------------------------------- M4: synchronization layer */
void      monitor_init(Monitor *mon, System *s, Mode mode, Policy policy, int interval_ms);
void      monitor_destroy(Monitor *mon);
ReqResult monitor_submit(Monitor *mon, int node, const int *req);  /* never blocks */
ReqResult monitor_request(Monitor *mon, int node, const int *req); /* blocks       */
ReqResult monitor_release(Monitor *mon, int node, const int *rel);
void      monitor_finish_node(Monitor *mon, int node);
void      monitor_process_queue_locked(Monitor *mon);
void      monitor_remove_waiter_locked(Monitor *mon, int node);

/* ------------------------------------------- M5: deadlock detection */
void  build_wfg(const System *s, int wfg[MAX_NODES][MAX_NODES]);
bool  find_cycle_dfs(int n, int wfg[MAX_NODES][MAX_NODES], int *cycle, int *cycle_len);
int   detect_deadlock(const System *s, bool *deadlocked, int *order, int *order_len);
bool  is_single_instance(const System *s);
int   monitor_detect_and_recover_locked(Monitor *mon, bool show);
void *detector_thread(void *arg);

/* ------------------------------------------- M6: recovery */
int  victim_cost(const System *s, int node);
int  select_victim(const System *s, const bool *deadlocked);
void apply_recovery(System *s, int victim, Policy policy);

/* ------------------------------------------- M2: node simulator */
int  script_load(Script *sc, const System *s, const char *path, char *err, size_t errlen);
int  simulate(const System *base, const SimConfig *cfg, Stats *out);

/* ------------------------------------------- M6: logging & reports */
extern bool g_color;
extern bool g_verbose;
double now_ms(void);
void   log_open(const char *path);
void   log_close(void);
void   log_event(EventType type, const System *s, int node, const int *vec, const char *msg);
void   say(const char *color, const char *fmt, ...);
void   print_tables(FILE *f, const System *s, bool color);
void   print_sequence(const System *s, const int *seq, int len);
void   save_snapshot(const System *s, const char *title);
int    write_rag_dot(const System *s, const bool *deadlocked, const char *path);
int    write_wfg_dot(const System *s, const bool *deadlocked, const char *path);
void   print_stats(FILE *f, const Stats *st);
int    write_comparison_report(const Stats *avoid, const Stats *detect,
                               const SimConfig *cfg, const System *s, const char *path);

#define C_RESET  "\033[0m"
#define C_RED    "\033[31m"
#define C_GREEN  "\033[32m"
#define C_YELLOW "\033[33m"
#define C_BLUE   "\033[34m"
#define C_CYAN   "\033[36m"
#define C_BOLD   "\033[1m"

#endif /* NRADE_H */
