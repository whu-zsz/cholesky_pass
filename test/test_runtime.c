#include "../runtime/runtime.h"
#include <stdio.h>
#include <math.h>

// 用最小矩阵 n=4, b=2 手动测试

static double A[4*4], L[4*4];
static int N = 4;

// 包装函数：把void**参数解包后调用真正的算子
__attribute__((noinline))
int cholesky(double *A, double *L, int n, int lda);
__attribute__((noinline))
void trsm(double *A, double *Bt, double *X, int m, int n, int lda);
__attribute__((noinline))
void madd(double *A, double *Bt, double *C, int m, int n, int lda);

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

int main() {
    // 初始化一个简单的正定矩阵
    // A = I*4（对角线为4，其余为0），方便验证
    for (int i = 0; i < 4*4; i++) A[i] = 0, L[i] = 0;
    A[0*4+0]=4; A[1*4+1]=4; A[2*4+2]=4; A[3*4+3]=4;

    runtime_init(1);

    int b=2, n=4;
    // 手动提交 block_cholesky(n=4,b=2) 的任务序列

    // i=0:
    //   cholesky(&A[0], &L[0], 2, 4)
    int i0=0, j2=2, k2=2, dim2=2;
    void *args0[] = {&A[0*4+0], &L[0*4+0], &dim2, &n};
    void *reads0[] = {&A[0*4+0]};
    runtime_submit(cholesky_wrapper, args0, 4,
                   &L[0*4+0], reads0, 1);

    //   trsm(&A[2*4+0], &L[0*4+0], &L[2*4+0], 2, 2, 4)
    void *args1[] = {&A[2*4+0], &L[0*4+0], &L[2*4+0], &dim2, &dim2, &n};
    void *reads1[] = {&A[2*4+0], &L[0*4+0]};
    runtime_submit(trsm_wrapper, args1, 6,
                   &L[2*4+0], reads1, 2);

    //   madd(&L[2*4+0], &L[2*4+0], &L[2*4+2], 2, 2, 4)
    void *args2[] = {&L[2*4+0], &L[2*4+0], &L[2*4+2], &dim2, &dim2, &n};
    void *reads2[] = {&L[2*4+0]};
    runtime_submit(madd_wrapper, args2, 6,
                   &L[2*4+2], reads2, 1);

    // i=2:
    //   cholesky(&A[2*4+2], &L[2*4+2], 2, 4)
    void *args3[] = {&A[2*4+2], &L[2*4+2], &dim2, &n};
    void *reads3[] = {&A[2*4+2], &L[2*4+2]};
    runtime_submit(cholesky_wrapper, args3, 4,
                   &L[2*4+2], reads3, 2);

    runtime_wait_all();
    runtime_destroy();

    // 验证：L * L^T 应该等于 A
    printf("\nL matrix:\n");
    for (int i=0; i<4; i++) {
        for (int j=0; j<4; j++)
            printf("%6.3f ", L[i*4+j]);
        printf("\n");
    }
    return 0;
}