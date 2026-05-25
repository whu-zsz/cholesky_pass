#include <math.h>

// 朴素 Cholesky 分解
__attribute__((noinline))
int cholesky(double *A, double *L, int n, int lda) {
    for (int i = 0; i < n; i++) {
        L[i * lda + i] = sqrt(A[i * lda + i] - L[i * lda + i]);
        for (int j = i + 1; j < n; j++) {
            L[j * lda + i] = (A[j * lda + i] - L[j * lda + i]) / L[i * lda + i];
            for (int k = i + 1; k < j + 1; k++) {
                L[j * lda + k] -= L[j * lda + i] * L[k * lda + i];
            }
        }
    }
    return 0;
}

// solve X * B = A
__attribute__((noinline))
void trsm(double *A, double *Bt, double *X, int m, int n, int lda) {
    for (int i = 0; i < m; i++) {
        for (int j = 0; j < n; j++) {
            double sum = A[i * lda + j] - X[i * lda + j];
            for (int k = 0; k < j; k++) {
                // Bt[k][j] 实际上是 B[j][k]
                sum -= X[i * lda + k] * Bt[j * lda + k];
            }
            // 除以对角线元素 Bt[j][j]（即 B[j][j]）
            X[i * lda + j] = sum / Bt[j * lda + j];
        }
    }
}

// C = A * Bt + C
__attribute__((noinline))
void madd(double *A, double *Bt, double *C, int m, int n, int lda) {
    for (int i = 0; i < m; i++) {
        for (int j = 0; j < m; j++) {
            for (int k = 0; k < n; k++) {
                C[i * lda + j] += A[i * lda + k] * Bt[j * lda + k];
            }
        }
    }
}

// 分块 Cholesky 分解
int block_cholesky(double *A, double *L, int n, int b) {
    for (int i = 0; i < n; i += b) {
        int i_dim = (i + b < n) ? b : n - i;
        cholesky(&A[i * n + i], &L[i * n + i], i_dim, n);

        for (int j = i + b; j < n; j += b) {
            int j_dim = (j + b < n) ? b : n - j;
            trsm(&A[j * n + i], &L[i * n + i], &L[j * n + i], i_dim, j_dim, n);

            for (int k = i + b; k < n; k += b) {
                int k_dim = (k + b < n) ? b : n - k;
                madd(&L[j * n + i], &L[k * n + i], &L[j * n + k], j_dim, k_dim, n);
            }
        }
    }
    return 0;
}
