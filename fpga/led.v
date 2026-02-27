// This file is part of the SPI controller for the Private Eye P4 HMD.
// It is in the public domain.
// (C) 2026 Stanislav Datskovskiy (www.loper-os.org)

`default_nettype none

module led
  #(parameter CLK_FREQ = 16000000)
   (
    input wire       reset, // Asynchronous reset (active high)
    input wire       clk,   // System clock
    input wire [7:0] seq,   // Blink code (2 seconds total)
    output wire      lamp   // Output to LED
    );

   wire next; // 4HZ
   counter #(.N(CLK_FREQ / 4)) blink_sequence_clock
     (.reset(reset), .clk(clk), .inc(1'b1), .zap(next), .lim(next));

   reg [2:0] i;
   always @(posedge clk or posedge reset)
     if (reset)
       i <= 3'b0;
     else
       i <= i + next;
   
   assign lamp = seq[i];
   
endmodule // led
