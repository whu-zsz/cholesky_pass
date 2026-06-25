#include "kernels.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <vector>

namespace contest {

int block_cholesky(const double *A, double *L, int n, int b) {
    if (b <= 0) {
        throw std::invalid_argument("Block size must be positive");
    }
    if (n % b != 0) {
        throw std::invalid_argument("Current baseline requires n to be divisible by b");
    }

    std::copy(A, A + static_cast<std::size_t>(n) * n, L);

    for (int i = 0; i < n; i += b) {
        cholesky(&L[i * n + i], &L[i * n + i], b, n);

        for (int j = i + b; j < n; j += b) {
            trsm(&L[j * n + i], &L[i * n + i], &L[j * n + i], b, n);
        }

        for (int j = i + b; j < n; j += b) {
            for (int k = j; k < n; k += b) {
                madd(&L[k * n + i], &L[j * n + i], &L[k * n + j], b, n);
            }
        }
    }

    for (int row = 0; row < n; ++row) {
        for (int col = row + 1; col < n; ++col) {
            L[row * n + col] = 0.0;
        }
    }

    return 0;
}

}  // namespace contest
