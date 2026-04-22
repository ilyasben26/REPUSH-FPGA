/* PUF peripheral — memory-mapped interface to the CHOICE PUF core.
 *
 * Sits on the picorv32 bus at base address 0x80000040.
 * Instantiates the VHDL entities from PUF-FPGA/src/:
 *   PUF_controller, pattern_ctrl_unit, tune_ctrl_unit,
 *   request_ctrl_unit, CHOICE_PUF_gen, CHOICE_PUF
 * Those files must be added to the Gowin IDE project alongside this file.
 *
 * Register map (addr = mem_addr[3:0]):
 *   addr[3:2] == 2'b00  (offset 0x0, word 0x80000040)
 *       Write: payload_hi — bits [63:32] of the 64-bit PUF command payload
 *       Read : response_hi — bits [63:32] of the last PUF response
 *
 *   addr[3:2] == 2'b01  (offset 0x4, word 0x80000044)
 *       Write: payload_lo — bits [31:0] of the 64-bit PUF command payload
 *       Read : response_lo — bits [31:0] of the last PUF response
 *
 *   addr[3:2] == 2'b10  (offset 0x8, word 0x80000048)
 *       Write: trigger — any write fires the command {payload_hi, payload_lo}
 *       Read : status  — bit[0] = 1 when a REQ response is ready to read
 *
 * Command encoding (top nibble of payload_hi, matching PUF UART protocol):
 *   0x1_0000000:0x00000000  — PUF_REQ        (request measurement)
 *   0x2_0000000:data        — PUF_TUNE        (data[7:5]=upper, [4:2]=lower)
 *   0x3_0000000:data        — PUF_CHOICE_SET  (data[3:2]=top, [1:0]=bottom)
 *   0x4_000000x:pattern     — TOP_PATTERN_SET (payload_hi[0]=in_bit)
 *   0x5_000000x:pattern     — BOTTOM_PATTERN  (payload_hi[0]=in_bit)
 *
 * After a REQ command, poll status until bit[0] is set, then read
 * response_hi and response_lo for the 64-bit PUF result.
 */

module puf_peripheral (
    input  wire        clk,
    input  wire        reset_n,
    input  wire        puf_sel,
    input  wire [3:0]  addr,
    input  wire [31:0] data_i,
    input  wire [3:0]  wstrb,
    output wire        puf_ready,
    output wire [31:0] data_o
);

    // FSM states
    localparam S_IDLE      = 2'd0;
    localparam S_SEND      = 2'd1;
    localparam S_WAIT_RESP = 2'd2;
    localparam S_ACK       = 2'd3;

    reg [1:0]  state;

    // CPU-written payload registers
    reg [31:0] payload_hi_r;
    reg [31:0] payload_lo_r;

    // Latched payload sent to PUF controller
    reg [63:0] puf_payload_lat;
    reg        is_req;

    // Captured PUF response
    reg [31:0] response_hi_r;
    reg [31:0] response_lo_r;
    reg        resp_valid;

    // PUF controller handshake
    reg  puf_read_valid;
    reg  puf_write_ready;
    wire puf_read_ready;
    wire puf_write_valid;

    // PUF controller → CHOICE_PUF_gen
    wire        ff_reset_w;
    wire        asr_enable_w;
    wire [11:0] asr_tuning_w;
    wire [3:0]  asr_choice_w;

    // CHOICE_PUF_gen output
    wire [63:0] puf_response_w;

    // Combinatorial bus signals
    wire write_access = puf_sel && (|wstrb);
    wire trigger_w    = write_access && (addr[3:2] == 2'b10);

    // Immediate ready (like tang_leds)
    assign puf_ready = puf_sel;

    // Read data is combinatorial from registered state
    assign data_o = (addr[3:2] == 2'b00) ? response_hi_r :
                    (addr[3:2] == 2'b01) ? response_lo_r :
                    (addr[3:2] == 2'b10) ? {31'b0, resp_valid} :
                                           32'hdeadbeef;

    // Payload register writes and FSM
    always @(posedge clk or negedge reset_n) begin
        if (!reset_n) begin
            state           <= S_IDLE;
            payload_hi_r    <= 32'h0;
            payload_lo_r    <= 32'h0;
            puf_payload_lat <= 64'h0;
            puf_read_valid  <= 1'b0;
            puf_write_ready <= 1'b0;
            is_req          <= 1'b0;
            response_hi_r   <= 32'h0;
            response_lo_r   <= 32'h0;
            resp_valid      <= 1'b0;
        end else begin
            puf_read_valid  <= 1'b0;
            puf_write_ready <= 1'b0;

            // Latch payload_hi / payload_lo on write
            if (write_access) begin
                if (addr[3:2] == 2'b00) payload_hi_r <= data_i;
                if (addr[3:2] == 2'b01) payload_lo_r <= data_i;
            end

            case (state)
                S_IDLE: begin
                    if (trigger_w && puf_read_ready) begin
                        puf_payload_lat <= {payload_hi_r, payload_lo_r};
                        is_req          <= (payload_hi_r[31:28] == 4'h1);
                        resp_valid      <= 1'b0;
                        puf_read_valid  <= 1'b1;
                        state           <= S_SEND;
                    end
                end

                S_SEND: begin
                    // puf_read_valid was pulsed last cycle
                    state <= is_req ? S_WAIT_RESP : S_IDLE;
                end

                S_WAIT_RESP: begin
                    if (puf_write_valid) begin
                        response_hi_r   <= puf_response_w[63:32];
                        response_lo_r   <= puf_response_w[31:0];
                        puf_write_ready <= 1'b1;
                        state           <= S_ACK;
                    end
                end

                S_ACK: begin
                    resp_valid <= 1'b1;
                    state      <= S_IDLE;
                end

                default: state <= S_IDLE;
            endcase
        end
    end

    // PUF_controller — receives 64-bit command payload and drives the PUF
    PUF_controller puf_ctrl_inst (
        .clk         (clk),
        .payload     (puf_payload_lat),
        .read_valid  (puf_read_valid),
        .write_ready (puf_write_ready),
        .read_ready  (puf_read_ready),
        .write_valid (puf_write_valid),
        .ff_reset    (ff_reset_w),
        .ASR_enable  (asr_enable_w),
        .ASR_tuning  (asr_tuning_w),
        .ASR_choice  (asr_choice_w)
    );

    // CHOICE_PUF_gen — 64 individual PUF cells, produces the response
    CHOICE_PUF_gen #(.PUF_WIDTH(64)) puf_gen_inst (
        .clk             (clk),
        .ff_reset        (ff_reset_w),
        .chip_enable     (asr_enable_w),
        .ASR_length_conf (asr_tuning_w),
        .ASR_data_conf   (asr_choice_w),
        .puf_response    (puf_response_w)
    );

endmodule
