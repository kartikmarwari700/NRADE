/*
 * NRADE - menu driven and command line front end.
 */
#include "nrade.h"
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static System  g_sys;
static Monitor g_mon;
static Script  g_script;
static bool    g_loaded;
static Mode    g_mode = MODE_AVOIDANCE;
static Policy  g_policy = POLICY_PREEMPT;

static const char *mode_name(Mode m)   { return m == MODE_AVOIDANCE ? "AVOIDANCE" : "DETECTION"; }
static const char *policy_name(Policy p) { return p == POLICY_PREEMPT ? "PREEMPT" : "TERMINATE"; }

/* ------------------------------------------------------------ input help */

static bool read_line(const char *prompt, char *buf, size_t len)
{
    printf("%s", prompt);
    fflush(stdout);
    if (!fgets(buf, (int)len, stdin)) return false;
    if (!strchr(buf, '\n')) {               /* throw away an over-long rest */
        int c;
        while ((c = getchar()) != '\n' && c != EOF)
            ;
    }
    buf[strcspn(buf, "\r\n")] = '\0';
    return true;
}

static bool to_number(const char *text, long lo, long hi, long *out)
{
    char *end;
    errno = 0;
    long v = strtol(text, &end, 10);
    if (end == text || *end || errno || v < lo || v > hi) return false;
    *out = v;
    return true;
}

/* Asks for a node name and its resource vector. */
static bool read_node_vector(const char *what, int *node, int *vec)
{
    char line[256], prompt[96];

    if (!read_line("Node name (e.g. N1): ", line, sizeof line)) return false;
    *node = system_find_node(&g_sys, line);
    if (*node < 0) {
        say(C_RED, "ERROR: unknown node '%s'\n", line);
        return false;
    }
    snprintf(prompt, sizeof prompt, "%s vector (%d numbers): ", what, g_sys.m);
    if (!read_line(prompt, line, sizeof line)) return false;

    char *tok = strtok(line, " ,\t");
    for (int j = 0; j < g_sys.m; j++) {
        long v;
        if (!tok || !to_number(tok, INT_MIN, INT_MAX, &v)) {
            say(C_RED, "ERROR: enter exactly %d numbers\n", g_sys.m);
            return false;
        }
        vec[j] = (int)v;
        tok = strtok(NULL, " ,\t");
    }
    if (tok) {
        say(C_RED, "ERROR: enter exactly %d numbers\n", g_sys.m);
        return false;
    }
    return true;
}

/* ---------------------------------------------------------- operations */

static void start_monitor(void)
{
    monitor_init(&g_mon, &g_sys, g_mode, g_policy, 10);
}

static void restart_monitor(void)
{
    monitor_destroy(&g_mon);
    start_monitor();
}

static bool load_config(const char *path)
{
    char err[200];
    if (g_loaded) monitor_destroy(&g_mon);
    if (topology_load(&g_sys, path, err, sizeof err)) {
        say(C_RED, "ERROR: %s\n", err);
        if (g_loaded) start_monitor();
        return false;
    }
    g_loaded = true;
    start_monitor();
    say(C_GREEN, "Loaded %s: %d nodes, %d resource types\n", path, g_sys.n, g_sys.m);
    return true;
}

static void show_safety(void)
{
    int seq[MAX_NODES], len;
    if (safety_check(&g_sys, seq, &len)) {
        say(C_GREEN, "State is SAFE. Safe sequence: ");
        print_sequence(&g_sys, seq, len);
        printf("\n");
    } else {
        say(C_RED, "State is UNSAFE. Only these nodes are sure to finish: ");
        print_sequence(&g_sys, seq, len);
        printf("\n");
    }
}

static void do_request(int node, const int *vec)
{
    ReqResult r = monitor_submit(&g_mon, node, vec);
    const char *c = r == REQ_GRANTED ? C_GREEN : r < REQ_PREEMPTED ? C_YELLOW : C_RED;

    say(c, "%s\n", req_result_text(r));
    if (r == REQ_GRANTED && g_mode == MODE_AVOIDANCE)
        show_safety();
    if (r == REQ_WAIT_UNSAFE)
        printf("Reason: after this allocation no safe sequence would exist.\n");
    if (r == REQ_WAIT_UNAVAILABLE)
        printf("The request is kept in the FIFO wait queue.\n");
}

/* Shows the graphs, the DFS result and the Work/Finish result. */
static bool analyse_deadlock(void)
{
    int wfg[MAX_NODES][MAX_NODES], cycle[MAX_NODES], clen;
    int order[MAX_NODES], olen;
    bool dead[MAX_NODES];

    build_wfg(&g_sys, wfg);
    printf("\nWait-For Graph edges:\n");
    int edges = 0;
    for (int i = 0; i < g_sys.n; i++)
        for (int j = 0; j < g_sys.n; j++)
            if (wfg[i][j]) {
                printf("  %s -> %s\n", g_sys.node[i].name, g_sys.node[j].name);
                edges++;
            }
    if (!edges) printf("  (none)\n");

    if (find_cycle_dfs(g_sys.n, wfg, cycle, &clen)) {
        printf("DFS found a cycle: ");
        for (int k = 0; k < clen; k++)
            printf("%s -> ", g_sys.node[cycle[k]].name);
        printf("%s\n", g_sys.node[cycle[0]].name);
        if (!is_single_instance(&g_sys))
            printf("  (multi-instance resources: a cycle alone does not prove "
                   "deadlock, see Work/Finish)\n");
    } else {
        printf("DFS found no cycle.\n");
    }

    int count = detect_deadlock(&g_sys, dead, order, &olen);
    printf("Work/Finish finish order: ");
    print_sequence(&g_sys, order, olen);
    printf("\n");
    if (count) {
        say(C_RED, "DEADLOCK DETECTED. Deadlocked nodes:");
        for (int i = 0; i < g_sys.n; i++)
            if (dead[i]) say(C_RED, " %s", g_sys.node[i].name);
        printf("\n");
        int v = select_victim(&g_sys, dead);
        if (v >= 0)
            printf("Cheapest victim: %s (cost %d)\n", g_sys.node[v].name,
                   victim_cost(&g_sys, v));
    } else {
        say(C_GREEN, "NO DEADLOCK.\n");
    }
    write_rag_dot(&g_sys, dead, OUTPUT_DIR "/rag.dot");
    write_wfg_dot(&g_sys, dead, OUTPUT_DIR "/wfg.dot");
    printf("Graphs written to %s/rag.dot and %s/wfg.dot\n", OUTPUT_DIR, OUTPUT_DIR);
    return count > 0;
}

static void do_recover(void)
{
    pthread_mutex_lock(&g_mon.lock);
    int victims = monitor_detect_and_recover_locked(&g_mon, true);
    pthread_mutex_unlock(&g_mon.lock);

    if (!victims) {
        say(C_GREEN, "No deadlock, nothing to recover.\n");
        return;
    }
    int order[MAX_NODES], len;
    if (detect_deadlock(&g_sys, NULL, order, &len) == 0) {
        say(C_GREEN, "Deadlock removed with %d victim(s); the remaining nodes can finish.\n",
            victims);
    }
    print_tables(stdout, &g_sys, g_color);
}

static void run_one(const SimConfig *cfg)
{
    Stats st;
    say(C_BOLD, "\n--- Simulation: %s mode, %s workload ---\n", mode_name(cfg->mode),
        cfg->script ? "scripted" : "random");
    if (simulate(&g_sys, cfg, &st))
        say(C_RED, "ERROR: could not start all threads\n");
    print_stats(stdout, &st);
    printf("Event log: %s/events.log\n", OUTPUT_DIR);
}

static void run_compare(SimConfig cfg)
{
    Stats a, d;
    cfg.mode = MODE_AVOIDANCE;
    log_event(EV_INFO, NULL, -1, NULL, "comparison: avoidance run");
    int bad = simulate(&g_sys, &cfg, &a);
    cfg.mode = MODE_DETECTION;
    log_event(EV_INFO, NULL, -1, NULL, "comparison: detection run");
    bad |= simulate(&g_sys, &cfg, &d);
    if (bad) say(C_RED, "ERROR: could not start all threads\n");
    write_comparison_report(&a, &d, &cfg, &g_sys, OUTPUT_DIR "/report.txt");
    printf("\nReport saved to %s/report.txt\n", OUTPUT_DIR);
}

/* ---------------------------------------------------------------- menu */

static long ask_number(const char *prompt, long lo, long hi, long fallback)
{
    char line[64];
    long v;
    if (!read_line(prompt, line, sizeof line) || !*line) return fallback;
    if (to_number(line, lo, hi, &v)) return v;
    printf("Invalid value, using %ld\n", fallback);
    return fallback;
}

static void menu_simulation(bool compare)
{
    char path[256], err[200];
    SimConfig cfg = { g_mode, g_policy, 50, 1, 10, NULL };

    if (!read_line("Script file (press Enter for random requests): ", path, sizeof path))
        return;
    if (*path) {
        if (script_load(&g_script, &g_sys, path, err, sizeof err)) {
            say(C_RED, "ERROR: %s\n", err);
            return;
        }
        cfg.script = &g_script;
    } else {
        cfg.ops_per_node = (int)ask_number("Operations per node [50]: ", 1, 100000, 50);
        cfg.seed = (unsigned)ask_number("Random seed [1]: ", 0, 1000000, 1);
    }
    cfg.interval_ms = (int)ask_number("Detector interval in ms [10]: ", 1, 10000, 10);
    g_verbose = ask_number("Show every event live? 0=no 1=yes [0]: ", 0, 1, 0) == 1;
    if (compare) run_compare(cfg);
    else run_one(&cfg);
    g_verbose = false;
}

static void menu(void)
{
    char line[256];

    for (;;) {
        say(C_BOLD, "\n===== NRADE : Network Resource Allocation & Deadlock Detection =====\n");
        printf("  mode: %s   recovery: %s\n", mode_name(g_mode), policy_name(g_policy));
        printf("   1. Load configuration file       8. Run deadlock detection (RAG/WFG)\n"
               "   2. Enter topology manually       9. Recover from deadlock\n"
               "   3. Show tables                  10. Switch mode (avoidance/detection)\n"
               "   4. Safety check (Banker)        11. Switch recovery policy\n"
               "   5. Request resources            12. Run multithreaded simulation\n"
               "   6. Release resources            13. Compare both modes (report)\n"
               "   7. Finish a node                 0. Exit\n");
        if (!read_line("Choice: ", line, sizeof line)) break;

        long ch;
        if (!to_number(line, 0, 13, &ch)) {
            say(C_RED, "Invalid choice\n");
            continue;
        }
        if (ch == 0) break;
        if (ch == 1) {
            if (read_line("Config file (e.g. config/textbook.txt): ", line, sizeof line))
                load_config(line);
            continue;
        }
        if (ch == 2) {
            if (g_loaded) monitor_destroy(&g_mon);
            if (topology_from_menu(&g_sys, stdin) == 0) {
                g_loaded = true;
                say(C_GREEN, "Topology created\n");
            }
            if (g_loaded) start_monitor();
            continue;
        }
        if (ch == 10) {
            g_mode = g_mode == MODE_AVOIDANCE ? MODE_DETECTION : MODE_AVOIDANCE;
            if (g_loaded) {
                restart_monitor();
                if (g_mode == MODE_AVOIDANCE && !safety_check(&g_sys, NULL, NULL))
                    say(C_YELLOW, "Warning: the current state is already unsafe. "
                                  "Reload the configuration for a clean start.\n");
            }
            continue;
        }
        if (ch == 11) {
            g_policy = g_policy == POLICY_PREEMPT ? POLICY_TERMINATE : POLICY_PREEMPT;
            if (g_loaded) g_mon.policy = g_policy;
            continue;
        }
        if (!g_loaded) {
            say(C_YELLOW, "First load a configuration (1) or enter one (2).\n");
            continue;
        }

        int node, vec[MAX_RES] = { 0 };
        switch (ch) {
        case 3:
            print_tables(stdout, &g_sys, g_color);
            break;
        case 4:
            show_safety();
            break;
        case 5:
            if (read_node_vector("Request", &node, vec))
                do_request(node, vec);
            break;
        case 6:
            if (read_node_vector("Release", &node, vec)) {
                ReqResult r = monitor_release(&g_mon, node, vec);
                say(r == REQ_GRANTED ? C_GREEN : C_RED, "%s\n",
                    r == REQ_GRANTED ? "RELEASED" : req_result_text(r));
            }
            break;
        case 7:
            if (read_line("Node name: ", line, sizeof line)) {
                node = system_find_node(&g_sys, line);
                if (node < 0)
                    say(C_RED, "ERROR: unknown node\n");
                else if (g_sys.node[node].state != NODE_READY)
                    say(C_RED, "ERROR: only a READY node can finish\n");
                else {
                    monitor_finish_node(&g_mon, node);
                    say(C_GREEN, "Node finished and released its resources\n");
                }
            }
            break;
        case 8:
            analyse_deadlock();
            break;
        case 9:
            do_recover();
            break;
        case 12:
            menu_simulation(false);
            break;
        case 13:
            menu_simulation(true);
            break;
        }
    }
}

/* -------------------------------------------------------- command line */

static void usage(const char *prog)
{
    printf("Usage: %s                      interactive menu\n"
           "       %s -c FILE [action] [options]\n"
           "Actions:\n"
           "  --check               tables and Banker safety check\n"
           "  --detect              deadlock detection on the loaded state\n"
           "  --recover             detection followed by recovery\n"
           "  --simulate            multithreaded simulation\n"
           "  --compare             run both modes and write a report\n"
           "Options:\n"
           "  --mode avoid|detect   --policy preempt|terminate\n"
           "  --script FILE         --ops N        --seed N\n"
           "  --interval MS         --verbose      --no-color\n", prog, prog);
}

int main(int argc, char **argv)
{
    const char *config = NULL, *script = NULL, *action = NULL;
    SimConfig cfg = { MODE_AVOIDANCE, POLICY_PREEMPT, 50, 1, 10, NULL };
    bool verbose = false;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *val = i + 1 < argc ? argv[i + 1] : NULL;
        long num;

        if (!strcmp(a, "--check") || !strcmp(a, "--detect") || !strcmp(a, "--recover") ||
            !strcmp(a, "--simulate") || !strcmp(a, "--compare"))
            action = a;
        else if (!strcmp(a, "--verbose"))  verbose = true;
        else if (!strcmp(a, "--no-color")) g_color = false;
        else if (!strcmp(a, "--help"))     { usage(argv[0]); return 0; }
        else if (!strcmp(a, "-c") && val)       { config = val; i++; }
        else if (!strcmp(a, "--script") && val) { script = val; i++; }
        else if (!strcmp(a, "--mode") && val &&
                 (!strcmp(val, "avoid") || !strcmp(val, "detect"))) {
            cfg.mode = val[0] == 'a' ? MODE_AVOIDANCE : MODE_DETECTION;
            i++;
        } else if (!strcmp(a, "--policy") && val &&
                   (!strcmp(val, "preempt") || !strcmp(val, "terminate"))) {
            cfg.policy = val[0] == 'p' ? POLICY_PREEMPT : POLICY_TERMINATE;
            i++;
        } else if (!strcmp(a, "--ops") && val && to_number(val, 1, 100000, &num)) {
            cfg.ops_per_node = (int)num; i++;
        } else if (!strcmp(a, "--seed") && val && to_number(val, 0, 1000000, &num)) {
            cfg.seed = (unsigned)num; i++;
        } else if (!strcmp(a, "--interval") && val && to_number(val, 1, 10000, &num)) {
            cfg.interval_ms = (int)num; i++;
        } else {
            fprintf(stderr, "invalid argument: %s\n", a);
            usage(argv[0]);
            return 2;
        }
    }

    log_open(OUTPUT_DIR "/events.log");
    g_mode = cfg.mode;
    g_policy = cfg.policy;
    if (config && !load_config(config)) {
        log_close();
        return 1;
    }

    if (!action) {
        menu();
    } else if (!g_loaded) {
        fprintf(stderr, "%s needs -c FILE\n", action);
        log_close();
        return 2;
    } else {
        if (script) {
            char err[200];
            if (script_load(&g_script, &g_sys, script, err, sizeof err)) {
                fprintf(stderr, "ERROR: %s\n", err);
                log_close();
                return 1;
            }
            cfg.script = &g_script;
        }
        g_verbose = verbose;
        if (!strcmp(action, "--check")) {
            print_tables(stdout, &g_sys, g_color);
            show_safety();
        } else if (!strcmp(action, "--detect")) {
            print_tables(stdout, &g_sys, g_color);
            analyse_deadlock();
        } else if (!strcmp(action, "--recover")) {
            print_tables(stdout, &g_sys, g_color);
            analyse_deadlock();
            do_recover();
        } else if (!strcmp(action, "--simulate")) {
            run_one(&cfg);
        } else {
            run_compare(cfg);
        }
    }

    if (g_loaded) monitor_destroy(&g_mon);
    log_close();
    return 0;
}
