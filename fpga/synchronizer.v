// This file is part of the SPI controller for the Private Eye P4 HMD.
// It is in the public domain.
// (C) 2026 Stanislav Datskovskiy (www.loper-os.org)

`default_nettype none

module synchronizer
  #(parameter STAGES = 2)
   (
    input      clk,
    input      async,
    output reg synced
    );
   
   reg [STAGES-1:0] pipe = {STAGES{1'b0}};

   always @(posedge clk) begin
      pipe <= {pipe[STAGES-2:0], async};
      synced <= pipe[STAGES-1];
   end

endmodule // synchronizer
