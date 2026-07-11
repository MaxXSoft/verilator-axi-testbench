`ifndef AXI_TB_PORTS_SVH
`define AXI_TB_PORTS_SVH

// Canonical Verilator-visible boundary for an AXI4 initiator DUT.
//
// All port lanes are packed, with the lane index as the left-most dimension.
// This makes every signal a stable, flattenable Verilator member named axi_*.
// The including module must define NUM_AXI, ADDR_WIDTH, DATA_WIDTH, and
// ID_WIDTH as parameters before expanding this macro in its port list.
`define AXI_TB_INITIATOR_PORTS \
  input  logic                                      clk, \
  input  logic                                      aresetn, \
  output logic [NUM_AXI-1:0]                        axi_aw_valid, \
  input  logic [NUM_AXI-1:0]                        axi_aw_ready, \
  output logic [NUM_AXI-1:0][ID_WIDTH-1:0]          axi_aw_id, \
  output logic [NUM_AXI-1:0][ADDR_WIDTH-1:0]        axi_aw_addr, \
  output logic [NUM_AXI-1:0][7:0]                   axi_aw_len, \
  output logic [NUM_AXI-1:0][2:0]                   axi_aw_size, \
  output logic [NUM_AXI-1:0][1:0]                   axi_aw_burst, \
  output logic [NUM_AXI-1:0]                        axi_aw_lock, \
  output logic [NUM_AXI-1:0][3:0]                   axi_aw_cache, \
  output logic [NUM_AXI-1:0][2:0]                   axi_aw_prot, \
  output logic [NUM_AXI-1:0]                        axi_w_valid, \
  input  logic [NUM_AXI-1:0]                        axi_w_ready, \
  output logic [NUM_AXI-1:0][DATA_WIDTH-1:0]        axi_w_data, \
  output logic [NUM_AXI-1:0][(DATA_WIDTH/8)-1:0]    axi_w_strb, \
  output logic [NUM_AXI-1:0]                        axi_w_last, \
  input  logic [NUM_AXI-1:0]                        axi_b_valid, \
  output logic [NUM_AXI-1:0]                        axi_b_ready, \
  input  logic [NUM_AXI-1:0][ID_WIDTH-1:0]          axi_b_id, \
  input  logic [NUM_AXI-1:0][1:0]                   axi_b_resp, \
  output logic [NUM_AXI-1:0]                        axi_ar_valid, \
  input  logic [NUM_AXI-1:0]                        axi_ar_ready, \
  output logic [NUM_AXI-1:0][ID_WIDTH-1:0]          axi_ar_id, \
  output logic [NUM_AXI-1:0][ADDR_WIDTH-1:0]        axi_ar_addr, \
  output logic [NUM_AXI-1:0][7:0]                   axi_ar_len, \
  output logic [NUM_AXI-1:0][2:0]                   axi_ar_size, \
  output logic [NUM_AXI-1:0][1:0]                   axi_ar_burst, \
  output logic [NUM_AXI-1:0]                        axi_ar_lock, \
  output logic [NUM_AXI-1:0][3:0]                   axi_ar_cache, \
  output logic [NUM_AXI-1:0][2:0]                   axi_ar_prot, \
  input  logic [NUM_AXI-1:0]                        axi_r_valid, \
  output logic [NUM_AXI-1:0]                        axi_r_ready, \
  input  logic [NUM_AXI-1:0][ID_WIDTH-1:0]          axi_r_id, \
  input  logic [NUM_AXI-1:0][DATA_WIDTH-1:0]        axi_r_data, \
  input  logic [NUM_AXI-1:0][1:0]                   axi_r_resp, \
  input  logic [NUM_AXI-1:0]                        axi_r_last

`endif  // AXI_TB_PORTS_SVH
