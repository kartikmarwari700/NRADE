/*
 * Automatic tests: TC1 - TC8 of the synopsis plus recovery, validation,
 * starvation/stress and performance checks.
 */
#include "nrade.h"
#include <string.h>

static int checks, failures;

#define CHECK(cond, text)                                        \
    do {                                                         \
        checks++;                                                \
        if (cond) printf("   pass  %s\n", text);                 \
        else { failures++; printf("   FAIL  %s (line %d)\n", text, __LINE__); } \
    } while (0)

static System sys;
static Monitor mon;
static Script script;

static void load(const char *path)
{
    char err[200];
    if (topology_load(&sys, path, err, sizeof err)) {
        printf("cannot load %s: %s\n", path, err);
        failures++;
    }
}

static bool seq_is(const int *seq, int len, const char *expected)
{
    char buf[512] = "";
    for (int k = 0; k < len; k++) {
        if (k) strcat(buf, " ");
        strcat(buf, sys.node[seq[k]].name);
    }
    if (strcmp(buf, expected)) printf("         got: %s\n", buf);
    return strcmp(buf, expected) == 0;
}

/* Is "order" a legal finish order for the detection algorithm? */
static bool valid_finish_order(const int *order, int len)
{
    int work[MAX_RES];
    memcpy(work, sys.available, sizeof work);
    for (int k = 0; k < len; k++) {
        int i = order[k];
        if (!vec_leq(sys.request[i], work, sys.m)) return false;
        for (int j = 0; j < sys.m; j++) work[j] += sys.alloc[i][j];
    }
    return true;
}

static void tc1_to_tc4(void)
{
    int seq[MAX_NODES], len = 0;

    printf("\nTC1 safety check on the textbook state\n");
    load("config/textbook.txt");
    CHECK(sys.available[0] == 3 && sys.available[1] == 3 && sys.available[2] == 2,
          "Available = (3,3,2)");
    CHECK(safety_check(&sys, seq, &len), "state is SAFE");
    CHECK(seq_is(seq, len, "N1 N3 N4 N0 N2"), "safe sequence <N1,N3,N4,N0,N2>");

    monitor_init(&mon, &sys, MODE_AVOIDANCE, POLICY_PREEMPT, 10);

    printf("\nTC2 N1 requests (1,0,2)\n");
    int r1[MAX_RES] = { 1, 0, 2 };
    CHECK(monitor_submit(&mon, 1, r1) == REQ_GRANTED, "GRANTED");
    CHECK(safety_check(&sys, NULL, NULL), "state remains safe");

    printf("\nTC3 N0 requests (0,2,0)\n");
    int r2[MAX_RES] = { 0, 2, 0 };
    CHECK(monitor_submit(&mon, 0, r2) == REQ_WAIT_UNSAFE, "DENIED because UNSAFE");
    CHECK(sys.node[0].state == NODE_BLOCKED, "N0 waits");
    CHECK(sys.available[1] == 3 && sys.alloc[0][1] == 1, "pretend allocation rolled back");

    printf("\nTC4 request above the maximum claim\n");
    System before = sys;
    int r3[MAX_RES] = { 0, 3, 0 };      /* N3 needs only (0,1,1) */
    CHECK(monitor_submit(&mon, 3, r3) == ERR_EXCEEDS_NEED, "error: request exceeds need");
    CHECK(!memcmp(before.alloc, sys.alloc, sizeof sys.alloc) &&
          !memcmp(before.available, sys.available, sizeof sys.available) &&
          !memcmp(before.need, sys.need, sizeof sys.need), "matrices unchanged");
    monitor_destroy(&mon);
}

static void tc5(void)
{
    int wfg[MAX_NODES][MAX_NODES], cycle[MAX_NODES], len = 0;
    bool dead[MAX_NODES];

    printf("\nTC5 single-instance circular wait\n");
    load("config/single_cycle.txt");
    build_wfg(&sys, wfg);
    CHECK(is_single_instance(&sys), "all resources are single instance");
    CHECK(find_cycle_dfs(sys.n, wfg, cycle, &len), "DFS detects a cycle");
    CHECK(seq_is(cycle, len, "N1 N2 N3"), "cycle N1 -> N2 -> N3 -> N1");
    CHECK(detect_deadlock(&sys, dead, NULL, NULL) == 3, "all three nodes deadlocked");
    CHECK(write_rag_dot(&sys, dead, OUTPUT_DIR "/tc5_rag.dot") == 0 &&
          write_wfg_dot(&sys, dead, OUTPUT_DIR "/tc5_wfg.dot") == 0, "DOT graphs generated");
}

static void tc6_tc7(void)
{
    int order[MAX_NODES], len = 0;
    bool dead[MAX_NODES];

    printf("\nTC6 multi-instance detection, no deadlock\n");
    load("config/multi_nodeadlock.txt");
    CHECK(vec_is_zero(sys.available, sys.m), "Available = (0,0,0)");
    CHECK(detect_deadlock(&sys, dead, order, &len) == 0, "NO deadlock");
    CHECK(seq_is(order, len, "N0 N2 N3 N4 N1"), "finish order <N0,N2,N3,N4,N1>");
    int synopsis_order[] = { 0, 2, 3, 1, 4 };
    CHECK(valid_finish_order(synopsis_order, 5),
          "synopsis order <N0,N2,N3,N1,N4> is also valid");

    printf("\nTC7 N2 asks for one more instance\n");
    load("config/multi_deadlock.txt");
    CHECK(detect_deadlock(&sys, dead, NULL, NULL) == 4, "4 nodes deadlocked");
    CHECK(!dead[0] && dead[1] && dead[2] && dead[3] && dead[4], "they are N1 N2 N3 N4");

    int cheapest = select_victim(&sys, dead);
    bool is_min = cheapest >= 0;
    for (int i = 0; i < sys.n; i++)
        if (dead[i] && victim_cost(&sys, i) < victim_cost(&sys, cheapest)) is_min = false;
    CHECK(is_min, "victim has the lowest cost");

    monitor_init(&mon, &sys, MODE_DETECTION, POLICY_PREEMPT, 10);
    pthread_mutex_lock(&mon.lock);
    int victims = monitor_detect_and_recover_locked(&mon, false);
    pthread_mutex_unlock(&mon.lock);
    CHECK(victims == 1, "one victim is enough");
    CHECK(detect_deadlock(&sys, dead, NULL, NULL) == 0, "the rest can finish");
    CHECK(system_invariant_ok(&sys), "invariant holds after recovery");
    monitor_destroy(&mon);

    printf("\nTC7b same deadlock with the TERMINATE policy\n");
    load("config/multi_deadlock.txt");
    monitor_init(&mon, &sys, MODE_DETECTION, POLICY_TERMINATE, 10);
    pthread_mutex_lock(&mon.lock);
    monitor_detect_and_recover_locked(&mon, false);
    pthread_mutex_unlock(&mon.lock);
    CHECK(sys.node[cheapest].state == NODE_TERMINATED, "victim is terminated");
    CHECK(detect_deadlock(&sys, dead, NULL, NULL) == 0, "deadlock removed");
    monitor_destroy(&mon);
}

static void tc8_stress(void)
{
    load("config/stress.txt");
    for (int mode = 0; mode < 2; mode++) {
        for (int policy = 0; policy < 2; policy++) {
            if (mode == MODE_AVOIDANCE && policy == POLICY_TERMINATE) continue;
            SimConfig cfg = { (Mode)mode, (Policy)policy, 50, 7, 2, NULL };
            Stats st;
            printf("\nTC8 stress: 20 threads x 50 operations, %s%s\n",
                   mode == MODE_AVOIDANCE ? "avoidance" : "detection",
                   mode == MODE_AVOIDANCE ? "" :
                   policy == POLICY_PREEMPT ? " + preempt" : " + terminate");
            CHECK(simulate(&sys, &cfg, &st) == 0, "all threads started and joined");
            CHECK(st.completed + st.terminated == 20, "every node ended (no hang)");
            CHECK(st.invariant_violations == 0, "Available + Allocation = Total always");
            CHECK(st.errors == 0, "no invalid operation");
            if (mode == MODE_AVOIDANCE)
                CHECK(st.deadlocks == 0 && st.victims == 0, "avoidance never deadlocks");
            printf("         grants=%ld blocks=%ld deadlocks=%ld victims=%ld\n",
                   st.grants, st.blocks, st.deadlocks, st.victims);
        }
    }
}

static void scripted_ring(void)
{
    char err[200];
    Stats st;

    printf("\nExtra: scripted ring with real threads\n");
    load("config/single_cycle.txt");
    CHECK(script_load(&script, &sys, "config/ring_script.txt", err, sizeof err) == 0,
          "script loaded");

    SimConfig cfg = { MODE_DETECTION, POLICY_PREEMPT, 0, 1, 5, &script };
    simulate(&sys, &cfg, &st);
    CHECK(st.deadlocks >= 1, "detection mode: deadlock happens and is detected");
    CHECK(st.completed == 3 && st.invariant_violations == 0, "all nodes finish after recovery");

    cfg.mode = MODE_AVOIDANCE;
    simulate(&sys, &cfg, &st);
    CHECK(st.deadlocks == 0 && st.unsafe_denials >= 1,
          "avoidance mode: unsafe request delayed, no deadlock");
    CHECK(st.completed == 3, "all nodes finish");
}

static void invalid_input(void)
{
    char err[200];
    int neg[MAX_RES] = { -1, 0, 0 }, huge[MAX_RES] = { 99, 0, 0 }, one[MAX_RES] = { 1, 0, 0 };

    printf("\nExtra: invalid input is rejected (NFR5)\n");
    load("config/textbook.txt");
    CHECK(validate_request(&sys, 42, one) == ERR_BAD_NODE, "unknown node");
    CHECK(validate_request(&sys, -1, one) == ERR_BAD_NODE, "negative node id");
    CHECK(validate_request(&sys, 0, neg) == ERR_NEGATIVE, "negative value");
    CHECK(validate_request(&sys, 0, huge) == ERR_EXCEEDS_TOTAL, "more than total");
    CHECK(release_resources(&sys, 0, one) == ERR_EXCEEDS_ALLOC, "release more than held");
    CHECK(topology_load(&sys, "config/does_not_exist.txt", err, sizeof err) != 0,
          "missing file");
    CHECK(topology_load(&sys, "Makefile", err, sizeof err) != 0, "wrong file format");
    CHECK(sys.n == 5 && system_invariant_ok(&sys), "old system kept after failed load");
}

/* NFR2: detection on 50 nodes x 10 resource types in under 50 ms. */
static void performance(void)
{
    static System big;
    bool dead[MAX_NODES];

    printf("\nExtra: detection speed, 50 nodes x 10 resource types (NFR2)\n");
    system_init(&big);
    big.n = 50;
    big.m = 10;
    for (int j = 0; j < 10; j++) big.total[j] = 50;
    /* worst case: nodes can only finish in reverse order, one per pass */
    for (int i = 0; i < 50; i++)
        for (int j = 0; j < 10; j++) {
            big.alloc[i][j] = 1;
            big.request[i][j] = 49 - i;
        }

    double t0 = now_ms();
    int count = 0;
    for (int k = 0; k < 100; k++)
        count = detect_deadlock(&big, dead, NULL, NULL);
    double each = (now_ms() - t0) / 100;
    printf("         average time: %.4f ms\n", each);
    CHECK(count == 0, "correct result");
    CHECK(each < 50.0, "under 50 ms");
}

int main(void)
{
    g_color = false;
    log_open(OUTPUT_DIR "/test_events.log");

    tc1_to_tc4();
    tc5();
    tc6_tc7();
    tc8_stress();
    scripted_ring();
    invalid_input();
    performance();

    log_close();
    printf("\n%d checks, %d failed\n", checks, failures);
    return failures != 0;
}
