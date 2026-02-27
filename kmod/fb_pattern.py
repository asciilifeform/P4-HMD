#!/usr/bin/env python3
"""
Framebuffer test pattern sender using mmap.

Writes patterns directly to the framebuffer via mmap and uses
DIRTYFB ioctl to notify the driver of damage.
"""

import argparse
import ctypes
import fcntl
import mmap
import os
import struct
import sys
import time

# Framebuffer ioctls
FBIOGET_VSCREENINFO = 0x4600
FBIOGET_FSCREENINFO = 0x4602
FBIO_WAITFORVSYNC = 0x40044620

# DRM damage ioctl (for DRM fbdev emulation)
# struct
DRM_IOCTL_MODE_DIRTYFB = 0xC01C64B1

# fb_var_screeninfo structure (partial)
class fb_var_screeninfo(ctypes.Structure):
    _fields_ = [
        ("xres", ctypes.c_uint32),
        ("yres", ctypes.c_uint32),
        ("xres_virtual", ctypes.c_uint32),
        ("yres_virtual", ctypes.c_uint32),
        ("xoffset", ctypes.c_uint32),
        ("yoffset", ctypes.c_uint32),
        ("bits_per_pixel", ctypes.c_uint32),
        ("grayscale", ctypes.c_uint32),
        # ... more fields we don't need
        ("_padding", ctypes.c_uint8 * 128),
    ]

# fb_fix_screeninfo structure (partial)
class fb_fix_screeninfo(ctypes.Structure):
    _fields_ = [
        ("id", ctypes.c_char * 16),
        ("smem_start", ctypes.c_ulong),
        ("smem_len", ctypes.c_uint32),
        ("type", ctypes.c_uint32),
        ("type_aux", ctypes.c_uint32),
        ("visual", ctypes.c_uint32),
        ("xpanstep", ctypes.c_uint16),
        ("ypanstep", ctypes.c_uint16),
        ("ywrapstep", ctypes.c_uint16),
        ("line_length", ctypes.c_uint32),
        # ... more fields
        ("_padding", ctypes.c_uint8 * 64),
    ]

def find_fb_device():
    """Find the P4 framebuffer device."""
    for i in range(10):
        path = f"/dev/fb{i}"
        if os.path.exists(path):
            try:
                with open(path, 'rb') as f:
                    fix = fb_fix_screeninfo()
                    fcntl.ioctl(f.fileno(), FBIOGET_FSCREENINFO, fix)
                    name = fix.id.decode('utf-8', errors='ignore').rstrip('\x00')
                    if 'p4' in name.lower() or 'drm' in name.lower():
                        return path
            except:
                pass
    # Default to fb0
    return "/dev/fb0"

class Framebuffer:
    def __init__(self, device=None, verbose=False):
        if device is None:
            device = find_fb_device()
        
        self.fd = os.open(device, os.O_RDWR)
        
        # Get variable screen info
        self.var = fb_var_screeninfo()
        fcntl.ioctl(self.fd, FBIOGET_VSCREENINFO, self.var)
        
        # Get fixed screen info
        self.fix = fb_fix_screeninfo()
        fcntl.ioctl(self.fd, FBIOGET_FSCREENINFO, self.fix)
        
        self.width = self.var.xres
        self.height = self.var.yres
        self.bpp = self.var.bits_per_pixel
        self.line_length = self.fix.line_length
        self.size = self.fix.smem_len
        
        if verbose:
            print(f"Framebuffer: {device}")
            print(f"  Resolution: {self.width}x{self.height}")
            print(f"  BPP: {self.bpp}")
            print(f"  Line length: {self.line_length}")
            print(f"  Size: {self.size}")
        
        # Memory map the framebuffer
        self.mm = mmap.mmap(self.fd, self.size, mmap.MAP_SHARED, 
                           mmap.PROT_READ | mmap.PROT_WRITE)
    
    def close(self):
        self.mm.close()
        os.close(self.fd)
    
    def fill(self, color):
        """Fill entire screen with a color (XRGB8888)."""
        # Pack as little-endian BGRX
        pixel = struct.pack('<I', color)
        row = pixel * self.width
        
        self.mm.seek(0)
        for y in range(self.height):
            self.mm.write(row)
    
    def fill_rect(self, x, y, w, h, color):
        """Fill a rectangle with a color."""
        pixel = struct.pack('<I', color)
        row = pixel * w
        
        for row_y in range(y, y + h):
            offset = row_y * self.line_length + x * (self.bpp // 8)
            self.mm.seek(offset)
            self.mm.write(row)
    
    def write_raw(self, data):
        """Write raw XRGB8888 data to framebuffer."""
        self.mm.seek(0)
        self.mm.write(data)
    
    def set_pixel(self, x, y, color):
        """Set a single pixel."""
        offset = y * self.line_length + x * (self.bpp // 8)
        self.mm.seek(offset)
        self.mm.write(struct.pack('<I', color))
    
    def sync(self):
        """Sync the framebuffer (flush to display)."""
        self.mm.flush()
        # Try WAITFORVSYNC - may not be supported
        try:
            arg = ctypes.c_uint32(0)
            fcntl.ioctl(self.fd, FBIO_WAITFORVSYNC, arg)
        except:
            pass

# Colors (XRGB8888)
WHITE = 0x00FFFFFF
BLACK = 0x00000000
RED = 0x00FF0000
GREEN = 0x0000FF00
BLUE = 0x000000FF

def pattern_white(fb):
    """Fill with white - write entire buffer at once."""
    fb.mm.seek(0)
    pixel = struct.pack('<I', WHITE)
    # Write full rows at a time
    row = pixel * fb.width
    for y in range(fb.height):
        fb.mm.write(row)

def pattern_black(fb):
    """Fill with black - write entire buffer at once."""
    fb.mm.seek(0)
    pixel = struct.pack('<I', BLACK)
    row = pixel * fb.width
    for y in range(fb.height):
        fb.mm.write(row)

def pattern_checkerboard(fb, block_size=8):
    """Checkerboard pattern - write row by row."""
    white_pixel = struct.pack('<I', WHITE)
    black_pixel = struct.pack('<I', BLACK)
    
    fb.mm.seek(0)
    for y in range(fb.height):
        row = b''
        for x in range(fb.width):
            if ((x // block_size) + (y // block_size)) % 2:
                row += white_pixel
            else:
                row += black_pixel
        fb.mm.write(row)

def pattern_hstripes(fb, stripe_height=8):
    """Horizontal stripes - write row by row."""
    white_row = struct.pack('<I', WHITE) * fb.width
    black_row = struct.pack('<I', BLACK) * fb.width
    
    fb.mm.seek(0)
    for y in range(fb.height):
        if (y // stripe_height) % 2:
            fb.mm.write(white_row)
        else:
            fb.mm.write(black_row)

def pattern_vstripes(fb, stripe_width=8):
    """Vertical stripes - write row by row."""
    white_pixel = struct.pack('<I', WHITE)
    black_pixel = struct.pack('<I', BLACK)
    
    row = b''
    for x in range(fb.width):
        if (x // stripe_width) % 2:
            row += white_pixel
        else:
            row += black_pixel
    
    fb.mm.seek(0)
    for y in range(fb.height):
        fb.mm.write(row)

def pattern_gradient_h(fb):
    """Horizontal gradient - write row by row."""
    row = b''
    for x in range(fb.width):
        gray = (x * 255) // fb.width
        color = (gray << 16) | (gray << 8) | gray
        row += struct.pack('<I', color)
    
    fb.mm.seek(0)
    for y in range(fb.height):
        fb.mm.write(row)

def pattern_gradient_v(fb):
    """Vertical gradient - write row by row."""
    fb.mm.seek(0)
    for y in range(fb.height):
        gray = (y * 255) // fb.height
        color = (gray << 16) | (gray << 8) | gray
        row = struct.pack('<I', color) * fb.width
        fb.mm.write(row)

PATTERNS = {
    'white': pattern_white,
    'black': pattern_black,
    'checkerboard': pattern_checkerboard,
    'checker': pattern_checkerboard,  # alias
    'hstripes': pattern_hstripes,
    'vstripes': pattern_vstripes,
    'gradient_h': pattern_gradient_h,
    'gradient_v': pattern_gradient_v,
}

def main():
    parser = argparse.ArgumentParser(description='Framebuffer test pattern sender')
    parser.add_argument('--device', '-d', help='Framebuffer device (default: auto-detect)')
    parser.add_argument('--pattern', '-p', choices=PATTERNS.keys(), default='white',
                        help='Pattern to display')
    parser.add_argument('--raw', '-r', help='Raw XRGB8888 file to write')
    parser.add_argument('--loop', '-l', action='store_true',
                        help='Loop through all patterns')
    parser.add_argument('--delay', type=float, default=0.5,
                        help='Delay between patterns in loop mode')
    parser.add_argument('--count', '-c', type=int, default=1,
                        help='Number of times to send pattern (for testing)')
    parser.add_argument('--verbose', '-v', action='store_true',
                        help='Print framebuffer info')
    args = parser.parse_args()
    
    fb = Framebuffer(args.device, verbose=args.verbose)
    
    try:
        if args.raw:
            # Write raw file directly
            with open(args.raw, 'rb') as f:
                data = f.read()
            fb.write_raw(data)
            fb.sync()
        elif args.loop:
            while True:
                for name, func in PATTERNS.items():
                    print(f"Pattern: {name}")
                    func(fb)
                    fb.sync()
                    time.sleep(args.delay)
        else:
            func = PATTERNS[args.pattern]
            for i in range(args.count):
                func(fb)
                fb.sync()
                if args.count > 1:
                    time.sleep(0.1)
    except KeyboardInterrupt:
        print("\nInterrupted")
    finally:
        fb.close()

if __name__ == '__main__':
    main()
