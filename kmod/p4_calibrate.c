// SPDX-License-Identifier: GPL-2.0
/*
 * P4 Display SPI Calibration Utility
 *
 * Tests SPI connectivity and determines maximum reliable SPI speed.
 * Uses the FPGA's echo feature: when ENABLE is low, each byte clocked
 * out on MOSI is echoed back on MISO (delayed by one byte).
 *
 * Also supports sending test patterns using the packet encoder from
 * update.h, for debugging the encoder and transmission independently
 * from the kernel driver.
 *
 * Usage: sudo ./p4_calibrate [--config p4_pins.conf] [--verbose]
 *        sudo ./p4_calibrate --pattern <type> [--frames N]
 *
 * Requirements:
 * - drm_p4 module must NOT be loaded
 * - spidev module will be loaded/unloaded as needed
 * - Must run as root (GPIO/SPI access)
 */

#define _DEFAULT_SOURCE  /* For usleep */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <linux/spi/spidev.h>
#include <linux/gpio.h>

#include "display.h"
#include "update.h"

/* Convert milliseconds to microseconds for usleep() */
#define MS_TO_US(ms)    ((ms) * 1000)

/* Pin configuration (loaded from p4_pins.conf) */
static struct {
    int spi_bus;
    int spi_cs;
    int speed_hz;
    int gpio_chip;      /* gpiochip number (e.g., 0 for /dev/gpiochip0) */
    int gpio_enable;
    int gpio_ready;
    int gpio_nreset;
    int gpio_cold;
    int gpio_nsleep;
    int gpio_vsync;
    int gpio_cs;        /* CS pin for manual control (optional) */
} config = {
    .spi_bus = 0,
    .spi_cs = 0,
    .speed_hz = 28000000,
    .gpio_chip = 0,
    .gpio_enable = -1,
    .gpio_ready = -1,
    .gpio_nreset = -1,
    .gpio_cold = -1,
    .gpio_nsleep = -1,
    .gpio_vsync = -1,
    .gpio_cs = -1,
};

static bool verbose = false;
static bool test_vblank = false;
static int spi_fd = -1;
static int gpiochip_fd = -1;

/* Pattern mode */
static const char *pattern_type = NULL;
static int pattern_frames = 1;
static bool pattern_bitrev = false;
static int stripe_count = 90;  /* Number of stripes for hstripes pattern (1-90) */

/* Test mode flags */
static bool test_frame_sync = false;
static bool test_spi_freq = false;
static bool test_stress = false;
static bool manual_cs = false;              /* Control CS via GPIO, not SPI controller */
static bool straight_loopback = false;      /* Expect MISO = MOSI (no delay) */

/* Display control commands */
static bool cmd_fill_white_rle = false;
static bool cmd_fill_white_raw = false;
static bool cmd_fill_black_rle = false;
static bool cmd_fill_black_raw = false;
static bool cmd_power_on = false;
static bool cmd_power_off = false;

/* Debug options */
static bool hexdump_spi = false;  /* Hex dump all SPI bytes */

/* GPIO line handles */
static int gpio_enable_fd = -1;
static int gpio_nreset_fd = -1;
static int gpio_ready_fd = -1;
static int gpio_ready_edge_fd = -1;  /* For edge-triggered waiting in pattern mode */
static int gpio_cold_fd = -1;
static int gpio_nsleep_fd = -1;
static int gpio_vsync_fd = -1;
static int gpio_cs_fd = -1;          /* For manual CS control */

/*
 * GPIO chardev helpers - using modern /dev/gpiochipN interface
 */
static int gpio_request_output(int line, int initial_value, const char *name)
{
    struct gpiohandle_request req = {0};

    req.lineoffsets[0] = line;
    req.flags = GPIOHANDLE_REQUEST_OUTPUT;
    req.default_values[0] = initial_value;
    strncpy(req.consumer_label, name, sizeof(req.consumer_label) - 1);
    req.lines = 1;

    if (ioctl(gpiochip_fd, GPIO_GET_LINEHANDLE_IOCTL, &req) < 0) {
        fprintf(stderr, "Failed to request GPIO %d as output: %s\n",
                line, strerror(errno));
        return -1;
    }

    return req.fd;
}

static int gpio_request_input(int line, const char *name)
{
    struct gpiohandle_request req = {0};

    req.lineoffsets[0] = line;
    req.flags = GPIOHANDLE_REQUEST_INPUT;
    strncpy(req.consumer_label, name, sizeof(req.consumer_label) - 1);
    req.lines = 1;

    if (ioctl(gpiochip_fd, GPIO_GET_LINEHANDLE_IOCTL, &req) < 0) {
        fprintf(stderr, "Failed to request GPIO %d as input: %s\n",
                line, strerror(errno));
        return -1;
    }

    return req.fd;
}

static int gpio_set_value(int fd, int value)
{
    struct gpiohandle_data data = {0};
    data.values[0] = value ? 1 : 0;

    if (ioctl(fd, GPIOHANDLE_SET_LINE_VALUES_IOCTL, &data) < 0)
        return -1;

    return 0;
}

static int gpio_get_value(int fd)
{
    struct gpiohandle_data data = {0};

    if (ioctl(fd, GPIOHANDLE_GET_LINE_VALUES_IOCTL, &data) < 0)
        return -1;

    return data.values[0];
}

/*
 * Request GPIO line for edge events (used for VSYNC)
 */
static int gpio_request_edge(int line, const char *name)
{
    struct gpioevent_request req = {0};

    req.lineoffset = line;
    req.handleflags = GPIOHANDLE_REQUEST_INPUT;
    req.eventflags = GPIOEVENT_REQUEST_RISING_EDGE;
    strncpy(req.consumer_label, name, sizeof(req.consumer_label) - 1);

    if (ioctl(gpiochip_fd, GPIO_GET_LINEEVENT_IOCTL, &req) < 0) {
        fprintf(stderr, "Failed to request GPIO %d for events: %s\n",
                line, strerror(errno));
        return -1;
    }

    return req.fd;
}

/*
 * Parse p4_pins.conf
 */
static int parse_config(const char *filename)
{
    FILE *f;
    char line[256];

    f = fopen(filename, "r");
    if (!f) {
        fprintf(stderr, "Failed to open %s: %s\n", filename, strerror(errno));
        return -1;
    }

    while (fgets(line, sizeof(line), f)) {
        char *p = line;

        /* Skip whitespace */
        while (*p == ' ' || *p == '\t')
            p++;

        /* Skip comments and empty lines */
        if (*p == '#' || *p == '\n' || *p == '\0')
            continue;

        /* Parse KEY = VALUE */
        char key[32], value[32];
        if (sscanf(p, "%31[A-Za-z_] = %31s", key, value) == 2) {
            if (strcmp(key, "SPI_BUS") == 0)
                config.spi_bus = atoi(value);
            else if (strcmp(key, "SPI_CS") == 0)
                config.spi_cs = atoi(value);
            else if (strcmp(key, "SPI_SPEED") == 0)
                config.speed_hz = atoi(value);
            else if (strcmp(key, "GPIO_CHIP") == 0)
                config.gpio_chip = atoi(value);
            else if (strcmp(key, "GPIO_ENABLE") == 0)
                config.gpio_enable = atoi(value);
            else if (strcmp(key, "GPIO_READY") == 0)
                config.gpio_ready = atoi(value);
            else if (strcmp(key, "GPIO_NRESET") == 0)
                config.gpio_nreset = atoi(value);
            else if (strcmp(key, "GPIO_COLD") == 0)
                config.gpio_cold = atoi(value);
            else if (strcmp(key, "GPIO_NSLEEP") == 0)
                config.gpio_nsleep = atoi(value);
            else if (strcmp(key, "GPIO_VSYNC") == 0)
                config.gpio_vsync = atoi(value);
            else if (strcmp(key, "GPIO_CS") == 0)
                config.gpio_cs = atoi(value);
        }
    }

    fclose(f);

    /* Validate required pins */
    if (config.gpio_enable < 0) {
        fprintf(stderr, "Missing GPIO_ENABLE in config\n");
        return -1;
    }
    if (config.gpio_ready < 0) {
        fprintf(stderr, "Missing GPIO_READY in config\n");
        return -1;
    }
    if (config.gpio_nreset < 0) {
        fprintf(stderr, "Missing GPIO_NRESET in config\n");
        return -1;
    }
    if (config.gpio_cold < 0) {
        fprintf(stderr, "Missing GPIO_COLD in config\n");
        return -1;
    }
    if (config.gpio_nsleep < 0) {
        fprintf(stderr, "Missing GPIO_NSLEEP in config\n");
        return -1;
    }
    if (config.gpio_vsync < 0) {
        fprintf(stderr, "Missing GPIO_VSYNC in config\n");
        return -1;
    }

    return 0;
}

/*
 * Initialize GPIOs via chardev interface
 */
static int init_gpios(void)
{
    char path[32];

    /* Open the GPIO chip */
    snprintf(path, sizeof(path), "/dev/gpiochip%d", config.gpio_chip);
    gpiochip_fd = open(path, O_RDONLY);
    if (gpiochip_fd < 0) {
        fprintf(stderr, "Failed to open %s: %s\n", path, strerror(errno));
        return -1;
    }

    /* Request output GPIOs */
    gpio_enable_fd = gpio_request_output(config.gpio_enable, 0, "p4-enable");
    if (gpio_enable_fd < 0)
        return -1;

    gpio_nreset_fd = gpio_request_output(config.gpio_nreset, 1, "p4-nreset");
    if (gpio_nreset_fd < 0)
        return -1;

    /* NSLEEP is active-low: start HIGH to ensure FPGA clock is running */
    gpio_nsleep_fd = gpio_request_output(config.gpio_nsleep, 1, "p4-nsleep");
    if (gpio_nsleep_fd < 0)
        return -1;

    /* Request input GPIOs */
    gpio_ready_fd = gpio_request_input(config.gpio_ready, "p4-ready");
    if (gpio_ready_fd < 0)
        return -1;

    gpio_cold_fd = gpio_request_input(config.gpio_cold, "p4-cold");
    if (gpio_cold_fd < 0)
        return -1;

    /* CS GPIO for manual control - start HIGH (deasserted) */
    if (manual_cs && config.gpio_cs >= 0) {
        gpio_cs_fd = gpio_request_output(config.gpio_cs, 1, "p4-cs");
        if (gpio_cs_fd < 0)
            return -1;
    }

    /* VSYNC is optional - requested separately for edge events if needed */

    return 0;
}

/*
 * Cleanup GPIOs - disable device first, then release lines
 */
static void cleanup_gpios(void)
{
    /* Disable device before releasing GPIOs */
    if (gpio_enable_fd >= 0)
        gpio_set_value(gpio_enable_fd, 0);
    
    if (gpio_ready_edge_fd >= 0) {
        close(gpio_ready_edge_fd);
        gpio_ready_edge_fd = -1;
    }
    if (gpio_enable_fd >= 0) {
        close(gpio_enable_fd);
        gpio_enable_fd = -1;
    }
    if (gpio_nreset_fd >= 0) {
        close(gpio_nreset_fd);
        gpio_nreset_fd = -1;
    }
    if (gpio_nsleep_fd >= 0) {
        close(gpio_nsleep_fd);
        gpio_nsleep_fd = -1;
    }
    if (gpio_ready_fd >= 0) {
        close(gpio_ready_fd);
        gpio_ready_fd = -1;
    }
    if (gpio_cold_fd >= 0) {
        close(gpio_cold_fd);
        gpio_cold_fd = -1;
    }
    if (gpio_vsync_fd >= 0) {
        close(gpio_vsync_fd);
        gpio_vsync_fd = -1;
    }
    if (gpio_cs_fd >= 0) {
        /* Ensure CS is deasserted (high) before releasing */
        gpio_set_value(gpio_cs_fd, 1);
        close(gpio_cs_fd);
        gpio_cs_fd = -1;
    }
    if (gpiochip_fd >= 0) {
        close(gpiochip_fd);
        gpiochip_fd = -1;
    }
}

/*
 * Wrapper functions to use the correct fd for each GPIO
 */
static int gpio_enable_set(int value)
{
    return gpio_set_value(gpio_enable_fd, value);
}

static int gpio_nreset_set(int value)
{
    return gpio_set_value(gpio_nreset_fd, value);
}

static int gpio_ready_get(void)
{
    return gpio_get_value(gpio_ready_fd);
}

static int gpio_cold_get(void)
{
    return gpio_get_value(gpio_cold_fd);
}

static int gpio_cs_set(int value)
{
    if (gpio_cs_fd >= 0)
        return gpio_set_value(gpio_cs_fd, value);
    return 0;
}

/*
 * Open SPI device
 */
static int open_spi(uint32_t speed_hz)
{
    char path[32];
    uint8_t mode = SPI_MODE_0;
    uint8_t bits = 8;

    snprintf(path, sizeof(path), "/dev/spidev%d.%d",
             config.spi_bus, config.spi_cs);

    spi_fd = open(path, O_RDWR);
    if (spi_fd < 0) {
        fprintf(stderr, "Failed to open %s: %s\n", path, strerror(errno));
        return -1;
    }

    /* If manual CS mode, add SPI_NO_CS flag */
    if (manual_cs)
        mode |= SPI_NO_CS;

    if (ioctl(spi_fd, SPI_IOC_WR_MODE, &mode) < 0) {
        fprintf(stderr, "Failed to set SPI mode\n");
        close(spi_fd);
        spi_fd = -1;
        return -1;
    }

    if (ioctl(spi_fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0) {
        fprintf(stderr, "Failed to set SPI bits\n");
        close(spi_fd);
        spi_fd = -1;
        return -1;
    }

    if (ioctl(spi_fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed_hz) < 0) {
        fprintf(stderr, "Failed to set SPI speed\n");
        close(spi_fd);
        spi_fd = -1;
        return -1;
    }

    return 0;
}

static void close_spi(void)
{
    if (spi_fd >= 0) {
        close(spi_fd);
        spi_fd = -1;
    }
}

/*
 * Reset device and verify READY response
 */
static int reset_and_verify(void)
{
    int ready;

    printf("Resetting device...\n");

    /* Assert reset (active low) */
    gpio_nreset_set(0);
    usleep(MS_TO_US(RESET_ASSERT_MS));
    gpio_nreset_set(1);
    usleep(MS_TO_US(RESET_SETTLE_MS));

    /* ENABLE low -> READY should be low */
    gpio_enable_set(0);
    /* no delay needed */
    ready = gpio_ready_get();
    if (ready != 0) {
        fprintf(stderr, "FAIL: READY=%d when ENABLE=0 (expected 0)\n", ready);
        fprintf(stderr, "      Check wiring or device power\n");
        return -1;
    }
    printf("  ENABLE=0, READY=0: OK\n");

    /* ENABLE high -> READY should go high (FIFO has space after reset) */
    gpio_enable_set(1);
    /* no delay needed */
    ready = gpio_ready_get();
    if (ready != 1) {
        fprintf(stderr, "FAIL: READY=%d when ENABLE=1 (expected 1)\n", ready);
        fprintf(stderr, "      Device not responding or FIFO issue\n");
        gpio_enable_set(0);
        return -1;
    }
    printf("  ENABLE=1, READY=1: OK\n");

    /* Back to ENABLE low for SPI echo test */
    gpio_enable_set(0);
    /* no delay needed */

    /* Verify COLD is high after reset */
    int cold = gpio_cold_get();
    if (cold != 1) {
        fprintf(stderr, "FAIL: COLD=%d after reset (expected 1)\n", cold);
        return -1;
    }
    printf("  COLD=1 after reset: OK\n");

    printf("Device connectivity verified.\n\n");
    return 0;
}

/*
 * Send a clear packet and verify COLD falls.
 *
 * The send_clear packet is an RLE packet that fills the entire framebuffer
 * with zeros. After receiving a full screen of data (25200 bytes), the FPGA
 * should clear the COLD flag.
 *
 * Uses data packet format from update.h with RLE and new_frame flags.
 * Wire length is shifted left by 1 (LSB must be 0).
 */
#define HANDSHAKE_ITERATIONS 3

/*
 * Build a send_clear packet using update.h structures.
 * Uses data packet with explicit addr=0 to ensure address is reset
 * even if previous operations left it non-zero.
 * Returns the packet size (header + 1 data byte).
 */
static size_t build_clear_packet(uint8_t *buf)
{
    struct packet_header pkt;
    
    /* Initialize data packet with addr=0, RLE enabled for full screen clear */
    u8 flags = 0;  /* Normal display flags */
    pkt_init_data(&pkt, flags, true /* new_frame */, true /* rle */, false /* bitrev */,
                  0 /* addr */, FB_SIZE);
    
    /* Copy header to buffer */
    memcpy(buf, pkt_wire(&pkt), pkt_hdr_size(&pkt));
    
    /* Add RLE data byte (zero) */
    buf[pkt_hdr_size(&pkt)] = 0x00;
    
    return pkt_hdr_size(&pkt) + 1;
}

/*
 * Verify ENABLE/READY handshake works correctly.
 * Returns 0 on success, -1 on failure.
 */
static int verify_enable_ready(const char *context)
{
    int ready;

    /* ENABLE low -> READY should be low */
    gpio_enable_set(0);
    /* no delay needed */
    ready = gpio_ready_get();
    if (ready != 0) {
        fprintf(stderr, "FAIL: %s: READY=%d when ENABLE=0 (expected 0)\n",
                context, ready);
        return -1;
    }

    /* ENABLE high -> READY should go high */
    gpio_enable_set(1);
    /* no delay needed */
    ready = gpio_ready_get();
    if (ready != 1) {
        fprintf(stderr, "FAIL: %s: READY=%d when ENABLE=1 (expected 1)\n",
                context, ready);
        gpio_enable_set(0);
        return -1;
    }

    /* Return to ENABLE low */
    gpio_enable_set(0);
    /* no delay needed */

    return 0;
}

/*
 * Perform one iteration of the handshake test:
 * 1. Verify ENABLE/READY handshake
 * 2. Verify COLD is high
 * 3. Send clear packet
 * 4. Verify COLD falls
 * 5. Verify ENABLE/READY handshake again
 * 6. Reset device
 * 7. Verify COLD rises
 */
static int test_handshake_iteration(int iteration)
{
    uint8_t send_clear_packet[8];  /* Max header size + 1 data byte */
    size_t packet_len = build_clear_packet(send_clear_packet);

    struct spi_ioc_transfer xfer = {0};
    int cold;

    printf("  Iteration %d:\n", iteration + 1);

    /* Step 1: Verify ENABLE/READY before send_clear */
    if (verify_enable_ready("before send_clear") < 0)
        return -1;
    printf("    ENABLE/READY handshake: OK\n");

    /* Step 2: Verify COLD is high */
    cold = gpio_cold_get();
    if (cold != 1) {
        fprintf(stderr, "FAIL: COLD=%d before send_clear (expected 1)\n", cold);
        return -1;
    }
    printf("    COLD=1 before send_clear: OK\n");

    /* Step 3: Send clear packet */
    gpio_enable_set(1);
    /* no delay needed */
    
    /* Wait for READY to go high - use simple retry loop */
    {
        int retries = 100;  /* 100 * 1ms = 100ms timeout */
        while (retries-- > 0 && gpio_ready_get() != 1) {
            /* no delay needed */
        }
        if (gpio_ready_get() != 1) {
            fprintf(stderr, "FAIL: READY did not go high after ENABLE\n");
            gpio_enable_set(0);
            return -1;
        }
    }

    xfer.tx_buf = (unsigned long)send_clear_packet;
    xfer.rx_buf = 0;
    xfer.len = packet_len;
    xfer.speed_hz = config.speed_hz;
    xfer.bits_per_word = 8;

    if (ioctl(spi_fd, SPI_IOC_MESSAGE(1), &xfer) < 0) {
        fprintf(stderr, "FAIL: SPI transfer failed: %s\n", strerror(errno));
        gpio_enable_set(0);
        return -1;
    }
    gpio_enable_set(0);
    printf("    send_clear packet sent: OK\n");

    /*
     * Wait for COLD to fall. The FPGA processes the RLE packet and
     * transfers to display. Full screen refresh takes ~40ms.
     * Wait up to 100ms to be safe.
     */
    {
        int wait_ms = 0;
        while (gpio_cold_get() == 1 && wait_ms < 100) {
            usleep(1000);
            wait_ms++;
        }
        cold = gpio_cold_get();
        if (cold != 0) {
            fprintf(stderr, "FAIL: COLD=%d after send_clear (expected 0 within 100ms)\n", cold);
            return -1;
        }
    }
    printf("    COLD=0 after send_clear: OK\n");

    /* Step 5: Verify ENABLE/READY after send_clear */
    if (verify_enable_ready("after send_clear") < 0)
        return -1;
    printf("    ENABLE/READY handshake: OK\n");

    /* Step 6: Reset device */
    gpio_nreset_set(0);
    usleep(MS_TO_US(RESET_ASSERT_MS));
    gpio_nreset_set(1);
    usleep(MS_TO_US(RESET_SETTLE_MS));

    /* Step 7: Verify COLD rises */
    cold = gpio_cold_get();
    if (cold != 1) {
        fprintf(stderr, "FAIL: COLD=%d after reset (expected 1)\n", cold);
        return -1;
    }
    printf("    COLD=1 after reset: OK\n");

    return 0;
}

static int test_cold_flag(void)
{
    printf("Testing ENABLE/READY handshake and COLD flag (%d iterations)...\n",
           HANDSHAKE_ITERATIONS);

    for (int i = 0; i < HANDSHAKE_ITERATIONS; i++) {
        if (test_handshake_iteration(i) < 0)
            return -1;
    }

    printf("Handshake and COLD flag behavior verified.\n\n");
    return 0;
}

/*
 * Test VSYNC signal - measure frequency.
 *
 * Uses GPIO chardev edge events to count rising edges efficiently
 * without busy-looping. The VSYNC signal is active-high, pulsing at the
 * display's refresh rate.
 */
#define VBLANK_MEASURE_MS   10000   /* Measurement period */
#define VBLANK_MIN_HZ       40      /* Minimum expected frequency */
#define VBLANK_MAX_HZ       70      /* Maximum expected frequency */

static int test_vblank_signal(void)
{
    struct pollfd pfd;
    struct timespec start, now;
    struct gpioevent_data event;
    int edges = 0;
    long elapsed_ms;
    double freq;

    printf("Testing VSYNC signal (measuring for %d seconds)...\n",
           VBLANK_MEASURE_MS / 1000);

    /* Request the VSYNC line for edge events */
    gpio_vsync_fd = gpio_request_edge(config.gpio_vsync, "p4-vsync");
    if (gpio_vsync_fd < 0) {
        fprintf(stderr, "FAIL: Cannot configure VSYNC for edge events\n");
        return -1;
    }

    pfd.fd = gpio_vsync_fd;
    pfd.events = POLLIN;

    clock_gettime(CLOCK_MONOTONIC, &start);

    /* Count rising edges using poll() */
    do {
        int ret = poll(&pfd, 1, 100); /* 100ms timeout */

        if (ret > 0 && (pfd.revents & POLLIN)) {
            /* Read the event to acknowledge it */
            if (read(gpio_vsync_fd, &event, sizeof(event)) == sizeof(event)) {
                edges++;
            }
        }

        clock_gettime(CLOCK_MONOTONIC, &now);
        elapsed_ms = (now.tv_sec - start.tv_sec) * 1000 +
                     (now.tv_nsec - start.tv_nsec) / 1000000;

    } while (elapsed_ms < VBLANK_MEASURE_MS);

    /* Close the event fd (releases the line) */
    close(gpio_vsync_fd);
    gpio_vsync_fd = -1;

    /* Calculate frequency */
    freq = (double)edges * 1000.0 / elapsed_ms;

    printf("  Measured %d edges in %.1f seconds\n", edges, elapsed_ms / 1000.0);
    printf("  VSYNC frequency: %.2f Hz\n", freq);

    if (edges == 0) {
        fprintf(stderr, "FAIL: No VSYNC edges detected\n");
        fprintf(stderr, "      Check VSYNC wiring or display power\n");
        return -1;
    }

    if (freq < VBLANK_MIN_HZ || freq > VBLANK_MAX_HZ) {
        fprintf(stderr, "WARNING: VSYNC frequency %.2f Hz outside expected range (%d-%d Hz)\n",
                freq, VBLANK_MIN_HZ, VBLANK_MAX_HZ);
        /* Don't fail, just warn - display might have non-standard refresh */
    } else {
        printf("  Frequency within expected range: OK\n");
    }

    printf("VSYNC test complete.\n\n");
    return 0;
}

/* SPI frequency test parameters */
#define DEFAULT_CHUNK_KB    4               /* Default chunk size in KB */
#define DEFAULT_TEST_MB     16              /* Default test size in MB */

/* Configurable test parameters */
static size_t freq_test_chunk = DEFAULT_CHUNK_KB * 1024;
static size_t freq_test_bytes = DEFAULT_TEST_MB * 1024 * 1024;
static bool check_ready = true;             /* Check READY GPIO between chunks */
static bool no_cs_toggle = false;           /* Keep CS asserted between chunks */
static size_t batch_xfers = 1;              /* Transfers per SPI_IOC_MESSAGE */

/* Test result structure */
struct speed_test_result {
    int errors;             /* -1 = system error, 0 = pass, >0 = byte errors */
    int ready_errors;       /* Number of times READY was high (should be 0) */
    double elapsed_secs;    /* Time taken for SPI transfers + READY checks */
    double actual_kbps;     /* Actual throughput in KB/sec */
    double actual_mhz;      /* Estimated actual MHz (kbps * 8 / 1000) */
};

/*
 * Run SPI echo test at given speed.
 *
 * The FPGA echoes MOSI to MISO with a 1-byte delay when ENABLE is low.
 * This means rx_buf[i] should equal tx_buf[i-1] for i >= 1.
 * The first received byte is undefined (previous state of shift register).
 *
 * To ensure we're talking to a real FPGA (not floating MISO or loopback):
 * - We use pseudo-random data that varies per speed
 * - We verify the 1-byte delay pattern specifically
 * - Floating MISO would give 0xFF or 0x00
 * - Direct loopback would give tx_buf[i] == rx_buf[i], not tx_buf[i-1]
 *
 * Between chunks, we read READY to simulate driver behavior and confirm
 * it stays low throughout (ENABLE is low, so READY should remain low).
 *
 * Timing includes SPI transfers AND READY checks - matching real driver overhead.
 */
static void run_echo_test(uint32_t speed_hz, size_t test_bytes, 
                          struct speed_test_result *result)
{
    uint8_t *tx_buf, *rx_buf, *expected_buf;
    struct spi_ioc_transfer xfer = {0};
    size_t num_chunks, last_chunk_size;
    struct timespec start, end;
    int ready_high_count = 0;

    result->errors = 0;
    result->ready_errors = 0;
    result->elapsed_secs = 0;
    result->actual_kbps = 0;
    result->actual_mhz = 0;

    /* Calculate chunks */
    num_chunks = (test_bytes + freq_test_chunk - 1) / freq_test_chunk;
    last_chunk_size = test_bytes - (num_chunks - 1) * freq_test_chunk;

    /* Allocate buffers for entire test */
    tx_buf = malloc(test_bytes);
    rx_buf = malloc(test_bytes);
    expected_buf = malloc(test_bytes);
    if (!tx_buf || !rx_buf || !expected_buf) {
        free(tx_buf);
        free(rx_buf);
        free(expected_buf);
        result->errors = -1;
        return;
    }

    /* Set speed for this test */
    if (ioctl(spi_fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed_hz) < 0) {
        fprintf(stderr, "Failed to set speed to %u Hz\n", speed_hz);
        free(tx_buf);
        free(rx_buf);
        free(expected_buf);
        result->errors = -1;
        return;
    }

    /* Pre-generate all random TX data */
    srand(speed_hz ^ 0xDEADBEEF);
    for (size_t i = 0; i < test_bytes; i++) {
        tx_buf[i] = rand() & 0xFF;
    }

    /*
     * Pre-compute expected RX data:
     * - Normal mode: TX shifted by 1 byte (FPGA echo with delay)
     * - Straight loopback: TX = RX (direct MOSI->MISO connection)
     */
    if (straight_loopback) {
        /* Direct loopback: expect exact match */
        memcpy(expected_buf, tx_buf, test_bytes);
    } else {
        /* FPGA echo: 1-byte delay, first byte undefined */
        expected_buf[0] = 0;  /* Don't care - will skip in verification */
        for (size_t i = 1; i < test_bytes; i++) {
            expected_buf[i] = tx_buf[i - 1];
        }
    }

    /* Ensure ENABLE is low for echo mode */
    gpio_enable_set(0);
    /* no delay needed */

    /* === TIMED SECTION: SPI transfers + READY checks === */
    clock_gettime(CLOCK_MONOTONIC, &start);

    if (no_cs_toggle) {
        /*
         * Batch mode: send multiple transfers in one SPI_IOC_MESSAGE call.
         * CS stays asserted across all transfers in the batch.
         */
        struct spi_ioc_transfer xfers[16];  /* Max 16 transfers per batch */
        size_t max_batch = batch_xfers < 16 ? batch_xfers : 16;
        size_t offset = 0;
        size_t c = 0;
        
        while (c < num_chunks) {
            size_t batch_count = 0;
            
            /* Build batch */
            while (batch_count < max_batch && c < num_chunks) {
                size_t chunk = (c == num_chunks - 1) ? last_chunk_size : freq_test_chunk;
                
                memset(&xfers[batch_count], 0, sizeof(xfers[0]));
                xfers[batch_count].tx_buf = (unsigned long)(tx_buf + offset);
                xfers[batch_count].rx_buf = (unsigned long)(rx_buf + offset);
                xfers[batch_count].len = chunk;
                xfers[batch_count].speed_hz = speed_hz;
                xfers[batch_count].bits_per_word = 8;
                /* cs_change=0 means keep CS asserted after this transfer */
                xfers[batch_count].cs_change = 0;
                
                offset += chunk;
                batch_count++;
                c++;
            }
            
            /* Last transfer in batch should deassert CS */
            xfers[batch_count - 1].cs_change = 1;
            
            if (ioctl(spi_fd, SPI_IOC_MESSAGE(batch_count), xfers) < 0) {
                clock_gettime(CLOCK_MONOTONIC, &end);
                fprintf(stderr, "SPI transfer failed: %s\n", strerror(errno));
                free(tx_buf);
                free(rx_buf);
                free(expected_buf);
                result->errors = -1;
                return;
            }
        }
    } else if (manual_cs) {
        /*
         * Manual CS mode: control CS via GPIO, check READY between chunks.
         * CS stays asserted for all chunks, only toggled at start/end.
         */
        size_t offset = 0;
        
        /* Assert CS (active low) */
        gpio_cs_set(0);
        
        for (size_t c = 0; c < num_chunks; c++) {
            size_t chunk = (c == num_chunks - 1) ? last_chunk_size : freq_test_chunk;

            memset(&xfer, 0, sizeof(xfer));
            xfer.tx_buf = (unsigned long)(tx_buf + offset);
            xfer.rx_buf = (unsigned long)(rx_buf + offset);
            xfer.len = chunk;
            xfer.speed_hz = speed_hz;
            xfer.bits_per_word = 8;

            if (ioctl(spi_fd, SPI_IOC_MESSAGE(1), &xfer) < 0) {
                gpio_cs_set(1);  /* Deassert CS on error */
                clock_gettime(CLOCK_MONOTONIC, &end);
                fprintf(stderr, "SPI transfer failed: %s\n", strerror(errno));
                free(tx_buf);
                free(rx_buf);
                free(expected_buf);
                result->errors = -1;
                return;
            }

            /* Check READY between chunks (simulates driver behavior) */
            if (check_ready && gpio_ready_get() != 0) {
                ready_high_count++;
            }

            offset += chunk;
        }
        
        /* Deassert CS */
        gpio_cs_set(1);
    } else {
        /*
         * Normal mode: one transfer per ioctl, CS toggles between each.
         */
        size_t offset = 0;
        for (size_t c = 0; c < num_chunks; c++) {
            size_t chunk = (c == num_chunks - 1) ? last_chunk_size : freq_test_chunk;

            memset(&xfer, 0, sizeof(xfer));
            xfer.tx_buf = (unsigned long)(tx_buf + offset);
            xfer.rx_buf = (unsigned long)(rx_buf + offset);
            xfer.len = chunk;
            xfer.speed_hz = speed_hz;
            xfer.bits_per_word = 8;

            if (ioctl(spi_fd, SPI_IOC_MESSAGE(1), &xfer) < 0) {
                clock_gettime(CLOCK_MONOTONIC, &end);
                fprintf(stderr, "SPI transfer failed: %s\n", strerror(errno));
                free(tx_buf);
                free(rx_buf);
                free(expected_buf);
                result->errors = -1;
                return;
            }

            /* Check READY between chunks (simulates driver behavior) */
            if (check_ready && gpio_ready_get() != 0) {
                ready_high_count++;
            }

            offset += chunk;
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &end);
    /* === END TIMED SECTION === */

    result->elapsed_secs = (end.tv_sec - start.tv_sec) + 
                           (end.tv_nsec - start.tv_nsec) / 1e9;
    result->ready_errors = ready_high_count;

    if (result->elapsed_secs > 0) {
        result->actual_kbps = (test_bytes / 1024.0) / result->elapsed_secs;
        /* MHz = bits/sec / 1e6 = (bytes * 8) / (secs * 1e6) */
        result->actual_mhz = (test_bytes * 8.0) / (result->elapsed_secs * 1e6);
    }

    /*
     * Verify received data:
     * - Normal mode: skip first byte (undefined in FPGA echo)
     * - Straight loopback: verify all bytes
     */
    size_t start_byte = straight_loopback ? 0 : 1;
    for (size_t i = start_byte; i < test_bytes; i++) {
        if (rx_buf[i] != expected_buf[i]) {
            result->errors++;
            if (verbose && result->errors <= 10) {
                printf("    Mismatch at byte %zu: expected 0x%02x, got 0x%02x\n",
                       i, expected_buf[i], rx_buf[i]);
            }
        }
    }

    free(tx_buf);
    free(rx_buf);
    free(expected_buf);
}

/*
 * Test a specific speed.
 * Returns: true if passed (zero errors), false otherwise
 */
static bool test_speed(uint32_t speed_hz)
{
    struct speed_test_result result;

    printf("  Testing %5.1f MHz (%zu MB)... ", 
           speed_hz / 1e6, freq_test_bytes / (1024 * 1024));
    fflush(stdout);

    run_echo_test(speed_hz, freq_test_bytes, &result);
    
    if (result.errors < 0) {
        printf("SYSTEM ERROR\n");
        return false;
    }
    
    /* Print timing results */
    printf("%5.0f KB/s (%4.1f MHz) ", result.actual_kbps, result.actual_mhz);
    
    if (result.errors > 0) {
        printf("FAILED (%d byte errors)\n", result.errors);
        return false;
    }
    
    if (result.ready_errors > 0) {
        printf("FAILED (READY high %d times)\n", result.ready_errors);
        return false;
    }
    
    printf("OK\n");
    return true;
}

/*
 * Get the actual SPI speed that the driver will use.
 * The driver rounds requested speeds to supported values.
 */
static uint32_t get_actual_speed(uint32_t requested)
{
    uint32_t actual = requested;

    if (ioctl(spi_fd, SPI_IOC_WR_MAX_SPEED_HZ, &requested) < 0)
        return 0;

    if (ioctl(spi_fd, SPI_IOC_RD_MAX_SPEED_HZ, &actual) < 0)
        return 0;

    return actual;
}

/*
 * Discover supported SPI speeds by probing from config speed upward.
 *
 * The SPI driver rounds requested speeds to the nearest supported value.
 * We probe upward and collect unique actual speeds.
 */
#define MAX_SPEEDS 64

static int discover_speeds(uint32_t *speeds, int max_count, uint32_t start_speed)
{
    int count = 0;
    uint32_t last_actual = 0;

    /*
     * Start from config speed and probe upward to 125 MHz.
     * The driver will round to supported values.
     */
    for (uint32_t probe = start_speed; probe <= 125000000 && count < max_count; ) {
        uint32_t actual = get_actual_speed(probe);

        if (actual > 0 && actual != last_actual) {
            /* New unique speed found */
            speeds[count++] = actual;
            last_actual = actual;

            if (verbose) {
                printf("  Discovered: %.1f MHz (requested %.1f MHz)\n", 
                       actual / 1000000.0, probe / 1000000.0);
            }
        }

        /* Increment probe value - 1 MHz steps */
        probe += 1000000;
    }

    return count;
}

/*
 * Find maximum reliable SPI speed by testing discovered speeds.
 * Starts from the speed in config file and tests upward.
 * Stops on first error.
 */
static uint32_t find_max_speed(uint32_t start_speed)
{
    uint32_t speeds[MAX_SPEEDS];
    int num_speeds;
    uint32_t max_good = 0;

    printf("Discovering supported SPI speeds (starting at %.1f MHz)...\n",
           start_speed / 1000000.0);
    num_speeds = discover_speeds(speeds, MAX_SPEEDS, start_speed);

    if (num_speeds == 0) {
        fprintf(stderr, "FAIL: Could not discover any SPI speeds\n");
        return 0;
    }

    printf("  Found %d speeds to test (%.1f MHz - %.1f MHz)\n\n",
           num_speeds,
           speeds[0] / 1000000.0,
           speeds[num_speeds - 1] / 1000000.0);

    printf("Testing SPI speeds (%zu MB each, %zu KB chunks, READY: %s, CS: %s, loopback: %s)...\n",
           freq_test_bytes / (1024 * 1024),
           freq_test_chunk / 1024,
           check_ready ? "ON" : "OFF",
           manual_cs ? "manual" : (no_cs_toggle ? "no-toggle" : "normal"),
           straight_loopback ? "straight" : "delayed");

    for (int i = 0; i < num_speeds; i++) {
        if (test_speed(speeds[i])) {
            max_good = speeds[i];
        } else {
            /*
             * Stop on error - higher speeds unlikely to work.
             */
            printf("  Stopping - higher speeds unlikely to work\n");
            break;
        }
    }

    return max_good;
}

/*
 * Load/unload spidev module
 */
static int load_spidev(void)
{
    char cmd[128];
    size_t bufsiz;
    
    /* Unload first in case it's loaded with wrong bufsiz */
    if (system("rmmod spidev 2>/dev/null") != 0) {
        /* Ignore - module may not be loaded */
    }
    /* no delay needed */
    
    /*
     * Calculate bufsiz needed:
     * - For normal mode: just the chunk size
     * - For no-cs-toggle mode: we want multiple chunks in one message
     *   Use 4 chunks per batch as a reasonable default
     */
    if (no_cs_toggle) {
        batch_xfers = 4;
        bufsiz = freq_test_chunk * batch_xfers;
    } else {
        batch_xfers = 1;
        bufsiz = freq_test_chunk;
    }
    
    /* Minimum 4KB */
    if (bufsiz < 4096)
        bufsiz = 4096;
    
    /* Load with calculated bufsiz */
    snprintf(cmd, sizeof(cmd), "modprobe spidev bufsiz=%zu 2>/dev/null", bufsiz);
    return system(cmd);
}

static void unload_spidev(void)
{
    int ret __attribute__((unused));
    /* Only unload if we loaded it */
    ret = system("rmmod spidev 2>/dev/null");
}

/* ===== Pattern Test Mode ===== */

/*
 * Read GPIO value from event fd.
 * The event fd can also be used to get current line value.
 */
/*
 * Wait for READY signal using edge events with timeout.
 * If READY is already high, returns immediately.
 * Otherwise waits for rising edge using poll().
 * Returns 0 on success, -1 on timeout.
 */
static int wait_ready_edge(int timeout_ms)
{
    struct pollfd pfd;
    struct gpioevent_data event;
    struct gpiohandle_data data = {0};
    
    /*
     * Check if READY is already high FIRST.
     * This handles the case where we just sent a small chunk and READY
     * never went low (FIFO had plenty of space).
     * GPIOHANDLE_GET_LINE_VALUES_IOCTL works on event fds since kernel 4.8.
     */
    if (ioctl(gpio_ready_edge_fd, GPIOHANDLE_GET_LINE_VALUES_IOCTL, &data) == 0) {
        if (data.values[0] == 1)
            return 0;
    }
    
    /*
     * READY is low - drain any pending events and wait for rising edge.
     */
    pfd.fd = gpio_ready_edge_fd;
    pfd.events = POLLIN;
    while (poll(&pfd, 1, 0) > 0) {
        if (read(gpio_ready_edge_fd, &event, sizeof(event)) != sizeof(event))
            break;
    }
    
    /* Wait for rising edge */
    int ret = poll(&pfd, 1, timeout_ms);
    if (ret < 0) {
        fprintf(stderr, "poll() failed: %s\n", strerror(errno));
        return -1;
    }
    if (ret == 0) {
        return -1;  /* Timeout */
    }
    
    /* Consume the event */
    if (read(gpio_ready_edge_fd, &event, sizeof(event)) != sizeof(event)) {
        fprintf(stderr, "Failed to read GPIO event: %s\n", strerror(errno));
        return -1;
    }
    
    return 0;
}

/*
 * Send raw bytes via SPI with chunking.
 * Waits for READY before each 4096-byte chunk.
 * 
 * Key insight: We should send as much as possible while READY is high,
 * only waiting when READY drops (FIFO getting full).
 */
/*
 * Timing data for READY wait analysis.
 * Records when READY transitions happened and how many bytes were sent.
 */
#define MAX_READY_SAMPLES 2048
static struct {
    struct timespec timestamp;
    size_t bytes_sent;      /* Total bytes sent up to this READY wait */
} ready_samples[MAX_READY_SAMPLES];
static int ready_sample_count = 0;

static int spi_send_bytes_timed(const u8 *data, size_t len, int timeout_ms, size_t *bytes_at_wait)
{
    size_t offset = 0;
    struct gpiohandle_data gpio_data = {0};
    int ready_waits = 0;
    
    /* Wait for initial READY */
    if (wait_ready_edge(timeout_ms) < 0) {
        fprintf(stderr, "Timeout waiting for initial READY (%dms)\n", timeout_ms);
        return -1;
    }
    
    /* Record initial READY */
    if (ready_sample_count < MAX_READY_SAMPLES) {
        clock_gettime(CLOCK_MONOTONIC, &ready_samples[ready_sample_count].timestamp);
        ready_samples[ready_sample_count].bytes_sent = bytes_at_wait ? *bytes_at_wait : 0;
        ready_sample_count++;
    }
    ready_waits++;
    
    while (offset < len) {
        size_t chunk = len - offset;
        if (chunk > SPI_CHUNK_MAX)
            chunk = SPI_CHUNK_MAX;
        
        /* Check READY is still high before this chunk */
        if (ioctl(gpio_ready_edge_fd, GPIOHANDLE_GET_LINE_VALUES_IOCTL, &gpio_data) < 0) {
            fprintf(stderr, "Failed to read READY GPIO\n");
            return -1;
        }
        
        if (gpio_data.values[0] != 1) {
            /* READY dropped - wait for it to go high again */
            if (verbose) {
                printf("    READY dropped at offset %zu, waiting...\n", offset);
            }
            if (wait_ready_edge(timeout_ms) < 0) {
                fprintf(stderr, "Timeout waiting for READY (%dms)\n", timeout_ms);
                return -1;
            }
            
            /* Record this READY transition */
            if (ready_sample_count < MAX_READY_SAMPLES) {
                clock_gettime(CLOCK_MONOTONIC, &ready_samples[ready_sample_count].timestamp);
                ready_samples[ready_sample_count].bytes_sent = 
                    (bytes_at_wait ? *bytes_at_wait : 0) + offset;
                ready_sample_count++;
            }
            ready_waits++;
        }
        
        /* Send SPI data FIRST, before any printf that could delay us */
        struct spi_ioc_transfer xfer = {0};
        xfer.tx_buf = (unsigned long)(data + offset);
        xfer.len = chunk;
        xfer.speed_hz = config.speed_hz;
        xfer.bits_per_word = 8;
        
        if (ioctl(spi_fd, SPI_IOC_MESSAGE(1), &xfer) < 0) {
            fprintf(stderr, "SPI transfer failed: %s\n", strerror(errno));
            return -1;
        }
        
        /* Hex dump AFTER sending (so printf doesn't delay the transfer) */
        if (hexdump_spi) {
            printf("SPI TX [%zu bytes]:", chunk);
            for (size_t i = 0; i < chunk; i++) {
                if (i % 16 == 0)
                    printf("\n  %04zx:", offset + i);
                printf(" %02x", data[offset + i]);
            }
            printf("\n");
        }
        
        offset += chunk;
    }
    
    /* Update cumulative bytes sent */
    if (bytes_at_wait)
        *bytes_at_wait += len;
    
    if (verbose && ready_waits > 1) {
        printf("    Total READY waits: %d for %zu bytes\n", ready_waits, len);
    }
    
    /* Return number of READY waits (positive = success) */
    return ready_waits;
}

static int spi_send_bytes_timeout(const u8 *data, size_t len, int timeout_ms)
{
    return spi_send_bytes_timed(data, len, timeout_ms, NULL);
}

/*
 * Packet emit callback for encoder.
 * Sends packet header + data via SPI.
 * Combines header and data into a single buffer to avoid multiple READY waits.
 */
static bool emit_packet_spi(const struct packet_header *pkt, void *ctx)
{
    int *packet_count = (int *)ctx;
    const u8 *wire = pkt_wire(pkt);
    size_t hdr_size = pkt_hdr_size(pkt);
    size_t data_len = pkt_data_len(pkt);
    u8 cmd = wire[0];
    bool has_new_frame = (cmd & 0x40) != 0;  /* Bit 6 is new_frame */
    
    /* Always print if new_frame is set (should only be first packet!) */
    if (has_new_frame && *packet_count > 0) {
        fprintf(stderr, "WARNING: new_frame set on packet %d (should only be packet 0)!\n",
                *packet_count);
    }
    
    if (verbose) {
        printf("  PKT[%d]: wire={", *packet_count);
        for (size_t i = 0; i < hdr_size; i++)
            printf("%s0x%02x", i ? "," : "", wire[i]);
        printf("} (nf=%d) addr=%u len=%u data_len=%zu\n",
               has_new_frame ? 1 : 0,
               pkt_addr(pkt), pkt_len(pkt), data_len);
    }
    
    /* Combine header and data into single buffer */
    size_t total_len = hdr_size + data_len;
    u8 buf[16];  /* Max header (6) + small data, larger data sent separately */
    
    if (total_len <= sizeof(buf)) {
        /* Small packet - combine into single send */
        memcpy(buf, wire, hdr_size);
        if (data_len > 0 && pkt_data(pkt))
            memcpy(buf + hdr_size, pkt_data(pkt), data_len);
        
        if (spi_send_bytes_timeout(buf, total_len, SPI_READY_TIMEOUT_MS) < 0)
            return false;
    } else {
        /* Large data - send header + data together but may need chunking */
        /* First, build combined buffer */
        u8 *combined = malloc(total_len);
        if (!combined) {
            fprintf(stderr, "Failed to allocate %zu bytes\n", total_len);
            return false;
        }
        memcpy(combined, wire, hdr_size);
        if (data_len > 0 && pkt_data(pkt))
            memcpy(combined + hdr_size, pkt_data(pkt), data_len);
        
        int ret = spi_send_bytes_timeout(combined, total_len, SPI_READY_TIMEOUT_MS);
        free(combined);
        if (ret < 0)
            return false;
    }
    
    (*packet_count)++;
    
    return true;
}

/*
 * Fill a region with a uniform byte value using encode_region().
 * This exercises the full encoder path including RLE detection.
 */
static int fill_region(u16 addr, size_t len, u8 fill_byte, bool new_frame)
{
    /* Allocate and fill buffer */
    u8 *buf = malloc(len);
    if (!buf) {
        fprintf(stderr, "Failed to allocate %zu bytes\n", len);
        return -1;
    }
    memset(buf, fill_byte, len);
    
    /* Set up encoder */
    int packet_count = 0;
    struct encoder_state enc;
    encoder_init(&enc, 0, new_frame, pattern_bitrev, emit_packet_spi, &packet_count);
    
    /* Encode and send */
    bool ok = encode_region(&enc, addr, buf, len);
    
    free(buf);
    
    if (!ok) {
        fprintf(stderr, "Encoding/transmission failed\n");
        return -1;
    }
    
    return packet_count;
}

/*
 * Fill entire screen with a byte value using raw (non-RLE) packets.
 * Uses data packet with explicit addr=0 to ensure
 * address is reset even if previous operations left it non-zero.
 */
static int fill_raw(u8 fill_byte)
{
    /* Allocate framebuffer */
    u8 *buf = malloc(FB_SIZE);
    if (!buf) {
        fprintf(stderr, "Failed to allocate %d bytes\n", FB_SIZE);
        return -1;
    }
    memset(buf, fill_byte, FB_SIZE);
    
    /* Build data packet with addr=0, no RLE */
    struct packet_header pkt;
    u8 flags = 0;  /* Normal display flags */
    pkt_init_data(&pkt, flags, true /* new_frame */, false /* rle */, pattern_bitrev,
                  0 /* addr */, FB_SIZE);
    pkt.data = buf;
    pkt.data_len = FB_SIZE;
    
    int packet_count = 0;
    bool ok = emit_packet_spi(&pkt, &packet_count);
    
    free(buf);
    
    if (!ok) {
        fprintf(stderr, "Raw transmission failed\n");
        return -1;
    }
    
    printf("Sent %d bytes raw (no RLE)\n", FB_SIZE);
    return 0;
}

/*
 * Fill entire screen with a byte value using RLE compression.
 * Uses data packet with explicit addr=0 to ensure
 * address is reset even if previous operations left it non-zero.
 */
static int fill_rle(u8 fill_byte)
{
    /* Build data packet with addr=0, RLE enabled */
    struct packet_header pkt;
    u8 flags = 0;  /* Normal display flags */
    pkt_init_data(&pkt, flags, true /* new_frame */, true /* rle */, pattern_bitrev,
                  0 /* addr */, FB_SIZE);
    pkt.data = &fill_byte;
    pkt.data_len = 1;
    
    int packet_count = 0;
    bool ok = emit_packet_spi(&pkt, &packet_count);
    
    if (!ok) {
        fprintf(stderr, "RLE transmission failed\n");
        return -1;
    }
    
    printf("Sent RLE packet (1 byte expands to %d)\n", FB_SIZE);
    return 0;
}

/*
 * Send display flags packet (power control, etc.)
 */
static int send_display_flags(u8 flags)
{
    struct packet_header pkt;
    pkt_init_flags_only(&pkt, flags);
    
    int packet_count = 0;
    bool ok = emit_packet_spi(&pkt, &packet_count);
    
    if (!ok) {
        fprintf(stderr, "Flags transmission failed\n");
        return -1;
    }
    
    printf("Sent flags packet: 0x%02x\n", flags);
    return 0;
}

/*
 * Generate pattern into framebuffer.
 */
static void generate_pattern(u8 *fb, const char *type, int frame)
{
    memset(fb, 0, FB_SIZE);
    
    if (strcmp(type, "white") == 0) {
        memset(fb, 0xFF, FB_SIZE);
    }
    else if (strcmp(type, "black") == 0) {
        /* Already zeroed */
    }
    else if (strcmp(type, "checker") == 0) {
        /* 8x8 pixel checkerboard */
        for (int y = 0; y < HW_HEIGHT; y++) {
            for (int x = 0; x < HW_WIDTH; x++) {
                int byte_idx = (y * HW_WIDTH + x) / 8;
                int bit_idx = 7 - (x % 8);
                int checker = ((x / 8) + (y / 8)) & 1;
                if (checker)
                    fb[byte_idx] |= (1 << bit_idx);
            }
        }
    }
    else if (strcmp(type, "vstripes") == 0) {
        /* Vertical stripes (8 pixels wide) */
        for (int y = 0; y < HW_HEIGHT; y++) {
            for (int x = 0; x < HW_WIDTH; x++) {
                int byte_idx = (y * HW_WIDTH + x) / 8;
                int bit_idx = 7 - (x % 8);
                if ((x / 8) & 1)
                    fb[byte_idx] |= (1 << bit_idx);
            }
        }
    }
    else if (strcmp(type, "hstripes") == 0) {
        /* Horizontal stripes (8 pixels tall), limited by stripe_count */
        int max_rows = stripe_count * 8;
        if (max_rows > HW_HEIGHT) max_rows = HW_HEIGHT;
        for (int y = 0; y < max_rows; y++) {
            if ((y / 8) & 1)
                memset(fb + (y * HW_WIDTH / 8), 0xFF, HW_WIDTH / 8);
        }
    }
    else if (strcmp(type, "gradient") == 0) {
        /* Vertical gradient using dithering */
        for (int y = 0; y < HW_HEIGHT; y++) {
            int threshold = (y * 256) / HW_HEIGHT;
            for (int x = 0; x < HW_WIDTH; x++) {
                int byte_idx = (y * HW_WIDTH + x) / 8;
                int bit_idx = 7 - (x % 8);
                /* Simple ordered dither */
                int dither = ((x & 7) * 32 + (y & 7) * 4) & 0xFF;
                if (dither < threshold)
                    fb[byte_idx] |= (1 << bit_idx);
            }
        }
    }
    else if (strcmp(type, "text") == 0) {
        /*
         * Simulate fbcon-style sparse text updates.
         * Create scattered 8x16 character cells with simple patterns.
         * Same as driver's fill_pattern_text().
         */
        for (int row = 0; row < HW_HEIGHT / 16; row++) {
            for (int col = 0; col < HW_WIDTH / 8; col++) {
                /* Skip some cells to make it sparse */
                if (((row + col) % 3) == 0)
                    continue;
                
                /* Draw a simple character pattern in this cell */
                for (int y = row * 16 + 2; y < row * 16 + 14 && y < HW_HEIGHT; y++) {
                    for (int x = col * 8 + 1; x < col * 8 + 7 && x < HW_WIDTH; x++) {
                        int byte_idx = (y * HW_WIDTH + x) / 8;
                        int bit_idx = 7 - (x % 8);
                        fb[byte_idx] |= (1 << bit_idx);
                    }
                }
            }
        }
    }
    else if (strcmp(type, "animate") == 0) {
        /* Moving vertical bar */
        int bar_x = (frame * 8) % HW_WIDTH;
        int bar_width = 32;
        for (int y = 0; y < HW_HEIGHT; y++) {
            for (int x = bar_x; x < bar_x + bar_width && x < HW_WIDTH; x++) {
                int byte_idx = (y * HW_WIDTH + x) / 8;
                int bit_idx = 7 - (x % 8);
                fb[byte_idx] |= (1 << bit_idx);
            }
        }
    }
    else {
        fprintf(stderr, "Unknown pattern: %s\n", type);
    }
}

/*
 * Buffered frame sender - accumulates all packet data then sends in one burst.
 * This avoids waiting for READY between small packets.
 */
static u8 *frame_buffer = NULL;
static size_t frame_buffer_size = 0;
static size_t frame_buffer_used = 0;

static bool emit_packet_to_buffer(const struct packet_header *pkt, void *ctx)
{
    int *packet_count = (int *)ctx;
    const u8 *wire = pkt_wire(pkt);
    size_t hdr_size = pkt_hdr_size(pkt);
    size_t data_len = pkt_data_len(pkt);
    
    if (verbose) {
        u8 cmd = wire[0];
        bool has_new_frame = (cmd & 0x40) != 0;
        printf("  PKT[%d]: wire={", *packet_count);
        for (size_t i = 0; i < hdr_size; i++)
            printf("%s0x%02x", i ? "," : "", wire[i]);
        printf("} (nf=%d) addr=%u len=%u data_len=%zu\n",
               has_new_frame ? 1 : 0,
               pkt_addr(pkt), pkt_len(pkt), data_len);
    }
    
    /* Ensure buffer has space */
    size_t needed = frame_buffer_used + hdr_size + data_len;
    if (needed > frame_buffer_size) {
        fprintf(stderr, "Frame buffer overflow: need %zu, have %zu\n",
                needed, frame_buffer_size);
        return false;
    }
    
    /* Copy header */
    memcpy(frame_buffer + frame_buffer_used, wire, hdr_size);
    frame_buffer_used += hdr_size;
    
    /* Copy data */
    if (data_len > 0 && pkt_data(pkt)) {
        memcpy(frame_buffer + frame_buffer_used, pkt_data(pkt), data_len);
        frame_buffer_used += data_len;
    }
    
    (*packet_count)++;
    return true;
}

/*
 * Send frame buffer with timing, tracking cumulative bytes.
 */
static size_t cumulative_bytes_sent = 0;

static int send_frame_buffer_timed(void)
{
    if (frame_buffer_used == 0)
        return 0;
    
    int ret = spi_send_bytes_timed(frame_buffer, frame_buffer_used, 
                                    SPI_READY_TIMEOUT_MS, &cumulative_bytes_sent);
    frame_buffer_used = 0;
    return ret;
}

static int send_frame_buffer(void)
{
    if (frame_buffer_used == 0)
        return 0;
    
    int ret = spi_send_bytes_timeout(frame_buffer, frame_buffer_used, SPI_READY_TIMEOUT_MS);
    frame_buffer_used = 0;
    return ret;
}

/*
 * Encode differential update into frame buffer (does NOT send).
 * Returns packet count, or -1 on error.
 */
static int encode_diff_to_buffer(const u8 *old_fb, const u8 *new_fb, bool new_frame)
{
    int packet_count = 0;
    struct encoder_state enc;
    
    encoder_init(&enc, 0, new_frame, pattern_bitrev, emit_packet_to_buffer, &packet_count);
    
    if (!encode_diff(&enc, old_fb, new_fb, 0, FB_SIZE)) {
        fprintf(stderr, "Diff encoding failed\n");
        return -1;
    }
    
    return packet_count;
}

/*
 * Encode full frame into frame buffer (does NOT send).
 * Returns packet count, or -1 on error.
 */
static int encode_full_to_buffer(const u8 *fb, bool new_frame)
{
    int packet_count = 0;
    struct encoder_state enc;
    
    encoder_init(&enc, 0, new_frame, pattern_bitrev, emit_packet_to_buffer, &packet_count);
    
    if (!encode_region(&enc, 0, fb, FB_SIZE)) {
        fprintf(stderr, "Full frame encoding failed\n");
        return -1;
    }
    
    return packet_count;
}

/*
 * Fill a region into the frame buffer (no send yet).
 * Used when we need to batch multiple operations before sending.
 */
static int fill_region_buffered(u16 addr, size_t len, u8 fill_byte, bool new_frame, int *packet_count)
{
    /* Allocate and fill buffer */
    u8 *buf = malloc(len);
    if (!buf) {
        fprintf(stderr, "Failed to allocate %zu bytes\n", len);
        return -1;
    }
    memset(buf, fill_byte, len);
    
    /* Set up encoder to emit to frame buffer */
    struct encoder_state enc;
    encoder_init(&enc, 0, new_frame, pattern_bitrev, emit_packet_to_buffer, packet_count);
    
    /* Encode into buffer (no send) */
    bool ok = encode_region(&enc, addr, buf, len);
    
    free(buf);
    
    if (!ok) {
        fprintf(stderr, "Encoding failed\n");
        return -1;
    }
    
    return 0;
}

/*
 * Run pattern test mode.
 * Supports comma-separated patterns: first is sent as full frame,
 * subsequent patterns are sent as diffs with 3 second delays.
 */
static int run_pattern_test(void)
{
    u8 *fb = malloc(FB_SIZE);
    u8 *old_fb = malloc(FB_SIZE);
    int total_packets = 0;
    int total_ready_waits = 0;
    struct timespec start, end;
    char *patterns_copy = NULL;
    char *pattern_list[32];  /* Max 32 patterns */
    int num_patterns = 0;
    
    if (!fb || !old_fb) {
        fprintf(stderr, "Failed to allocate framebuffer\n");
        free(fb);
        free(old_fb);
        return -1;
    }
    
    /* Allocate frame buffer for batched sending.
     * For animation, we batch multiple frames. Each diff frame is ~12KB worst case.
     * Allocate enough for several frames to allow efficient chunking.
     */
    frame_buffer_size = 256 * 1024;  /* 256KB - enough for many animation frames */
    frame_buffer = malloc(frame_buffer_size);
    if (!frame_buffer) {
        fprintf(stderr, "Failed to allocate frame buffer\n");
        free(fb);
        free(old_fb);
        return -1;
    }
    frame_buffer_used = 0;
    
    /* Parse comma-separated pattern list */
    patterns_copy = strdup(pattern_type);
    if (!patterns_copy) {
        fprintf(stderr, "Failed to allocate pattern string\n");
        free(fb);
        free(old_fb);
        free(frame_buffer);
        frame_buffer = NULL;
        return -1;
    }
    
    char *token = strtok(patterns_copy, ",");
    while (token && num_patterns < 32) {
        /* Skip leading whitespace */
        while (*token == ' ' || *token == '\t')
            token++;
        pattern_list[num_patterns++] = token;
        token = strtok(NULL, ",");
    }
    
    if (num_patterns == 0) {
        fprintf(stderr, "No patterns specified\n");
        free(patterns_copy);
        free(fb);
        free(old_fb);
        free(frame_buffer);
        frame_buffer = NULL;
        return -1;
    }
    
    printf("Pattern test: %d pattern(s), %d frame(s) each, bitrev=%d\n",
           num_patterns, pattern_frames, pattern_bitrev);
    printf("Patterns: ");
    for (int i = 0; i < num_patterns; i++) {
        printf("%s%s", pattern_list[i], (i < num_patterns - 1) ? ", " : "\n");
    }
    printf("Framebuffer: %dx%d, %d bytes\n",
           HW_WIDTH, HW_HEIGHT, FB_SIZE);
    printf("\n");
    
    /* Enable device */
    gpio_enable_set(1);
    /* no delay needed */
    
    /* Wait for ready using edge events */
    if (wait_ready_edge(1000) < 0) {
        fprintf(stderr, "Device not ready after enable\n");
        free(patterns_copy);
        free(fb);
        free(old_fb);
        free(frame_buffer);
        frame_buffer = NULL;
        return -1;
    }
    
    clock_gettime(CLOCK_MONOTONIC, &start);
    
    memset(old_fb, 0, FB_SIZE);
    
    /* Reset timing tracking */
    ready_sample_count = 0;
    cumulative_bytes_sent = 0;
    
    for (int p = 0; p < num_patterns; p++) {
        const char *current_pattern = pattern_list[p];
        
        /* Wait 3 seconds between patterns (except before first) */
        if (p > 0) {
            printf("Waiting 3 seconds...\n");
            sleep(3);
        }
        
        printf("Pattern '%s':\n", current_pattern);
        
        for (int frame = 0; frame < pattern_frames; frame++) {
            int packets;
            
            generate_pattern(fb, current_pattern, frame);
            
            if (p == 0 && frame == 0) {
                /* First pattern, first frame: encode full frame into buffer */
                frame_buffer_used = 0;
                packets = encode_full_to_buffer(fb, true);
                if (packets < 0) {
                    gpio_enable_set(0);
                    free(patterns_copy);
                    free(fb);
                    free(old_fb);
                    free(frame_buffer);
                    frame_buffer = NULL;
                    return -1;
                }
                total_packets += packets;
                
                /* Send the first frame immediately (non-animate patterns) */
                if (strcmp(current_pattern, "animate") != 0) {
                    int waits = send_frame_buffer_timed();
                    if (waits < 0) {
                        gpio_enable_set(0);
                        free(patterns_copy);
                        free(fb);
                        free(old_fb);
                        free(frame_buffer);
                        frame_buffer = NULL;
                        return -1;
                    }
                    total_ready_waits += waits;
                    printf("  Frame %d: %d packets\n", frame + 1, packets);
                }
            } else if (strcmp(current_pattern, "animate") == 0) {
                /* Animation: buffer diff (no send yet) */
                packets = encode_diff_to_buffer(old_fb, fb, true /* new_frame */);
                if (packets < 0) {
                    gpio_enable_set(0);
                    free(patterns_copy);
                    free(fb);
                    free(old_fb);
                    free(frame_buffer);
                    frame_buffer = NULL;
                    return -1;
                }
                total_packets += packets;
                
                /* Send buffer when it's getting full (leave room for next frame) */
                if (frame_buffer_used > frame_buffer_size - 20000) {
                    int waits = send_frame_buffer_timed();
                    if (waits < 0) {
                        gpio_enable_set(0);
                        free(patterns_copy);
                        free(fb);
                        free(old_fb);
                        free(frame_buffer);
                        frame_buffer = NULL;
                        return -1;
                    }
                    total_ready_waits += waits;
                }
            } else {
                /* Non-animate patterns: diff and send immediately */
                if (memcmp(old_fb, fb, FB_SIZE) == 0) {
                    printf("  Frame %d: identical, skipped\n", frame + 1);
                    memcpy(old_fb, fb, FB_SIZE);
                    continue;
                }
                frame_buffer_used = 0;
                packets = encode_diff_to_buffer(old_fb, fb, (frame == 0));
                if (packets < 0) {
                    gpio_enable_set(0);
                    free(patterns_copy);
                    free(fb);
                    free(old_fb);
                    free(frame_buffer);
                    frame_buffer = NULL;
                    return -1;
                }
                int waits = send_frame_buffer_timed();
                if (waits < 0) {
                    gpio_enable_set(0);
                    free(patterns_copy);
                    free(fb);
                    free(old_fb);
                    free(frame_buffer);
                    frame_buffer = NULL;
                    return -1;
                }
                total_ready_waits += waits;
                total_packets += packets;
                printf("  Frame %d: %d packets\n", frame + 1, packets);
            }
            
            memcpy(old_fb, fb, FB_SIZE);
        }
        
        /* Flush any remaining buffered data (for animate) */
        if (strcmp(current_pattern, "animate") == 0 && frame_buffer_used > 0) {
            int waits = send_frame_buffer_timed();
            if (waits < 0) {
                gpio_enable_set(0);
                free(patterns_copy);
                free(fb);
                free(old_fb);
                free(frame_buffer);
                frame_buffer = NULL;
                return -1;
            }
            total_ready_waits += waits;
        }
    }
    
    clock_gettime(CLOCK_MONOTONIC, &end);
    
    long elapsed_ms = (end.tv_sec - start.tv_sec) * 1000 +
                      (end.tv_nsec - start.tv_nsec) / 1000000;
    
    printf("\n");
    printf("Total: %d packets, %zu bytes sent\n", total_packets, cumulative_bytes_sent);
    printf("READY waits: %d, samples recorded: %d\n", total_ready_waits, ready_sample_count);
    
    if (ready_sample_count >= 2) {
        printf("\nFirst 10 READY samples:\n");
        for (int i = 0; i < ready_sample_count && i < 10; i++) {
            long ms = (ready_samples[i].timestamp.tv_sec - ready_samples[0].timestamp.tv_sec) * 1000 +
                      (ready_samples[i].timestamp.tv_nsec - ready_samples[0].timestamp.tv_nsec) / 1000000;
            printf("  [%d] t=%ld ms, bytes_sent=%zu\n", i, ms, ready_samples[i].bytes_sent);
        }
        if (ready_sample_count > 10) {
            printf("  ...\n");
            for (int i = ready_sample_count - 3; i < ready_sample_count; i++) {
                long ms = (ready_samples[i].timestamp.tv_sec - ready_samples[0].timestamp.tv_sec) * 1000 +
                          (ready_samples[i].timestamp.tv_nsec - ready_samples[0].timestamp.tv_nsec) / 1000000;
                printf("  [%d] t=%ld ms, bytes_sent=%zu\n", i, ms, ready_samples[i].bytes_sent);
            }
        }
        
        /*
         * Accurate timing: exclude FIFO fill at start and drain at end.
         * FIFO is ~16KB, so:
         * - Start: find first sample where bytes_sent >= 16KB (FIFO filled once)
         * - End: find last sample where bytes_sent <= (total - 16KB) (16KB still in FIFO)
         */
        const size_t FIFO_SIZE = 16384;
        int start_idx = -1, end_idx = -1;
        
        for (int i = 0; i < ready_sample_count; i++) {
            if (start_idx < 0 && ready_samples[i].bytes_sent >= FIFO_SIZE)
                start_idx = i;
            if (ready_samples[i].bytes_sent <= cumulative_bytes_sent - FIFO_SIZE)
                end_idx = i;
        }
        
        printf("\nTiming analysis (excluding FIFO fill/drain):\n");
        printf("  FIFO size: %zu bytes\n", FIFO_SIZE);
        
        if (start_idx >= 0 && end_idx > start_idx) {
            long start_ms = (ready_samples[start_idx].timestamp.tv_sec - ready_samples[0].timestamp.tv_sec) * 1000 +
                            (ready_samples[start_idx].timestamp.tv_nsec - ready_samples[0].timestamp.tv_nsec) / 1000000;
            long end_ms = (ready_samples[end_idx].timestamp.tv_sec - ready_samples[0].timestamp.tv_sec) * 1000 +
                          (ready_samples[end_idx].timestamp.tv_nsec - ready_samples[0].timestamp.tv_nsec) / 1000000;
            
            long measured_ms = end_ms - start_ms;
            size_t bytes_measured = ready_samples[end_idx].bytes_sent - ready_samples[start_idx].bytes_sent;
            
            printf("  Start sample[%d]: t=%ld ms, bytes=%zu\n", 
                   start_idx, start_ms, ready_samples[start_idx].bytes_sent);
            printf("  End sample[%d]: t=%ld ms, bytes=%zu\n", 
                   end_idx, end_ms, ready_samples[end_idx].bytes_sent);
            printf("  Measured window: %ld ms, %zu bytes\n", measured_ms, bytes_measured);
            
            double bytes_per_frame = (double)cumulative_bytes_sent / pattern_frames;
            double frames_measured = bytes_measured / bytes_per_frame;
            
            printf("  Bytes per frame (avg): %.1f\n", bytes_per_frame);
            printf("  Frames in measured window: %.1f\n", frames_measured);
            
            if (measured_ms > 0 && frames_measured > 1) {
                double fps = frames_measured * 1000.0 / measured_ms;
                double ms_per_frame = measured_ms / frames_measured;
                printf("\n  => Time per frame: %.2f ms (%.1f fps)\n", ms_per_frame, fps);
                printf("  => Expected: 20.0 ms per frame (50 fps)\n");
                printf("  => Ratio: %.2fx expected time\n", ms_per_frame / 20.0);
            }
        } else {
            printf("  Not enough samples for accurate timing\n");
            printf("  (need samples spanning at least 2x FIFO size)\n");
        }
    }
    
    printf("\nWall clock: %ld ms total\n", elapsed_ms);
    
    /* Keep display enabled for viewing */
    printf("\nPattern displayed. Press Enter to disable and exit...\n");
    getchar();
    
    gpio_enable_set(0);
    
    free(patterns_copy);
    free(fb);
    free(old_fb);
    free(frame_buffer);
    frame_buffer = NULL;
    
    return 0;
}

/* ===== Frame Synchronization Test ===== */

#define FRAME_TEST_REGION       4000  /* Must be < window/2 so both fill+clear fit */

/* iterations uses pattern_frames variable (set via --frames) */

/*
 * Run frame synchronization test.
 * Tests that new_frame bit controls when FPGA presents buffer to display.
 * Uses encode_region() to exercise the full encoder path.
 */
static int run_frame_sync_test(void)
{
    struct timespec start, end;
    int iterations = pattern_frames > 0 ? pattern_frames : 25;  /* Default 25 if not specified */
    
    /* Calculate PE-expanded size: RLE fill expands to full region */
    int pe_bytes_per_op = FRAME_TEST_REGION;  /* Each RLE fill/clear expands to this */
    int pe_bytes_per_pair = pe_bytes_per_op * 2;  /* fill + clear */
    double pe_time_per_pair_ms = pe_bytes_per_pair * 8.0 / 8000000.0 * 1000.0;
    
    printf("Frame Synchronization Test\n");
    printf("==========================\n\n");
    
    printf("This test verifies the FPGA's new_frame synchronization logic.\n");
    printf("It rapidly fills and clears screen regions. If new_frame works\n");
    printf("correctly, the clear operation completes before vblank and the\n");
    printf("region appears solid without flickering.\n\n");
    
    printf("Test region: %d bytes\n", FRAME_TEST_REGION);
    printf("PE transmission per fill+clear pair: %d bytes = %.1f ms at 8MHz\n",
           pe_bytes_per_pair, pe_time_per_pair_ms);
    printf("Active window: ~15ms (must fit: %.1f ms < 15ms? %s)\n",
           pe_time_per_pair_ms, pe_time_per_pair_ms < 15.0 ? "YES" : "NO!");
    printf("Iterations: %d (use --frames N to change)\n\n", iterations);
    
    /* Allocate frame buffer for batched sending. */
    frame_buffer_size = iterations * 2 * 8 + 1024;  /* 2 packets per iter, ~7 bytes each */
    frame_buffer = malloc(frame_buffer_size);
    if (!frame_buffer) {
        fprintf(stderr, "Failed to allocate frame buffer\n");
        return -1;
    }
    frame_buffer_used = 0;
    
    /* Enable device */
    gpio_enable_set(1);
    /* no delay needed */
    
    /* Initial ready check */
    if (wait_ready_edge(1000) < 0) {
        fprintf(stderr, "Device not ready after enable\n");
        free(frame_buffer);
        frame_buffer = NULL;
        return -1;
    }
    
    /*
     * Phase A: First region (addr=0)
     * Fill white (new_frame=1), then clear black (new_frame=0)
     * Both operations buffered and sent together so they arrive in FIFO
     * before the fill even starts transmitting to PE.
     */
    printf("Phase A: First region of screen (top)\n");
    printf("  Expected: SOLID WHITE (no flickering to black)\n");
    printf("  If you see flickering, new_frame logic is broken.\n");
    printf("  Press Enter to begin Phase A...");
    fflush(stdout);
    getchar();
    
    printf("  Running %d iterations...\n", iterations);
    
    clock_gettime(CLOCK_MONOTONIC, &start);
    
    /* Buffer ALL iterations first, then send in one burst */
    int packet_count = 0;
    frame_buffer_used = 0;
    
    for (int i = 0; i < iterations; i++) {
        /* Buffer fill white (new_frame=1 to sync to vblank) */
        if (fill_region_buffered(0, FRAME_TEST_REGION, 0xFF, true, &packet_count) < 0) {
            free(frame_buffer);
            frame_buffer = NULL;
            return -1;
        }
        
        /* Buffer clear black (new_frame=0, same frame as fill) */
        if (fill_region_buffered(0, FRAME_TEST_REGION, 0x00, false, &packet_count) < 0) {
            free(frame_buffer);
            frame_buffer = NULL;
            return -1;
        }
    }
    
    /* Send all iterations in one burst */
    printf("  Sending %zu bytes in chunks...\n", frame_buffer_used);
    int waits_a = send_frame_buffer();
    if (waits_a < 0) {
        free(frame_buffer);
        frame_buffer = NULL;
        return -1;
    }
    
    clock_gettime(CLOCK_MONOTONIC, &end);
    {
        long elapsed_ms = (end.tv_sec - start.tv_sec) * 1000 +
                          (end.tv_nsec - start.tv_nsec) / 1000000;
        printf("  Phase A: SPI complete in %ld ms, %d READY waits\n", elapsed_ms, waits_a);
        printf("  (FPGA still displaying - will take ~%d ms for %d frames at 50Hz)\n\n", 
               iterations * 20, iterations);
    }
    
    /*
     * Phase B: Second region (addr=FRAME_TEST_REGION)
     * Fill white (new_frame=1), then clear black (new_frame=0)
     * Should appear solid white if new_frame works correctly.
     */
    printf("Phase B: Second region of screen (below first region)\n");
    printf("  Expected: SOLID WHITE (no flickering to black)\n");
    printf("  If you see flickering, new_frame logic is broken.\n");
    printf("  Press Enter to begin Phase B...");
    fflush(stdout);
    getchar();
    
    printf("  Running %d iterations...\n", iterations);
    
    clock_gettime(CLOCK_MONOTONIC, &start);
    
    /* Buffer ALL iterations first, then send in one burst */
    packet_count = 0;
    frame_buffer_used = 0;
    
    for (int i = 0; i < iterations; i++) {
        /* Buffer fill white (new_frame=1 to sync to vblank) */
        if (fill_region_buffered(FRAME_TEST_REGION, FRAME_TEST_REGION, 0xFF, true, &packet_count) < 0) {
            free(frame_buffer);
            frame_buffer = NULL;
            return -1;
        }
        
        /* Buffer clear black (new_frame=0, same frame as fill) */
        if (fill_region_buffered(FRAME_TEST_REGION, FRAME_TEST_REGION, 0x00, false, &packet_count) < 0) {
            free(frame_buffer);
            frame_buffer = NULL;
            return -1;
        }
    }
    
    /* Send all iterations in one burst */
    printf("  Sending %zu bytes in chunks...\n", frame_buffer_used);
    int waits_b = send_frame_buffer();
    if (waits_b < 0) {
        free(frame_buffer);
        frame_buffer = NULL;
        return -1;
    }
    
    clock_gettime(CLOCK_MONOTONIC, &end);
    {
        long elapsed_ms = (end.tv_sec - start.tv_sec) * 1000 +
                          (end.tv_nsec - start.tv_nsec) / 1000000;
        printf("  Phase B: SPI complete in %ld ms, %d READY waits\n", elapsed_ms, waits_b);
        printf("  (FPGA still displaying - will take ~%d ms for %d frames at 50Hz)\n\n", 
               iterations * 20, iterations);
    }
    
    /* Clear screen and ask for result */
    printf("Clearing screen...\n");
    if (fill_region(0, FB_SIZE, 0x00, true) < 0)
        return -1;
    
    printf("\nTest complete.\n");
    printf("Did both phases display SOLID WHITE without flickering? (y/n): ");
    fflush(stdout);
    
    int c = getchar();
    /* Consume newline */
    while (getchar() != '\n' && !feof(stdin));
    
    if (c == 'y' || c == 'Y') {
        printf("\nPASS: Frame synchronization is working correctly.\n");
        free(frame_buffer);
        frame_buffer = NULL;
        return 0;
    } else {
        printf("\nFAIL: Frame synchronization test failed.\n");
        printf("The FPGA may not be respecting the new_frame bit.\n");
        free(frame_buffer);
        frame_buffer = NULL;
        return -1;
    }
}

/*
 * Stress test: Generate pseudorandom packets to exercise FPGA packet parsing.
 * 
 * Modes:
 *   STRESS_OVERLAP    - Writes can overlap (same addr written multiple times)
 *   STRESS_NONOVERLAP - Writes never overlap (each addr written once max)
 *   STRESS_LINES      - Alternating lines black/white (tests regular patterns)
 *
 * Uses same SPI chunking and READY checking as animate test.
 */
#define STRESS_OVERLAP     0
#define STRESS_NONOVERLAP  1
#define STRESS_LINES       2

static int stress_test_max_bytes = 10000;
static bool stress_test_use_rle = true;
static int stress_test_mode = STRESS_OVERLAP;
static int stress_test_iterations = 100;
static bool stress_test_clear_each = false;  /* Clear before each frame like driver force_full */

/*
 * Simple PRNG for reproducible tests.
 */
static uint32_t stress_seed = 12345;

static uint32_t stress_rand(void)
{
    stress_seed = stress_seed * 1103515245 + 12345;
    return (stress_seed >> 16) & 0x7FFF;
}

static void stress_srand(uint32_t seed)
{
    stress_seed = seed;
}

/*
 * Emit callback that writes to frame_buffer (same as emit_packet_to_buffer).
 */
static bool stress_emit_packet(const struct packet_header *pkt, void *ctx)
{
    int *packet_count = (int *)ctx;
    const u8 *wire = pkt_wire(pkt);
    size_t hdr_size = pkt_hdr_size(pkt);
    size_t data_len = pkt_data_len(pkt);
    size_t total = hdr_size + data_len;
    
    if (frame_buffer_used + total > frame_buffer_size) {
        fprintf(stderr, "Frame buffer overflow!\n");
        return false;
    }
    
    memcpy(frame_buffer + frame_buffer_used, wire, hdr_size);
    frame_buffer_used += hdr_size;
    
    if (data_len > 0 && pkt_data(pkt)) {
        memcpy(frame_buffer + frame_buffer_used, pkt_data(pkt), data_len);
        frame_buffer_used += data_len;
    }
    
    (*packet_count)++;
    return true;
}

/*
 * Generate and send a stress test frame with pseudorandom packets.
 */
static int stress_generate_frame(int iteration, bool new_frame)
{
    int packet_count = 0;
    struct encoder_state enc;
    int bytes_remaining = stress_test_max_bytes;
    u8 *used_map = NULL;  /* For non-overlapping mode */
    
    (void)iteration;  /* Used for future per-iteration variation */
    
    /* For non-overlapping mode, track which addresses are used */
    if (stress_test_mode == STRESS_NONOVERLAP) {
        used_map = calloc(FB_SIZE, 1);
        if (!used_map) {
            fprintf(stderr, "Failed to allocate used_map\n");
            return -1;
        }
    }
    
    /* NOTE: caller is responsible for resetting frame_buffer_used */
    
    /* Initialize encoder */
    encoder_init(&enc, 0, new_frame, pattern_bitrev, stress_emit_packet, &packet_count);
    
    if (stress_test_mode == STRESS_LINES) {
        /*
         * Alternating lines mode: write each line as black or white.
         * Line = 35 bytes (280 pixels / 8 bits per pixel).
         */
        const int line_bytes = 35;
        const int num_lines = 720;
        u8 line_data[35];
        
        for (int line = 0; line < num_lines && bytes_remaining > 0; line++) {
            u8 fill = (line & 1) ? 0xFF : 0x00;  /* Odd lines white, even black */
            memset(line_data, fill, line_bytes);
            
            u16 addr = line * line_bytes;
            size_t len = line_bytes;
            
            /* Always use encode_region - same as driver */
            if (!encode_region(&enc, addr, line_data, len)) {
                fprintf(stderr, "encode_region failed at line %d\n", line);
                free(used_map);
                return -1;
            }
            
            bytes_remaining -= len;
        }
    } else {
        /*
         * Random packets mode (overlap or non-overlap).
         */
        while (bytes_remaining > 0) {
            /* Random address */
            u16 addr = stress_rand() % FB_SIZE;
            
            /* Random length (1 to 256, but not past FB end) */
            size_t max_len = FB_SIZE - addr;
            if (max_len > 256) max_len = 256;
            if (max_len > (size_t)bytes_remaining) max_len = bytes_remaining;
            if (max_len == 0) break;
            
            size_t len = (stress_rand() % max_len) + 1;
            
            /* For non-overlapping mode, find unused region */
            if (stress_test_mode == STRESS_NONOVERLAP) {
                /* Find first unused addr >= our random addr */
                while (addr < FB_SIZE && used_map[addr])
                    addr++;
                if (addr >= FB_SIZE) break;
                
                /* Shrink len to fit in unused region */
                max_len = 0;
                for (size_t i = addr; i < FB_SIZE && !used_map[i] && max_len < len; i++)
                    max_len++;
                if (max_len == 0) break;
                len = max_len;
                
                /* Mark as used */
                for (size_t i = 0; i < len; i++)
                    used_map[addr + i] = 1;
            }
            
            /* Random data or uniform fill */
            u8 *data = malloc(len);
            if (!data) {
                fprintf(stderr, "Failed to allocate %zu bytes\n", len);
                free(used_map);
                return -1;
            }
            
            /* Use random fill pattern */
            u8 fill = stress_rand() & 0xFF;
            if (stress_test_use_rle && (stress_rand() % 3 == 0)) {
                /* Sometimes use uniform fill to trigger RLE */
                memset(data, fill, len);
            } else {
                /* Pseudorandom data (less likely to RLE) */
                for (size_t i = 0; i < len; i++)
                    data[i] = stress_rand() & 0xFF;
            }
            
            /* Always use encode_region - same as driver */
            if (!encode_region(&enc, addr, data, len)) {
                fprintf(stderr, "encode_region failed at addr %u len %zu\n", addr, len);
                free(data);
                free(used_map);
                return -1;
            }
            
            free(data);
            bytes_remaining -= len;
        }
    }
    
    free(used_map);
    
    if (verbose) {
        printf("  Iteration %d: %d packets, %zu bytes in buffer\n",
               iteration, packet_count, frame_buffer_used);
    }
    
    return packet_count;
}

/*
 * Load a framebuffer dump file (25200 bytes, 280x720 mono).
 * Returns allocated buffer or NULL on error.
 */
static u8 *load_fbdump(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Failed to open %s: %s\n", path, strerror(errno));
        return NULL;
    }
    
    /* Check file size */
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    if (size != FB_SIZE) {
        fprintf(stderr, "Warning: %s is %ld bytes, expected %d\n", path, size, FB_SIZE);
    }
    
    u8 *buf = malloc(FB_SIZE);
    if (!buf) {
        fprintf(stderr, "Failed to allocate buffer\n");
        fclose(f);
        return NULL;
    }
    
    /* Initialize to zero in case file is short */
    memset(buf, 0, FB_SIZE);
    
    size_t read_size = fread(buf, 1, FB_SIZE, f);
    fclose(f);
    
    if (read_size < (size_t)FB_SIZE) {
        fprintf(stderr, "Warning: only read %zu bytes from %s\n", read_size, path);
    }
    
    /* Compute CRC for verification */
    uint32_t crc = 0;
    for (int i = 0; i < FB_SIZE; i++) {
        crc = crc * 31 + buf[i];
    }
    printf("Loaded %s: %zu bytes, checksum %08x\n", path, read_size, crc);
    
    return buf;
}

/*
 * Run fbdump replay test.
 * If two files given: display first, then diff to second.
 * If one file given: clear to black, then display file.
 */
static int run_fbdump_test(const char *file1, const char *file2)
{
    u8 *fb1 = NULL;
    u8 *fb2 = NULL;
    u8 *black = NULL;
    int ret = -1;
    int pkt_count;
    struct encoder_state enc;
    
    printf("P4 Framebuffer Dump Replay\n");
    printf("==========================\n\n");
    
    /* Load file(s) */
    fb1 = load_fbdump(file1);
    if (!fb1)
        goto out;
    
    if (file2) {
        fb2 = load_fbdump(file2);
        if (!fb2)
            goto out;
    }
    
    /* Allocate frame buffer for packets */
    frame_buffer_size = 256 * 1024;
    frame_buffer = malloc(frame_buffer_size);
    if (!frame_buffer) {
        fprintf(stderr, "Failed to allocate frame buffer\n");
        goto out;
    }
    
    /* Allocate black reference */
    black = calloc(FB_SIZE, 1);
    if (!black) {
        fprintf(stderr, "Failed to allocate black buffer\n");
        goto out;
    }
    
    /* Enable device */
    gpio_enable_set(1);
    
    /* Wait for ready */
    if (wait_ready_edge(1000) < 0) {
        fprintf(stderr, "Device not ready after enable\n");
        goto out;
    }
    
    if (file2) {
        /*
         * Two-file mode: display fb1, then diff to fb2.
         * This simulates the driver's incremental update path.
         */
        printf("\nPhase 1: Displaying %s (full frame)...\n", file1);
        
        /* First, clear screen */
        frame_buffer_used = 0;
        pkt_count = 0;
        encoder_init(&enc, 0, true /* new_frame */, pattern_bitrev, stress_emit_packet, &pkt_count);
        encode_region(&enc, 0, black, FB_SIZE);
        
        printf("  Clear: %d packets, %zu bytes\n", pkt_count, frame_buffer_used);
        
        int waits = spi_send_bytes_timeout(frame_buffer, frame_buffer_used, SPI_READY_TIMEOUT_MS);
        if (waits < 0) {
            fprintf(stderr, "Clear send failed\n");
            goto out;
        }
        printf("  Sent with %d READY waits\n", waits);
        
        /* Now send fb1 as diff from black */
        frame_buffer_used = 0;
        pkt_count = 0;
        encoder_init(&enc, 0, false /* new_frame */, pattern_bitrev, stress_emit_packet, &pkt_count);
        if (!encode_diff(&enc, black, fb1, 0, FB_SIZE)) {
            fprintf(stderr, "Encode diff (black->fb1) failed\n");
            goto out;
        }
        
        printf("  Diff black->fb1: %d packets, %zu bytes\n", pkt_count, frame_buffer_used);
        
        waits = spi_send_bytes_timeout(frame_buffer, frame_buffer_used, SPI_READY_TIMEOUT_MS);
        if (waits < 0) {
            fprintf(stderr, "Diff send failed\n");
            goto out;
        }
        printf("  Sent with %d READY waits\n", waits);
        
        /* Check COLD */
        if (gpio_cold_get() == 1) {
            fprintf(stderr, "\nFAIL: COLD high after fb1 - FPGA fault!\n");
            goto out;
        }
        
        printf("\nPhase 2: Diffing to %s...\n", file2);
        printf("  (Press Enter to continue, or Ctrl-C to abort)\n");
        getchar();
        
        /* Now diff from fb1 to fb2 */
        frame_buffer_used = 0;
        pkt_count = 0;
        encoder_init(&enc, 0, true /* new_frame */, pattern_bitrev, stress_emit_packet, &pkt_count);
        if (!encode_diff(&enc, fb1, fb2, 0, FB_SIZE)) {
            fprintf(stderr, "Encode diff (fb1->fb2) failed\n");
            goto out;
        }
        
        printf("  Diff fb1->fb2: %d packets, %zu bytes\n", pkt_count, frame_buffer_used);
        
        waits = spi_send_bytes_timeout(frame_buffer, frame_buffer_used, SPI_READY_TIMEOUT_MS);
        if (waits < 0) {
            fprintf(stderr, "Diff send failed\n");
            goto out;
        }
        printf("  Sent with %d READY waits\n", waits);
        
    } else {
        /*
         * Single-file mode: clear to black, then display fb1.
         * This simulates the driver's force_full path.
         */
        printf("\nDisplaying %s (with clear)...\n", file1);
        
        /* Clear screen first */
        frame_buffer_used = 0;
        pkt_count = 0;
        encoder_init(&enc, 0, true /* new_frame */, pattern_bitrev, stress_emit_packet, &pkt_count);
        encode_region(&enc, 0, black, FB_SIZE);
        
        printf("  Clear: %d packets, %zu bytes\n", pkt_count, frame_buffer_used);
        
        int waits = spi_send_bytes_timeout(frame_buffer, frame_buffer_used, SPI_READY_TIMEOUT_MS);
        if (waits < 0) {
            fprintf(stderr, "Clear send failed\n");
            goto out;
        }
        printf("  Sent with %d READY waits\n", waits);
        
        /* Now send fb1 as diff from black (like driver does after force_full) */
        frame_buffer_used = 0;
        pkt_count = 0;
        encoder_init(&enc, 0, false /* new_frame */, pattern_bitrev, stress_emit_packet, &pkt_count);
        if (!encode_diff(&enc, black, fb1, 0, FB_SIZE)) {
            fprintf(stderr, "Encode diff failed\n");
            goto out;
        }
        
        printf("  Diff: %d packets, %zu bytes\n", pkt_count, frame_buffer_used);
        
        waits = spi_send_bytes_timeout(frame_buffer, frame_buffer_used, SPI_READY_TIMEOUT_MS);
        if (waits < 0) {
            fprintf(stderr, "Diff send failed\n");
            goto out;
        }
        printf("  Sent with %d READY waits\n", waits);
    }
    
    /* Final COLD check */
    if (gpio_cold_get() == 1) {
        fprintf(stderr, "\nFAIL: COLD high at end - FPGA fault!\n");
        goto out;
    }
    
    printf("\nSuccess! Display should show the framebuffer content.\n");
    printf("COLD is low (no fault detected).\n");
    ret = 0;
    
out:
    free(fb1);
    free(fb2);
    free(black);
    free(frame_buffer);
    frame_buffer = NULL;
    return ret;
}

/*
 * Run stress test - send many frames with pseudorandom packets.
 */
static int run_stress_test(void)
{
    int total_packets = 0;
    int total_ready_waits = 0;
    struct timespec start, end;
    const char *mode_name;
    
    switch (stress_test_mode) {
    case STRESS_OVERLAP:
        mode_name = "overlap (random addresses, may overlap)";
        break;
    case STRESS_NONOVERLAP:
        mode_name = "non-overlap (each address written once per frame)";
        break;
    case STRESS_LINES:
        mode_name = "alternating lines (black/white stripes)";
        break;
    default:
        mode_name = "unknown";
        break;
    }
    
    printf("Stress Test\n");
    printf("===========\n\n");
    printf("Mode:       %s\n", mode_name);
    printf("RLE:        %s\n", stress_test_use_rle ? "enabled" : "disabled");
    printf("Clear each: %s\n", stress_test_clear_each ? "yes (like driver force_full)" : "no");
    printf("Max bytes:  %d per frame\n", stress_test_max_bytes);
    printf("Iterations: %d\n", stress_test_iterations);
    printf("Seed:       %u\n", stress_seed);
    printf("\n");
    
    /* Allocate frame buffer */
    frame_buffer_size = 256 * 1024;  /* 256KB should be plenty */
    frame_buffer = malloc(frame_buffer_size);
    if (!frame_buffer) {
        fprintf(stderr, "Failed to allocate frame buffer\n");
        return -1;
    }
    
    /* Enable device */
    gpio_enable_set(1);
    
    /* Wait for ready */
    if (wait_ready_edge(1000) < 0) {
        fprintf(stderr, "Device not ready after enable\n");
        free(frame_buffer);
        frame_buffer = NULL;
        return -1;
    }
    
    /* First, clear to black with new_frame=1 */
    printf("Clearing screen...\n");
    frame_buffer_used = 0;
    {
        int pkt_count = 0;
        struct encoder_state enc;
        u8 *black = calloc(FB_SIZE, 1);
        if (!black) {
            fprintf(stderr, "Failed to allocate clear buffer\n");
            free(frame_buffer);
            frame_buffer = NULL;
            return -1;
        }
        encoder_init(&enc, 0, true, pattern_bitrev, stress_emit_packet, &pkt_count);
        encode_region(&enc, 0, black, FB_SIZE);
        free(black);
        
        int waits = spi_send_bytes_timeout(frame_buffer, frame_buffer_used, SPI_READY_TIMEOUT_MS);
        if (waits < 0) {
            fprintf(stderr, "Clear failed!\n");
            free(frame_buffer);
            frame_buffer = NULL;
            return -1;
        }
        total_ready_waits += waits;
        printf("  Clear: %d packets, %zu bytes, %d READY waits\n",
               pkt_count, frame_buffer_used, waits);
    }
    
    /* Reset PRNG for reproducible test */
    stress_srand(stress_seed);
    
    printf("\nRunning stress test...\n");
    clock_gettime(CLOCK_MONOTONIC, &start);
    
    for (int i = 0; i < stress_test_iterations; i++) {
        frame_buffer_used = 0;
        int frame_packets = 0;
        
        if (stress_test_clear_each) {
            /*
             * Clear-each-frame mode: matches driver's force_full behavior.
             * Clear gets new_frame=1, subsequent diff gets new_frame=0.
             */
            int pkt_count = 0;
            struct encoder_state enc;
            u8 *black = calloc(FB_SIZE, 1);
            if (!black) {
                fprintf(stderr, "Failed to allocate clear buffer\n");
                free(frame_buffer);
                frame_buffer = NULL;
                return -1;
            }
            encoder_init(&enc, 0, true /* new_frame */, pattern_bitrev, stress_emit_packet, &pkt_count);
            encode_region(&enc, 0, black, FB_SIZE);
            free(black);
            frame_packets += pkt_count;
            
            /* Generate random packets with new_frame=0 */
            int packets = stress_generate_frame(i, false /* new_frame */);
            if (packets < 0) {
                fprintf(stderr, "Frame generation failed at iteration %d\n", i);
                free(frame_buffer);
                frame_buffer = NULL;
                return -1;
            }
            frame_packets += packets;
        } else {
            /* Normal mode: first packet of frame gets new_frame=1 */
            int packets = stress_generate_frame(i, true /* new_frame */);
            if (packets < 0) {
                fprintf(stderr, "Frame generation failed at iteration %d\n", i);
                free(frame_buffer);
                frame_buffer = NULL;
                return -1;
            }
            frame_packets = packets;
        }
        
        total_packets += frame_packets;
        
        /* Send using same path as animate test */
        int waits = spi_send_bytes_timeout(frame_buffer, frame_buffer_used, SPI_READY_TIMEOUT_MS);
        if (waits < 0) {
            fprintf(stderr, "Send failed at iteration %d\n", i);
            free(frame_buffer);
            frame_buffer = NULL;
            return -1;
        }
        total_ready_waits += waits;
        
        /* Progress indicator */
        if ((i + 1) % 10 == 0 || i == stress_test_iterations - 1) {
            printf("  %d/%d iterations, %d packets, %d READY waits\r",
                   i + 1, stress_test_iterations, total_packets, total_ready_waits);
            fflush(stdout);
        }
    }
    
    clock_gettime(CLOCK_MONOTONIC, &end);
    double elapsed = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
    
    printf("\n\nStress test completed successfully!\n");
    printf("  Iterations:   %d\n", stress_test_iterations);
    printf("  Total packets: %d\n", total_packets);
    printf("  READY waits:   %d\n", total_ready_waits);
    printf("  Elapsed:       %.2f seconds\n", elapsed);
    printf("  Rate:          %.1f frames/sec\n", stress_test_iterations / elapsed);
    
    free(frame_buffer);
    frame_buffer = NULL;
    return 0;
}

static void print_usage(const char *prog)
{
    printf("Usage: %s [options]\n", prog);
    printf("\n");
    printf("Calibration mode (default):\n");
    printf("  --config FILE   Use specified config file (default: p4_pins.conf)\n");
    printf("  --verbose       Show detailed test output\n");
    printf("  --vblank        Test VSYNC signal and measure frequency\n");
    printf("  --test-frames   Test new_frame synchronization logic\n");
    printf("\n");
    printf("SPI frequency test mode:\n");
    printf("  --spi-freq      Discover and test all SPI frequencies (bypasses other tests)\n");
    printf("  --test-mb N     Test data size in MB (default: %d)\n", DEFAULT_TEST_MB);
    printf("  --chunk-kb N    Chunk size in KB (default: %d, range: 1-16)\n", DEFAULT_CHUNK_KB);
    printf("  --no-ready-check  Skip READY GPIO reads between chunks (for timing comparison)\n");
    printf("  --no-cs-toggle  Keep CS asserted between chunks (batch transfers)\n");
    printf("  --manual-cs     Control CS via GPIO instead of SPI controller (requires GPIO_CS in config)\n");
    printf("  --straight-loopback  Expect MISO=MOSI (no 1-byte delay), for direct loopback testing\n");
    printf("\n");
    printf("Pattern test mode:\n");
    printf("  --pattern TYPE  Send test pattern (bypasses calibration)\n");
    printf("  --frames N      Number of frames to send (default: 1)\n");
    printf("  --stripes N     Number of stripes for hstripes pattern (1-90, default: 90)\n");
    printf("  --bitrev        Enable bit reversal in packets\n");
    printf("\n");
    printf("Pattern types:\n");
    printf("  white      - All white\n");
    printf("  black      - All black\n");
    printf("  checker    - 8x8 checkerboard\n");
    printf("  vstripes   - Vertical stripes\n");
    printf("  hstripes   - Horizontal stripes\n");
    printf("  gradient   - Vertical gradient (dithered)\n");
    printf("  text       - Sparse text-like blocks (tests diff encoding)\n");
    printf("  animate    - Moving vertical bar (use with --frames N)\n");
    printf("\n");
    printf("Display control commands:\n");
    printf("  --fill-white-rle   Fill screen with 0xFF using RLE compression\n");
    printf("  --fill-white-raw   Fill screen with 0xFF without RLE (raw data)\n");
    printf("  --fill-black-rle   Fill screen with 0x00 using RLE compression\n");
    printf("  --fill-black-raw   Fill screen with 0x00 without RLE (raw data)\n");
    printf("  --power-on         Send power-on flags (exit standby)\n");
    printf("  --power-off        Send power-off flags (enter standby)\n");
    printf("\n");
    printf("Debug options:\n");
    printf("  --hexdump          Hex dump all bytes sent via SPI\n");
    printf("\n");
    printf("Stress test mode:\n");
    printf("  --stress           Run stress test with pseudorandom packets\n");
    printf("  --stress-overlap   Random addresses, may overlap (default)\n");
    printf("                     Display: random noise, changes each frame\n");
    printf("  --stress-nonoverlap  Each address written at most once per frame\n");
    printf("                     Display: random noise, changes each frame\n");
    printf("  --stress-lines     Alternating black/white lines\n");
    printf("                     Display: horizontal stripes (stable)\n");
    printf("  --stress-bytes N   Max bytes per frame (default: 10000)\n");
    printf("  --stress-iter N    Number of iterations (default: 100)\n");
    printf("  --stress-seed N    PRNG seed for reproducibility (default: 12345)\n");
    printf("  --stress-clear     Clear before each frame (like driver force_full)\n");
    printf("                     Display: flashes black then pattern each frame\n");
    printf("  --no-rle           Generate random data (less RLE compression)\n");
    printf("\n");
    printf("Framebuffer dump replay mode:\n");
    printf("  --fbdump FILE      Display captured framebuffer (25200 bytes, 280x720 mono)\n");
    printf("  --fbdump F1 F2     Display F1, then diff from F1 to F2\n");
    printf("                     Use with captures from p4_capture_fb.py\n");
    printf("\n");
    printf("Default mode tests basic SPI connectivity and GPIO handshaking.\n");
    printf("Use --spi-freq to find the maximum reliable SPI speed.\n");
    printf("The drm_p4 module must NOT be loaded.\n");
    printf("\n");
    printf("Required GPIOs: ENABLE, READY, NRESET, COLD\n");
    printf("Optional GPIO:  VSYNC (for --vblank test)\n");
}

int main(int argc, char **argv)
{
    const char *config_file = "p4_pins.conf";
    const char *fbdump_file1 = NULL;
    const char *fbdump_file2 = NULL;
    uint32_t max_speed;
    int ret = 1;

    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            config_file = argv[++i];
        } else if (strcmp(argv[i], "--verbose") == 0) {
            verbose = true;
        } else if (strcmp(argv[i], "--vblank") == 0) {
            test_vblank = true;
        } else if (strcmp(argv[i], "--test-frames") == 0) {
            test_frame_sync = true;
        } else if (strcmp(argv[i], "--spi-freq") == 0) {
            test_spi_freq = true;
        } else if (strcmp(argv[i], "--fbdump") == 0 && i + 1 < argc) {
            fbdump_file1 = argv[++i];
            /* Check if there's a second file (not starting with --) */
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                fbdump_file2 = argv[++i];
            }
        } else if (strcmp(argv[i], "--test-mb") == 0 && i + 1 < argc) {
            int mb = atoi(argv[++i]);
            if (mb < 1) mb = 1;
            if (mb > 1024) mb = 1024;  /* Cap at 1GB */
            freq_test_bytes = (size_t)mb * 1024 * 1024;
        } else if (strcmp(argv[i], "--chunk-kb") == 0 && i + 1 < argc) {
            int kb = atoi(argv[++i]);
            if (kb < 1) kb = 1;
            if (kb > 16) kb = 16;  /* Cap at 16KB (FPGA FIFO size) */
            freq_test_chunk = (size_t)kb * 1024;
        } else if (strcmp(argv[i], "--no-ready-check") == 0) {
            check_ready = false;
        } else if (strcmp(argv[i], "--no-cs-toggle") == 0) {
            no_cs_toggle = true;
        } else if (strcmp(argv[i], "--manual-cs") == 0) {
            manual_cs = true;
        } else if (strcmp(argv[i], "--straight-loopback") == 0) {
            straight_loopback = true;
        } else if (strcmp(argv[i], "--fill-white-rle") == 0) {
            cmd_fill_white_rle = true;
        } else if (strcmp(argv[i], "--fill-white-raw") == 0) {
            cmd_fill_white_raw = true;
        } else if (strcmp(argv[i], "--fill-black-rle") == 0) {
            cmd_fill_black_rle = true;
        } else if (strcmp(argv[i], "--fill-black-raw") == 0) {
            cmd_fill_black_raw = true;
        } else if (strcmp(argv[i], "--power-on") == 0) {
            cmd_power_on = true;
        } else if (strcmp(argv[i], "--power-off") == 0) {
            cmd_power_off = true;
        } else if (strcmp(argv[i], "--hexdump") == 0) {
            hexdump_spi = true;
        } else if (strcmp(argv[i], "--pattern") == 0 && i + 1 < argc) {
            pattern_type = argv[++i];
        } else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            pattern_frames = atoi(argv[++i]);
            if (pattern_frames < 1) pattern_frames = 1;
        } else if (strcmp(argv[i], "--stripes") == 0 && i + 1 < argc) {
            stripe_count = atoi(argv[++i]);
            if (stripe_count < 1) stripe_count = 1;
            if (stripe_count > 90) stripe_count = 90;
        } else if (strcmp(argv[i], "--bitrev") == 0) {
            pattern_bitrev = true;
        } else if (strcmp(argv[i], "--stress") == 0) {
            test_stress = true;
        } else if (strcmp(argv[i], "--stress-overlap") == 0) {
            test_stress = true;
            stress_test_mode = STRESS_OVERLAP;
        } else if (strcmp(argv[i], "--stress-nonoverlap") == 0) {
            test_stress = true;
            stress_test_mode = STRESS_NONOVERLAP;
        } else if (strcmp(argv[i], "--stress-lines") == 0) {
            test_stress = true;
            stress_test_mode = STRESS_LINES;
        } else if (strcmp(argv[i], "--stress-bytes") == 0 && i + 1 < argc) {
            stress_test_max_bytes = atoi(argv[++i]);
            if (stress_test_max_bytes < 1) stress_test_max_bytes = 1;
            if (stress_test_max_bytes > FB_SIZE) stress_test_max_bytes = FB_SIZE;
        } else if (strcmp(argv[i], "--stress-iter") == 0 && i + 1 < argc) {
            stress_test_iterations = atoi(argv[++i]);
            if (stress_test_iterations < 1) stress_test_iterations = 1;
        } else if (strcmp(argv[i], "--stress-seed") == 0 && i + 1 < argc) {
            stress_seed = (uint32_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--stress-clear") == 0) {
            stress_test_clear_each = true;
        } else if (strcmp(argv[i], "--no-rle") == 0) {
            stress_test_use_rle = false;
        } else if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    if (pattern_type) {
        printf("P4 Display Pattern Test\n");
        printf("=======================\n\n");
    } else if (test_frame_sync) {
        printf("P4 Display Frame Sync Test\n");
        printf("==========================\n\n");
    } else if (test_spi_freq) {
        printf("P4 Display SPI Frequency Test\n");
        printf("==============================\n\n");
    } else if (test_vblank) {
        printf("P4 Display VSYNC Test\n");
        printf("=====================\n\n");
    } else if (test_stress) {
        printf("P4 Display Stress Test\n");
        printf("======================\n\n");
    } else if (fbdump_file1) {
        /* Banner printed by run_fbdump_test */
    } else {
        printf("P4 Display Calibration Utility\n");
        printf("===============================\n\n");
    }

    /* Check for root */
    if (geteuid() != 0) {
        fprintf(stderr, "Error: must run as root\n");
        return 1;
    }

    /* Check drm_p4 not loaded */
    if (system("lsmod | grep -q drm_p4") == 0) {
        fprintf(stderr, "Error: drm_p4 module is loaded. Unload it first:\n");
        fprintf(stderr, "  sudo rmmod drm_p4\n");
        return 1;
    }

    /* Parse config */
    printf("Loading config from %s...\n", config_file);
    if (parse_config(config_file) < 0) {
        return 1;
    }
    printf("  SPI:    /dev/spidev%d.%d\n", config.spi_bus, config.spi_cs);
    printf("  GPIO:   /dev/gpiochip%d\n", config.gpio_chip);
    printf("  ENABLE: line %d\n", config.gpio_enable);
    printf("  READY:  line %d\n", config.gpio_ready);
    printf("  NRESET: line %d\n", config.gpio_nreset);
    printf("  COLD:   line %d\n", config.gpio_cold);
    if (config.gpio_vsync >= 0)
        printf("  VSYNC:  line %d\n", config.gpio_vsync);
    if (config.gpio_cs >= 0)
        printf("  CS:     line %d\n", config.gpio_cs);
    printf("\n");

    /* Validate manual_cs requirements */
    if (manual_cs && config.gpio_cs < 0) {
        fprintf(stderr, "Error: --manual-cs requires GPIO_CS in config file\n");
        return 1;
    }

    /* Load spidev */
    printf("Loading spidev module...\n");
    load_spidev();
    /* no delay needed */

    /* Initialize GPIOs */
    printf("Initializing GPIOs...\n");
    if (init_gpios() < 0) {
        goto cleanup;
    }

    /* Open SPI */
    if (open_spi(config.speed_hz) < 0) {
        fprintf(stderr, "Failed to open SPI device.\n");
        fprintf(stderr, "Make sure spidev is enabled in /boot/config.txt:\n");
        fprintf(stderr, "  dtparam=spi=on\n");
        goto cleanup;
    }
    printf("\n");

    /* Fbdump replay mode */
    if (fbdump_file1) {
        /* Close regular input handle before requesting edge events on same line */
        if (gpio_ready_fd >= 0) {
            close(gpio_ready_fd);
            gpio_ready_fd = -1;
        }
        
        /* Request READY line for edge events */
        gpio_ready_edge_fd = gpio_request_edge(config.gpio_ready, "p4-ready-edge");
        if (gpio_ready_edge_fd < 0) {
            fprintf(stderr, "Failed to configure READY for edge events\n");
            goto cleanup;
        }
        
        /* Reset device first (active low) */
        gpio_nreset_set(0);
        usleep(MS_TO_US(RESET_ASSERT_MS));
        gpio_nreset_set(1);
        usleep(MS_TO_US(RESET_SETTLE_MS));
        
        ret = run_fbdump_test(fbdump_file1, fbdump_file2);
        
        /* Clean up edge fd */
        close(gpio_ready_edge_fd);
        gpio_ready_edge_fd = -1;
        
        goto cleanup;
    }

    /* Pattern mode: skip calibration, just send pattern */
    if (pattern_type) {
        /* Close regular input handle before requesting edge events on same line */
        if (gpio_ready_fd >= 0) {
            close(gpio_ready_fd);
            gpio_ready_fd = -1;
        }
        
        /* Request READY line for edge events */
        gpio_ready_edge_fd = gpio_request_edge(config.gpio_ready, "p4-ready-edge");
        if (gpio_ready_edge_fd < 0) {
            fprintf(stderr, "Failed to configure READY for edge events\n");
            goto cleanup;
        }
        
        /* Reset device first (active low) */
        gpio_nreset_set(0);
        usleep(MS_TO_US(RESET_ASSERT_MS));
        gpio_nreset_set(1);
        usleep(MS_TO_US(RESET_SETTLE_MS));
        
        ret = run_pattern_test();
        
        /* Clean up edge fd */
        close(gpio_ready_edge_fd);
        gpio_ready_edge_fd = -1;
        
        goto cleanup;
    }

    /* Frame sync test mode */
    if (test_frame_sync) {
        /* Close regular input handle before requesting edge events on same line */
        if (gpio_ready_fd >= 0) {
            close(gpio_ready_fd);
            gpio_ready_fd = -1;
        }
        
        /* Request READY line for edge events */
        gpio_ready_edge_fd = gpio_request_edge(config.gpio_ready, "p4-ready-edge");
        if (gpio_ready_edge_fd < 0) {
            fprintf(stderr, "Failed to configure READY for edge events\n");
            goto cleanup;
        }
        
        /* Reset device first (active low) */
        gpio_nreset_set(0);
        usleep(MS_TO_US(RESET_ASSERT_MS));
        gpio_nreset_set(1);
        usleep(MS_TO_US(RESET_SETTLE_MS));
        
        ret = run_frame_sync_test();
        
        /* Disable device */
        gpio_enable_set(0);
        
        /* Clean up edge fd */
        close(gpio_ready_edge_fd);
        gpio_ready_edge_fd = -1;
        
        goto cleanup;
    }

    /* SPI frequency test mode: only test SPI speeds, no other tests */
    if (test_spi_freq) {
        /* Ensure ENABLE is low */
        gpio_enable_set(0);
        
        /* Verify READY is low (device idle) */
        if (gpio_ready_get() != 0) {
            fprintf(stderr, "Warning: READY is high with ENABLE low\n");
        }
        
        max_speed = find_max_speed(config.speed_hz);

        printf("\n");
        printf("==============================\n");
        if (max_speed > 0) {
            printf("Maximum reliable SPI speed: %u Hz (%.1f MHz)\n",
                   max_speed, max_speed / 1000000.0);
            printf("\n");
            if (max_speed != (uint32_t)config.speed_hz) {
                printf("Recommendation: Update p4_pins.conf:\n");
                printf("  SPI_SPEED = %u\n", max_speed);
            } else {
                printf("Current config SPI_SPEED = %d is optimal.\n",
                       config.speed_hz);
            }
            ret = 0;
        } else {
            printf("ERROR: No reliable SPI speed found!\n");
            printf("Check your wiring and device power.\n");
            ret = 1;
        }
        printf("==============================\n");
        goto cleanup;
    }

    /* Standalone VSYNC test mode */
    if (test_vblank) {
        printf("P4 Display VSYNC Test\n");
        printf("=====================\n\n");
        
        if (test_vblank_signal() < 0) {
            ret = 1;
        } else {
            ret = 0;
        }
        goto cleanup;
    }

    /* Stress test mode */
    if (test_stress) {
        /* Close regular input handle before requesting edge events on same line */
        if (gpio_ready_fd >= 0) {
            close(gpio_ready_fd);
            gpio_ready_fd = -1;
        }
        
        /* Request READY line for edge events */
        gpio_ready_edge_fd = gpio_request_edge(config.gpio_ready, "p4-ready-edge");
        if (gpio_ready_edge_fd < 0) {
            fprintf(stderr, "Failed to configure READY for edge events\n");
            goto cleanup;
        }
        
        /* Reset device first */
        gpio_nreset_set(0);
        usleep(MS_TO_US(RESET_ASSERT_MS));
        gpio_nreset_set(1);
        usleep(MS_TO_US(RESET_SETTLE_MS));
        
        ret = run_stress_test();
        
        /* Disable device */
        gpio_enable_set(0);
        
        /* Clean up edge fd */
        close(gpio_ready_edge_fd);
        gpio_ready_edge_fd = -1;
        
        goto cleanup;
    }

    /* Display control commands - need READY edge events */
    if (cmd_fill_white_rle || cmd_fill_white_raw || cmd_fill_black_rle || cmd_fill_black_raw ||
        cmd_power_on || cmd_power_off) {
        /* Close regular input handle before requesting edge events on same line */
        if (gpio_ready_fd >= 0) {
            close(gpio_ready_fd);
            gpio_ready_fd = -1;
        }
        
        /* Request READY line for edge events */
        gpio_ready_edge_fd = gpio_request_edge(config.gpio_ready, "p4-ready-edge");
        if (gpio_ready_edge_fd < 0) {
            fprintf(stderr, "Failed to configure READY for edge events\n");
            goto cleanup;
        }
        
        /* Reset device first for fill commands */
        if (cmd_fill_white_rle || cmd_fill_white_raw || cmd_fill_black_rle || cmd_fill_black_raw) {
            gpio_nreset_set(0);
            usleep(MS_TO_US(RESET_ASSERT_MS));
            gpio_nreset_set(1);
            usleep(MS_TO_US(RESET_SETTLE_MS));
        }
        
        /* Assert ENABLE to allow data transfer (READY will go high when buffer has space) */
        gpio_enable_set(1);
        
        if (cmd_fill_white_rle) {
            printf("Filling screen with 0xFF (RLE)...\n");
            ret = fill_rle(0xFF);
        } else if (cmd_fill_white_raw) {
            printf("Filling screen with 0xFF (raw)...\n");
            ret = fill_raw(0xFF);
        } else if (cmd_fill_black_rle) {
            printf("Filling screen with 0x00 (RLE)...\n");
            ret = fill_rle(0x00);
        } else if (cmd_fill_black_raw) {
            printf("Filling screen with 0x00 (raw)...\n");
            ret = fill_raw(0x00);
        } else if (cmd_power_on) {
            printf("Sending power-on (exit standby)...\n");
            union display_flags_u flags = { .byte = 0 };  /* All flags clear = normal operation */
            ret = send_display_flags(flags.byte);
        } else if (cmd_power_off) {
            printf("Sending power-off (enter standby)...\n");
            union display_flags_u flags = { .byte = 0 };
            flags.f.standby = 1;
            ret = send_display_flags(flags.byte);
        }
        
        /* Disable device */
        gpio_enable_set(0);
        
        /* Clean up edge fd */
        close(gpio_ready_edge_fd);
        gpio_ready_edge_fd = -1;
        
        goto cleanup;
    }

    /* Reset and verify connectivity */
    if (reset_and_verify() < 0) {
        goto cleanup;
    }

    /* Test COLD flag behavior */
    if (test_cold_flag() < 0) {
        goto cleanup;
    }

    /* Default mode: basic connectivity tests passed */
    printf("\n");
    printf("===============================\n");
    printf("All connectivity tests passed!\n");
    printf("===============================\n");
    printf("\n");
    printf("Use --spi-freq to find maximum reliable SPI speed.\n");
    ret = 0;

cleanup:
    close_spi();
    cleanup_gpios();
    unload_spidev();

    return ret;
}
