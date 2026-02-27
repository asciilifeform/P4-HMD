// This file is part of the SPI controller for the Private Eye P4 HMD.
// It is in the public domain.
// (C) 2026 Stanislav Datskovskiy (www.loper-os.org)

// Controller for Reflection Technology's "Private Eye" electromechanical HMD.
//
// Transaction Format (normal operation) :
//  ____________________________________________________________________________
// |    First (Low) Command Byte   |   Second (High) Command Byte  |..Payload..~
// |-------------------------------|-------------------------------|------------
// |BOS|BOS|BOS|       |BOS CMD|BOS|                               |           ~
// |DUP|NEW|REV|       | BYTES |FLG|         BOS: not sent         |           ~
// |FLG|FLG|FLG|_______| COUNT |   |_______________________________|   count   ~
// | 7 | 6 | 5 | 4 | 3 | 2 | 1 | 0 | 7 | 6 | 5 | 4 | 3 | 2 | 1 | 0 |  ~bytes~  ~
// |                           |BOS|                               |           ~
// | ~BOS: Byte Count (low 7)  |FLG|   ~BOS: Byte Count (high 8)   |           ~
// ----------------------------------------------------------------------------~

// Allow BOS headers up to 15 bytes long! (fuzzing for undocumented commands)
// `define BOS_FUZZ

`default_nettype none

module priveye
  #(parameter 
    CLK_MHZ = 16, // Input clock frequency in MHz
    READY_TIMEOUT_MS = 15, // PE is offline when nRDY active at least this long
    PE_LONG_NRDY_MIN_MS = 3, // Min active time (mS) of a 'long' nRDY interval
    PE_SLEEP_MIN_MS = 14, // nRDY inactive period indicating that PE scan is off
    WINDOW_INIT_BYTES = 10000, // Window size on reset
    WINDOW_LUFT_BYTES = 100, // Window is 'closing' when smaller than this count
    OP_TIMEOUT_MS = 100, // Max time in ms to wait for a pending byte during op
    FULL_REFRESH_BYTES = 25200, // 720*280/8 for standard resolution
    BOS_T_TICKS = 4, // Minimum width of a BOS active pulse (clk=16MHz)
    NBOS_T_TICKS = 4 // Minimum interval between BOS active pulses (clk=16MHz)
    )
   (
    input wire       clk,      // Twice the target frequency of pe_clock.
    input wire       reset,    // Active-high asynchronous reset.
    // Data I/O for host, with handshaking:
    input wire       send,     // Host ts high until ack rises (or online falls).
    input wire [7:0] data,     // Byte from host. Must be valid before send rises.
    output reg       ack,      // Pulses high to tell host that byte was accepted.
    // Status signals to host:
    output wire      online,   // High if Private Eye is connected and booted up.
    output wire      cold,     // High if Private Eye needs a full screen refresh.
    output reg       idle,     // High when nothing is pending
    output reg       frame,    // Pulses high when starting a BOS payload.
    output reg       tear,     // Pulses high if cycle ended inside image payload.
    output wire      timeout,  // Pulses high if an op has timed out.
    output wire      sleep,    // High when Private Eye is in sleep mode.
    output reg       vblank,   // High when Private Eye is in long-nRDY interval.
    // I/O connections, via level shifters, to the Private Eye device:
    output wire      pe_data,  // Data to PE. Valid on rising edge of pe_clock.
    output wire      pe_clock, // Serial clock to Private Eye. 8MHz max per docs.
    output wire      pe_nbos,  // "not Beginning of Screen" signal to Private Eye.
    input wire       pe_nrdy   // "not Ready" status signal from Private Eye.
    );
   
   // Serial burst transmitter (see tx.v) transfers data to the Private Eye:
   wire [7:0] tx_data; // Data byte being offered to the transmitter
   reg        transmit; // High: transmitter burst enable
   wire       tx_ack, tx_idle; // tx_ack high when transmitter accepts tx_data
   tx transmitter
     (.clk(clk), .reset(reset),
      .transmit(transmit), .data(tx_data), .ack(tx_ack), .idle(tx_idle),
      .sck(pe_clock), .sdat(pe_data) // Serial clock/data to PE
      );
   
   // Sync for nRDY ("not ready") asynchronous input signal from Private Eye.
   // Note: nRDY is pulled high and so will rise if the cable is unplugged.
   wire nrdy;
   synchronizer nrdy_synchronizer
     (.clk(clk), .async(pe_nrdy), .synced(nrdy));
   
   // The Private Eye's internal cycle period is exactly 20ms (i.e. 50HZ.)
   // 'ready' is active (high) for ~15ms (with a ~160us break in the ~middle),
   // followed by a ~5ms inactive interval (when PE is copying its framebuffer.)
   // The PE's mirror moves during the entire cycle, while the micro-LED array
   // lights up each row in turn ('portrait' scan) from the copied framebuffer.
   // Per the vendor docs, PE can be fed at most 2 bytes after going 'unready.'
   wire ready = !nrdy; // High at all times (after sync delay) when PE is ready
   // Note: one cycle can fit, at 8MHz, only ~half of a full screen refresh!
   
   reg  nbos; // "Not Beginning of Screen" signal to Private Eye
   assign pe_nbos = nbos;
   wire bos = !nbos; // High when BOS mode is active
   
   wire bos_t; // High: BOS has been active for at least BOS_T_TICKS
   counter #(.N(BOS_T_TICKS)) bos_t_ticks
     (.reset(reset), .clk(clk), .inc(bos), .zap(nbos), .lim(bos_t));
   
   wire nbos_t; // High: BOS has been inactive for at least NBOS_T_TICKS
   counter #(.N(NBOS_T_TICKS)) nbos_t_ticks
     (.reset(reset), .clk(clk), .inc(nbos), .zap(bos), .lim(nbos_t));
   
   // Clock dividers:
   wire clk_us; // One pulse per microsecond
   counter #(.N(CLK_MHZ)) usec_clock
     (.reset(reset), .clk(clk), .inc(1'b1), .zap(clk_us), .lim(clk_us));
   
   wire clk_ms; // One pulse per millisecond
   counter #(.N(1000)) msec_clock
     (.reset(reset), .clk(clk), .inc(clk_us), .zap(clk_ms), .lim(clk_ms));

   // Determine if we timed out during an op while waiting for a pending byte
   wire op_dog = idle || ack || tx_ack; // High: clear op timer
   wire n_timeout;
   counter #(.N(OP_TIMEOUT_MS)) op_byte_timeout_msec
     (.reset(reset), .clk(clk), .inc(clk_ms),
      .zap(op_dog), .n_lim(n_timeout), .lim(timeout));
   
   // 'online' is low at reset, rises when nRDY falls (PE becomes ready),
   // and falls again whenever nRDY has remained high for READY_TIMEOUT_MS.
   counter #(.N(READY_TIMEOUT_MS), .INIT(READY_TIMEOUT_MS)) online_timeout_msec
     (.reset(reset), .clk(clk), .inc(clk_ms), .zap(ready), .n_lim(online));
   
   // 'cold' rises at reset and at any time the Private Eye goes offline,
   // and falls again after one complete screen refresh has taken place.
   counter #(.N(FULL_REFRESH_BYTES)) refresh_bytes
     (.reset(reset), .clk(clk),
      .inc(tx_ack), .zap(!online || (cold && bos)), .n_lim(cold));

   // 'sleep' is high when PE is powered but not scanning (nRDY is DC low)
   wire sleeping;
   counter #(.N(PE_SLEEP_MIN_MS)) sleep_timer_msec
     (.reset(reset), .clk(clk), .inc(clk_ms), .zap(nrdy), .lim(sleeping));
   assign sleep = sleeping && ready; // Falls immediately when nRDY rises
   
   // 'long nRDY': PE copies its internal frame buffer during a ~5ms nRDY high.
   // This is actually the vblank interval, but we can't detect its beginning
   // here, only its end (we will use the window logic to find its beginning.)
   wire l_nrdy_min; // High: nRDY now active for at least PE_LONG_NRDY_MIN_MS
   counter #(.N(PE_LONG_NRDY_MIN_MS)) long_nrdy_timer_msec
     (.reset(reset), .clk(clk), .inc(clk_ms), .zap(ready), .lim(l_nrdy_min));
   
   // High for precisely one tick at the start of a PE active cycle:
   wire reopen = l_nrdy_min && ready; // long-nRDY (or sleep) has ended just now
   
   // Operation logic:
   reg  image; // High: we expect an image (rather than BOS) payload
   reg  dup_mode; // High: byte duping mode (image only: repeat given byte)
   wire dup = dup_mode && image; // Dup mode does not apply to BOS data
   reg  new_mode; // High: new frame mode (image only: don't use the window)
   wire new_image = new_mode && image; // BOS payloads always use the window
   reg  rev_mode; // High: flip bit order in image payload bytes
   wire byte_rev = rev_mode && image; // BOS payload bytes are never flipped
   wire [7:0] data_flipped; // Mirror image of the current data byte
   reverse #(.WIDTH(8)) flipper (.in(data), .out(data_flipped));
   assign tx_data = byte_rev ? data_flipped : data; // Select flipped/straight
   
   localparam BYTES_BITS = 15, // Up to 32767 bytes in an image payload
              BYTES_MAX = {BYTES_BITS{1'b1}}, // 'maxint'
              BYTES_NIL = BYTES_BITS'b0; // zero
   reg [BYTES_BITS-1:0] bytes; // Bytes expected/remaining in the payload
   wire                       n_payload = !bytes; // High: no bytes are pending
   wire                       payload = !n_payload; // High if any bytes pending
   
   // A sham transmitter, to count how many bytes could have fit in an interval:
   wire                       pseudo_transmit = ready && !sleep;
   wire                       pseudo_tx_ack; // High: _could have_ sent a byte
   tx pseudo_transmitter // Unused logic and i/o will be pruned by synthesizer
     (.clk(clk), .reset(reset), .transmit(pseudo_transmit), .ack(pseudo_tx_ack));
   
   // Assuming host fills the FIFO on time when bytes pending (i.e. no stalls):
   reg [BYTES_BITS-1:0] window; // Estimate bytes we could send NOW w/out a tear
   reg [BYTES_BITS-1:0] refill; // Size of the window when it reopens
   reg                  pass; // Proposed payload is not blocked by the window
   always @(posedge clk or posedge reset)
     if (reset) begin
        window <= BYTES_NIL;
        refill <= WINDOW_INIT_BYTES;
        pass <= 1'b0;
        vblank <= 1'b0;
     end else begin
        if (reopen || sleep) begin // New cycle has started, or PE is sleeping:
           pass <= 1'b1; // Must let payload go now regardless of its size
           vblank <= 1'b0; // Remains low while window is open or PE is sleeping
           window <= refill; // Window is opened to its estimated full size
           refill <= sleep ? WINDOW_INIT_BYTES : BYTES_NIL; // Get the next size
        end else begin // During the cycle:
           // If image start, or window too small, payload waits for next cycle:
           if (new_image || (window <= bytes)) pass <= 1'b0;
           // If clocks drift, window may not fully close prior to long-nRDY.
           // When window is near closing, vblank rises with nRDY (if PE online)
           if (nrdy && (window < WINDOW_LUFT_BYTES)) vblank <= online;
           // Every time when a byte-sending time slot elapses:
           if (pseudo_tx_ack) begin
              window <= window - (window != BYTES_NIL); // Shrink current window
              refill <= refill + (refill != BYTES_MAX); // Grow refill for next
           end
        end
     end
   
   // Note: the commands are particular to this controller, rather than the PE.
   // Format spec for the LOW command byte (HIGH is just top 8 bits of count):
   wire bos_en = data[0]; // Set LSB of low command byte indicates a BOS payload
   wire bos_rev_en = data[5]; // (bos_en) Set: image bytes must get flipped
   wire bos_new_en = data[6]; // (bos_en) Set: image payload must wait for reopen
   wire bos_dup_en = data[7]; // (bos_en) Set: image payload is a repeated byte
   wire [6:0] image_low_seven = data[7:1]; // Low 7 bits of byte count
`ifdef BOS_FUZZ // Fuzzing: allow sending up to 15 bytes in a BOS sequence:
   wire [6:0] bos_low_seven = {3'b0, data[4:1]};
`else // Otherwise: per vendor docs, BOS sequences may have 0/1/3 bytes only:
   wire [6:0] bos_low_seven = {5'b0, data[2], data[2] || data[1]}; // 0/1/3
`endif
   
   // Operation control FSM:
   localparam OP_CODE_LOW  = 2'b00, // Get flags and bottom 7 bits of byte count
              OP_CODE_HIGH = 2'b01, // (If image) get top 8 bits of byte count
              OP_WAIT      = 2'b11, // Wait to start the payload, if required
              OP_PAYLOAD   = 2'b10; // Set up BOS, if req'd, and send payload
   
   reg [1:0]  state, next;
   always @(posedge clk or posedge reset)
     if (reset) begin
        state <= OP_CODE_LOW;
        idle <= 1'b0;
        bytes <= BYTES_NIL;
        image <= 1'b0;
        dup_mode <= 1'b0; // dup = dup_mode && image
        new_mode <= 1'b0; // new_image = new_mode && image
        rev_mode <= 1'b0; // byte_rev = rev_mode && image
        frame <= 1'b0;
        tear <= 1'b0;
        ack <= 1'b0;
        transmit <= 1'b0;
        nbos <= 1'b1;
     end else begin
        idle <= 1'b0;
        ack <= 1'b0;
        transmit <= 1'b0;
        nbos <= 1'b1;
        frame <= 1'b0;
        tear <= 1'b0;
        next = state;
        case (state)
          OP_CODE_LOW: // BOS enable, dup enable, bottom 7 bits of byte count:
            if (send) begin
               ack <= 1'b1; // Let host know that we got the first command byte
               if (bos_en) begin // If set: BOS payload
                  image <= 1'b0;
                  bytes <= {8'b0, bos_low_seven}; // Load the bottom 7 bits
                  dup_mode <= bos_dup_en; // Dup mode toggle (sticky!)
                  new_mode <= bos_new_en; // New-frame mode toggle (sticky!)
                  rev_mode <= bos_rev_en; // Reverse image bits order (sticky!)
                  next = OP_WAIT; // BOS: no second command byte
               end else begin // If not set: image payload
                  image <= 1'b1;
                  bytes <= {8'b0, image_low_seven}; // Load the bottom 7 bits
                  next = OP_CODE_HIGH; // Image: get second command byte
               end
            end else idle <= tx_idle; // Let host know when nothing is happening
          
          OP_CODE_HIGH: // Top 8 bits of byte count (only for image payload)
            if (send) begin
               ack <= 1'b1; // Let host know that we got the second command byte
               bytes <= {data, bytes[6:0]}; // Load the top 8 bits of byte count
               next = OP_WAIT;
            end
          
          OP_WAIT: // Wait for conditions required to send the payload
            if (nbos_t && // Ensure that nbos_t has elapsed after previous op
                (image || (tx_idle && ready)) && // If BOS: PE ready, tx emptied
                (n_payload || (pass && send))) // If payload: fits and waiting
              begin
                 frame <= 1'b1; // Pulses to tell host we're starting a payload
                 tear <= 1'b0; // Clear tear alarm
                 next = OP_PAYLOAD;
              end
          
          OP_PAYLOAD: begin // Send the BOS or image payload data
             nbos <= image; // nBOS: low if this is a BOS payload; high if image
             if (payload) begin // Payload bytes are pending:
                transmit <= ready && send; // Send current tx_data byte to PE
                ack <= tx_ack && !dup; // Ask host for next byte, unless duping
                bytes <= bytes - tx_ack; // Decrement count of remaining bytes
                tear <= tear || vblank; // High: most recent payload was torn!
             end else // Payload done (if BOS: let bos_t elapse and tx empty)
               if (image || (bos_t && tx_idle)) begin
                  ack <= dup; // If we were duping, finally ack the dup byte
                  next = OP_CODE_LOW;
               end
          end
        endcase // case (state)

        // End the current op if a byte timed out or Private Eye went offline
        state <= (online && n_timeout) ? next : OP_CODE_LOW;
     end

endmodule // priveye
