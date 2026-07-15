/* Minimal single-hart environment for the upstream riscv-tests ISA sources. */
#ifndef AXI_TB_RISCV_TEST_H
#define AXI_TB_RISCV_TEST_H

#include "axi_tb_platform.h"

#define TESTNUM gp

/* The selected RV32 I/M/A tests require no privilege-mode transition. */
#define RVTEST_RV32U
#define RVTEST_RV32M

#define RVTEST_CODE_BEGIN         \
  .section.text, "ax", @progbits; \
  .align 2;                       \
  .globl axi_tb_test_entry;       \
  axi_tb_test_entry:

#define RVTEST_CODE_END unimp

/* PASS is exactly exit code zero. */
#define RVTEST_PASS        \
  fence;                   \
  li t6, AXI_TB_EXIT_BASE; \
  sw zero, 0(t6);          \
  .Laxi_tb_pass_halt : j.Laxi_tb_pass_halt

/* Preserve the failing test number in gp; defensively make zero nonzero. */
#define RVTEST_FAIL                                \
  fence;                                           \
  bnez TESTNUM, .Laxi_tb_fail_nonzero;             \
  li TESTNUM, 1;                                   \
  .Laxi_tb_fail_nonzero : li t6, AXI_TB_EXIT_BASE; \
  sw TESTNUM, 0(t6);                               \
  .Laxi_tb_fail_halt : j.Laxi_tb_fail_halt

#define RVTEST_DATA_BEGIN \
  .align 4;               \
  .globl begin_signature; \
  begin_signature:

#define RVTEST_DATA_END \
  .align 4;             \
  .globl end_signature; \
  end_signature:

#endif /* AXI_TB_RISCV_TEST_H */
