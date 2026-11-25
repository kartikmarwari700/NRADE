/*
 * M6 (part 2): Logging, tables, DOT graphs and reports (owner: Kartik)
 */
#include "nrade.h"
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

bool g_color = true;
bool g_verbose = false;

static FILE  *log_file;
static double log_start;

static const char *EVENT_NAMES[] = {
    "REQUEST", "GRANT", "BLOCK", "RELEASE", "DEADLOCK", "RECOVERY", "ERROR", "INFO"
};
static const char *EVENT_COLORS[] = {
    C_CYAN, C_GREEN, C_YELLOW, C_BLUE, C_RED, C_YELLOW, C_RED, C_RESET
};

double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

void log_open(const char *path)
{
    mkdir(OUTPUT_DIR, 0755);                /* fails harmlessly if present */
    log_file = fopen(path, "w");
    if (!log_file)
        fprintf(stderr, "warning: cannot write %s\n", path);
    log_start = now_ms();
}

void log_close(void)
{
    if (log_file) fclose(log_file);
    log_file = NULL;
}

/* Coloured printf (colour is skipped with --no-color). */
void say(const char *color, const char *fmt, ...)
{
    va_list ap;
    if (g_color) fputs(color, stdout);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    if (g_color) fputs(C_RESET, stdout);
}

/* One event record. Called while the monitor lock is held, so lines never mix. */
void log_event(EventType type, const System *s, int node, const int *vec, const char *msg)
{
    if (!log_file && !g_verbose) return;

    char line[2400];
    size_t used;
    const char *who = (s && node >= 0 && node < s->n) ? s->node[node].name : "-";

    used = (size_t)snprintf(line, sizeof line, "[%9.3f ms] %-8s %-6s",
                            now_ms() - log_start, EVENT_NAMES[type], who);
    if (vec && s) {
        used += (size_t)snprintf(line + used, sizeof line - used, " (");
        for (int j = 0; j < s->m; j++)
            used += (size_t)snprintf(line + used, sizeof line - used, "%s%d",
                                     j ? " " : "", vec[j]);
        used += (size_t)snprintf(line + used, sizeof line - used, ")");
    }
    snprintf(line + used, sizeof line - used, "  %s", msg ? msg : "");

    if (log_file) fprintf(log_file, "%s\n", line);
    if (g_verbose) say(EVENT_COLORS[type], "%s\n", line);
}

static const char *state_name(NodeState st)
{
    static const char *names[] = { "READY", "BLOCKED", "DONE", "KILLED" };
    return names[st];
}

static void print_vector(FILE *f, const int *v, int m)
{
    for (int j = 0; j < m; j++)
        fprintf(f, "%3d", v[j]);
}

static void print_header(FILE *f, const char *title, int m)
{
    int width = m * 3;
    fprintf(f, " | %-*s", width < 8 ? 8 : width, title);
}

static void print_cell(FILE *f, const int *v, int m)
{
    fprintf(f, " | ");
    print_vector(f, v, m);
    for (int k = m * 3; k < 8; k++) fputc(' ', f);
}

void print_tables(FILE *f, const System *s, bool color)
{
    const char *bold = color ? C_BOLD : "", *off = color ? C_RESET : "";

    fprintf(f, "\n%sResources%s\n", bold, off);
    fprintf(f, "  %-4s %-12s %-8s %6s %10s\n", "ID", "Name", "Type", "Total", "Available");
    for (int j = 0; j < s->m; j++)
        fprintf(f, "  R%-3d %-12s %-8s %6d %10d\n", j, s->res[j].name,
                res_type_name(s->res[j].type), s->total[j], s->available[j]);

    fprintf(f, "\n%s  %-6s %4s %-8s%s", bold, "Node", "Prio", "State", "");
    print_header(f, "Max", s->m);
    print_header(f, "Alloc", s->m);
    print_header(f, "Need", s->m);
    print_header(f, "Request", s->m);
    fprintf(f, "%s\n", off);

    for (int i = 0; i < s->n; i++) {
        const char *c = "";
        if (color && s->node[i].state == NODE_BLOCKED)    c = C_YELLOW;
        if (color && s->node[i].state == NODE_TERMINATED) c = C_RED;
        fprintf(f, "%s  %-6s %4d %-8s", c, s->node[i].name, s->node[i].priority,
                state_name(s->node[i].state));
        print_cell(f, s->max[i], s->m);
        print_cell(f, s->alloc[i], s->m);
        print_cell(f, s->need[i], s->m);
        print_cell(f, s->request[i], s->m);
        fprintf(f, "%s\n", off);
    }
    fprintf(f, "  Available = (");
    print_vector(f, s->available, s->m);
    fprintf(f, " )   invariant Available + Allocation = Total : %s\n\n",
            system_invariant_ok(s) ? "OK" : "BROKEN");
}

void print_sequence(const System *s, const int *seq, int len)
{
    printf("<");
    for (int k = 0; k < len; k++)
        printf("%s%s", k ? ", " : "", s->node[seq[k]].name);
    printf(">");
}

/* Appends the current allocation tables to output/snapshots.txt. */
void save_snapshot(const System *s, const char *title)
{
    FILE *f = fopen(OUTPUT_DIR "/snapshots.txt", "a");
    if (!f) return;
    fprintf(f, "==================== %s ====================\n", title);
    print_tables(f, s, false);
    fclose(f);
}

/* Resource Allocation Graph: R -> N is "assigned", N -> R is "waiting for". */
int write_rag_dot(const System *s, const bool *deadlocked, const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "digraph RAG {\n  rankdir=LR;\n  label=\"Resource Allocation Graph\";\n");
    for (int i = 0; i < s->n; i++)
        fprintf(f, "  \"%s\" [shape=circle%s];\n", s->node[i].name,
                deadlocked && deadlocked[i] ? ", style=filled, fillcolor=salmon" : "");
    for (int j = 0; j < s->m; j++)
        fprintf(f, "  \"res_%s\" [shape=box, label=\"%s\\n%s x%d\"];\n",
                s->res[j].name, s->res[j].name,
                res_type_name(s->res[j].type), s->total[j]);
    for (int i = 0; i < s->n; i++)
        for (int j = 0; j < s->m; j++) {
            if (s->alloc[i][j] > 0)
                fprintf(f, "  \"res_%s\" -> \"%s\" [label=\"%d\"];\n",
                        s->res[j].name, s->node[i].name, s->alloc[i][j]);
            if (s->request[i][j] > 0)
                fprintf(f, "  \"%s\" -> \"res_%s\" [label=\"%d\", style=dashed];\n",
                        s->node[i].name, s->res[j].name, s->request[i][j]);
        }
    fprintf(f, "}\n");
    return fclose(f);
}

int write_wfg_dot(const System *s, const bool *deadlocked, const char *path)
{
    int wfg[MAX_NODES][MAX_NODES];
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    build_wfg(s, wfg);
    fprintf(f, "digraph WFG {\n  label=\"Wait-For Graph\";\n  node [shape=circle];\n");
    for (int i = 0; i < s->n; i++)
        fprintf(f, "  \"%s\"%s;\n", s->node[i].name,
                deadlocked && deadlocked[i] ? " [style=filled, fillcolor=salmon]" : "");
    for (int i = 0; i < s->n; i++)
        for (int j = 0; j < s->n; j++)
            if (wfg[i][j])
                fprintf(f, "  \"%s\" -> \"%s\";\n", s->node[i].name, s->node[j].name);
    fprintf(f, "}\n");
    return fclose(f);
}

static double throughput(const Stats *st)
{
    return st->elapsed_ms > 0 ? st->grants * 1000.0 / st->elapsed_ms : 0;
}

static double average_wait(const Stats *st)
{
    return st->waits ? st->total_wait_ms / st->waits : 0;
}

void print_stats(FILE *f, const Stats *st)
{
    fprintf(f, "  requests / grants / blocks : %ld / %ld / %ld\n",
            st->requests, st->grants, st->blocks);
    fprintf(f, "  releases                   : %ld\n", st->releases);
    fprintf(f, "  unsafe denials (Banker)    : %ld\n", st->unsafe_denials);
    fprintf(f, "  deadlocks / victims        : %ld / %ld\n", st->deadlocks, st->victims);
    fprintf(f, "  nodes completed / killed   : %d / %d\n", st->completed, st->terminated);
    fprintf(f, "  elapsed time               : %.2f ms\n", st->elapsed_ms);
    fprintf(f, "  throughput                 : %.1f grants/s\n", throughput(st));
    fprintf(f, "  average waiting time       : %.3f ms\n", average_wait(st));
    fprintf(f, "  rejected invalid requests  : %ld\n", st->errors);
    fprintf(f, "  invariant violations       : %ld\n", st->invariant_violations);
}

static void comparison_table(FILE *f, const Stats *a, const Stats *d)
{
#define ROW_L(name, field) \
    fprintf(f, "  %-28s %14ld %14ld\n", name, (long)a->field, (long)d->field)
    fprintf(f, "  %-28s %14s %14s\n", "Metric", "Avoidance", "Detection");
    fprintf(f, "  ----------------------------------------------------------\n");
    ROW_L("Requests", requests);
    ROW_L("Grants", grants);
    ROW_L("Blocked requests", blocks);
    ROW_L("Unsafe denials (Banker)", unsafe_denials);
    ROW_L("Deadlocks", deadlocks);
    ROW_L("Victims", victims);
    ROW_L("Nodes completed", completed);
    ROW_L("Nodes killed", terminated);
    ROW_L("Invariant violations", invariant_violations);
    fprintf(f, "  %-28s %14.2f %14.2f\n", "Elapsed time (ms)",
            a->elapsed_ms, d->elapsed_ms);
    fprintf(f, "  %-28s %14.1f %14.1f\n", "Throughput (grants/s)",
            throughput(a), throughput(d));
    /* grants/s also counts work that was redone after a rollback;
       completed nodes per second counts only finished jobs */
    fprintf(f, "  %-28s %14.2f %14.2f\n", "Completed nodes per second",
            a->elapsed_ms > 0 ? a->completed * 1000.0 / a->elapsed_ms : 0,
            d->elapsed_ms > 0 ? d->completed * 1000.0 / d->elapsed_ms : 0);
    fprintf(f, "  %-28s %14.3f %14.3f\n", "Average wait (ms)",
            average_wait(a), average_wait(d));
#undef ROW_L
}

/* FR8: statistics report comparing both modes. */
int write_comparison_report(const Stats *avoid, const Stats *detect,
                            const SimConfig *cfg, const System *s, const char *path)
{
    printf("\n");
    comparison_table(stdout, avoid, detect);

    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "NRADE comparison report\n");
    fprintf(f, "nodes=%d resource-types=%d ", s->n, s->m);
    if (cfg->script) fprintf(f, "workload=script(%d steps)\n\n", cfg->script->count);
    else fprintf(f, "workload=random ops-per-node=%d seed=%u\n\n",
                 cfg->ops_per_node, cfg->seed);
    comparison_table(f, avoid, detect);
    fprintf(f, "\nNotes: times change from run to run because thread scheduling "
               "is not fixed.\nAvoidance never deadlocks but may make nodes wait "
               "even when resources are free.\nDetection grants freely but pays "
               "for deadlocks with rollback of work.\n");
    return fclose(f);
}
