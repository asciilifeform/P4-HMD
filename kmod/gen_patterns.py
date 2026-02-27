#!/usr/bin/env python3
"""
Generate test patterns for P4 display (32bpp XRGB8888)

Supports two modes:
  - Rotated (default): 720x280 userspace, driver rotates 90°
  - Native (--native): 280x720 userspace, no rotation

Output format: Raw framebuffer data for DRM XRGB8888 format
Each pixel is 4 bytes: Blue, Green, Red, X (padding)

For monochrome conversion:
- White = 0xFFFFFFFF
- Black = 0x00000000
- Threshold is typically around 50% luminance
"""

import os
import sys

# Default: rotated mode (720x280)
WIDTH = 720
HEIGHT = 280
NATIVE_MODE = False

BPP = 4  # Bytes per pixel (XRGB8888)

def get_dimensions():
    """Return (width, height) based on current mode."""
    if NATIVE_MODE:
        return (280, 720)
    return (720, 280)

def row_bytes():
    w, _ = get_dimensions()
    return w * BPP

def total_bytes():
    w, h = get_dimensions()
    return w * h * BPP

# Colors in XRGB8888 format (little-endian: B G R X in memory)
WHITE = b'\xff\xff\xff\x00'  # B=255, G=255, R=255, X=0
BLACK = b'\x00\x00\x00\x00'  # B=0, G=0, R=0, X=0

# Bayer 4x4 dither matrix for gradients
BAYER_4X4 = [
    [0, 8, 2, 10],
    [12, 4, 14, 6],
    [3, 11, 1, 9],
    [15, 7, 13, 5],
]


def make_solid(color: bytes) -> bytes:
    """All pixels same color"""
    w, h = get_dimensions()
    return color * (w * h)


def make_checkerboard(square_size: int = 8) -> bytes:
    """Checkerboard pattern with given square size"""
    w, h = get_dimensions()
    data = bytearray(total_bytes())
    for y in range(h):
        for x in range(w):
            if ((x // square_size) + (y // square_size)) % 2 == 0:
                color = WHITE
            else:
                color = BLACK
            offset = (y * w + x) * BPP
            data[offset:offset + BPP] = color
    return bytes(data)


def make_hstripes(stripe_height: int = 8) -> bytes:
    """Horizontal stripes"""
    w, h = get_dimensions()
    data = bytearray(total_bytes())
    for y in range(h):
        color = WHITE if (y // stripe_height) % 2 == 0 else BLACK
        for x in range(w):
            offset = (y * w + x) * BPP
            data[offset:offset + BPP] = color
    return bytes(data)


def make_vstripes(stripe_width: int = 8) -> bytes:
    """Vertical stripes"""
    w, h = get_dimensions()
    data = bytearray(total_bytes())
    for y in range(h):
        for x in range(w):
            color = WHITE if (x // stripe_width) % 2 == 0 else BLACK
            offset = (y * w + x) * BPP
            data[offset:offset + BPP] = color
    return bytes(data)


def make_gradient_h() -> bytes:
    """Horizontal gradient (left=white, right=black) with dithering"""
    w, h = get_dimensions()
    data = bytearray(total_bytes())
    for y in range(h):
        for x in range(w):
            level = (x * 16) // w  # 0-15
            threshold = BAYER_4X4[y % 4][x % 4]
            color = BLACK if level > threshold else WHITE
            offset = (y * w + x) * BPP
            data[offset:offset + BPP] = color
    return bytes(data)


def make_gradient_v() -> bytes:
    """Vertical gradient (top=white, bottom=black) with dithering"""
    w, h = get_dimensions()
    data = bytearray(total_bytes())
    for y in range(h):
        level = (y * 16) // h  # 0-15
        for x in range(w):
            threshold = BAYER_4X4[y % 4][x % 4]
            color = BLACK if level > threshold else WHITE
            offset = (y * w + x) * BPP
            data[offset:offset + BPP] = color
    return bytes(data)


def make_grid(spacing: int = 8) -> bytes:
    """Grid pattern (single pixel lines)"""
    w, h = get_dimensions()
    data = bytearray(total_bytes())
    for y in range(h):
        for x in range(w):
            if x % spacing == 0 or y % spacing == 0:
                color = BLACK
            else:
                color = WHITE
            offset = (y * w + x) * BPP
            data[offset:offset + BPP] = color
    return bytes(data)


PATTERNS = {
    "white": lambda: make_solid(WHITE),
    "black": lambda: make_solid(BLACK),
    "checkerboard": make_checkerboard,
    "hstripes": make_hstripes,
    "vstripes": make_vstripes,
    "gradient_h": make_gradient_h,
    "gradient_v": make_gradient_v,
    "grid": make_grid,
}


def generate_all(output_dir: str = ".") -> None:
    """Generate all patterns to files"""
    w, h = get_dimensions()
    mode = "native" if NATIVE_MODE else "rotated"
    print(f"Generating test patterns for {w}x{h} display ({mode} mode)...")
    print(f"  Row bytes: {row_bytes()}, Total: {total_bytes()} bytes")

    for name, gen_func in PATTERNS.items():
        filename = os.path.join(output_dir, f"pattern_{name}.raw")
        print(f"  {filename}")
        data = gen_func()
        expected = total_bytes()
        assert len(data) == expected, f"{name}: got {len(data)}, expected {expected}"
        with open(filename, "wb") as f:
            f.write(data)

    print("Done.")


def main() -> int:
    global NATIVE_MODE
    
    args = sys.argv[1:]
    
    # Check for --native flag
    if "--native" in args:
        NATIVE_MODE = True
        args.remove("--native")
    
    if args and args[0] in ("-h", "--help"):
        print(f"Usage: {sys.argv[0]} [--native] [output_dir]")
        print(f"       {sys.argv[0]} [--native] --list")
        print(f"       {sys.argv[0]} [--native] --pattern NAME")
        print()
        print("Generate test patterns for P4 display.")
        print()
        print("Options:")
        print("  --native        Use native 280x720 mode (default: 720x280 rotated)")
        print("  --list          List available pattern names")
        print("  --pattern NAME  Output single pattern to stdout (for piping)")
        print("  output_dir      Directory to write pattern_*.raw files (default: .)")
        return 0

    if args and args[0] == "--list":
        for name in PATTERNS:
            print(name)
        return 0

    if len(args) >= 2 and args[0] == "--pattern":
        name = args[1]
        if name not in PATTERNS:
            print(f"Unknown pattern: {name}", file=sys.stderr)
            print(f"Available: {', '.join(PATTERNS.keys())}", file=sys.stderr)
            return 1
        sys.stdout.buffer.write(PATTERNS[name]())
        return 0

    output_dir = args[0] if args else "."
    generate_all(output_dir)
    return 0


if __name__ == "__main__":
    sys.exit(main())
