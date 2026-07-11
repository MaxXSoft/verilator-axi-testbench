`include "axi_tb_ports.svh"

// Self-checking initiator used both for width-shape lint and as an end-to-end
// RTL/C++ fabric smoke. It writes RAM, reads the value back, then writes
// 0 (or 1 on mismatch) to the testbench exit register.
module axi_tb_protocol_fixture #(
  parameter int unsigned NUM_AXI = 1,
  parameter int unsigned ADDR_WIDTH = 64,
  parameter int unsigned DATA_WIDTH = 64,
  parameter int unsigned ID_WIDTH = 4,
  parameter logic [ADDR_WIDTH-1:0] TEST_ADDRESS =
      ADDR_WIDTH'('h8000_0100),
  parameter logic [ADDR_WIDTH-1:0] EXIT_ADDRESS =
      ADDR_WIDTH'('h1000_1000)
) (
  `AXI_TB_INITIATOR_PORTS
);

  localparam logic [2:0] MEMORY_BEAT_SIZE = 3'($clog2(DATA_WIDTH / 8));
  localparam logic [DATA_WIDTH-1:0] TEST_DATA =
      DATA_WIDTH'('h0123_4567_89ab_cdef);

  typedef enum logic [2:0] {
    WRITE_MEMORY,
    WAIT_MEMORY_RESPONSE,
    READ_MEMORY,
    WAIT_READ_RESPONSE,
    WRITE_EXIT,
    WAIT_EXIT_RESPONSE,
    DONE
  } state_t;

  state_t state;
  logic aw_pending;
  logic w_pending;
  logic ar_pending;
  logic [31:0] exit_code;

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

  always_ff @(posedge clk) begin
    if (!aresetn) begin
      state <= WRITE_MEMORY;
      aw_pending <= 1'b1;
      w_pending <= 1'b1;
      ar_pending <= 1'b0;
      exit_code <= 32'd1;
    end else begin
      if (axi_aw_valid[0] && axi_aw_ready[0]) begin
        aw_pending <= 1'b0;
      end
      if (axi_w_valid[0] && axi_w_ready[0]) begin
        w_pending <= 1'b0;
      end
      if (axi_ar_valid[0] && axi_ar_ready[0]) begin
        ar_pending <= 1'b0;
      end

      case (state)
        WRITE_MEMORY: begin
          if (!aw_pending && !w_pending) begin
            state <= WAIT_MEMORY_RESPONSE;
          end
        end
        WAIT_MEMORY_RESPONSE: begin
          if (axi_b_valid[0] && axi_b_ready[0]) begin
            if (axi_b_resp[0] != 2'b00) begin
              exit_code <= 32'd1;
              aw_pending <= 1'b1;
              w_pending <= 1'b1;
              state <= WRITE_EXIT;
            end else begin
              ar_pending <= 1'b1;
              state <= READ_MEMORY;
            end
          end
        end
        READ_MEMORY: begin
          if (!ar_pending) begin
            state <= WAIT_READ_RESPONSE;
          end
        end
        WAIT_READ_RESPONSE: begin
          if (axi_r_valid[0] && axi_r_ready[0] && axi_r_last[0]) begin
            exit_code <= (axi_r_resp[0] == 2'b00 &&
                          axi_r_data[0] == TEST_DATA) ? 32'd0 : 32'd1;
            aw_pending <= 1'b1;
            w_pending <= 1'b1;
            state <= WRITE_EXIT;
          end
        end
        WRITE_EXIT: begin
          if (!aw_pending && !w_pending) begin
            state <= WAIT_EXIT_RESPONSE;
          end
        end
        WAIT_EXIT_RESPONSE: begin
          if (axi_b_valid[0] && axi_b_ready[0]) begin
            state <= DONE;
          end
        end
        default: state <= DONE;
      endcase
    end
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

    if (state == WRITE_MEMORY) begin
      axi_aw_valid[0] = aw_pending;
      axi_aw_id[0] = ID_WIDTH'(1);
      axi_aw_addr[0] = TEST_ADDRESS;
      axi_aw_size[0] = MEMORY_BEAT_SIZE;
      axi_aw_burst[0] = 2'b01;
      axi_w_valid[0] = w_pending;
      axi_w_data[0] = TEST_DATA;
      axi_w_strb[0] = '1;
      axi_w_last[0] = 1'b1;
    end else if (state == WRITE_EXIT) begin
      axi_aw_valid[0] = aw_pending;
      axi_aw_id[0] = ID_WIDTH'(3);
      axi_aw_addr[0] = EXIT_ADDRESS;
      axi_aw_size[0] = 3'd2;
      axi_aw_burst[0] = 2'b01;
      axi_w_valid[0] = w_pending;
      axi_w_data[0][31:0] = exit_code;
      axi_w_strb[0][3:0] = 4'b1111;
      axi_w_last[0] = 1'b1;
    end

    axi_b_ready[0] = (state == WAIT_MEMORY_RESPONSE) ||
                     (state == WAIT_EXIT_RESPONSE);

    if (state == READ_MEMORY) begin
      axi_ar_valid[0] = ar_pending;
      axi_ar_id[0] = ID_WIDTH'(2);
      axi_ar_addr[0] = TEST_ADDRESS;
      axi_ar_size[0] = MEMORY_BEAT_SIZE;
      axi_ar_burst[0] = 2'b01;
    end
    axi_r_ready[0] = (state == WAIT_READ_RESPONSE);
  end

endmodule
