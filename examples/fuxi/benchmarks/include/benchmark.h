#ifndef FUXI_BENCHMARK_H
#define FUXI_BENCHMARK_H
#include <stdint.h>
void benchmark_start(void);
void benchmark_stop(void);
uint64_t benchmark_cycles(void);
void benchmark_report(const char *name, unsigned iterations);
__attribute__((noreturn)) void benchmark_exit(unsigned code);
#endif
