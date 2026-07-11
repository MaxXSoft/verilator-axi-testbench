`include "axi_tb_ports.svh"

// Thin, structural adapter from FuxiWrapper's three named AXI4 initiators to
// the testbench's packed canonical boundary.  Lane ordering is an ABI:
//   0 = instruction cache, 1 = data cache, 2 = uncached/MMIO.
module fuxi_axi_adapter #(
  parameter int unsigned NUM_AXI    = 3,
  parameter int unsigned ADDR_WIDTH = 32,
  parameter int unsigned DATA_WIDTH = 32,
  parameter int unsigned ID_WIDTH   = 4
) (
  `AXI_TB_INITIATOR_PORTS
);

  logic [3:0] inst_wid;
  logic [3:0] data_wid;
  logic [3:0] uncached_wid;

  // Optional one-shot test hook.  A write of 'Q' to UART THR raises the soft
  // IRQ immediately after its W handshake and keeps it asserted while the
  // uncached master waits for B.  The trap handler clears it by writing UART
  // MCR.  The hook is inert unless explicitly enabled with the plusarg.
  logic        irq_soft;
  logic        mmio_irq_hook_enabled;
  logic        mmio_irq_hook_fired;
  logic [31:0] uncached_write_addr;

  initial begin
    mmio_irq_hook_enabled = $test$plusargs("fuxi-mmio-store-irq");
  end

  always_ff @(posedge clk) begin
    if (!aresetn) begin
      irq_soft            <= 1'b0;
      mmio_irq_hook_fired <= 1'b0;
      uncached_write_addr <= '0;
    end else begin
      if (axi_aw_valid[2] && axi_aw_ready[2])
        uncached_write_addr <= axi_aw_addr[2];

      if (mmio_irq_hook_enabled && !mmio_irq_hook_fired &&
          axi_w_valid[2] && axi_w_ready[2] && axi_w_strb[2][0] &&
          uncached_write_addr == 32'h1000_0000 &&
          axi_w_data[2][7:0] == 8'h51) begin
        irq_soft            <= 1'b1;
        mmio_irq_hook_fired <= 1'b1;
      end

      if (mmio_irq_hook_enabled && axi_w_valid[2] && axi_w_ready[2] &&
          uncached_write_addr == 32'h1000_0004)
        irq_soft <= 1'b0;
    end
  end

  // Fuxi predates AXI4 and still exposes AXI3 WID.  It only ever emits ID 0;
  // assert that invariant every live cycle, then intentionally discard WID.
  always_ff @(posedge clk) begin
    if (aresetn) begin
      assert (inst_wid == 4'b0000)
        else $error("Fuxi instruction WID must remain zero");
      assert (data_wid == 4'b0000)
        else $error("Fuxi data WID must remain zero");
      assert (uncached_wid == 4'b0000)
        else $error("Fuxi uncached WID must remain zero");
    end
  end

  initial begin
    if (NUM_AXI != 3 || ADDR_WIDTH != 32 || DATA_WIDTH != 32 || ID_WIDTH != 4)
      $fatal(1, "Fuxi requires NUM_AXI=3, ADDR_WIDTH=32, DATA_WIDTH=32, ID_WIDTH=4");
  end

  /* verilator lint_off PINCONNECTEMPTY */
  FuxiWrapper fuxi (
    .clk              (clk),
    .rst              (~aresetn),
    .irq_timer        (1'b0),
    .irq_soft         (irq_soft),
    .irq_extern       (1'b0),
    .debug_wen        (),
    .debug_waddr      (),
    .debug_wdata      (),
    .debug_pc         (),

    .inst_arready     (axi_ar_ready[0]),
    .inst_arvalid     (axi_ar_valid[0]),
    .inst_araddr      (axi_ar_addr[0]),
    .inst_arid        (axi_ar_id[0]),
    .inst_arsize      (axi_ar_size[0]),
    .inst_arlen       (axi_ar_len[0]),
    .inst_arburst     (axi_ar_burst[0]),
    .inst_arlock      (axi_ar_lock[0]),
    .inst_arcache     (axi_ar_cache[0]),
    .inst_arprot      (axi_ar_prot[0]),
    .inst_rready      (axi_r_ready[0]),
    .inst_rvalid      (axi_r_valid[0]),
    .inst_rdata       (axi_r_data[0]),
    .inst_rid         (axi_r_id[0]),
    .inst_rlast       (axi_r_last[0]),
    .inst_rresp       (axi_r_resp[0]),
    .inst_awready     (axi_aw_ready[0]),
    .inst_awvalid     (axi_aw_valid[0]),
    .inst_awaddr      (axi_aw_addr[0]),
    .inst_awid        (axi_aw_id[0]),
    .inst_awsize      (axi_aw_size[0]),
    .inst_awlen       (axi_aw_len[0]),
    .inst_awburst     (axi_aw_burst[0]),
    .inst_awlock      (axi_aw_lock[0]),
    .inst_awcache     (axi_aw_cache[0]),
    .inst_awprot      (axi_aw_prot[0]),
    .inst_wready      (axi_w_ready[0]),
    .inst_wvalid      (axi_w_valid[0]),
    .inst_wdata       (axi_w_data[0]),
    .inst_wid         (inst_wid),
    .inst_wlast       (axi_w_last[0]),
    .inst_wstrb       (axi_w_strb[0]),
    .inst_bready      (axi_b_ready[0]),
    .inst_bvalid      (axi_b_valid[0]),
    .inst_bid         (axi_b_id[0]),
    .inst_bresp       (axi_b_resp[0]),

    .data_arready     (axi_ar_ready[1]),
    .data_arvalid     (axi_ar_valid[1]),
    .data_araddr      (axi_ar_addr[1]),
    .data_arid        (axi_ar_id[1]),
    .data_arsize      (axi_ar_size[1]),
    .data_arlen       (axi_ar_len[1]),
    .data_arburst     (axi_ar_burst[1]),
    .data_arlock      (axi_ar_lock[1]),
    .data_arcache     (axi_ar_cache[1]),
    .data_arprot      (axi_ar_prot[1]),
    .data_rready      (axi_r_ready[1]),
    .data_rvalid      (axi_r_valid[1]),
    .data_rdata       (axi_r_data[1]),
    .data_rid         (axi_r_id[1]),
    .data_rlast       (axi_r_last[1]),
    .data_rresp       (axi_r_resp[1]),
    .data_awready     (axi_aw_ready[1]),
    .data_awvalid     (axi_aw_valid[1]),
    .data_awaddr      (axi_aw_addr[1]),
    .data_awid        (axi_aw_id[1]),
    .data_awsize      (axi_aw_size[1]),
    .data_awlen       (axi_aw_len[1]),
    .data_awburst     (axi_aw_burst[1]),
    .data_awlock      (axi_aw_lock[1]),
    .data_awcache     (axi_aw_cache[1]),
    .data_awprot      (axi_aw_prot[1]),
    .data_wready      (axi_w_ready[1]),
    .data_wvalid      (axi_w_valid[1]),
    .data_wdata       (axi_w_data[1]),
    .data_wid         (data_wid),
    .data_wlast       (axi_w_last[1]),
    .data_wstrb       (axi_w_strb[1]),
    .data_bready      (axi_b_ready[1]),
    .data_bvalid      (axi_b_valid[1]),
    .data_bid         (axi_b_id[1]),
    .data_bresp       (axi_b_resp[1]),

    .uncached_arready (axi_ar_ready[2]),
    .uncached_arvalid (axi_ar_valid[2]),
    .uncached_araddr  (axi_ar_addr[2]),
    .uncached_arid    (axi_ar_id[2]),
    .uncached_arsize  (axi_ar_size[2]),
    .uncached_arlen   (axi_ar_len[2]),
    .uncached_arburst (axi_ar_burst[2]),
    .uncached_arlock  (axi_ar_lock[2]),
    .uncached_arcache (axi_ar_cache[2]),
    .uncached_arprot  (axi_ar_prot[2]),
    .uncached_rready  (axi_r_ready[2]),
    .uncached_rvalid  (axi_r_valid[2]),
    .uncached_rdata   (axi_r_data[2]),
    .uncached_rid     (axi_r_id[2]),
    .uncached_rlast   (axi_r_last[2]),
    .uncached_rresp   (axi_r_resp[2]),
    .uncached_awready (axi_aw_ready[2]),
    .uncached_awvalid (axi_aw_valid[2]),
    .uncached_awaddr  (axi_aw_addr[2]),
    .uncached_awid    (axi_aw_id[2]),
    .uncached_awsize  (axi_aw_size[2]),
    .uncached_awlen   (axi_aw_len[2]),
    .uncached_awburst (axi_aw_burst[2]),
    .uncached_awlock  (axi_aw_lock[2]),
    .uncached_awcache (axi_aw_cache[2]),
    .uncached_awprot  (axi_aw_prot[2]),
    .uncached_wready  (axi_w_ready[2]),
    .uncached_wvalid  (axi_w_valid[2]),
    .uncached_wdata   (axi_w_data[2]),
    .uncached_wid     (uncached_wid),
    .uncached_wlast   (axi_w_last[2]),
    .uncached_wstrb   (axi_w_strb[2]),
    .uncached_bready  (axi_b_ready[2]),
    .uncached_bvalid  (axi_b_valid[2]),
    .uncached_bid     (axi_b_id[2]),
    .uncached_bresp   (axi_b_resp[2])
  );
  /* verilator lint_on PINCONNECTEMPTY */

endmodule
