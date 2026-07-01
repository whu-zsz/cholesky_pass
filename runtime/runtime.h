#pragma once
#include <stddef.h>

void runtime_init(int num_threads);
void runtime_submit(void (*func)(void**), void **args, int nargs,
                    void *write_ptr, void **read_ptrs, int nreads);
void runtime_wait_all();
void runtime_destroy();
