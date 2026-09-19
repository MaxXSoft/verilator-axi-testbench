  .section .text, "ax", @progbits
  .globl _start
_start:
  li a0, 1
  li t0, AXI_TB_ROM_BASE
  lw t1, 0(t0)
  li t2, 0x544f4f42
  bne t1, t2, done
  la t0, payload_data
  lw t1, 0(t0)
  li t2, 0x12345678
  bne t1, t2, done
  la t0, payload_bss
  lw t1, 0(t0)
  bnez t1, done
  li a0, 0
done:
  li t0, AXI_TB_EXIT_BASE
  sw a0, 0(t0)
1: j 1b
  .section .data
payload_data: .word 0x12345678
  .section .bss
payload_bss: .space 16
