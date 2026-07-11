`include "axi_tb_ports.svh"

// Minimal canonical adapter/top.  It deliberately remains idle and is useful
// as a build/binding smoke target.  Real integrations can copy this module,
// retain its parameter and port list, and replace the assignments below with
// an instantiation of the initiator being tested.
module axi_tb_canonical_top #(
  parameter int unsigned NUM_AXI = 1,
  parameter int unsigned ADDR_WIDTH = 64,
  parameter int unsigned DATA_WIDTH = 64,
  parameter int unsigned ID_WIDTH = 4
) (
  `AXI_TB_INITIATOR_PORTS
);

  initial begin
    assert (NUM_AXI > 0)
      else $fatal(1, "NUM_AXI must be positive");
    assert ((ADDR_WIDTH == 32) || (ADDR_WIDTH == 64))
      else $fatal(1, "ADDR_WIDTH must be 32 or 64");
    assert ((DATA_WIDTH == 32) || (DATA_WIDTH == 64) ||
            (DATA_WIDTH == 128))
      else $fatal(1, "DATA_WIDTH must be 32, 64, or 128");
    assert ((ID_WIDTH >= 1) && (ID_WIDTH <= 32))
      else $fatal(1, "ID_WIDTH must be in [1, 32]");
  end

  always_comb begin
    axi_aw_valid = '0;
    axi_aw_id = '0;
    axi_aw_addr = '0;
    axi_aw_len = '0;
    axi_aw_size = '0;
    axi_aw_burst = '0;
    axi_aw_lock = '0;
    axi_aw_cache = '0;
    axi_aw_prot = '0;

    axi_w_valid = '0;
    axi_w_data = '0;
    axi_w_strb = '0;
    axi_w_last = '0;
    axi_b_ready = '0;

    axi_ar_valid = '0;
    axi_ar_id = '0;
    axi_ar_addr = '0;
    axi_ar_len = '0;
    axi_ar_size = '0;
    axi_ar_burst = '0;
    axi_ar_lock = '0;
    axi_ar_cache = '0;
    axi_ar_prot = '0;

    axi_r_ready = '0;
  end

endmodule
