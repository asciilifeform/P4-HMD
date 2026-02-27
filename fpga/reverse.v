// This file is part of the SPI controller for the Private Eye P4 HMD.
// It is in the public domain.
// (C) 2026 Stanislav Datskovskiy (www.loper-os.org)

`default_nettype none

module reverse #(parameter WIDTH = 8)
   (
    input wire [WIDTH-1:0]  in,
    output wire [WIDTH-1:0] out
    );

   genvar i;
   generate
      for (i = 0; i < WIDTH; i = i + 1) begin
         assign out[i] = in[WIDTH-1-i];
      end
   endgenerate

endmodule // reverse
