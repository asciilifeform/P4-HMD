// This file is part of the SPI controller for the Private Eye P4 HMD.
// It is in the public domain.
// (C) 2026 Stanislav Datskovskiy (www.loper-os.org)

`default_nettype none

module resetter #(parameter RESET_TICKS = 16)
   (
    input wire  clk,
    input wire  reset_in,
    output wire reset_out
    );

   reg [RESET_TICKS-1:0] reset_shift = RESET_TICKS'b0;
   reg                   reset = 1;
   assign reset_out = reset || reset_in;

   always @(posedge clk or posedge reset_in)
     if (reset_in) begin
        reset_shift <= RESET_TICKS'b0;
        reset <= 1'b1;
     end else begin
        reset_shift <= {reset_shift[RESET_TICKS-2:0], 1'b1};
        reset <= !reset_shift[RESET_TICKS-1];
     end
   
endmodule // resetter
