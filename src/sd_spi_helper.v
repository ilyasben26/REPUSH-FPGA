/* Copyright 2025 Grug Huhler.  License SPDX BSD-2-Clause.
 *
 * This file implements features that allow SD card SPI mode to be
 * bit-banged in software.  It also provides a "helper" function that
 * does a single 8-bit SPI transaction in hardware.  This supports a
 * hybrid implementation that still uses some bit-banging.
 * 
 * Three 8-bit registers for software to use:
 *   0x0 sd_tx:  Write value to send to slave, act of writing begins the send.
 *   0x4 sd_rx:  Contains value from slave after transaction finishes
 *   0x8 sd_cmd: 7:5 reserved (read as 0, ignore write)
 *                 4 busy, 1 while transaction in progress
 *                 3 miso, value of miso signal at last clock
 *                 2 cs, output (also changed by tx process)
 *                 1 mosi, output (also changed by tx process)
 *                 0 clk, output (also changed by tx process)
 */

module sd_spi_helper
  (
   input wire         clk,
   input wire         reset_n,
   input wire         sd_spi_sel,
   input wire [7:0]   sd_spi_data_i,
   input wire         we,
   input wire [1:0]   addr,
   input wire         sd_miso,
   output wire        sd_spi_ready,
   output wire [31:0] sd_spi_data_o,
   output wire        sd_mosi,
   output wire        sd_clk,
   output wire        sd_cs
   );

   `include "sys_parameters.v" /* Gets CLK_FREQ */
   
   /* Set desired SPI speed here.  I did not test past 2.25 MHz */
   localparam         SPI_FREQ=2*2250000; /* Twice desired SPI freqency */
   localparam         CLK_FACTOR_T1=(CLK_FREQ-SPI_FREQ)/SPI_FREQ;
   /* Add 1 if not even factor */
   localparam         CLK_FACTOR=((CLK_FREQ-SPI_FREQ)%SPI_FREQ>0) ? CLK_FACTOR_T1 + 1
                                                                  : CLK_FACTOR_T1;
   
   reg [7:0]          sd_tx = 'b0;
   reg [7:0]          sd_rx = 'b0;
   reg                sd_mosi_reg = 'b0;
   reg                sd_cs_reg = 'b1;
   reg                sd_clk_reg = 'b0;
   reg [4:0]          bit_cnt = 'b0;
   reg [7:0]          time_cnt;

   assign sd_spi_data_o = (addr == 2'b00) ? {24'b0, sd_tx} :
                          (addr == 2'b01) ? {24'b0, sd_rx} :
                          (addr == 2'b10) ? {24'b0, (bit_cnt != 'b0), sd_miso,
                                             sd_cs_reg, sd_mosi_reg, sd_clk_reg}
                                                           : 32'b0;
   
   assign sd_spi_ready = sd_spi_sel;
   assign sd_cs = sd_cs_reg;
   assign sd_mosi = sd_mosi_reg;
   assign sd_clk = sd_clk_reg;

   always @(posedge clk or negedge reset_n)
     if (!reset_n) begin
        sd_tx <= 'b0;
        sd_rx <= 'b0;
        sd_mosi_reg <= 'b0;
        sd_clk_reg <= 'b0;
        sd_cs_reg <= 'b1;
        bit_cnt <= 'b0;
        time_cnt <= 'b0;
        end else begin
        if (bit_cnt > 'b0) begin
           /* transaction running */
           if (time_cnt > 'b0) begin
              time_cnt <= time_cnt - 1'b1;
           end else begin
              /* time_cnt is 0 so act on edge */
              if (sd_clk_reg) begin
                 /* falling edge processing: set next mosi bit */
                 sd_mosi_reg <= sd_tx[7];
                 sd_tx <= {sd_tx[6:0], 1'b0};
              end else begin
                 /* rising edge processing: sample miso */
                 sd_rx <= {sd_rx[6:0], sd_miso};
              end
              sd_clk_reg <= ~sd_clk_reg;
              bit_cnt <= bit_cnt - 1'b1;
              time_cnt <= CLK_FACTOR;
           end
        end else begin
           /* no transaction running, OK to write registers */
           if (sd_spi_sel & we) begin
              case (addr)
                2'b00: /* sd_tx */
                  begin
                     /* This starts a transaction.  Need to mosi so the slave can
                      * sample it on the next rising SPI clock.  Set sd_tx to
                      * hold the -remaing- 7 bits to send. */
                     sd_mosi_reg <= sd_spi_data_i[7];
                     sd_tx <= {sd_spi_data_i[6:0], 1'b0};
                     bit_cnt <= 'd16;
                     time_cnt <= CLK_FACTOR;
                  end
                2'b01: /* sd_rx */
                  begin
                     sd_rx <= sd_spi_data_i;
                  end
                2'b10: /* sd_cmd */
                  begin
                     sd_clk_reg <= sd_spi_data_i[0];
                     sd_mosi_reg <= sd_spi_data_i[1];
                     sd_cs_reg <= sd_spi_data_i[2];
                  end
              endcase // case (addr)
           end // if (sd_spi_sel & we)
        end // else: !if(bit_cnt > 'b0)
     end // else: !if(!reset_n)

endmodule
