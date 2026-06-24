#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>

#ifdef RUNTIME_DEBUG
  #define DLOG(...) do { fprintf(stderr, __VA_ARGS__); fflush(stderr); } while(0)
#else
  #define DLOG(...) do {} while(0)
#endif

#define INIT_READS_CAP 4
#define INIT_DEPS_CAP  8
#define INIT_SUCCS_CAP 8

// ── 哈希表：指针 → 任务ID ──────────────────────────
// 用于 O(1) 依赖查找，替代原来的 O(n) 全历史扫描

#define HASH_SIZE 262144  // 2^18，足够大避免碰撞
#define HASH_EMPTY -1

typedef struct {
    void *key;
    int   val;  // task_id，HASH_EMPTY表示空槽
} HashEntry;

typedef struct {
    HashEntry *buckets;
    int        size;
} HashMap;

static void hashmap_init(HashMap *m, int size) {
    m->size    = size;
    m->buckets = (HashEntry*)malloc(size * sizeof(HashEntry));
    for (int i = 0; i < size; i++) {
        m->buckets[i].key = NULL;
        m->buckets[i].val = HASH_EMPTY;
    }
}

static void hashmap_free(HashMap *m) {
    free(m->buckets);
    m->buckets = NULL;
}

static int hashmap_slot(HashMap *m, void *key) {
    uintptr_t h = (uintptr_t)key;
    h ^= h >> 16;
    h *= 0x45d9f3b;
    h ^= h >> 16;
    return (int)(h % (uintptr_t)m->size);
}

// 返回 key 对应的 val，不存在返回 HASH_EMPTY
static int hashmap_get(HashMap *m, void *key) {
    int slot = hashmap_slot(m, key);
    for (int i = 0; i < m->size; i++) {
        int s = (slot + i) % m->size;
        if (m->buckets[s].val == HASH_EMPTY) return HASH_EMPTY;
        if (m->buckets[s].key == key) return m->buckets[s].val;
    }
    return HASH_EMPTY;
}

// 设置 key → val（覆盖旧值）
static void hashmap_set(HashMap *m, void *key, int val) {
    int slot = hashmap_slot(m, key);
    for (int i = 0; i < m->size; i++) {
        int s = (slot + i) % m->size;
        if (m->buckets[s].val == HASH_EMPTY || m->buckets[s].key == key) {
            m->buckets[s].key = key;
            m->buckets[s].val = val;
            return;
        }
    }
    // 哈希表满了（不应该发生，HASH_SIZE足够大）
    fprintf(stderr, "[runtime] FATAL: hash table full\n");
    exit(1);
}

// 多值哈希表：指针 → 任务ID列表（用于WAR依赖）
#define READERS_HASH_SIZE 262144
#define READERS_LIST_CAP  8

typedef struct {
    void *key;
    int  *vals;
    int   nvals;
    int   cap;
} ReadersEntry;

typedef struct {
    ReadersEntry *buckets;
    int           size;
} ReadersMap;

static void readersmap_init(ReadersMap *m, int size) {
    m->size    = size;
    m->buckets = (ReadersEntry*)calloc(size, sizeof(ReadersEntry));
}

static void readersmap_free(ReadersMap *m) {
    for (int i = 0; i < m->size; i++)
        if (m->buckets[i].vals) free(m->buckets[i].vals);
    free(m->buckets);
    m->buckets = NULL;
}

static int readersmap_slot(ReadersMap *m, void *key) {
    uintptr_t h = (uintptr_t)key;
    h ^= h >> 16;
    h *= 0x45d9f3b;
    h ^= h >> 16;
    return (int)(h % (uintptr_t)m->size);
}

static ReadersEntry *readersmap_get_or_create(ReadersMap *m, void *key) {
    int slot = readersmap_slot(m, key);
    for (int i = 0; i < m->size; i++) {
        int s = (slot + i) % m->size;
        if (m->buckets[s].key == NULL) {
            m->buckets[s].key  = key;
            m->buckets[s].nvals = 0;
            m->buckets[s].cap  = READERS_LIST_CAP;
            m->buckets[s].vals = (int*)malloc(READERS_LIST_CAP * sizeof(int));
            return &m->buckets[s];
        }
        if (m->buckets[s].key == key) return &m->buckets[s];
    }
    fprintf(stderr, "[runtime] FATAL: readers map full\n");
    exit(1);
}

static void readersmap_append(ReadersMap *m, void *key, int val) {
    ReadersEntry *e = readersmap_get_or_create(m, key);
    if (e->nvals >= e->cap) {
        e->cap  *= 2;
        e->vals  = (int*)realloc(e->vals, e->cap * sizeof(int));
    }
    e->vals[e->nvals++] = val;
}

static void readersmap_clear_key(ReadersMap *m, void *key) {
    int slot = readersmap_slot(m, key);
    for (int i = 0; i < m->size; i++) {
        int s = (slot + i) % m->size;
        if (m->buckets[s].key == NULL) return;
        if (m->buckets[s].key == key) {
            m->buckets[s].nvals = 0;
            return;
        }
    }
}

// ── Task 结构体 ────────────────────────────────────

typedef struct {
    void   (*func)(void**);
    void  **args;
    int     nargs;
    void   *write_ptr;
    void  **read_ptrs;
    int     nreads;
    int     read_cap;
    int    *deps;
    int     ndeps;
    int     dep_cap;
    int    *succs;
    int     nsuccs;
    int     succ_cap;
    atomic_int dep_count;
    atomic_int done;
} Task;

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

// O(1) 依赖查找表
static HashMap    last_writer;   // write_ptr → 最近写入该块的task_id
static ReadersMap readers;       // read_ptr  → 读取该块的task_id列表

// ── 内部工具函数 ──────────────────────────────────

static void ensure_tasks_capacity(int need) {
    if (need <= tasks_capacity) return;
    int new_cap = tasks_capacity == 0 ? 4096 : tasks_capacity;
    while (new_cap < need) new_cap *= 2;
    Task *new_tasks = (Task*)realloc(tasks, (size_t)new_cap * sizeof(Task));
    if (!new_tasks) { fprintf(stderr, "[runtime] FATAL: realloc failed\n"); exit(1); }
    tasks = new_tasks;
    tasks_capacity = new_cap;
}

static void task_array_init(Task *t) {
    t->read_cap  = INIT_READS_CAP;
    t->read_ptrs = (void**)malloc(t->read_cap * sizeof(void*));
    t->nreads    = 0;
    t->dep_cap   = INIT_DEPS_CAP;
    t->deps      = (int*)malloc(t->dep_cap * sizeof(int));
    t->ndeps     = 0;
    t->succ_cap  = INIT_SUCCS_CAP;
    t->succs     = (int*)malloc(t->succ_cap * sizeof(int));
    t->nsuccs    = 0;
}

static void task_push_dep(Task *t, int dep_id) {
    if (t->ndeps >= t->dep_cap) {
        t->dep_cap *= 2;
        t->deps = (int*)realloc(t->deps, t->dep_cap * sizeof(int));
    }
    t->deps[t->ndeps++] = dep_id;
}

static void task_push_succ(Task *t, int succ_id) {
    if (t->nsuccs >= t->succ_cap) {
        t->succ_cap *= 2;
        t->succs = (int*)realloc(t->succs, t->succ_cap * sizeof(int));
    }
    t->succs[t->nsuccs++] = succ_id;
}

// 建立 from_id → to_id 的依赖边（避免重复）
static void add_dep(int from_id, int to_id) {
    Task *from = &tasks[from_id];
    Task *to   = &tasks[to_id];
    // 检查是否已经有这条边
    for (int i = 0; i < to->ndeps; i++)
        if (to->deps[i] == from_id) return;
    task_push_dep(to, from_id);
    task_push_succ(from, to_id);
}

static void queue_init(TaskQueue *q, int capacity) {
    q->data     = (int*)malloc((size_t)capacity * sizeof(int));
    q->capacity = capacity;
    atomic_store(&q->head, 0);
    atomic_store(&q->tail, 0);
    pthread_mutex_init(&q->lock, NULL);
    pthread_cond_init(&q->cond, NULL);
}

static void queue_destroy(TaskQueue *q) {
    free(q->data);
    q->data = NULL;
    pthread_mutex_destroy(&q->lock);
    pthread_cond_destroy(&q->cond);
}

static int queue_size_locked(TaskQueue *q) {
    return atomic_load(&q->tail) - atomic_load(&q->head);
}

static void queue_push(TaskQueue *q, int task_id) {
    pthread_mutex_lock(&q->lock);
    if (queue_size_locked(q) >= q->capacity) {
        int old_cap   = q->capacity;
        int new_cap   = old_cap * 2;
        int *new_data = (int*)malloc((size_t)new_cap * sizeof(int));
        int head  = atomic_load(&q->head);
        int tail  = atomic_load(&q->tail);
        int count = tail - head;
        for (int i = 0; i < count; i++)
            new_data[i] = q->data[(head + i) % old_cap];
        free(q->data);
        q->data     = new_data;
        q->capacity = new_cap;
        atomic_store(&q->head, 0);
        atomic_store(&q->tail, count);
    }
    int tail = atomic_load(&q->tail);
    q->data[tail % q->capacity] = task_id;
    atomic_store(&q->tail, tail + 1);
    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->lock);
}

static int queue_pop(TaskQueue *q) {
    pthread_mutex_lock(&q->lock);
    while (atomic_load(&q->head) == atomic_load(&q->tail)) {
        if (atomic_load(&finished_count) == ntasks) {
            pthread_mutex_unlock(&q->lock);
            return -1;
        }
        pthread_cond_wait(&q->cond, &q->lock);
    }
    int head    = atomic_load(&q->head);
    int task_id = q->data[head % q->capacity];
    atomic_store(&q->head, head + 1);
    pthread_mutex_unlock(&q->lock);
    return task_id;
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
            Task *succ    = &tasks[t->succs[i]];
            int remaining = atomic_fetch_sub(&succ->dep_count, 1) - 1;
            if (remaining == 0)
                queue_push(&queue, t->succs[i]);
        }
        pthread_mutex_lock(&queue.lock);
        pthread_cond_broadcast(&queue.cond);
        pthread_mutex_unlock(&queue.lock);
    }
    return NULL;
}

// ── 公开接口 ──────────────────────────────────────

void runtime_init(int num_threads) {
    ntasks   = 0;
    nthreads = num_threads;
    atomic_store(&finished_count, 0);
    queue_init(&queue, 4096);
    thread_pool = (pthread_t*)malloc(nthreads * sizeof(pthread_t));
    hashmap_init(&last_writer, HASH_SIZE);
    readersmap_init(&readers, READERS_HASH_SIZE);
    DLOG("[runtime] init threads=%d\n", num_threads);
}

void runtime_submit(void (*func)(void**), void **args, int nargs,
                    void *write_ptr,
                    void **read_ptrs, int nreads) {
    ensure_tasks_capacity(ntasks + 1);
    Task *t = &tasks[ntasks];
    task_array_init(t);
    t->func      = func;
    t->nargs     = nargs;
    t->write_ptr = write_ptr;
    atomic_store(&t->done, 0);

    t->args = (void**)malloc(nargs * sizeof(void*));
    memcpy(t->args, args, nargs * sizeof(void*));

    if (nreads > t->read_cap) {
        t->read_cap  = nreads;
        t->read_ptrs = (void**)realloc(t->read_ptrs, t->read_cap * sizeof(void*));
    }
    for (int i = 0; i < nreads; i++)
        t->read_ptrs[i] = read_ptrs[i];
    t->nreads = nreads;

    // ── O(1) 依赖分析 ──────────────────────────────
    // RAW：我读的块，找最近的写入者
    for (int r = 0; r < nreads; r++) {
        int prev_id = hashmap_get(&last_writer, read_ptrs[r]);
        if (prev_id != HASH_EMPTY)
            add_dep(prev_id, ntasks);
    }

    // WAW：我写的块，找最近的写入者
    {
        int prev_id = hashmap_get(&last_writer, write_ptr);
        if (prev_id != HASH_EMPTY)
            add_dep(prev_id, ntasks);
    }

    // WAR：我写的块，找所有读取者
    {
        int slot = readersmap_slot(&readers, write_ptr);
        for (int i = 0; i < readers.size; i++) {
            int s = (slot + i) % readers.size;
            if (readers.buckets[s].key == NULL) break;
            if (readers.buckets[s].key == write_ptr) {
                for (int j = 0; j < readers.buckets[s].nvals; j++)
                    add_dep(readers.buckets[s].vals[j], ntasks);
                break;
            }
        }
    }

    // 更新索引：我是这个write_ptr的最新写入者
    hashmap_set(&last_writer, write_ptr, ntasks);
    // 清除该块的读取者列表（因为我会写它，之后的读者依赖的是我）
    readersmap_clear_key(&readers, write_ptr);

    // 我读取的每个块，把我加入读取者列表
    for (int r = 0; r < nreads; r++)
        readersmap_append(&readers, read_ptrs[r], ntasks);

    atomic_store(&t->dep_count, t->ndeps);
    DLOG("[runtime] submit Task%d write=%p deps=%d\n",
         ntasks, write_ptr, t->ndeps);
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
    for (int i = 0; i < ntasks; i++) {
        free(tasks[i].args);
        free(tasks[i].read_ptrs);
        free(tasks[i].deps);
        free(tasks[i].succs);
    }
    ntasks = 0;
    atomic_store(&finished_count, 0);
    queue_destroy(&queue);
    hashmap_free(&last_writer);
    readersmap_free(&readers);
}

void runtime_destroy() {
    free(thread_pool);
    thread_pool    = NULL;
    free(tasks);
    tasks          = NULL;
    tasks_capacity = 0;
}

void cholesky(double*, double*, int, int);
void trsm(double*, double*, double*, int, int);
void madd(double*, double*, double*, int, int);

void cholesky_wrapper(void **args) {
    cholesky((double*)args[0], (double*)args[1],
             *(int*)args[2], *(int*)args[3]);
}
void trsm_wrapper(void **args) {
    trsm((double*)args[0], (double*)args[1], (double*)args[2],
         *(int*)args[3], *(int*)args[4]);
}
void madd_wrapper(void **args) {
    madd((double*)args[0], (double*)args[1], (double*)args[2],
         *(int*)args[3], *(int*)args[4]);
}
