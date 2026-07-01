#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <unistd.h>

#ifdef RUNTIME_DEBUG
  #define DLOG(...) do { fprintf(stderr, __VA_ARGS__); fflush(stderr); } while(0)
#else
  #define DLOG(...) do {} while(0)
#endif

#define MAX_TASKS 200000
#define POOL_SIZE (128*1024*1024)
#define HASH_SIZE 524288
#define HASH_EMPTY -1
#define RMAP_SIZE  524288
#define QSIZE      (1<<18)

// ── 内存池（主线程独占）──────────────────────────
static char   *pool_buf = NULL;
static size_t  pool_used = 0;
static size_t  pool_cap  = 0;

static void pool_init(void) {
    if (!pool_buf) {
        pool_buf = (char*)malloc(POOL_SIZE);
        pool_cap = POOL_SIZE;
    }
    pool_used = 0;
}
static void pool_reset(void) { pool_used = 0; }
static void *pool_alloc(size_t size) {
    size = (size + 7) & ~7;
    if (pool_used + size > pool_cap) {
        fprintf(stderr, "[runtime] FATAL: pool exhausted %zu/%zu\n", pool_used+size, pool_cap);
        exit(1);
    }
    void *ptr = pool_buf + pool_used;
    pool_used += size;
    return ptr;
}

// ── 哈希表 ────────────────────────────────────────
typedef struct { void *key; int val; } HashEntry;

static HashEntry *hw_buckets = NULL;  // last_writer

static inline int hslot(void *key) {
    uintptr_t h = (uintptr_t)key >> 3;
    h ^= h >> 16; h *= 0x45d9f3b; h ^= h >> 16;
    return (int)(h & (HASH_SIZE - 1));
}
static void hw_init(void) {
    if (!hw_buckets) hw_buckets = (HashEntry*)malloc(HASH_SIZE * sizeof(HashEntry));
    for (int i = 0; i < HASH_SIZE; i++) { hw_buckets[i].key = NULL; hw_buckets[i].val = HASH_EMPTY; }
}
static int hw_get(void *key) {
    int s = hslot(key);
    for (int i = 0; i < HASH_SIZE; i++) {
        int idx = (s+i) & (HASH_SIZE-1);
        if (hw_buckets[idx].val == HASH_EMPTY) return HASH_EMPTY;
        if (hw_buckets[idx].key == key) return hw_buckets[idx].val;
    }
    return HASH_EMPTY;
}
static void hw_set(void *key, int val) {
    int s = hslot(key);
    for (int i = 0; i < HASH_SIZE; i++) {
        int idx = (s+i) & (HASH_SIZE-1);
        if (hw_buckets[idx].val == HASH_EMPTY || hw_buckets[idx].key == key) {
            hw_buckets[idx].key = key; hw_buckets[idx].val = val; return;
        }
    }
    fprintf(stderr, "[runtime] FATAL: hw full\n"); exit(1);
}

typedef struct { void *key; int *vals; int nvals; int cap; } REntry;
static REntry *rmap = NULL;

static void rmap_init(void) {
    if (!rmap) rmap = (REntry*)malloc(RMAP_SIZE * sizeof(REntry));
    memset(rmap, 0, RMAP_SIZE * sizeof(REntry));
}
static REntry *rmap_find(void *key) {
    int s = hslot(key);
    for (int i = 0; i < RMAP_SIZE; i++) {
        int idx = (s+i) & (RMAP_SIZE-1);
        if (!rmap[idx].key) return NULL;
        if (rmap[idx].key == key) return &rmap[idx];
    }
    return NULL;
}
static REntry *rmap_get_or_create(void *key) {
    int s = hslot(key);
    for (int i = 0; i < RMAP_SIZE; i++) {
        int idx = (s+i) & (RMAP_SIZE-1);
        if (!rmap[idx].key) {
            rmap[idx].key = key; rmap[idx].nvals = 0; rmap[idx].cap = 4;
            rmap[idx].vals = (int*)pool_alloc(4*sizeof(int));
            return &rmap[idx];
        }
        if (rmap[idx].key == key) return &rmap[idx];
    }
    fprintf(stderr, "[runtime] FATAL: rmap full\n"); exit(1);
}
static void rmap_append(void *key, int val) {
    REntry *e = rmap_get_or_create(key);
    if (e->nvals >= e->cap) {
        int nc = e->cap * 2;
        int *nv = (int*)pool_alloc(nc * sizeof(int));
        memcpy(nv, e->vals, e->nvals * sizeof(int));
        e->vals = nv; e->cap = nc;
    }
    e->vals[e->nvals++] = val;
}
static void rmap_clear(void *key) {
    REntry *e = rmap_find(key);
    if (e) e->nvals = 0;
}

// ── Task ──────────────────────────────────────────
typedef struct {
    void   (*func)(void**);
    void  **args;
    int     nargs;
    void   *write_ptr;
    void  **read_ptrs;
    int     nreads;
    int    *succs;   // 动态，用pool分配（提交阶段主线程独占）
    int     nsuccs;
    int     succ_cap;
    int    *deps;    // 动态，用pool分配
    int     ndeps;
    atomic_int dep_count;
    atomic_int done;
} Task;

static Task        *tasks = NULL;
static int          ntasks_submitted = 0;
static atomic_int   ntasks_atomic;
static atomic_int   finished_count;
static atomic_int   submit_done;

// ── 队列 ──────────────────────────────────────────
typedef struct {
    int            *data;
    atomic_int      head;
    atomic_int      tail;
    pthread_mutex_t lock;
    pthread_cond_t  cond;
} TaskQueue;

static TaskQueue    queue;
static pthread_t   *thread_pool = NULL;
static int          nthreads = 0;

static void queue_init_q(void) {
    if (!queue.data) queue.data = (int*)malloc(QSIZE * sizeof(int));
    atomic_store(&queue.head, 0);
    atomic_store(&queue.tail, 0);
    pthread_mutex_init(&queue.lock, NULL);
    pthread_cond_init(&queue.cond, NULL);
}
static void queue_push(int task_id) {
    pthread_mutex_lock(&queue.lock);
    int tail = atomic_load(&queue.tail);
    queue.data[tail & (QSIZE-1)] = task_id;
    atomic_store(&queue.tail, tail + 1);
    pthread_cond_signal(&queue.cond);
    pthread_mutex_unlock(&queue.lock);
}
static int queue_pop(void) {
    pthread_mutex_lock(&queue.lock);
    while (atomic_load(&queue.head) == atomic_load(&queue.tail)) {
        if (atomic_load(&submit_done) &&
            atomic_load(&finished_count) == atomic_load(&ntasks_atomic)) {
            pthread_mutex_unlock(&queue.lock);
            return -1;
        }
        pthread_cond_wait(&queue.cond, &queue.lock);
    }
    int head = atomic_load(&queue.head);
    int tid  = queue.data[head & (QSIZE-1)];
    atomic_store(&queue.head, head + 1);
    pthread_mutex_unlock(&queue.lock);
    return tid;
}

static void *worker(void *arg) {
    (void)arg;
    while (1) {
        int id = queue_pop();
        if (id == -1) break;
        Task *t = &tasks[id];
        t->func(t->args);
        atomic_store(&t->done, 1);
        int fin = atomic_fetch_add(&finished_count, 1) + 1;
        for (int i = 0; i < t->nsuccs; i++) {
            int sid = t->succs[i];
            if (atomic_fetch_sub(&tasks[sid].dep_count, 1) - 1 == 0)
                queue_push(sid);
        }
        if (atomic_load(&submit_done) &&
            fin == atomic_load(&ntasks_atomic)) {
            pthread_mutex_lock(&queue.lock);
            pthread_cond_broadcast(&queue.cond);
            pthread_mutex_unlock(&queue.lock);
        }
    }
    return NULL;
}

static void add_dep(int from_id, int to_id) {
    Task *from = &tasks[from_id];
    Task *to   = &tasks[to_id];
    for (int i = 0; i < to->ndeps; i++)
        if (to->deps[i] == from_id) return;
    // deps用pool分配，不超过16个
    to->deps[to->ndeps++] = from_id;
    // succs用pool分配，动态扩容
    if (from->nsuccs >= from->succ_cap) {
        int nc = from->succ_cap * 2;
        int *nv = (int*)pool_alloc(nc * sizeof(int));
        memcpy(nv, from->succs, from->nsuccs * sizeof(int));
        from->succs = nv; from->succ_cap = nc;
    }
    from->succs[from->nsuccs++] = to_id;
}

void runtime_init(int num_threads) {
    ntasks_submitted = 0;
    atomic_store(&ntasks_atomic, 0);
    atomic_store(&finished_count, 0);
    atomic_store(&submit_done, 0);
    nthreads = num_threads > 0 ? num_threads : (int)sysconf(_SC_NPROCESSORS_ONLN);

    if (!tasks) tasks = (Task*)malloc(MAX_TASKS * sizeof(Task));
    pool_init();
    hw_init();
    rmap_init();
    queue_init_q();

    if (!thread_pool) thread_pool = (pthread_t*)malloc(nthreads * sizeof(pthread_t));
    for (int i = 0; i < nthreads; i++)
        pthread_create(&thread_pool[i], NULL, worker, NULL);

    DLOG("[runtime] init threads=%d\n", nthreads);
}

void runtime_submit(void (*func)(void**), void **args, int nargs,
                    void *write_ptr, void **read_ptrs, int nreads) {
    int cur = ntasks_submitted;
    Task *t = &tasks[cur];

    t->func = func; t->nargs = nargs;
    t->write_ptr = write_ptr; t->nreads = nreads;
    t->nsuccs = 0; t->succ_cap = 8; t->ndeps = 0;
    atomic_store(&t->done, 0);

    t->args      = (void**)pool_alloc(nargs  * sizeof(void*));
    memcpy(t->args, args, nargs * sizeof(void*));
    t->read_ptrs = (void**)pool_alloc(nreads * sizeof(void*));
    for (int i = 0; i < nreads; i++) t->read_ptrs[i] = read_ptrs[i];
    t->deps  = (int*)pool_alloc(16 * sizeof(int));
    t->succs = (int*)pool_alloc(8  * sizeof(int));

    for (int r = 0; r < nreads; r++) {
        int p = hw_get(read_ptrs[r]);
        if (p != HASH_EMPTY) add_dep(p, cur);
    }
    { int p = hw_get(write_ptr); if (p != HASH_EMPTY) add_dep(p, cur); }
    { REntry *e = rmap_find(write_ptr);
      if (e) for (int j = 0; j < e->nvals; j++) add_dep(e->vals[j], cur); }

    hw_set(write_ptr, cur);
    rmap_clear(write_ptr);
    for (int r = 0; r < nreads; r++) rmap_append(read_ptrs[r], cur);

    atomic_store(&t->dep_count, t->ndeps);
    ntasks_submitted++;
    atomic_store(&ntasks_atomic, ntasks_submitted);

    if (t->ndeps == 0) queue_push(cur);
}

void runtime_wait_all() {
    atomic_store(&submit_done, 1);
    pthread_mutex_lock(&queue.lock);
    pthread_cond_broadcast(&queue.cond);
    pthread_mutex_unlock(&queue.lock);

    if (ntasks_submitted > 0) {
        pthread_mutex_lock(&queue.lock);
        while (atomic_load(&finished_count) < ntasks_submitted)
            pthread_cond_wait(&queue.cond, &queue.lock);
        pthread_mutex_unlock(&queue.lock);
    }

    for (int i = 0; i < nthreads; i++)
        pthread_join(thread_pool[i], NULL);

    ntasks_submitted = 0;
    atomic_store(&ntasks_atomic, 0);
    atomic_store(&finished_count, 0);
    atomic_store(&submit_done, 0);
    pool_reset();
    hw_init();
    rmap_init();
    pthread_mutex_destroy(&queue.lock);
    pthread_cond_destroy(&queue.cond);
    queue_init_q();

    // 重新启动worker线程（下一轮block_cholesky调用准备）
    for (int i = 0; i < nthreads; i++)
        pthread_create(&thread_pool[i], NULL, worker, NULL);
}

void runtime_destroy() {
    // 停止worker
    atomic_store(&submit_done, 1);
    pthread_mutex_lock(&queue.lock);
    pthread_cond_broadcast(&queue.cond);
    pthread_mutex_unlock(&queue.lock);
    for (int i = 0; i < nthreads; i++)
        pthread_join(thread_pool[i], NULL);
    pthread_mutex_destroy(&queue.lock);
    pthread_cond_destroy(&queue.cond);
}

void cholesky(double*, double*, int, int);
void trsm(double*, double*, double*, int, int);
void madd(double*, double*, double*, int, int);

void cholesky_wrapper(void **args) {
    cholesky((double*)args[0], (double*)args[1], *(int*)args[2], *(int*)args[3]);
}
void trsm_wrapper(void **args) {
    trsm((double*)args[0], (double*)args[1], (double*)args[2],
         *(int*)args[3], *(int*)args[4]);
}
void madd_wrapper(void **args) {
    madd((double*)args[0], (double*)args[1], (double*)args[2],
         *(int*)args[3], *(int*)args[4]);
}
