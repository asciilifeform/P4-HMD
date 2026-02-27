#!/usr/bin/env python3
"""
Verify pixel-perfect output from P4 driver.

Usage:
    ./verify_output.py <dump_file> <pattern> <rotation>
    
Returns exit code 0 if pixel-perfect match, 1 otherwise.
Prints mismatch details to stderr on failure.
"""

import sys

# Hardware dimensions (what the display expects)
HW_WIDTH = 280
HW_HEIGHT = 720
FB_SIZE = (HW_WIDTH // 8) * HW_HEIGHT  # 25200 bytes


def generate_pattern(pattern, width, height):
    """Generate pattern as 2D bit array."""
    bits = []
    for y in range(height):
        row = []
        for x in range(width):
            if pattern == 'white':
                val = 1
            elif pattern == 'black':
                val = 0
            elif pattern == 'checker':
                val = 1 if ((x // 8) + (y // 8)) % 2 == 1 else 0
            elif pattern == 'hstripes':
                val = 1 if (y // 8) % 2 == 1 else 0
            elif pattern == 'vstripes':
                val = 1 if (x // 8) % 2 == 1 else 0
            else:
                raise ValueError(f"Unknown pattern: {pattern}")
            row.append(val)
        bits.append(row)
    return bits


def bits_to_bytes(bits, width, height):
    """Convert 2D bit array to packed mono bytes (MSB first)."""
    result = bytearray()
    for y in range(height):
        for x in range(0, width, 8):
            byte = 0
            for bit in range(8):
                if x + bit < width and bits[y][x + bit]:
                    byte |= (0x80 >> bit)
            result.append(byte)
    return bytes(result)


def rotate_90_ccw(bits, src_w, src_h):
    """Rotate bits 90° counter-clockwise."""
    dst_w, dst_h = src_h, src_w
    result = [[0] * dst_w for _ in range(dst_h)]
    for y in range(src_h):
        for x in range(src_w):
            result[src_w - 1 - x][y] = bits[y][x]
    return result


def compute_expected(pattern, rotation):
    """Compute expected mono output bytes."""
    # Determine framebuffer dimensions based on rotation
    if rotation in ('normal', 'inverted'):
        fb_width, fb_height = 720, 280
    else:
        fb_width, fb_height = 280, 720
    
    # Generate pattern at framebuffer size
    bits = generate_pattern(pattern, fb_width, fb_height)
    
    # Apply rotation transform
    if rotation in ('normal', 'inverted'):
        # 720x280 -> rotate 90° CCW -> 280x720
        bits = rotate_90_ccw(bits, fb_width, fb_height)
    # else: 280x720 passes through directly
    
    return bits_to_bytes(bits, HW_WIDTH, HW_HEIGHT)


def verify(dump_file, pattern, rotation):
    """Verify dump file matches expected output."""
    try:
        with open(dump_file, 'rb') as f:
            actual = f.read()
    except FileNotFoundError:
        print(f"ERROR: File not found: {dump_file}", file=sys.stderr)
        return False
    
    if len(actual) != FB_SIZE:
        print(f"ERROR: Wrong size: expected {FB_SIZE}, got {len(actual)}", file=sys.stderr)
        return False
    
    expected = compute_expected(pattern, rotation)
    
    if actual == expected:
        return True
    
    # Find mismatches
    mismatches = []
    for i in range(len(expected)):
        if actual[i] != expected[i]:
            mismatches.append(i)
    
    # Report details
    print(f"ERROR: {len(mismatches)} byte mismatches for {pattern}/{rotation}", file=sys.stderr)
    print(f"  Expected first 10: {' '.join(f'{b:02x}' for b in expected[:10])}", file=sys.stderr)
    print(f"  Actual first 10:   {' '.join(f'{b:02x}' for b in actual[:10])}", file=sys.stderr)
    
    # Show first mismatch
    i = mismatches[0]
    row = i // (HW_WIDTH // 8)
    col = (i % (HW_WIDTH // 8)) * 8
    print(f"  First mismatch at byte {i} (row {row}, col {col}): "
          f"expected 0x{expected[i]:02x}, got 0x{actual[i]:02x}", file=sys.stderr)
    
    return False


def main():
    if len(sys.argv) != 4:
        print(f"Usage: {sys.argv[0]} <dump_file> <pattern> <rotation>", file=sys.stderr)
        print("Patterns: white, black, checker, hstripes, vstripes", file=sys.stderr)
        print("Rotations: normal, left, inverted, right", file=sys.stderr)
        sys.exit(2)
    
    dump_file = sys.argv[1]
    pattern = sys.argv[2]
    rotation = sys.argv[3]
    
    if verify(dump_file, pattern, rotation):
        sys.exit(0)
    else:
        sys.exit(1)


if __name__ == '__main__':
    main()
