// This file is part of the SPI controller for the Private Eye P4 HMD.
// Originally cribbed from WWW, but can't presently find where.

`default_nettype none

module gray2bin #(parameter DATA_WIDTH = 32)
   (
    input  [DATA_WIDTH-1:0] gray_in,
    output [DATA_WIDTH-1:0] binary_out
    );

   genvar i;

   generate 
      for (i=0; i<DATA_WIDTH; i=i+1)
        begin
           assign binary_out[i] = ^ gray_in[DATA_WIDTH-1:i];
        end
   endgenerate

endmodule
