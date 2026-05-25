#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>

#define MAX_TASKS 65536
#define MAX_DEPS  256
#define MAX_READS 8
#define MAX_SUCCS 256

// 任务结构体
typedef struct {
    void   (*func)(void**);
    void  **args;
    int     nargs;
    void   *write_ptr;
    void   *read_ptrs[MAX_READS];
    int     nreads;
    int     deps[MAX_DEPS];
    int     ndeps;
    int     succs[MAX_SUCCS];   // 后继任务
    int     nsuccs;
    atomic_int dep_count;       // 剩余未满足依赖数，降到0则可执行
    atomic_int done;
} Task;

// 任务队列（简单环形队列）
#define QUEUE_SIZE 65536
typedef struct {
    int        data[QUEUE_SIZE];
    atomic_int head;
    atomic_int tail;
    pthread_mutex_t lock;
    pthread_cond_t  cond;
} TaskQueue;

static Task      tasks[MAX_TASKS];
static int       ntasks = 0;
static TaskQueue queue;
static pthread_t *thread_pool;
static int       nthreads = 0;
static atomic_int finished_count;  // 已完成任务数

// ── 队列操作 ──────────────────────────────────────

static void queue_init(TaskQueue *q) {
    atomic_store(&q->head, 0);
    atomic_store(&q->tail, 0);
    pthread_mutex_init(&q->lock, NULL);
    pthread_cond_init(&q->cond, NULL);
}

static void queue_push(TaskQueue *q, int task_id) {
    pthread_mutex_lock(&q->lock);
    int tail = atomic_load(&q->tail);
    q->data[tail % QUEUE_SIZE] = task_id;
    atomic_store(&q->tail, tail + 1);
    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->lock);
}

// 返回-1表示所有任务已完成
static int queue_pop(TaskQueue *q) {
    pthread_mutex_lock(&q->lock);
    while (atomic_load(&q->head) == atomic_load(&q->tail)) {
        // 队列为空，检查是否全部完成
        if (atomic_load(&finished_count) == ntasks) {
            pthread_mutex_unlock(&q->lock);
            return -1;
        }
        pthread_cond_wait(&q->cond, &q->lock);
    }
    int head = atomic_load(&q->head);
    int task_id = q->data[head % QUEUE_SIZE];
    atomic_store(&q->head, head + 1);
    pthread_mutex_unlock(&q->lock);
    return task_id;
}

// ── 工作线程 ──────────────────────────────────────

static void *worker(void *arg) {
    fprintf(stderr, "[worker] thread started\n");
    fflush(stderr);
    while (1) {
        int id = queue_pop(&queue);
        fprintf(stderr, "[worker] got task %d\n", id);
        fflush(stderr);
        if (id == -1) break;

        Task *t = &tasks[id];
        t->func(t->args);
        atomic_store(&t->done, 1);
        atomic_fetch_add(&finished_count, 1);
        fprintf(stderr, "[worker] Task%d done, finished=%d/%d\n",
                id, atomic_load(&finished_count), ntasks);
        fflush(stderr);

        for (int i = 0; i < t->nsuccs; i++) {
            Task *succ = &tasks[t->succs[i]];
            int remaining = atomic_fetch_sub(&succ->dep_count, 1) - 1;
            fprintf(stderr, "[worker] Task%d succ Task%d remaining=%d\n",
                    id, t->succs[i], remaining);
            fflush(stderr);
            if (remaining == 0)
                queue_push(&queue, t->succs[i]);
        }

        pthread_mutex_lock(&queue.lock);
        pthread_cond_broadcast(&queue.cond);
        pthread_mutex_unlock(&queue.lock);
    }
    fprintf(stderr, "[worker] thread exiting\n");
    fflush(stderr);
    return NULL;
}

// ── 公开接口 ──────────────────────────────────────

void runtime_init(int num_threads) {
    ntasks = 0;
    nthreads = num_threads;
    atomic_store(&finished_count, 0);
    queue_init(&queue);
    thread_pool = (pthread_t*)malloc(nthreads * sizeof(pthread_t));
    fprintf(stderr, "[runtime] init threads=%d\n", num_threads);  // 改用stderr
    fflush(stderr);
}

void runtime_submit(void (*func)(void**), void **args, int nargs,
                    void *write_ptr,
                    void **read_ptrs, int nreads) {
     fprintf(stderr, "[runtime] submit Task%d write=%p\n",
            ntasks, write_ptr);
    fflush(stderr);
    if (ntasks >= MAX_TASKS) {
        fprintf(stderr, "[runtime] too many tasks!\n");
        return;
    }

    Task *t = &tasks[ntasks];
    t->func      = func;
    t->nargs     = nargs;
    t->write_ptr = write_ptr;
    t->nreads    = nreads;
    t->ndeps     = 0;
    t->nsuccs    = 0;
    atomic_store(&t->done, 0);

    // 复制参数
    t->args = (void**)malloc(nargs * sizeof(void*));
    memcpy(t->args, args, nargs * sizeof(void*));

    // 复制读指针
    for (int i = 0; i < nreads && i < MAX_READS; i++)
        t->read_ptrs[i] = read_ptrs[i];

    // 找依赖，同时建立后继关系
 int dep_cnt = 0;
for (int i = 0; i < ntasks; i++) {
    Task *prev = &tasks[i];
    int has_dep = 0;

    // RAW：当前任务读的 == 前面任务写的
    for (int r = 0; r < t->nreads && !has_dep; r++) {
        if (prev->write_ptr == t->read_ptrs[r]) {
            has_dep = 1;
        }
    }

    // WAW：当前任务写的 == 前面任务写的
    if (!has_dep && prev->write_ptr == t->write_ptr) {
        has_dep = 1;
    }

    // WAR：当前任务写的 == 前面任务读的
    if (!has_dep) {
        for (int r = 0; r < prev->nreads && !has_dep; r++) {
            if (prev->read_ptrs[r] == t->write_ptr) {
                has_dep = 1;
            }
        }
    }

    if (has_dep) {
        t->deps[t->ndeps++] = i;
        prev->succs[prev->nsuccs++] = ntasks;
        dep_cnt++;
    }
}
atomic_store(&t->dep_count, dep_cnt);

    ntasks++;
       fprintf(stderr, "[runtime] Task%d dep_count=%d nsuccs_of_prev updated\n",
            ntasks, dep_cnt);
    fflush(stderr);
}

void runtime_wait_all() {
    fprintf(stderr, "[runtime] wait_all called, ntasks=%d\n", ntasks);
    fflush(stderr);

    // 先把dep_count==0的任务入队
    for (int i = 0; i < ntasks; i++) {
        if (atomic_load(&tasks[i].dep_count) == 0) {
            fprintf(stderr, "[runtime] Task%d ready, pushing to queue\n", i);
            fflush(stderr);
            queue_push(&queue, i);
        }
    }

    // 再启动线程池
    for (int i = 0; i < nthreads; i++)
        pthread_create(&thread_pool[i], NULL, worker, NULL);

    // 等待所有线程结束
    for (int i = 0; i < nthreads; i++)
        pthread_join(thread_pool[i], NULL);

    // 清理
    for (int i = 0; i < ntasks; i++)
        free(tasks[i].args);
    ntasks = 0;
    atomic_store(&finished_count, 0);

    fprintf(stderr, "[runtime] all done\n");
    fflush(stderr);
}

void runtime_destroy() {
    free(thread_pool);
    thread_pool = NULL;
    pthread_mutex_destroy(&queue.lock);
    pthread_cond_destroy(&queue.cond);
}

// ── wrapper函数 ───────────────────────────────────

int  cholesky(double*, double*, int, int);
void trsm(double*, double*, double*, int, int, int);
void madd(double*, double*, double*, int, int, int);

void cholesky_wrapper(void **args) {
    cholesky((double*)args[0], (double*)args[1],
             *(int*)args[2], *(int*)args[3]);
}
void trsm_wrapper(void **args) {
    trsm((double*)args[0], (double*)args[1], (double*)args[2],
         *(int*)args[3], *(int*)args[4], *(int*)args[5]);
}
void madd_wrapper(void **args) {
    madd((double*)args[0], (double*)args[1], (double*)args[2],
         *(int*)args[3], *(int*)args[4], *(int*)args[5]);
}