#!/usr/bin/env python3
"""
Parse and validate P4 display packet stream from hex dump.

Usage:
    # From kernel trace:
    cat /sys/kernel/debug/tracing/trace | grep 'tx=\[' | ./verify_packets.py
    
    # From hex dump file:
    ./verify_packets.py < hexdump.txt
    
    # From inline hex:
    echo "47 00 0b 18 02 00 0c 07 00 0b 21 10 00 ..." | ./verify_packets.py
"""

import sys
import re

FB_SIZE = 25200

def parse_hex_input(line):
    """Extract hex bytes from various input formats."""
    # Handle kernel trace format: tx=[47-00-0b-18-...]
    m = re.search(r'tx=\[([0-9a-f-]+)\]', line, re.I)
    if m:
        return bytes.fromhex(m.group(1).replace('-', ''))
    
    # Handle space-separated hex: 47 00 0b 18 ...
    hex_chars = re.findall(r'[0-9a-fA-F]{2}', line)
    if hex_chars:
        return bytes.fromhex(''.join(hex_chars))
    
    return b''

def parse_packet(data, offset):
    """
    Parse one packet starting at offset.
    Returns (packet_info, next_offset) or (None, offset) if invalid.
    """
    if offset >= len(data):
        return None, offset
    
    cmd = data[offset]
    
    # Bit 0 must be 1 for valid command byte
    if (cmd & 0x01) == 0:
        return {'error': f'Invalid cmd byte 0x{cmd:02x} at offset {offset} (bit 0 is 0)'}, offset
    
    # Decode command byte
    rle = (cmd >> 7) & 1
    new_frame = (cmd >> 6) & 1
    bitrev = (cmd >> 5) & 1
    cmd_len = (cmd >> 1) & 0x03
    
    # Determine header size based on cmd_len
    # cmd_len: 0=flags_only(2), 1=start_only(4), 2=reserved, 3=data(6)
    if cmd_len == 0:
        # Flags-only packet: cmd + flags
        hdr_size = 2
        if offset + hdr_size > len(data):
            return {'error': f'Truncated flags-only header at offset {offset}'}, offset
        flags = data[offset + 1]
        return {
            'type': 'flags_only',
            'offset': offset,
            'hdr_size': hdr_size,
            'data_size': 0,
            'cmd': cmd,
            'flags': flags,
            'rle': rle,
            'new_frame': new_frame,
            'bitrev': bitrev,
        }, offset + hdr_size
    
    elif cmd_len == 1:
        # Start-only packet: cmd + flags + addr_lo + addr_hi (no len field)
        # Length comes from next packet's addr or end of frame
        return {'error': f'Start-only packet (cmd_len=1) at offset {offset} - not supported'}, offset
    
    elif cmd_len == 2:
        return {'error': f'Reserved cmd_len=2 at offset {offset}'}, offset
    
    else:  # cmd_len == 3
        # Data packet: cmd + flags + addr_lo + addr_hi + len_lo + len_hi
        hdr_size = 6
        if offset + hdr_size > len(data):
            return {'error': f'Truncated data header at offset {offset}'}, offset
        
        flags = data[offset + 1]
        addr = (data[offset + 2] << 8) | data[offset + 3]  # Big-endian (high byte first)
        pkt_len_raw = data[offset + 4] | (data[offset + 5] << 8)  # Little-endian
        
        # Length field is (actual_len << 1), so divide by 2
        # Wait, let me check the actual encoding...
        # From update.h: len field stores (len << 1) to allow 0 to mean "use previous"
        # But we abolished start-only packets, so len=0 might be invalid
        pkt_len = pkt_len_raw >> 1
        
        if pkt_len == 0:
            return {'error': f'Zero-length data packet at offset {offset}'}, offset
        
        # Check address bounds
        end_addr = addr + pkt_len
        if end_addr > FB_SIZE:
            return {
                'error': f'Address overflow at offset {offset}: addr={addr} len={pkt_len} end={end_addr} > {FB_SIZE}',
                'offset': offset,
                'addr': addr,
                'len': pkt_len,
                'end': end_addr,
            }, offset
        
        # Determine data size
        if rle:
            data_size = 1  # RLE packets have 1 byte of data (the repeated value)
        else:
            data_size = pkt_len
        
        if offset + hdr_size + data_size > len(data):
            return {'error': f'Truncated data at offset {offset}: need {data_size} bytes'}, offset
        
        pkt_data = data[offset + hdr_size : offset + hdr_size + data_size]
        
        return {
            'type': 'data',
            'offset': offset,
            'hdr_size': hdr_size,
            'data_size': data_size,
            'total_size': hdr_size + data_size,
            'cmd': cmd,
            'flags': flags,
            'addr': addr,
            'len': pkt_len,
            'end': end_addr,
            'rle': rle,
            'new_frame': new_frame,
            'bitrev': bitrev,
            'data': pkt_data.hex(),
        }, offset + hdr_size + data_size

def validate_stream(data):
    """Parse and validate entire packet stream."""
    offset = 0
    packets = []
    errors = []
    
    while offset < len(data):
        pkt, next_offset = parse_packet(data, offset)
        
        if pkt is None:
            break
        
        if 'error' in pkt:
            errors.append(pkt)
            print(f"ERROR at offset {offset}: {pkt['error']}")
            # Try to recover by scanning for next valid command byte
            offset += 1
            while offset < len(data) and (data[offset] & 0x01) == 0:
                offset += 1
            continue
        
        packets.append(pkt)
        
        if pkt['type'] == 'data':
            print(f"PKT @{pkt['offset']:5d}: addr={pkt['addr']:5d} len={pkt['len']:5d} end={pkt['end']:5d} "
                  f"rle={pkt['rle']} nf={pkt['new_frame']} data={pkt['data'][:16]}...")
        else:
            print(f"PKT @{pkt['offset']:5d}: flags_only flags=0x{pkt['flags']:02x}")
        
        offset = next_offset
    
    print(f"\nTotal: {len(packets)} packets, {len(errors)} errors")
    return packets, errors

def main():
    # Read all input and concatenate hex data
    all_data = b''
    for line in sys.stdin:
        all_data += parse_hex_input(line)
    
    if not all_data:
        print("No hex data found in input")
        sys.exit(1)
    
    print(f"Parsing {len(all_data)} bytes of packet data...\n")
    packets, errors = validate_stream(all_data)
    
    if errors:
        sys.exit(1)

if __name__ == '__main__':
    main()
