#!/bin/bash
# FPS measurement script for P4 display driver
# Measures actual frame rate by counting vblank events

set -e

DURATION=${1:-10}  # seconds
PATTERN=${2:-checker}

echo "P4 FPS Measurement"
echo "=================="
echo "Duration: ${DURATION}s"
echo "Pattern: $PATTERN"
echo

# Check driver loaded
if ! lsmod | grep -q drm_p4; then
    echo "ERROR: drm_p4 module not loaded"
    exit 1
fi

# Disable fbcon
for vt in /sys/class/vtconsole/vtcon*/bind; do
    echo 0 | sudo tee "$vt" > /dev/null 2>&1 || true
done

# Find card number
CARD=$(ls /dev/dri/card* 2>/dev/null | head -1)
if [ -z "$CARD" ]; then
    echo "ERROR: No DRM card found"
    exit 1
fi
echo "Using: $CARD"

# Method 1: Count vblank events via DRM
echo
echo "Method 1: DRM vblank counter"
echo "----------------------------"

# Get initial vblank count
VBLANK_START=$(cat /sys/class/drm/card*/crtc-0/vblank_count 2>/dev/null | head -1 || echo "N/A")

if [ "$VBLANK_START" != "N/A" ]; then
    # Start a pattern to generate frames
    if [ -x ./p4_setmode ]; then
        sudo ./p4_setmode normal --pattern "$PATTERN" &
        SETMODE_PID=$!
    fi
    
    sleep "$DURATION"
    
    VBLANK_END=$(cat /sys/class/drm/card*/crtc-0/vblank_count 2>/dev/null | head -1)
    
    if [ -n "$SETMODE_PID" ]; then
        kill $SETMODE_PID 2>/dev/null || true
    fi
    
    VBLANK_DIFF=$((VBLANK_END - VBLANK_START))
    FPS=$(echo "scale=2; $VBLANK_DIFF / $DURATION" | bc)
    echo "Vblank events: $VBLANK_DIFF over ${DURATION}s"
    echo "Effective FPS: $FPS"
else
    echo "vblank_count not available"
fi

# Method 2: Parse dmesg for frame markers (requires debug mode)
echo
echo "Method 2: Debug frame markers"
echo "-----------------------------"

# Clear dmesg ring buffer section marker
MARKER="FPS_TEST_START_$(date +%s)"
echo "$MARKER" | sudo tee /dev/kmsg > /dev/null

# Generate some activity
if [ -x ./p4_setmode ]; then
    sudo ./p4_setmode normal --pattern white
    sleep 0.5
    sudo ./p4_setmode normal --pattern black
    sleep 0.5
    sudo ./p4_setmode normal --pattern checker
    sleep "$DURATION"
fi

# Count frame markers in dmesg
FRAME_COUNT=$(dmesg | sed -n "/$MARKER/,\$p" | grep -c "send_diff\|new_frame\|vblank" || echo 0)
if [ "$FRAME_COUNT" -gt 0 ]; then
    FPS2=$(echo "scale=2; $FRAME_COUNT / $DURATION" | bc)
    echo "Frame markers: $FRAME_COUNT over ${DURATION}s"
    echo "Estimated FPS: $FPS2"
    echo "(Enable debug mode for accurate count: echo 'module drm_p4 +p' | sudo tee /sys/kernel/debug/dynamic_debug/control)"
else
    echo "No frame markers found (debug mode may be disabled)"
fi

# Method 3: Measure time for N frame updates
echo
echo "Method 3: Timed pattern updates"
echo "--------------------------------"

if [ -x ./p4_setmode ]; then
    FRAMES=100
    START_TIME=$(date +%s.%N)
    
    for i in $(seq 1 $FRAMES); do
        if [ $((i % 2)) -eq 0 ]; then
            sudo ./p4_setmode normal --pattern white 2>/dev/null
        else
            sudo ./p4_setmode normal --pattern black 2>/dev/null
        fi
    done
    
    END_TIME=$(date +%s.%N)
    ELAPSED=$(echo "$END_TIME - $START_TIME" | bc)
    FPS3=$(echo "scale=2; $FRAMES / $ELAPSED" | bc)
    
    echo "Pushed $FRAMES frames in ${ELAPSED}s"
    echo "Max throughput: $FPS3 fps"
    echo "(This measures driver capacity, not display refresh)"
fi

echo
echo "Done"
