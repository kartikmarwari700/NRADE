/*
 * M6 (part 1): Recovery (owner: Kartik)
 * Victim selection by minimum cost, then termination or pre-emption.
 */
#include "nrade.h"
#include <string.h>

/*
 * cost = w1*priority + w2*resources held + w3*work done
 *        + penalty * number of times already chosen
 * The penalty stops the same node from being chosen again and again.
 */
int victim_cost(const System *s, int node)
{
    int held = 0;
    for (int j = 0; j < s->m; j++)
        held += s->alloc[node][j];
    return W_PRIORITY * s->node[node].priority
         + W_HELD     * held
         + W_WORK     * s->node[node].cpu_time
         + W_VICTIM   * s->node[node].victim_count;
}

/* Cheapest deadlocked node that actually holds something; -1 if none. */
int select_victim(const System *s, const bool *deadlocked)
{
    int best = -1, best_cost = 0;
    for (int i = 0; i < s->n; i++) {
        if (!deadlocked[i] || vec_is_zero(s->alloc[i], s->m)) continue;
        int c = victim_cost(s, i);
        if (best < 0 || c < best_cost) {
            best = i;
            best_cost = c;
        }
    }
    return best;
}

/*
 * Takes back all resources of the victim. With pre-emption the node returns
 * to its checkpoint (the start of its job, where it held nothing).
 */
void apply_recovery(System *s, int victim, Policy policy)
{
    release_all(s, victim);
    memset(s->request[victim], 0, sizeof s->request[victim]);
    s->node[victim].victim_count++;
    s->node[victim].cpu_time = 0;         /* work since checkpoint is lost */
    if (policy == POLICY_TERMINATE) {
        memset(s->need[victim], 0, sizeof s->need[victim]);
        s->node[victim].state = NODE_TERMINATED;
    } else {
        s->node[victim].state = NODE_READY;
    }
}
