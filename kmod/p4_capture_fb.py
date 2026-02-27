#!/usr/bin/env python3
"""
p4_capture_fb.py - Capture framebuffer for replay testing

Captures the current framebuffer content and converts to P4's internal
280x720 mono format for injection via sysfs.

Usage:
    ./p4_capture_fb.py                    # capture to fb_capture.bin
    ./p4_capture_fb.py -o myfile.bin      # capture to myfile.bin
    ./p4_capture_fb.py -d /dev/fb1        # use specific fb device
    ./p4_capture_fb.py --list             # list available fb devices
"""

import argparse
import os
import struct
import sys
import fcntl
import mmap

# FBIOGET_VSCREENINFO ioctl
FBIOGET_VSCREENINFO = 0x4600
FBIOGET_FSCREENINFO = 0x4602

def get_fb_info(fb_path):
    """Get framebuffer dimensions and format."""
    with open(fb_path, 'rb') as fb:
        # Variable screen info (160 bytes)
        vinfo = bytearray(160)
        fcntl.ioctl(fb, FBIOGET_VSCREENINFO, vinfo)
        
        xres = struct.unpack_from('I', vinfo, 0)[0]
        yres = struct.unpack_from('I', vinfo, 4)[0]
        xres_virtual = struct.unpack_from('I', vinfo, 8)[0]
        yres_virtual = struct.unpack_from('I', vinfo, 12)[0]
        bits_per_pixel = struct.unpack_from('I', vinfo, 24)[0]
        
        # Fixed screen info (68 bytes on 32-bit, larger on 64-bit)
        finfo = bytearray(256)
        fcntl.ioctl(fb, FBIOGET_FSCREENINFO, finfo)
        
        line_length = struct.unpack_from('I', finfo, 48)[0]
        
        return {
            'xres': xres,
            'yres': yres,
            'xres_virtual': xres_virtual,
            'yres_virtual': yres_virtual,
            'bits_per_pixel': bits_per_pixel,
            'line_length': line_length,
        }

def find_p4_fb():
    """Find the P4 framebuffer device."""
    for i in range(10):
        fb_path = f'/dev/fb{i}'
        if not os.path.exists(fb_path):
            continue
        try:
            info = get_fb_info(fb_path)
            # P4 is 720x280 (rotated) or 280x720 (native)
            if (info['xres'] == 720 and info['yres'] == 280) or \
               (info['xres'] == 280 and info['yres'] == 720):
                return fb_path, info
        except:
            continue
    return None, None

def list_fb_devices():
    """List all framebuffer devices."""
    print("Available framebuffer devices:")
    for i in range(10):
        fb_path = f'/dev/fb{i}'
        if not os.path.exists(fb_path):
            continue
        try:
            info = get_fb_info(fb_path)
            print(f"  {fb_path}: {info['xres']}x{info['yres']} @ {info['bits_per_pixel']}bpp")
        except Exception as e:
            print(f"  {fb_path}: (error: {e})")

def xrgb8888_to_mono(pixel):
    """Convert XRGB8888 pixel to mono (0 or 1)."""
    # Extract RGB (ignore X/alpha)
    b = pixel & 0xFF
    g = (pixel >> 8) & 0xFF
    r = (pixel >> 16) & 0xFF
    # Simple luminance threshold
    lum = (r * 299 + g * 587 + b * 114) // 1000
    return 1 if lum > 127 else 0

def capture_fb(fb_path, output_path, rotate=True):
    """
    Capture framebuffer and convert to P4 internal format.
    
    If rotate=True, assumes 720x280 userspace and rotates 90° to 280x720.
    If rotate=False, assumes 280x720 userspace (native mode).
    """
    info = get_fb_info(fb_path)
    print(f"Framebuffer: {info['xres']}x{info['yres']} @ {info['bits_per_pixel']}bpp")
    
    if info['bits_per_pixel'] != 32:
        print(f"Warning: expected 32bpp, got {info['bits_per_pixel']}bpp")
    
    # Read framebuffer
    fb_size = info['line_length'] * info['yres']
    with open(fb_path, 'rb') as fb:
        fb_data = fb.read(fb_size)
    
    print(f"Read {len(fb_data)} bytes from framebuffer")
    
    # P4 internal format: 280x720 mono, 1 bit per pixel, 35 bytes per line
    P4_WIDTH = 280
    P4_HEIGHT = 720
    mono_buf = bytearray(P4_WIDTH * P4_HEIGHT // 8)  # 25200 bytes
    
    if rotate:
        # Rotate 90°: (x,y) in 720x280 -> (279-y, x) in 280x720
        if info['xres'] != 720 or info['yres'] != 280:
            print(f"Warning: expected 720x280, got {info['xres']}x{info['yres']}")
        
        for src_y in range(min(info['yres'], 280)):
            for src_x in range(min(info['xres'], 720)):
                # Read source pixel (XRGB8888)
                src_offset = src_y * info['line_length'] + src_x * 4
                if src_offset + 4 > len(fb_data):
                    continue
                pixel = struct.unpack_from('<I', fb_data, src_offset)[0]
                
                # Convert to mono
                mono = xrgb8888_to_mono(pixel)
                
                # Rotate: dst_x = 279 - src_y, dst_y = src_x
                dst_x = 279 - src_y
                dst_y = src_x
                
                # Pack into mono buffer (MSB first)
                byte_idx = dst_y * (P4_WIDTH // 8) + dst_x // 8
                bit_idx = 7 - (dst_x % 8)
                if mono:
                    mono_buf[byte_idx] |= (1 << bit_idx)
    else:
        # Native mode: no rotation
        if info['xres'] != 280 or info['yres'] != 720:
            print(f"Warning: expected 280x720, got {info['xres']}x{info['yres']}")
        
        for src_y in range(min(info['yres'], P4_HEIGHT)):
            for src_x in range(min(info['xres'], P4_WIDTH)):
                # Read source pixel (XRGB8888)
                src_offset = src_y * info['line_length'] + src_x * 4
                if src_offset + 4 > len(fb_data):
                    continue
                pixel = struct.unpack_from('<I', fb_data, src_offset)[0]
                
                # Convert to mono
                mono = xrgb8888_to_mono(pixel)
                
                # Pack into mono buffer (MSB first)
                byte_idx = src_y * (P4_WIDTH // 8) + src_x // 8
                bit_idx = 7 - (src_x % 8)
                if mono:
                    mono_buf[byte_idx] |= (1 << bit_idx)
    
    # Write output
    with open(output_path, 'wb') as f:
        f.write(mono_buf)
    
    print(f"Wrote {len(mono_buf)} bytes to {output_path}")
    
    # Compute CRC for verification
    import binascii
    crc = binascii.crc32(mono_buf) & 0xFFFFFFFF
    print(f"CRC32: {crc:08x}")
    
    return mono_buf

def main():
    parser = argparse.ArgumentParser(description='Capture P4 framebuffer for replay')
    parser.add_argument('-d', '--device', help='Framebuffer device (default: auto-detect)')
    parser.add_argument('-o', '--output', default='fb_capture.bin', help='Output file')
    parser.add_argument('--no-rotate', action='store_true', help='Native mode (no rotation)')
    parser.add_argument('--list', action='store_true', help='List framebuffer devices')
    args = parser.parse_args()
    
    if args.list:
        list_fb_devices()
        return
    
    if args.device:
        fb_path = args.device
        if not os.path.exists(fb_path):
            print(f"Error: {fb_path} not found")
            sys.exit(1)
    else:
        fb_path, info = find_p4_fb()
        if not fb_path:
            print("Error: Could not find P4 framebuffer")
            print("Use --list to see available devices, or -d to specify one")
            sys.exit(1)
        print(f"Found P4 framebuffer: {fb_path}")
    
    capture_fb(fb_path, args.output, rotate=not args.no_rotate)
    
    print(f"\nTo inject this capture:")
    print(f"  sudo ./p4_test_inject.sh enable")
    print(f"  sudo ./p4_test_inject.sh file {args.output}")

if __name__ == '__main__':
    main()
