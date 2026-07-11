#include "riscv_test.h"

RVTEST_CODE_BEGIN
  /* Fetch through the instruction cache from an unmapped cacheable address.
     The RRESP error must become an instruction access-fault exception. */
  li t0, 0x40000000
  jr t0

  li TESTNUM, 1
  RVTEST_FAIL
RVTEST_CODE_END
