#include "riscv_test.h"

RVTEST_CODE_BEGIN
  /* Cacheable, but outside every testbench device window.  The D-cache must
     turn the resulting AXI RRESP error into a load access-fault exception. */
  li t0, 0x40000000
  lw t1, 0(t0)

  /* Reaching this point means the error response was incorrectly ignored. */
  li TESTNUM, 5
  RVTEST_FAIL
RVTEST_CODE_END
