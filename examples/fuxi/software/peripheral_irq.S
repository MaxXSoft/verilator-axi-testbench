#include "riscv_test.h"
#include "fuxi_platform.h"

RVTEST_CODE_BEGIN
  la t0, machine_handler
  csrw mtvec, t0
  csrw mideleg, zero
  li s0, 0

  /* Real CLINT MSIP, with no adapter test hook. */
  li TESTNUM, 1
  li t0, 8
  csrw mie, t0
  csrs mstatus, t0
  li t0, FUXI_CLINT_BASE
  li t1, 1
  sw t1, 0(t0)
1:
  beqz s0, 1b
  csrci mstatus, 8

  /* Arm mtimecmp using the safe RV32 low/high/low sequence. */
  li TESTNUM, 2
  li s0, 0
  li t0, FUXI_CLINT_BASE + 0xbff8
  lw t1, 0(t0)
  addi t1, t1, 1000
  li t0, FUXI_CLINT_BASE + 0x4000
  li t2, -1
  sw t2, 0(t0)
  sw zero, 4(t0)
  sw t1, 0(t0)
  li t0, 0x80
  csrw mie, t0
  csrsi mstatus, 8
2:
  beqz s0, 2b
  csrci mstatus, 8

  /* UART RX -> PLIC source 10 -> hart 0 M context -> MEIP. */
  li TESTNUM, 3
  li s0, 0
  li t0, FUXI_PLIC_BASE + FUXI_UART_IRQ * 4
  li t1, 1
  sw t1, 0(t0)
  li t0, FUXI_PLIC_BASE + 0x2000
  li t1, 1 << FUXI_UART_IRQ
  sw t1, 0(t0)
  li t0, AXI_TB_UART_BASE
  li t1, 0x10
  sb t1, 4(t0)             /* Internal loopback, no host input dependency. */
  li t1, 1
  sb t1, 2(t0)             /* FIFO enable */
  sb t1, 1(t0)             /* RX interrupt enable */
  li t1, 'L'
  sb t1, 0(t0)
  li t0, 0x800
  csrw mie, t0
  csrsi mstatus, 8
3:
  beqz s0, 3b
  csrci mstatus, 8
  li t0, FUXI_PLIC_BASE + 0x200004
  lw t1, 0(t0)
  bnez t1, fail

  /* Route the next UART interrupt to the supervisor context only. */
  li TESTNUM, 4
  li s0, 0
  li t0, FUXI_PLIC_BASE + 0x2000
  sw zero, 0(t0)
  li t1, 1 << FUXI_UART_IRQ
  sw t1, 0x80(t0)
  li t0, 0x200
  csrw mideleg, t0
  csrw mie, t0
  la t0, supervisor_entry
  csrw mepc, t0
  li t0, 0x880              /* MPP=S, MPIE=1 */
  csrw mstatus, t0
  mret

supervisor_entry:
  la t0, supervisor_handler
  csrw stvec, t0
  li t0, AXI_TB_UART_BASE
  li t1, 'S'
  sb t1, 0(t0)
  csrsi sstatus, 2
4:
  beqz s0, 4b
  csrci sstatus, 2
  RVTEST_PASS

  .align 2
machine_handler:
  csrr t0, mcause
  li t1, 0x80000003
  beq t0, t1, software_irq
  li t1, 0x80000007
  beq t0, t1, timer_irq
  li t1, 0x8000000b
  bne t0, t1, fail
  li t0, 3
  bne TESTNUM, t0, fail
  li t0, FUXI_PLIC_BASE + 0x200004
  lw t1, 0(t0)
  li t2, FUXI_UART_IRQ
  bne t1, t2, fail
  li t2, AXI_TB_UART_BASE
  lbu t3, 0(t2)
  li t2, 'L'
  bne t3, t2, fail
  sw t1, 0(t0)             /* Complete only after clearing UART's level. */
  j machine_return
software_irq:
  li t0, 1
  bne TESTNUM, t0, fail
  li t0, FUXI_CLINT_BASE
  sw zero, 0(t0)
  j machine_return
timer_irq:
  li t0, 2
  bne TESTNUM, t0, fail
  li t0, FUXI_CLINT_BASE + 0x4000
  li t1, -1
  sw t1, 4(t0)
machine_return:
  bnez s0, fail
  li s0, 1
  mret

  .align 2
supervisor_handler:
  csrr t0, scause
  li t1, 0x80000009
  bne t0, t1, fail
  li t0, FUXI_PLIC_BASE + 0x201004
  lw t1, 0(t0)
  li t2, FUXI_UART_IRQ
  bne t1, t2, fail
  li t2, AXI_TB_UART_BASE
  lbu t3, 0(t2)
  li t2, 'S'
  bne t3, t2, fail
  sw t1, 0(t0)
  bnez s0, fail
  li s0, 1
  sret
fail:
  RVTEST_FAIL
RVTEST_CODE_END
