/*
 * M1: Topology & Resource Manager (owner: Ishita)
 * Reads nodes and resources, validates them and builds the Banker matrices.
 *
 * Config file format ('#' starts a comment):
 *   resource <name> <LINK_BW|BUFFER|PORT|CHANNEL> <instances>
 *   node <name> <priority>
 *   max     <node> v1 v2 ... vM
 *   alloc   <node> v1 v2 ... vM      (optional)
 *   request <node> v1 v2 ... vM      (optional pending request)
 */
#include "nrade.h"
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static const char *TYPE_NAMES[] = { "LINK_BW", "BUFFER", "PORT", "CHANNEL" };

const char *res_type_name(ResType t) { return TYPE_NAMES[t]; }

void system_init(System *s)
{
    memset(s, 0, sizeof *s);
}

int system_find_node(const System *s, const char *name)
{
    for (int i = 0; i < s->n; i++)
        if (strcmp(s->node[i].name, name) == 0)
            return i;
    return -1;
}

static int parse_int(const char *tok, int *out)
{
    char *end;
    if (!tok) return -1;
    errno = 0;
    long v = strtol(tok, &end, 10);
    if (errno || *end || end == tok || v < INT_MIN || v > INT_MAX) return -1;
    *out = (int)v;
    return 0;
}

/* Reads exactly m non-negative numbers using strtok. */
static int parse_vector(int *v, int m)
{
    for (int j = 0; j < m; j++)
        if (parse_int(strtok(NULL, " \t\r\n"), &v[j]) || v[j] < 0)
            return -1;
    return strtok(NULL, " \t\r\n") ? -1 : 0;
}

static int add_resource(System *s, const char *name, const char *type, int count)
{
    if (s->m >= MAX_RES || count <= 0 || count > 100000 || strlen(name) >= NAME_LEN)
        return -1;
    for (int t = 0; t < 4; t++) {
        if (strcmp(type, TYPE_NAMES[t]) == 0) {
            Resource *r = &s->res[s->m];
            r->id = s->m;
            r->type = (ResType)t;
            strcpy(r->name, name);
            r->instances = count;
            s->total[s->m] = count;
            s->m++;
            return 0;
        }
    }
    return -1;
}

static int add_node(System *s, const char *name, int priority)
{
    if (s->n >= MAX_NODES || strlen(name) >= NAME_LEN || priority < 0 ||
        priority > 1000 || system_find_node(s, name) >= 0)
        return -1;
    Node *nd = &s->node[s->n];
    nd->id = s->n;
    strcpy(nd->name, name);
    nd->priority = priority;
    nd->state = NODE_READY;
    s->n++;
    return 0;
}

/* Checks the whole system and computes Need and Available. */
int system_finalize(System *s, char *err, size_t errlen)
{
    if (s->n == 0 || s->m == 0) {
        snprintf(err, errlen, "need at least one node and one resource");
        return -1;
    }
    for (int j = 0; j < s->m; j++) {
        long used = 0;
        for (int i = 0; i < s->n; i++) {
            if (s->max[i][j] > s->total[j]) {
                snprintf(err, errlen, "max claim of %s for %s is more than total",
                         s->node[i].name, s->res[j].name);
                return -1;
            }
            if (s->alloc[i][j] + (long)s->request[i][j] > s->max[i][j]) {
                snprintf(err, errlen, "alloc + request of %s for %s is more than its max",
                         s->node[i].name, s->res[j].name);
                return -1;
            }
            s->need[i][j] = s->max[i][j] - s->alloc[i][j];
            used += s->alloc[i][j];
        }
        if (used > s->total[j]) {
            snprintf(err, errlen, "allocated instances of %s are more than total",
                     s->res[j].name);
            return -1;
        }
        s->available[j] = s->total[j] - (int)used;
    }
    for (int i = 0; i < s->n; i++)
        s->node[i].state = vec_is_zero(s->request[i], s->m) ? NODE_READY : NODE_BLOCKED;
    return 0;
}

int topology_load(System *s, const char *path, char *err, size_t errlen)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        snprintf(err, errlen, "cannot open '%s'", path);
        return -1;
    }
    System *tmp = calloc(1, sizeof *tmp);   /* keep old system if load fails */
    if (!tmp) { fclose(f); snprintf(err, errlen, "out of memory"); return -1; }

    char line[512];
    int lineno = 0, bad = 0;
    while (!bad && fgets(line, sizeof line, f)) {
        lineno++;
        char *hash = strchr(line, '#');
        if (hash) *hash = '\0';
        char *key = strtok(line, " \t\r\n");
        if (!key) continue;

        if (strcmp(key, "resource") == 0) {
            char *name = strtok(NULL, " \t\r\n");
            char *type = strtok(NULL, " \t\r\n");
            int count;
            bad = tmp->n > 0 || !name || !type ||
                  parse_int(strtok(NULL, " \t\r\n"), &count) ||
                  add_resource(tmp, name, type, count);
        } else if (strcmp(key, "node") == 0) {
            char *name = strtok(NULL, " \t\r\n");
            int prio;
            bad = !name || parse_int(strtok(NULL, " \t\r\n"), &prio) ||
                  add_node(tmp, name, prio);
        } else if (!strcmp(key, "max") || !strcmp(key, "alloc") || !strcmp(key, "request")) {
            char *name = strtok(NULL, " \t\r\n");
            int id = name ? system_find_node(tmp, name) : -1;
            int (*mat)[MAX_RES] = key[0] == 'm' ? tmp->max
                                : key[0] == 'a' ? tmp->alloc : tmp->request;
            bad = id < 0 || parse_vector(mat[id], tmp->m);
        } else {
            bad = 1;
        }
    }
    fclose(f);

    if (bad) {
        snprintf(err, errlen, "%s: invalid line %d", path, lineno);
        free(tmp);
        return -1;
    }
    if (system_finalize(tmp, err, errlen)) {
        free(tmp);
        return -1;
    }
    *s = *tmp;
    free(tmp);
    return 0;
}

/* Reads one integer from the keyboard inside [lo, hi]; -1 on end of input. */
static int ask_int(FILE *in, const char *prompt, int lo, int hi, int *out)
{
    char line[128];
    for (;;) {
        printf("%s [%d-%d]: ", prompt, lo, hi);
        fflush(stdout);
        if (!fgets(line, sizeof line, in)) return -1;
        char *tok = strtok(line, " \t\r\n");
        if (tok && !parse_int(tok, out) && *out >= lo && *out <= hi) return 0;
        printf("  invalid value, try again\n");
    }
}

/* FR1: enter the topology from the menu instead of a file. */
int topology_from_menu(System *s, FILE *in)
{
    System *t = calloc(1, sizeof *t);
    if (!t) return -1;
    int m, n, v, rc = -1;
    char prompt[96], name[NAME_LEN], err[160];

    if (ask_int(in, "Number of resource types", 1, MAX_RES, &m)) goto out;
    for (int j = 0; j < m; j++) {
        int type;
        snprintf(prompt, sizeof prompt,
                 "R%d type (0=LINK_BW 1=BUFFER 2=PORT 3=CHANNEL)", j);
        if (ask_int(in, prompt, 0, 3, &type)) goto out;
        snprintf(prompt, sizeof prompt, "R%d instances", j);
        if (ask_int(in, prompt, 1, 1000, &v)) goto out;
        snprintf(name, sizeof name, "R%d", j);
        add_resource(t, name, TYPE_NAMES[type], v);
    }
    if (ask_int(in, "Number of nodes", 1, MAX_NODES, &n)) goto out;
    for (int i = 0; i < n; i++) {
        snprintf(name, sizeof name, "N%d", i);
        snprintf(prompt, sizeof prompt, "%s priority", name);
        if (ask_int(in, prompt, 0, 1000, &v)) goto out;
        add_node(t, name, v);
        for (int j = 0; j < m; j++) {
            snprintf(prompt, sizeof prompt, "%s max claim of R%d", name, j);
            if (ask_int(in, prompt, 0, t->total[j], &t->max[i][j])) goto out;
        }
    }
    if (system_finalize(t, err, sizeof err)) {
        printf("%s\n", err);
        goto out;
    }
    *s = *t;
    rc = 0;
out:
    free(t);
    return rc;
}

/* Clean state for a fresh simulation: nobody holds or waits for anything. */
void system_reset_allocation(System *s)
{
    memset(s->alloc, 0, sizeof s->alloc);
    memset(s->request, 0, sizeof s->request);
    memcpy(s->need, s->max, sizeof s->need);
    memcpy(s->available, s->total, sizeof s->available);
    for (int i = 0; i < s->n; i++) {
        s->node[i].state = NODE_READY;
        s->node[i].cpu_time = 0;
        s->node[i].victim_count = 0;
        s->node[i].checkpoint = 0;
    }
}

/* NFR1: Available + sum(Allocation) must always equal Total. */
bool system_invariant_ok(const System *s)
{
    for (int j = 0; j < s->m; j++) {
        int sum = s->available[j];
        if (s->available[j] < 0) return false;
        for (int i = 0; i < s->n; i++) {
            if (s->alloc[i][j] < 0 || s->need[i][j] < 0) return false;
            sum += s->alloc[i][j];
        }
        if (sum != s->total[j]) return false;
    }
    return true;
}
