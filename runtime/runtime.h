#pragma once
#include <stddef.h>

// 初始化运行时，num_threads是线程数（串行版本传1）
void runtime_init(int num_threads);

// 提交一个任务
// func:      算子函数指针
// args:      参数数组，存放所有指针和整数参数
// nargs:     参数个数
// write_ptr: 这个任务写的内存地址（用于依赖判断）
// read_ptrs: 这个任务读的内存地址数组
// nreads:    读的地址个数
void runtime_submit(void (*func)(void**), void **args, int nargs,
                    void *write_ptr,
                    void **read_ptrs, int nreads);

// 等待所有已提交任务完成
void runtime_wait_all();

// 销毁运行时
void runtime_destroy();