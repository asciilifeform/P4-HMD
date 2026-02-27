#!/bin/bash
# Error monitoring script for P4 display driver
# Watches dmesg for errors, warnings, and state changes

RED='\033[0;31m'
YELLOW='\033[1;33m'
GREEN='\033[0;32m'
CYAN='\033[0;36m'
NC='\033[0m'

echo "P4 Error Monitor"
echo "================"
echo "Press Ctrl+C to stop"
echo

# Statistics
declare -A COUNTS
COUNTS[error]=0
COUNTS[warning]=0
COUNTS[offline]=0
COUNTS[online]=0
COUNTS[reset]=0
COUNTS[spi]=0

cleanup() {
    echo
    echo "================"
    echo "Session Summary:"
    echo "  Errors:    ${COUNTS[error]}"
    echo "  Warnings:  ${COUNTS[warning]}"
    echo "  Offline:   ${COUNTS[offline]}"
    echo "  Online:    ${COUNTS[online]}"
    echo "  Resets:    ${COUNTS[reset]}"
    echo "  SPI issues: ${COUNTS[spi]}"
    exit 0
}

trap cleanup INT

# Follow dmesg and colorize P4-related messages
sudo dmesg -wT 2>/dev/null | while read line; do
    # Skip non-P4 lines (optional - comment out to see all)
    if ! echo "$line" | grep -qiE "p4|drm_p4|spi"; then
        continue
    fi
    
    # Colorize and count
    if echo "$line" | grep -qiE "error|fail"; then
        echo -e "${RED}$line${NC}"
        ((COUNTS[error]++)) || true
    elif echo "$line" | grep -qiE "warning|warn"; then
        echo -e "${YELLOW}$line${NC}"
        ((COUNTS[warning]++)) || true
    elif echo "$line" | grep -qi "going offline\|device offline"; then
        echo -e "${RED}$line${NC}"
        ((COUNTS[offline]++)) || true
    elif echo "$line" | grep -qi "going online\|device online"; then
        echo -e "${GREEN}$line${NC}"
        ((COUNTS[online]++)) || true
    elif echo "$line" | grep -qi "reset"; then
        echo -e "${YELLOW}$line${NC}"
        ((COUNTS[reset]++)) || true
    elif echo "$line" | grep -qi "spi"; then
        echo -e "${CYAN}$line${NC}"
        ((COUNTS[spi]++)) || true
    else
        echo "$line"
    fi
done
