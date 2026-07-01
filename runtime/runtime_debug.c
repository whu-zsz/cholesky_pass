#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <unistd.h>

#define POOL_BLOCK 8388608

typedef struct PoolBlock {
    char *data; size_t used, cap;
    struct PoolBlock *next;
} PoolBlock;
typedef struct { PoolBlock *head; } Pool;
static Pool pool;

static void pool_init(Pool *p) {
    p->head = (PoolBlock*)malloc(sizeof(PoolBlock));
    p->head->data = (char*)malloc(POOL_BLOCK);
    p->head->used = 0; p->head->cap = POOL_BLOCK; p->head->next = NULL;
}
static void pool_free(Pool *p) {
    PoolBlock *b = p->head;
    while (b) { PoolBlock *n = b->next; free(b->data); free(b); b = n; }
    p->head = NULL;
}
static void *pool_alloc(Pool *p, size_t size) {
    size = (size + 7) & ~7;
    PoolBlock *b = p->head;
    if (b->used + size > b->cap) {
        size_t nc = size > POOL_BLOCK ? size * 2 : POOL_BLOCK;
        PoolBlock *nb = (PoolBlock*)malloc(sizeof(PoolBlock));
        nb->data = (char*)malloc(nc);
        nb->used = 0; nb->cap = nc; nb->next = p->head; p->head = nb; b = nb;
    }
    void *ptr = b->data + b->used; b->used += size; return ptr;
}

#define HASH_SIZE 524288
#define HASH_EMPTY -1
typedef struct { void *key; int val; } HashEntry;
typedef struct { HashEntry *buckets; int size; } HashMap;
static void hashmap_init(HashMap *m, int sz) {
    m->size = sz;
    m->buckets = (HashEntry*)malloc(sz * sizeof(HashEntry));
    for (int i = 0; i < sz; i++) { m->buckets[i].key = NULL; m->buckets[i].val = HASH_EMPTY; }
}
static void hashmap_free(HashMap *m) { free(m->buckets); m->buckets = NULL; }
static inline int hslot(int sz, void *k) {
    uintptr_t h = (uintptr_t)k >> 3;
    h ^= h >> 16; h *= 0x45d9f3b; h ^= h >> 16;
    return (int)(h & (uintptr_t)(sz - 1));
}
static int hashmap_get(HashMap *m, void *k) {
    int s = hslot(m->size, k);
    for (int i = 0; i < m->size; i++) {
        int idx = (s + i) & (m->size - 1);
        if (m->buckets[idx].val == HASH_EMPTY) return HASH_EMPTY;
        if (m->buckets[idx].key == k) return m->buckets[idx].val;
    }
    return HASH_EMPTY;
}
static void hashmap_set(HashMap *m, void *k, int v) {
    int s = hslot(m->size, k);
    for (int i = 0; i < m->size; i++) {
        int idx = (s + i) & (m->size - 1);
        if (m->buckets[idx].val == HASH_EMPTY || m->buckets[idx].key == k) {
            m->buckets[idx].key = k; m->buckets[idx].val = v; return;
        }
    }
    fprintf(stderr, "HASH FULL\n"); exit(1);
}

#define RMAP_SIZE 524288
#define RLIST_CAP 8
typedef struct { void *key; int *vals; int nvals, cap; } REntry;
typedef struct { REntry *buckets; int size; } RMap;
static void rmap_init(RMap *m, int sz) { m->size = sz; m->buckets = (REntry*)calloc(sz, sizeof(REntry)); }
static void rmap_free(RMap *m) {
    for (int i = 0; i < m->size; i++) if (m->buckets[i].vals) free(m->buckets[i].vals);
    free(m->buckets); m->buckets = NULL;
}
static REntry *rmap_get_or_create(RMap *m, void *k) {
    int s = hslot(m->size, k);
    for (int i = 0; i < m->size; i++) {
        int idx = (s + i) & (m->size - 1);
        if (!m->buckets[idx].key) {
            m->buckets[idx].key = k; m->buckets[idx].nvals = 0;
            m->buckets[idx].cap = RLIST_CAP;
            m->buckets[idx].vals = (int*)malloc(RLIST_CAP * sizeof(int));
            return &m->buckets[idx];
        }
        if (m->buckets[idx].key == k) return &m->buckets[idx];
    }
    fprintf(stderr, "RMAP FULL\n"); exit(1);
}
static void rmap_append(RMap *m, void *k, int v) {
    REntry *e = rmap_get_or_create(m, k);
    if (e->nvals >= e->cap) { e->cap *= 2; e->vals = (int*)realloc(e->vals, e->cap * sizeof(int)); }
    e->vals[e->nvals++] = v;
}
static void rmap_clear(RMap *m, void *k) {
    int s = hslot(m->size, k);
    for (int i = 0; i < m->size; i++) {
        int idx = (s + i) & (m->size - 1);
        if (!m->buckets[idx].key) return;
        if (m->buckets[idx].key == k) { m->buckets[idx].nvals = 0; return; }
    }
}

#define INIT_SUCCS_CAP 8
#define INIT_DEPS_CAP 8
typedef struct {
    void (*func)(void**);
    void **args; int nargs;
    void *write_ptr;
    void **read_ptrs; int nreads;
    int *succs; int nsuccs, succ_cap;
    int *deps; int ndeps, deps_cap;
    atomic_int dep_count;
    atomic_int done;
    int id; /* self-id for debugging */
} Task;

typedef struct {
    int *data; int capacity;
    atomic_int head, tail;
    pthread_mutex_t lock;
    pthread_cond_t cond;
} TaskQueue;

static Task *tasks = NULL;
static int tasks_capacity = 0, ntasks = 0;
static TaskQueue queue;
static pthread_t *thread_pool;
static int nthreads = 0;
static atomic_int finished_count;
static HashMap last_writer;
static RMap readers;

static void ensure_tasks_capacity(int need) {
    if (need <= tasks_capacity) return;
    int nc = tasks_capacity == 0 ? 8192 : tasks_capacity;
    while (nc < need) nc *= 2;
    Task *nt = (Task*)realloc(tasks, (size_t)nc * sizeof(Task));
    if (!nt) { fprintf(stderr, "REALLOC FAIL\n"); exit(1); }
    tasks = nt; tasks_capacity = nc;
}
static void task_push_succ(Task *t, int sid) {
    if (t->nsuccs >= t->succ_cap) { t->succ_cap *= 2; t->succs = (int*)realloc(t->succs, t->succ_cap * sizeof(int)); }
    t->succs[t->nsuccs++] = sid;
}
static void add_dep(int from, int to) {
    Task *tf = &tasks[from], *tt = &tasks[to];
    for (int i = 0; i < tt->ndeps; i++) if (tt->deps[i] == from) return;
    if (tt->ndeps >= tt->deps_cap) { tt->deps_cap *= 2; tt->deps = (int*)realloc(tt->deps, tt->deps_cap * sizeof(int)); }
    tt->deps[tt->ndeps++] = from;
    task_push_succ(tf, to);
}

static void queue_init(TaskQueue *q, int cap) {
    q->data = (int*)malloc((size_t)cap * sizeof(int));
    q->capacity = cap;
    atomic_store(&q->head, 0); atomic_store(&q->tail, 0);
    pthread_mutex_init(&q->lock, NULL); pthread_cond_init(&q->cond, NULL);
}
static void queue_destroy(TaskQueue *q) {
    free(q->data); q->data = NULL;
    pthread_mutex_destroy(&q->lock); pthread_cond_destroy(&q->cond);
}
static void queue_push(TaskQueue *q, int tid) {
    pthread_mutex_lock(&q->lock);
    int h = atomic_load(&q->head), t = atomic_load(&q->tail);
    int cnt = t - h;
    if (cnt >= q->capacity) {
        int oc = q->capacity, nc = oc * 2;
        int *nd = (int*)malloc((size_t)nc * sizeof(int));
        for (int i = 0; i < cnt; i++) nd[i] = q->data[(h+i)%oc];
        free(q->data); q->data = nd; q->capacity = nc;
        atomic_store(&q->head, 0); atomic_store(&q->tail, cnt);
    }
    t = atomic_load(&q->tail);
    q->data[t % q->capacity] = tid;
    atomic_store(&q->tail, t + 1);
    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->lock);
}
static int queue_pop(TaskQueue *q) {
    pthread_mutex_lock(&q->lock);
    while (atomic_load(&q->head) == atomic_load(&q->tail)) {
        if (atomic_load(&finished_count) == ntasks) { pthread_mutex_unlock(&q->lock); return -1; }
        pthread_cond_wait(&q->cond, &q->lock);
    }
    int h = atomic_load(&q->head);
    int tid = q->data[h % q->capacity];
    atomic_store(&q->head, h + 1);
    pthread_mutex_unlock(&q->lock);
    return tid;
}

static void *worker(void *arg) {
    (void)arg;
    while (1) {
        int id = queue_pop(&queue);
        if (id == -1) break;
        Task *t = &tasks[id];
        fprintf(stderr, "[W%d] exec T%d\n", (int)(intptr_t)arg, id);
        t->func(t->args);
        fprintf(stderr, "[W%d] done T%d\n", (int)(intptr_t)arg, id);
        atomic_store(&t->done, 1);
        atomic_fetch_add(&finished_count, 1);
        for (int i = 0; i < t->nsuccs; i++) {
            Task *su = &tasks[t->succs[i]];
            int prev = atomic_fetch_sub(&su->dep_count, 1);
            if (prev == 1) queue_push(&queue, t->succs[i]);
        }
        pthread_mutex_lock(&queue.lock);
        pthread_cond_broadcast(&queue.cond);
        pthread_mutex_unlock(&queue.lock);
    }
    return NULL;
}

/* Forward declarations for wrappers (used in runtime_submit for debug) */
void cholesky_wrapper(void **args);
void trsm_wrapper(void **args);
void madd_wrapper(void **args);

void runtime_init(int num_threads) {
    ntasks = 0;
    nthreads = num_threads > 0 ? num_threads : (int)sysconf(_SC_NPROCESSORS_ONLN);
    fprintf(stderr, "[INIT] nthreads=%d\n", nthreads);
    atomic_store(&finished_count, 0);
    queue_init(&queue, 8192);
    thread_pool = (pthread_t*)malloc(nthreads * sizeof(pthread_t));
    hashmap_init(&last_writer, HASH_SIZE);
    rmap_init(&readers, RMAP_SIZE);
    pool_init(&pool);
}

void runtime_submit(void (*func)(void**), void **args, int nargs,
                    void *write_ptr, void **read_ptrs, int nreads) {
    ensure_tasks_capacity(ntasks + 1);
    Task *t = &tasks[ntasks];
    t->func = func; t->nargs = nargs; t->write_ptr = write_ptr;
    t->nreads = nreads; t->nsuccs = 0; t->succ_cap = INIT_SUCCS_CAP;
    t->ndeps = 0;
    atomic_store(&t->done, 0);
    t->id = ntasks;

    t->args = (void**)pool_alloc(&pool, nargs * sizeof(void*));
    memcpy(t->args, args, nargs * sizeof(void*));
    t->read_ptrs = (void**)pool_alloc(&pool, nreads * sizeof(void*));
    for (int i = 0; i < nreads; i++) t->read_ptrs[i] = read_ptrs[i];
    t->deps = (int*)malloc(INIT_DEPS_CAP * sizeof(int));
    t->deps_cap = INIT_DEPS_CAP;
    t->succs = (int*)malloc(INIT_SUCCS_CAP * sizeof(int));

    const char *fname = (func == cholesky_wrapper) ? "CHOL" :
                        (func == trsm_wrapper) ? "TRSM" : "MADD";
    fprintf(stderr, "[SUB] T%d %s wp=%p rp=[", ntasks, fname, write_ptr);
    for (int r = 0; r < nreads; r++) fprintf(stderr, "%p%s", read_ptrs[r], r+1<nreads?",":"");
    fprintf(stderr, "]\n");

    /* RAW: for each read, check last writer */
    for (int r = 0; r < nreads; r++) {
        int p = hashmap_get(&last_writer, read_ptrs[r]);
        if (p != HASH_EMPTY) {
            fprintf(stderr, "  RAW: read %p <- last writer T%d\n", read_ptrs[r], p);
            add_dep(p, ntasks);
        }
    }
    /* WAW: check last writer of write_ptr */
    {
        int p = hashmap_get(&last_writer, write_ptr);
        if (p != HASH_EMPTY) {
            fprintf(stderr, "  WAW: write %p <- last writer T%d\n", write_ptr, p);
            add_dep(p, ntasks);
        }
    }
    /* WAR: check readers of write_ptr */
    {
        int s = hslot(readers.size, write_ptr);
        for (int i = 0; i < readers.size; i++) {
            int idx = (s + i) & (readers.size - 1);
            if (!readers.buckets[idx].key) break;
            if (readers.buckets[idx].key == write_ptr) {
                for (int j = 0; j < readers.buckets[idx].nvals; j++) {
                    int rid = readers.buckets[idx].vals[j];
                    fprintf(stderr, "  WAR: write %p, reader T%d\n", write_ptr, rid);
                    add_dep(rid, ntasks);
                }
                break;
            }
        }
    }

    fprintf(stderr, "  deps(%d)=[", t->ndeps);
    for (int d = 0; d < t->ndeps; d++) fprintf(stderr, "T%d%s", t->deps[d], d+1<t->ndeps?",":"");
    fprintf(stderr, "] dep_count=%d\n", t->ndeps);

    hashmap_set(&last_writer, write_ptr, ntasks);
    rmap_clear(&readers, write_ptr);
    for (int r = 0; r < nreads; r++) rmap_append(&readers, read_ptrs[r], ntasks);

    atomic_store(&t->dep_count, t->ndeps);
    ntasks++;
}

void runtime_wait_all() {
    if (ntasks == 0) return;
    fprintf(stderr, "[WAIT] ntasks=%d\n", ntasks);
    for (int i = 0; i < ntasks; i++)
        if (atomic_load(&tasks[i].dep_count) == 0)
            queue_push(&queue, i);

    for (int i = 0; i < nthreads; i++)
        pthread_create(&thread_pool[i], NULL, worker, (void*)(intptr_t)i);
    for (int i = 0; i < nthreads; i++)
        pthread_join(thread_pool[i], NULL);

    for (int i = 0; i < ntasks; i++) {
        free(tasks[i].succs);
        free(tasks[i].deps);
    }
    ntasks = 0;
    atomic_store(&finished_count, 0);
    pool_free(&pool);
    hashmap_free(&last_writer);
    rmap_free(&readers);
    queue_destroy(&queue);
}

void runtime_destroy() {
    free(thread_pool); thread_pool = NULL;
    free(tasks); tasks = NULL; tasks_capacity = 0;
}

void cholesky(double*, double*, int, int);
void trsm(double*, double*, double*, int, int);
void madd(double*, double*, double*, int, int);

void cholesky_wrapper(void **args);
void trsm_wrapper(void **args);
void madd_wrapper(void **args);

void cholesky_wrapper(void **args) {
    cholesky((double*)args[0], (double*)args[1], *(int*)args[2], *(int*)args[3]);
}
void trsm_wrapper(void **args) {
    trsm((double*)args[0], (double*)args[1], (double*)args[2], *(int*)args[3], *(int*)args[4]);
}
void madd_wrapper(void **args) {
    madd((double*)args[0], (double*)args[1], (double*)args[2], *(int*)args[3], *(int*)args[4]);
}
