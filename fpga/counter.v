// This file is part of the SPI controller for the Private Eye P4 HMD.
// It is in the public domain.
// (C) 2026 Stanislav Datskovskiy (www.loper-os.org)

`default_nettype none

module counter #(parameter
                 N = 1,   // Limal value to count to (inclusive)
                 INIT = 0 // Initial value to count from
                 )
   (
    input wire  reset,  // Active-high asynchronous reset.
    input wire  clk,    // System clock
    input wire  inc,    // Enable increment
    input wire  zap,    // Zeroize count
    output wire zero,   // Count is zero
    output wire n_zero, // Count is not zero
    output wire lim,    // Count has reached limit N and stopped
    output wire n_lim   // Count has not yet reached N
    );

   localparam COUNT_BITS = $clog2(N + 1);
   reg [COUNT_BITS-1:0] count;
   assign zero = !count;
   assign n_zero = !zero;
   assign lim = (count == N);
   assign n_lim = !lim;
   
   always @(posedge clk or posedge reset)
     if (reset)
       count <= INIT;
     else
       count <= zap ? COUNT_BITS'b0 : count + (inc && n_lim);
   
endmodule // counter
