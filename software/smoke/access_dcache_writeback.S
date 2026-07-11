#include "riscv_test.h"

RVTEST_CODE_BEGIN
  /* These ROM lines differ by the 8 KiB direct-mapped D-cache capacity and
     therefore have the same cache index.  Dirty the first line, then replace
     it: the writeback to read-only ROM returns SLVERR and the second store
     must wait for that BRESP before reporting a store access fault. */
  li t0, 0x00004000
  li t1, 0x12345678
  sw t1, 0(t0)

  li t0, 0x00006000
  sw t1, 0(t0)

  li TESTNUM, 7
  RVTEST_FAIL
RVTEST_CODE_END
