#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

int block_cholesky(double *A, double *L, int n, int b);

void gen_spd(double *A, int n, int seed) {
    srand(seed);
    double *B = (double*)calloc(n*n, sizeof(double));
    for (int i = 0; i < n; i++)
        for (int j = 0; j <= i; j++)
            B[i*n+j] = (double)(rand() % 10 + 1) / 10.0;
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++) {
            double s = 0;
            for (int k = 0; k < n; k++)
                s += B[i*n+k] * B[j*n+k];
            A[i*n+j] = s + (i==j ? n : 0);
        }
    free(B);
}

// 验证方式：对比串行和并行的L对角线
// 这里直接打印对角线，外部脚本对比
int main() {
    int tests[][2] = {
        {4,2}, {8,2}, {8,4},
        {16,4}, {32,4}, {32,8},
        {0,0}
    };

    for (int t = 0; tests[t][0] != 0; t++) {
        int n = tests[t][0];
        int b = tests[t][1];

        double *A = (double*)calloc(n*n, sizeof(double));
        double *L = (double*)calloc(n*n, sizeof(double));

        gen_spd(A, n, 42);
        block_cholesky(A, L, n, b);

        // 打印对角线
        printf("n=%d b=%d:", n, b);
        for (int i = 0; i < n; i++)
            printf(" %.4f", L[i*n+i]);
        printf("\n");

        free(A); free(L);
    }
    return 0;
}