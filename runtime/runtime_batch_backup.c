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

// ── 内存池 ────────────────────────────────────────
// 用大块预分配替代每个Task零散malloc，减少allocator压力
#define POOL_BLOCK 8388608  // 8MB per block

typedef struct PoolBlock {
    char             *data;
    size_t            used;
    size_t            cap;
    struct PoolBlock *next;
} PoolBlock;

typedef struct {
    PoolBlock *head;
} Pool;

static Pool pool;

static void pool_init(Pool *p) {
    p->head       = (PoolBlock*)malloc(sizeof(PoolBlock));
    p->head->data = (char*)malloc(POOL_BLOCK);
    p->head->used = 0;
    p->head->cap  = POOL_BLOCK;
    p->head->next = NULL;
}

static void pool_free(Pool *p) {
    PoolBlock *b = p->head;
    while (b) {
        PoolBlock *next = b->next;
        free(b->data);
        free(b);
        b = next;
    }
    p->head = NULL;
}

static void *pool_alloc(Pool *p, size_t size) {
    size = (size + 7) & ~7;  // 8字节对齐
    PoolBlock *b = p->head;
    if (b->used + size > b->cap) {
        size_t new_cap    = size > POOL_BLOCK ? size * 2 : POOL_BLOCK;
        PoolBlock *nb     = (PoolBlock*)malloc(sizeof(PoolBlock));
        nb->data          = (char*)malloc(new_cap);
        nb->used          = 0;
        nb->cap           = new_cap;
        nb->next          = p->head;
        p->head           = nb;
        b                 = nb;
    }
    void *ptr  = b->data + b->used;
    b->used   += size;
    return ptr;
}

// ── 哈希表 ────────────────────────────────────────
#define HASH_SIZE  524288  // 必须是2的幂
#define HASH_EMPTY -1

typedef struct { void *key; int val; } HashEntry;
typedef struct { HashEntry *buckets; int size; } HashMap;

static void hashmap_init(HashMap *m, int size) {
    m->size    = size;
    m->buckets = (HashEntry*)malloc(size * sizeof(HashEntry));
    for (int i = 0; i < size; i++) {
        m->buckets[i].key = NULL;
        m->buckets[i].val = HASH_EMPTY;
    }
}
static void hashmap_free(HashMap *m) { free(m->buckets); m->buckets = NULL; }

static inline int hslot(int size, void *key) {
    uintptr_t h = (uintptr_t)key >> 3;
    h ^= h >> 16; h *= 0x45d9f3b; h ^= h >> 16;
    return (int)(h & (uintptr_t)(size - 1));
}

static int hashmap_get(HashMap *m, void *key) {
    int s = hslot(m->size, key);
    for (int i = 0; i < m->size; i++) {
        int idx = (s + i) & (m->size - 1);
        if (m->buckets[idx].val == HASH_EMPTY) return HASH_EMPTY;
        if (m->buckets[idx].key == key) return m->buckets[idx].val;
    }
    return HASH_EMPTY;
}
static void hashmap_set(HashMap *m, void *key, int val) {
    int s = hslot(m->size, key);
    for (int i = 0; i < m->size; i++) {
        int idx = (s + i) & (m->size - 1);
        if (m->buckets[idx].val == HASH_EMPTY || m->buckets[idx].key == key) {
            m->buckets[idx].key = key; m->buckets[idx].val = val; return;
        }
    }
    fprintf(stderr, "[runtime] FATAL: hash full\n"); exit(1);
}

// 读者表：指针 → task_id列表
#define RMAP_SIZE 524288
#define RLIST_CAP 8

typedef struct { void *key; int *vals; int nvals; int cap; } REntry;
typedef struct { REntry *buckets; int size; } RMap;

static void rmap_init(RMap *m, int size) {
    m->size    = size;
    m->buckets = (REntry*)calloc(size, sizeof(REntry));
}
static void rmap_free(RMap *m) {
    for (int i = 0; i < m->size; i++)
        if (m->buckets[i].vals) free(m->buckets[i].vals);
    free(m->buckets); m->buckets = NULL;
}
static REntry *rmap_get_or_create(RMap *m, void *key) {
    int s = hslot(m->size, key);
    for (int i = 0; i < m->size; i++) {
        int idx = (s + i) & (m->size - 1);
        if (!m->buckets[idx].key) {
            m->buckets[idx].key   = key;
            m->buckets[idx].nvals = 0;
            m->buckets[idx].cap   = RLIST_CAP;
            m->buckets[idx].vals  = (int*)malloc(RLIST_CAP * sizeof(int));
            return &m->buckets[idx];
        }
        if (m->buckets[idx].key == key) return &m->buckets[idx];
    }
    fprintf(stderr, "[runtime] FATAL: rmap full\n"); exit(1);
}
static void rmap_append(RMap *m, void *key, int val) {
    REntry *e = rmap_get_or_create(m, key);
    if (e->nvals >= e->cap) { e->cap *= 2; e->vals = (int*)realloc(e->vals, e->cap * sizeof(int)); }
    e->vals[e->nvals++] = val;
}
static void rmap_clear(RMap *m, void *key) {
    int s = hslot(m->size, key);
    for (int i = 0; i < m->size; i++) {
        int idx = (s + i) & (m->size - 1);
        if (!m->buckets[idx].key) return;
        if (m->buckets[idx].key == key) { m->buckets[idx].nvals = 0; return; }
    }
}

// ── Task结构体 ────────────────────────────────────
#define INIT_SUCCS_CAP 8

typedef struct {
    void   (*func)(void**);
    void  **args;       // 指向pool分配的内存
    int     nargs;
    void   *write_ptr;
    void  **read_ptrs;  // 指向pool分配的内存
    int     nreads;
    int    *succs;      // 动态分配（后继数量不可预知）
    int     nsuccs;
    int     succ_cap;
    int    *deps;       // 指向pool分配的内存（依赖数量有上界）
    int     ndeps;
    atomic_int dep_count;
    atomic_int done;
} Task;

// 任务队列
typedef struct {
    int       *data;
    int        capacity;
    atomic_int head;
    atomic_int tail;
    pthread_mutex_t lock;
    pthread_cond_t  cond;
} TaskQueue;

static Task      *tasks = NULL;
static int        tasks_capacity = 0;
static int        ntasks = 0;
static TaskQueue  queue;
static pthread_t *thread_pool;
static int        nthreads = 0;
static atomic_int finished_count;
static HashMap    last_writer;
static RMap       readers;

static void ensure_tasks_capacity(int need) {
    if (need <= tasks_capacity) return;
    int new_cap = tasks_capacity == 0 ? 8192 : tasks_capacity;
    while (new_cap < need) new_cap *= 2;
    Task *nt = (Task*)realloc(tasks, (size_t)new_cap * sizeof(Task));
    if (!nt) { fprintf(stderr, "[runtime] FATAL: realloc\n"); exit(1); }
    tasks = nt; tasks_capacity = new_cap;
}

static void task_push_succ(Task *t, int succ_id) {
    if (t->nsuccs >= t->succ_cap) {
        t->succ_cap *= 2;
        t->succs = (int*)realloc(t->succs, t->succ_cap * sizeof(int));
    }
    t->succs[t->nsuccs++] = succ_id;
}

static void add_dep(int from_id, int to_id) {
    Task *from = &tasks[from_id];
    Task *to   = &tasks[to_id];
    for (int i = 0; i < to->ndeps; i++)
        if (to->deps[i] == from_id) return;
    to->deps[to->ndeps++] = from_id;
    task_push_succ(from, to_id);
}

static void queue_init(TaskQueue *q, int cap) {
    q->data = (int*)malloc((size_t)cap * sizeof(int));
    q->capacity = cap;
    atomic_store(&q->head, 0); atomic_store(&q->tail, 0);
    pthread_mutex_init(&q->lock, NULL);
    pthread_cond_init(&q->cond, NULL);
}
static void queue_destroy(TaskQueue *q) {
    free(q->data); q->data = NULL;
    pthread_mutex_destroy(&q->lock);
    pthread_cond_destroy(&q->cond);
}
static void queue_push(TaskQueue *q, int task_id) {
    pthread_mutex_lock(&q->lock);
    int head = atomic_load(&q->head), tail = atomic_load(&q->tail);
    int count = tail - head;
    if (count >= q->capacity) {
        int old_cap = q->capacity, new_cap = old_cap * 2;
        int *nd = (int*)malloc((size_t)new_cap * sizeof(int));
        for (int i = 0; i < count; i++) nd[i] = q->data[(head+i)%old_cap];
        free(q->data); q->data = nd; q->capacity = new_cap;
        atomic_store(&q->head, 0); atomic_store(&q->tail, count);
    }
    tail = atomic_load(&q->tail);
    q->data[tail % q->capacity] = task_id;
    atomic_store(&q->tail, tail + 1);
    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->lock);
}
static int queue_pop(TaskQueue *q) {
    pthread_mutex_lock(&q->lock);
    while (atomic_load(&q->head) == atomic_load(&q->tail)) {
        if (atomic_load(&finished_count) == ntasks) {
            pthread_mutex_unlock(&q->lock); return -1;
        }
        pthread_cond_wait(&q->cond, &q->lock);
    }
    int head = atomic_load(&q->head);
    int tid  = q->data[head % q->capacity];
    atomic_store(&q->head, head + 1);
    pthread_mutex_unlock(&q->lock);
    return tid;
}

static void *worker(void *arg) {
    (void)arg;
    while (1) {
        int id = queue_pop(&queue);
        if (id == -1) break;
        Task *t = &tasks[id];
        t->func(t->args);
        atomic_store(&t->done, 1);
        atomic_fetch_add(&finished_count, 1);
        for (int i = 0; i < t->nsuccs; i++) {
            Task *succ = &tasks[t->succs[i]];
            if (atomic_fetch_sub(&succ->dep_count, 1) - 1 == 0)
                queue_push(&queue, t->succs[i]);
        }
        pthread_mutex_lock(&queue.lock);
        pthread_cond_broadcast(&queue.cond);
        pthread_mutex_unlock(&queue.lock);
    }
    return NULL;
}

void runtime_init(int num_threads) {
    ntasks   = 0;
    nthreads = num_threads > 0 ? num_threads : (int)sysconf(_SC_NPROCESSORS_ONLN);
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

    t->func      = func;
    t->nargs     = nargs;
    t->write_ptr = write_ptr;
    t->nreads    = nreads;
    t->nsuccs    = 0;
    t->succ_cap  = INIT_SUCCS_CAP;
    t->ndeps     = 0;
    atomic_store(&t->done, 0);

    // 用内存池分配args、read_ptrs、deps（避免零散malloc）
    t->args      = (void**)pool_alloc(&pool, nargs * sizeof(void*));
    memcpy(t->args, args, nargs * sizeof(void*));

    t->read_ptrs = (void**)pool_alloc(&pool, nreads * sizeof(void*));
    for (int i = 0; i < nreads; i++) t->read_ptrs[i] = read_ptrs[i];

    // deps最多和历史任务数一样多，但实际很小，预分配8个
    t->deps  = (int*)pool_alloc(&pool, 8 * sizeof(int));
    t->succs = (int*)malloc(INIT_SUCCS_CAP * sizeof(int));

    // O(1) 依赖分析
    for (int r = 0; r < nreads; r++) {
        int p = hashmap_get(&last_writer, read_ptrs[r]);
        if (p != HASH_EMPTY) add_dep(p, ntasks);
    }
    {
        int p = hashmap_get(&last_writer, write_ptr);
        if (p != HASH_EMPTY) add_dep(p, ntasks);
    }
    {
        int s = hslot(readers.size, write_ptr);
        for (int i = 0; i < readers.size; i++) {
            int idx = (s + i) & (readers.size - 1);
            if (!readers.buckets[idx].key) break;
            if (readers.buckets[idx].key == write_ptr) {
                for (int j = 0; j < readers.buckets[idx].nvals; j++)
                    add_dep(readers.buckets[idx].vals[j], ntasks);
                break;
            }
        }
    }

    hashmap_set(&last_writer, write_ptr, ntasks);
    rmap_clear(&readers, write_ptr);
    for (int r = 0; r < nreads; r++) rmap_append(&readers, read_ptrs[r], ntasks);

    atomic_store(&t->dep_count, t->ndeps);
    ntasks++;
}

void runtime_wait_all() {
    if (ntasks == 0) return;

    for (int i = 0; i < ntasks; i++)
        if (atomic_load(&tasks[i].dep_count) == 0)
            queue_push(&queue, i);

    for (int i = 0; i < nthreads; i++)
        pthread_create(&thread_pool[i], NULL, worker, NULL);
    for (int i = 0; i < nthreads; i++)
        pthread_join(thread_pool[i], NULL);

    for (int i = 0; i < ntasks; i++)
        free(tasks[i].succs);  // 只释放succs（其余由pool管理）
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
