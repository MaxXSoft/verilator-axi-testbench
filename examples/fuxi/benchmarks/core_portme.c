#include "benchmark.h"
#include "coremark.h"

volatile ee_s32 seed1_volatile = 0;
volatile ee_s32 seed2_volatile = 0;
volatile ee_s32 seed3_volatile = 0x66;
volatile ee_s32 seed4_volatile = ITERATIONS;
volatile ee_s32 seed5_volatile = 0;
ee_u32 default_num_contexts = 1;

void start_time(void) { benchmark_start(); }
void stop_time(void) { benchmark_stop(); }
CORE_TICKS get_time(void) {
  uint64_t ticks = benchmark_cycles();
  if (ticks >> 32) benchmark_exit(4);
  return (CORE_TICKS)ticks;
}
secs_ret time_in_secs(CORE_TICKS ticks) { return ticks / 50000000u; }

void portable_init(core_portable *p, int *argc, char *argv[]) {
  (void)argc;
  (void)argv;
  p->portable_id = 1;
}

void portable_fini(core_portable *p) {
  p->portable_id = 0;
  benchmark_report("coremark", ITERATIONS);
}
