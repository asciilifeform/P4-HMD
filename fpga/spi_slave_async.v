// This file is part of the SPI controller for the Private Eye P4 HMD.
// It is in the public domain.
// (C) 2026 Stanislav Datskovskiy (www.loper-os.org)

`default_nettype none

`define SPI_TRANSMIT

module spi_slave_async
  (
    input wire       reset,   // Asynchronous Reset (active high)
    input wire       sck,     // SPI Clock from Master
    input wire       cs_n,    // SPI Chip Select from Master (active low)
    input wire       mosi,    // SPI Data from Master (captured on rising sck)
    output wire      miso,    // SPI Data to Master (valid on rising sck)
    output reg [7:0] rx_data, // SPI Incoming Byte (valid when strobe high)
`ifdef SPI_TRANSMIT
    input wire [7:0] tx_data, // SPI Outgoing Byte (captured when strobe rises)
`endif
    output wire strobe // Incoming Byte valid / Outgoing Byte capture
   );

   wire init = reset | cs_n;

   reg  rx_en, eight;
   always @(posedge sck or posedge init)
     if (init)
       {rx_en, eight, rx_data} <= {1'b0, 1'b1, 8'b0};
     else
       {rx_en, eight, rx_data} <= {1'b1, eight ? 8'b1 : rx_data, mosi};

   // Rises when SCK falls after LSB, falls when SCK rises for MSB of next byte
   assign strobe = eight && rx_en && !sck;

`ifdef SPI_TRANSMIT

   reg [7:0] tx;
   always @(negedge sck)
     tx <= eight ? tx_data : {tx[6:0], 1'b0};

   assign miso = tx[7] && !init; // MISO idles low

`elsif SPI_LOOPBACK
   assign miso = mosi;
`else
   assign miso = 1'b0;
`endif
   
endmodule
