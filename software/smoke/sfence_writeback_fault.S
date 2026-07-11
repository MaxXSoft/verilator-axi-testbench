#include "riscv_test.h"

RVTEST_CODE_BEGIN
  /* Dirty a read-only ROM line, then use an uncached-looking SFENCE rs1.
     The resulting flush BRESP error must reach Mem as cause 7 rather than
     being hidden by the demand cache/uncached route. */
  li t0, 0x00004000
  li t1, 0x12345678
  sw t1, 0(t0)

  li t0, 0x10000000
  sfence.vma t0, zero

  li TESTNUM, 7
  RVTEST_FAIL
RVTEST_CODE_END
