// This file is part of the SPI controller for the Private Eye P4 HMD.
// It is in the public domain.
// (C) 2026 Stanislav Datskovskiy (www.loper-os.org)

// Serial burst transmitter (similar to an SPI master, but unidirectional.)
// Data byte is shifted out to sdat, ordered from MSB (first) to LSB (last).
// sck frequency is precisely one-half of the input clock.
// sdat will be valid on the rising edge of sck.
// When 'transmit' is raised, a complete data byte will be transmitted.

`default_nettype none

module tx
  (
   input wire       clk,
   input wire       reset,
   input wire       transmit,
   input wire [7:0] data,
   output reg       ack,
   output reg       idle,
   output wire      sdat,
   output reg       sck,
   );

   reg [7:0]  bits;
   assign sdat = bits[7];
   reg [3:0] ticks;
   wire      rising = ticks[0];
   always @(posedge clk or posedge reset)
     if (reset) begin
        bits <= 8'b0;
        sck <= 1'b0;
        ticks <= 4'b0;
        ack <= 1'b0;
        idle <= 1'b0;
     end else begin
        ack <= 1'b0;
        idle <= 1'b0;
        sck <= rising;
        ticks <= ticks + 1'b1;
        
        if (!ticks) begin
           if (transmit) begin
              ack <= 1'b1;
              bits <= data;
           end else begin
              idle <= 1'b1;
              bits <= 8'b0;
              ticks <= 4'b0;
           end
        end else bits <= rising ? bits : {bits[6:0], 1'b0};
     end // else: !if(reset)
endmodule
