// This file is part of the SPI controller for the Private Eye P4 HMD.
// It is in the public domain.
// (C) 2026 Stanislav Datskovskiy (www.loper-os.org)

`default_nettype none

module eye(input  CLK16,    // 16MHz oscillator
           output STATUS,   // LED
           // Host to SPI Slave on this device:
           input  SPI_SK,   // SPI Clock from Master
           input  SPI_DO,   // SPI Data Out from Master
           output SPI_DI,   // SPI Data In to Master
           input  SPI_NCS,  // SPI Chip Select (active low)
           // Status signals to host:
           input  NRESET,   // Asynchronous Reset from host (active low)
           input  ENABLE,   // Enable signal from host
           input  NSLEEP,   // Host allows stoppage of clock (active low)
           output DEV_RDY,  // Device Ready signal to host
           output COLD,     // Cold (PE needs full refresh) signal to host
           output VBLANK,   // VBlank signal to host
           // I/O connections, via level shifters, to the Private Eye device:
           output PE_CLK,   // Clock Out to Private Eye (8MHz when sending)
           output PE_DAT,   // Data Out to Private Eye (serial data)
           output PE_NBOS,  // Beginning of Screen to Private Eye (active low)
           input  PE_NRDY   // Not Ready status from Private Eye (active low)
           );
   
   localparam FIFO_BITNESS = 14,      // 16kB is all the SRAM in the ICE40
              BUF_MIN_SPACE = 4096,   // 4kB must be free for host to send
              MIN_SPACE_MARGIN = 32;  // Additional free space margin

   // Allow "power saving mode"
   wire       n_sleep; // Low: permit turning off clock (saves 1-2 mA...)
   SB_IO #(.PIN_TYPE(6'b0000_01), .PULLUP(1'b1)) nsleep_sb_io
     (.PACKAGE_PIN(NSLEEP), .D_IN_0(n_sleep));
   
   // 16MHz main clock.
   wire clk_in, clk;
   wire sleep = !n_sleep && !clk && allow_sleep;
   SB_IO #(.PIN_TYPE(6'b0000_11)) clk_sb_io
     (.PACKAGE_PIN(CLK16), .D_IN_0(clk_in), .LATCH_INPUT_VALUE(sleep));
   SB_GB clk_gb
     (.USER_SIGNAL_TO_GLOBAL_BUFFER(clk_in), .GLOBAL_BUFFER_OUTPUT(clk));
   
   // Reset generator.
   wire n_reset, soft_reset;
   SB_IO #(.PIN_TYPE(6'b0000_01), .PULLUP(1'b1)) nreset_sb_io
     (.PACKAGE_PIN(NRESET), .D_IN_0(n_reset));
   wire reset_in = !n_reset || soft_reset;
   wire reset;
   resetter reset_generator (.clk(clk), .reset_in(reset_in), .reset_out(reset));
   
   // SPI Chip Select (active low) signal from host. (See note further below.)
   wire spi_deselect;
   SB_IO #(.PIN_TYPE(6'b0000_01), .PULLUP(1'b1)) cs_sb_io
     (.PACKAGE_PIN(SPI_NCS), .D_IN_0(spi_deselect));

   // SPI clock
   wire spi_clk;
   SB_GB spi_clk_gb
     (.USER_SIGNAL_TO_GLOBAL_BUFFER(SPI_SK), .GLOBAL_BUFFER_OUTPUT(spi_clk));
   
   // Enable (active high) signal from host.
   wire enable;
   SB_IO #(.PIN_TYPE(6'b0000_01), .PULLUP(1'b1)) en_sb_io
     (.PACKAGE_PIN(ENABLE), .D_IN_0(enable));

   // Asynchronous SPI transceiver connected to the host.
   wire [7:0] spi_rx_data; // Byte received from host, valid when strobe high
   wire [7:0] spi_tx_data; // Byte to transmit to host, latched when strobe high
   wire       spi_strobe;  // Pulse: received a byte, and latched outgoing byte   
   spi_slave_async spitron
     (.reset(reset),
      .cs_n(spi_deselect),
      .sck(spi_clk),
      .mosi(SPI_DO),
      .miso(SPI_DI),
      .rx_data(spi_rx_data),
      .tx_data(spi_tx_data),
      .strobe(spi_strobe)
      );

   wire byte_strobe;
   SB_GB strobe_gb
     (.USER_SIGNAL_TO_GLOBAL_BUFFER(spi_strobe),
      .GLOBAL_BUFFER_OUTPUT(byte_strobe));
   
   // FIFO buffer for bytes received from the host via SPI.
   wire fifo_write_en; // High: enable writing to FIFO
   wire fifo_full_w, fifo_empty; // FIFO states
   wire no_data; // High when FIFO is empty and read FSM is idle
   wire [7:0] bus; // Data path for FIFO output
   reg        fifo_pop; // Pulse high to get a byte from the FIFO
   reg        fifo_reset; // When high, forcibly clear the FIFO
   wire       fifo_room; // FIFO has BUF_MIN_SPACE bytes available
   async_fifo
     #(.DATA_SIZE(8),
       .ADDR_SIZE(FIFO_BITNESS),
       .MIN_FREE(BUF_MIN_SPACE + MIN_SPACE_MARGIN)) spi_fifo
       (
        .w_data(spi_rx_data), // SPI bytes from the host enter the FIFO
        .w_en(fifo_write_en), // Write Enable for FIFO
        .w_clk(byte_strobe), // FIFO write is clocked by SPI strobe
        .w_rst(fifo_reset), // async reset, via FSM
        .w_full(fifo_full_w), // FIFO is full (write clk domain)
        .r_en(fifo_pop), // dequeue a byte, via FSM
        .r_clk(clk), // FIFO read in clock domain of FSM
        .r_rst(fifo_reset), // async reset, via FSM
        .r_data(bus), // Data path for bytes popped from FIFO
        .r_empty(fifo_empty), // FSM and status
        .avail(fifo_room) // Minimal space is available in FIFO
        );

   // Bring overflow alarm into the read clock domain (when clock is running)
   wire overflow;
   synchronizer #(.STAGES(1)) overflow_synchronizer
     (.clk(clk), .async(fifo_full_w), .synced(overflow));

   // Interface to Private Eye device.
   reg  send;
   wire ack, cold, pe_idle, online, pe_sleep, frame, tear, timeout;
   assign COLD = cold; // Status signal to host
   wire offline = !online;
   wire pe_standby = pe_idle && !send;
   priveye private_eye_display_controller
     (
      .clk(clk),
      .reset(reset),
      .send(send),
      .data(bus),
      .ack(ack),
      .online(online),
      .cold(cold),
      .idle(pe_idle),
      .frame(frame),
      .tear(tear),
      .timeout(timeout),
      .sleep(pe_sleep),
      .vblank(VBLANK),
      // Via level shifters:
      .pe_data(PE_DAT),
      .pe_clock(PE_CLK),
      .pe_nbos(PE_NBOS),
      .pe_nrdy(PE_NRDY)
      );
   
   // High when there is absolutely nothing happening or threatening to happen
   wire idle = pe_standby && no_data && spi_deselect;
   
   // Host violated protocol? accept no new data but let FIFO empty, then reset
   wire error = overflow || timeout; // Host made a 'coarse error of pilotage'
   reg  lockup; // High when locked up (FIFO write is disabled, DEV_RDY falls)
   wire locked = lockup || error; // High immediately on error and until reset
   assign soft_reset = lockup && idle; // If locked up, reset when we go idle
   always @(posedge clk or posedge reset)
     if (reset)
       lockup <= 1'b0;
     else
       lockup <= locked;
   
   // High: allow host to write bytes into the FIFO
   assign fifo_write_en = enable &&
                          !(offline || locked || fifo_reset || fifo_full_w);
   
   // High: let host know that we're accepting data.
   // Host should write at most BUF_MIN_SPACE bytes, then verify DEV_RDY again.
   assign DEV_RDY = fifo_write_en && fifo_room;
   
   // High when it is safe to turn off the clock:
   wire allow_sleep = idle && !enable && !locked;
   
   // SPI test mode: send back previous byte
   assign spi_tx_data = enable ? 8'b0 : spi_rx_data;
   
   // Status lamp
   wire busy = !idle;
   
   reg [7:0] code; // Currently-active blink code sequence
   led status_led(.clk(clk), .reset(reset), .lamp(STATUS), .seq(code));

   // Status lamp blink codes, in order of descending priority:
   always @*
     case (1'b1)
       locked:   code = 8'b11111110; // Controller is in lockup
       sleep:    code = 8'b0;        // Controller is sleeping
       offline:  code = 8'b0;        // PE is offline
       pe_sleep: code = 8'b10000000; // PE is in sleep mode
       busy:     code = 8'b10101010; // Controller is not idling
       cold:     code = 8'b11001100; // PE is cold
       tear:     code = 8'b11110000; // Most recent frame sent was torn
       default:  code = 8'b0; // Off
     endcase // case (1'b1)
   
   // FSM for reading FIFO
   localparam CLEAR     = 2'b00,
              GET_DATA  = 2'b01,
              FIFO_WAIT = 2'b11,
              SEND_DATA = 2'b10;
   
   reg [1:0]  state, next;
   always @(posedge clk or posedge reset)
     if (reset) begin
        state <= CLEAR;
     end else begin
        state <= offline ? CLEAR : next;
     end

   always @*
     begin
        next = state;
        fifo_reset = 1'b0;
        fifo_pop = 1'b0;
        no_data = 1'b0;
        send = 1'b0;
        
        case (state)
          CLEAR: begin
             fifo_reset = 1'b1;
             next = GET_DATA;
          end
          
          GET_DATA: begin
             if (!fifo_empty) begin
                fifo_pop = 1'b1;
                next = FIFO_WAIT;
             end else no_data = 1'b1;
          end

          FIFO_WAIT: begin
             next = SEND_DATA;
          end
          
          SEND_DATA: begin
             send = !ack;
             if (ack) next = GET_DATA;
          end
          
        endcase // case (state)
     end

endmodule // eye
