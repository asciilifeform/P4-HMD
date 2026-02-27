// SPDX-License-Identifier: GPL-2.0
/*
 * p4_setmode - Set display rotation using atomic modesetting
 *
 * Usage: p4_setmode [normal | left | right | inverted]
 *        p4_setmode --list
 *
 * This sets the DRM rotation property, which is what xrandr uses.
 * - normal:   0° rotation (720x280 framebuffer, driver rotates 90°)
 * - left:     90° rotation (280x720 framebuffer, no driver rotation)
 * - inverted: 180° rotation (720x280 + hw flip)
 * - right:    270° rotation (280x720 + hw flip)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#include <drm_mode.h>

#include "display.h"

static int find_p4_device(void)
{
	char path[64];
	int fd;
	drmVersionPtr ver;

	for (int i = 0; i < 16; i++) {
		snprintf(path, sizeof(path), "/dev/dri/card%d", i);
		fd = open(path, O_RDWR);
		if (fd < 0)
			continue;

		ver = drmGetVersion(fd);
		if (ver) {
			int match = (strcmp(ver->name, "p4_display") == 0);
			drmFreeVersion(ver);
			if (match) {
				if (drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1) < 0) {
					fprintf(stderr, "Warning: atomic not supported\n");
				}
				if (drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) < 0) {
					fprintf(stderr, "Warning: universal planes not supported\n");
				}
				return fd;
			}
		}
		close(fd);
	}
	return -1;
}

/* Get property ID and current value by name */
static uint32_t get_prop_id(int fd, uint32_t obj_id, uint32_t obj_type,
			    const char *name, uint64_t *value)
{
	drmModeObjectProperties *props = drmModeObjectGetProperties(fd, obj_id, obj_type);
	if (!props)
		return 0;

	uint32_t prop_id = 0;
	for (uint32_t i = 0; i < props->count_props; i++) {
		drmModePropertyRes *prop = drmModeGetProperty(fd, props->props[i]);
		if (!prop)
			continue;
		if (strcmp(prop->name, name) == 0) {
			prop_id = prop->prop_id;
			if (value)
				*value = props->prop_values[i];
			drmModeFreeProperty(prop);
			break;
		}
		drmModeFreeProperty(prop);
	}
	drmModeFreeObjectProperties(props);
	return prop_id;
}

static void list_info(int fd)
{
	drmModeRes *res;
	drmModeConnector *conn;
	drmModePlaneRes *plane_res;

	res = drmModeGetResources(fd);
	if (!res) {
		fprintf(stderr, "Failed to get DRM resources\n");
		return;
	}

	printf("Connectors: %d\n", res->count_connectors);

	for (int i = 0; i < res->count_connectors; i++) {
		conn = drmModeGetConnector(fd, res->connectors[i]);
		if (!conn)
			continue;

		printf("\nConnector %u: %s\n",
		       conn->connector_id,
		       conn->connection == DRM_MODE_CONNECTED ? "connected" : "disconnected");
		printf("  Modes: %d\n", conn->count_modes);

		for (int j = 0; j < conn->count_modes; j++) {
			drmModeModeInfo *mode = &conn->modes[j];
			printf("    %dx%d @ %dHz%s\n",
			       mode->hdisplay, mode->vdisplay,
			       mode->vrefresh,
			       (mode->type & DRM_MODE_TYPE_PREFERRED) ? " (preferred)" : "");
		}

		drmModeFreeConnector(conn);
	}

	/* List planes and their rotation property */
	plane_res = drmModeGetPlaneResources(fd);
	if (plane_res) {
		printf("\nPlanes: %d\n", plane_res->count_planes);
		for (uint32_t i = 0; i < plane_res->count_planes; i++) {
			drmModePlane *plane = drmModeGetPlane(fd, plane_res->planes[i]);
			if (!plane)
				continue;

			uint64_t rotation = 0;
			uint32_t rot_prop = get_prop_id(fd, plane->plane_id,
						       DRM_MODE_OBJECT_PLANE, "rotation", &rotation);

			printf("  Plane %u: rotation prop=%u, value=%lu\n",
			       plane->plane_id, rot_prop, (unsigned long)rotation);

			drmModeFreePlane(plane);
		}
		drmModeFreePlaneResources(plane_res);
	}

	drmModeFreeResources(res);
}

/* Fill buffer with test pattern */
static void fill_pattern(uint32_t *buf, int width, int height, int pitch_bytes, const char *pattern)
{
	int pitch = pitch_bytes / 4;
	uint32_t white = 0x00FFFFFF;
	uint32_t black = 0x00000000;

	for (int y = 0; y < height; y++) {
		for (int x = 0; x < width; x++) {
			uint32_t color;
			if (!pattern || strcmp(pattern, "black") == 0) {
				color = black;
			} else if (strcmp(pattern, "white") == 0) {
				color = white;
			} else if (strcmp(pattern, "checker") == 0) {
				color = ((x / 8) + (y / 8)) % 2 ? white : black;
			} else if (strcmp(pattern, "vstripes") == 0) {
				color = (x / 8) % 2 ? white : black;
			} else if (strcmp(pattern, "hstripes") == 0) {
				color = (y / 8) % 2 ? white : black;
			} else if (strcmp(pattern, "arrow") == 0) {
				/*
				 * Arrow pointing RIGHT with distinct corner markers.
				 * All dimensions are multiples of 8 for clean byte-aligned rendering.
				 * 
				 * In source framebuffer (before any rotation):
				 *   TL: small square
				 *   TR: horizontal bar (wider than tall)
				 *   BL: vertical bar (taller than wide)
				 *   BR: large square
				 *   Center: arrow pointing right
				 */
				
				int m = 8;  /* margin from edge */
				
				/* TL: small square 24x24 */
				int in_tl = (x >= m && x < m + 24 &&
				             y >= m && y < m + 24);
				
				/* TR: horizontal bar 48x16 */
				int in_tr = (x >= width - m - 48 && x < width - m &&
				             y >= m && y < m + 16);
				
				/* BL: vertical bar 16x48 */
				int in_bl = (x >= m && x < m + 16 &&
				             y >= height - m - 48 && y < height - m);
				
				/* BR: large square 40x40 */
				int in_br = (x >= width - m - 40 && x < width - m &&
				             y >= height - m - 40 && y < height - m);
				
				/* Arrow - simple right-pointing shape */
				int cx = width / 2;
				int cy = height / 2;
				
				/* Shaft: rectangle 64x16 */
				int in_shaft = (x >= cx - 64 && x < cx &&
				                y >= cy - 8 && y < cy + 8);
				
				/* Head: triangle using horizontal bands */
				int in_head = 0;
				if (x >= cx && x < cx + 48) {
					int progress = x - cx;  /* 0 at base, 48 at tip */
					int half_h = 32 - (progress * 32 / 48);
					in_head = (y >= cy - half_h && y < cy + half_h);
				}
				
				color = (in_tl || in_tr || in_bl || in_br || in_shaft || in_head) ? black : white;
			} else {
				color = black;
			}
			buf[y * pitch + x] = color;
		}
	}
}

/* Global for stdin data */
static uint8_t *g_stdin_data = NULL;
static size_t g_stdin_size = 0;

/* Create a dumb buffer */
static int create_dumb_fb(int fd, int width, int height, uint32_t *fb_id,
			  uint32_t *handle, const char *pattern)
{
	struct drm_mode_create_dumb create = {
		.width = width,
		.height = height,
		.bpp = 32,
	};

	if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0) {
		fprintf(stderr, "Failed to create dumb buffer: %s\n", strerror(errno));
		return -1;
	}

	*handle = create.handle;

	uint32_t handles[4] = { create.handle, 0, 0, 0 };
	uint32_t pitches[4] = { create.pitch, 0, 0, 0 };
	uint32_t offsets[4] = { 0, 0, 0, 0 };

	if (drmModeAddFB2(fd, width, height, DRM_FORMAT_XRGB8888,
			  handles, pitches, offsets, fb_id, 0) < 0) {
		fprintf(stderr, "Failed to create framebuffer: %s\n", strerror(errno));
		struct drm_mode_destroy_dumb destroy = { .handle = create.handle };
		drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
		return -1;
	}

	/* Map and fill the buffer */
	struct drm_mode_map_dumb map = { .handle = create.handle };
	if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map) == 0) {
		void *ptr = mmap(NULL, create.size, PROT_READ | PROT_WRITE,
				 MAP_SHARED, fd, map.offset);
		if (ptr != MAP_FAILED) {
			if (g_stdin_data && g_stdin_size > 0) {
				/* Copy stdin data, handling pitch */
				size_t row_bytes = width * 4;
				for (int y = 0; y < height; y++) {
					size_t src_off = y * row_bytes;
					size_t dst_off = y * create.pitch;
					if (src_off + row_bytes <= g_stdin_size) {
						memcpy((uint8_t*)ptr + dst_off, 
						       g_stdin_data + src_off, row_bytes);
					}
				}
			} else {
				fill_pattern(ptr, width, height, create.pitch, pattern);
			}
			/* Ensure data is flushed before atomic commit */
			msync(ptr, create.size, MS_SYNC);
			munmap(ptr, create.size);
		}
	}

	return 0;
}

static void destroy_dumb_fb(int fd, uint32_t fb_id, uint32_t handle)
{
	if (fb_id)
		drmModeRmFB(fd, fb_id);
	if (handle) {
		struct drm_mode_destroy_dumb destroy = { .handle = handle };
		drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
	}
}

static int set_rotation(int fd, uint32_t rotation, const char *pattern)
{
	drmModeRes *res = NULL;
	drmModeConnector *conn = NULL;
	drmModePlaneRes *plane_res = NULL;
	drmModeAtomicReq *req = NULL;
	drmModeModeInfo *mode = NULL;
	uint32_t conn_id = 0, crtc_id = 0, plane_id = 0;
	uint32_t fb_id = 0, handle = 0;
	uint32_t mode_blob_id = 0;
	int ret = -1;
	int fb_width, fb_height;

	/* Determine framebuffer dimensions based on rotation */
	if (rotation == DRM_MODE_ROTATE_0 || rotation == DRM_MODE_ROTATE_180) {
		fb_width = P4_WIDTH;
		fb_height = P4_HEIGHT;
	} else {
		fb_width = P4_HEIGHT;
		fb_height = P4_WIDTH;
	}

	res = drmModeGetResources(fd);
	if (!res) {
		fprintf(stderr, "Failed to get DRM resources\n");
		return -1;
	}

	/* Find connected connector */
	for (int i = 0; i < res->count_connectors; i++) {
		conn = drmModeGetConnector(fd, res->connectors[i]);
		if (!conn)
			continue;

		if (conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0) {
			conn_id = conn->connector_id;
			mode = &conn->modes[0];  /* Use first (only) mode */

			if (conn->encoder_id) {
				drmModeEncoder *enc = drmModeGetEncoder(fd, conn->encoder_id);
				if (enc) {
					crtc_id = enc->crtc_id;
					drmModeFreeEncoder(enc);
				}
			}
			if (!crtc_id && res->count_crtcs > 0)
				crtc_id = res->crtcs[0];

			break;
		}
		drmModeFreeConnector(conn);
		conn = NULL;
	}

	if (!conn_id || !crtc_id || !mode) {
		fprintf(stderr, "No suitable connector/CRTC found\n");
		goto out;
	}

	/* Find primary plane */
	plane_res = drmModeGetPlaneResources(fd);
	if (!plane_res) {
		fprintf(stderr, "Failed to get plane resources\n");
		goto out;
	}

	for (uint32_t i = 0; i < plane_res->count_planes; i++) {
		drmModePlane *plane = drmModeGetPlane(fd, plane_res->planes[i]);
		if (!plane)
			continue;

		int crtc_idx = -1;
		for (int j = 0; j < res->count_crtcs; j++) {
			if (res->crtcs[j] == crtc_id) {
				crtc_idx = j;
				break;
			}
		}

		if (crtc_idx >= 0 && (plane->possible_crtcs & (1 << crtc_idx))) {
			uint64_t type = 0;
			get_prop_id(fd, plane->plane_id, DRM_MODE_OBJECT_PLANE, "type", &type);
			if (type == DRM_PLANE_TYPE_PRIMARY) {
				plane_id = plane->plane_id;
				drmModeFreePlane(plane);
				break;
			}
		}
		drmModeFreePlane(plane);
	}

	if (!plane_id) {
		fprintf(stderr, "No primary plane found\n");
		goto out;
	}

	printf("Setting rotation %u on connector %u, CRTC %u, plane %u\n",
	       rotation, conn_id, crtc_id, plane_id);
	printf("Framebuffer dimensions: %dx%d\n", fb_width, fb_height);

	/* Create framebuffer */
	if (create_dumb_fb(fd, fb_width, fb_height, &fb_id, &handle, pattern) < 0)
		goto out;

	/* Create mode blob */
	if (drmModeCreatePropertyBlob(fd, mode, sizeof(*mode), &mode_blob_id) < 0) {
		fprintf(stderr, "Failed to create mode blob: %s\n", strerror(errno));
		goto out;
	}

	/* Get property IDs */
	uint32_t conn_crtc_id = get_prop_id(fd, conn_id, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID", NULL);
	uint32_t crtc_mode_id = get_prop_id(fd, crtc_id, DRM_MODE_OBJECT_CRTC, "MODE_ID", NULL);
	uint32_t crtc_active = get_prop_id(fd, crtc_id, DRM_MODE_OBJECT_CRTC, "ACTIVE", NULL);
	uint32_t plane_fb_id = get_prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "FB_ID", NULL);
	uint32_t plane_crtc_id = get_prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_ID", NULL);
	uint32_t plane_src_x = get_prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "SRC_X", NULL);
	uint32_t plane_src_y = get_prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "SRC_Y", NULL);
	uint32_t plane_src_w = get_prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "SRC_W", NULL);
	uint32_t plane_src_h = get_prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "SRC_H", NULL);
	uint32_t plane_crtc_x = get_prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_X", NULL);
	uint32_t plane_crtc_y = get_prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_Y", NULL);
	uint32_t plane_crtc_w = get_prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_W", NULL);
	uint32_t plane_crtc_h = get_prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_H", NULL);
	uint32_t plane_rotation = get_prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "rotation", NULL);

	if (!plane_rotation) {
		fprintf(stderr, "Rotation property not found\n");
		goto out;
	}

	/* Build atomic request */
	req = drmModeAtomicAlloc();
	if (!req) {
		fprintf(stderr, "Failed to allocate atomic request\n");
		goto out;
	}

	drmModeAtomicAddProperty(req, conn_id, conn_crtc_id, crtc_id);
	drmModeAtomicAddProperty(req, crtc_id, crtc_mode_id, mode_blob_id);
	drmModeAtomicAddProperty(req, crtc_id, crtc_active, 1);

	drmModeAtomicAddProperty(req, plane_id, plane_fb_id, fb_id);
	drmModeAtomicAddProperty(req, plane_id, plane_crtc_id, crtc_id);
	drmModeAtomicAddProperty(req, plane_id, plane_rotation, rotation);

	if (plane_src_x) drmModeAtomicAddProperty(req, plane_id, plane_src_x, 0);
	if (plane_src_y) drmModeAtomicAddProperty(req, plane_id, plane_src_y, 0);
	drmModeAtomicAddProperty(req, plane_id, plane_src_w, (uint64_t)fb_width << 16);
	drmModeAtomicAddProperty(req, plane_id, plane_src_h, (uint64_t)fb_height << 16);
	if (plane_crtc_x) drmModeAtomicAddProperty(req, plane_id, plane_crtc_x, 0);
	if (plane_crtc_y) drmModeAtomicAddProperty(req, plane_id, plane_crtc_y, 0);
	drmModeAtomicAddProperty(req, plane_id, plane_crtc_w, mode->hdisplay);
	drmModeAtomicAddProperty(req, plane_id, plane_crtc_h, mode->vdisplay);

	/* Commit */
	ret = drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
	if (ret < 0) {
		fprintf(stderr, "Atomic commit failed: %s\n", strerror(errno));
		ret = -1;
	} else {
		printf("Rotation set successfully\n");
		fb_id = 0;
		handle = 0;
		ret = 0;
	}

out:
	if (req)
		drmModeAtomicFree(req);
	if (mode_blob_id)
		drmModeDestroyPropertyBlob(fd, mode_blob_id);
	if (fb_id || handle)
		destroy_dumb_fb(fd, fb_id, handle);
	if (plane_res)
		drmModeFreePlaneResources(plane_res);
	if (conn)
		drmModeFreeConnector(conn);
	if (res)
		drmModeFreeResources(res);
	return ret;
}

static void usage(const char *prog)
{
	printf("Usage: %s [ROTATION | --list]\n", prog);
	printf("       %s ROTATION --pattern NAME\n", prog);
	printf("       %s ROTATION --stdin\n", prog);
	printf("\n");
	printf("Set P4 display rotation using atomic modesetting.\n");
	printf("This mimics what xrandr --rotate does.\n");
	printf("\n");
	printf("Rotations:\n");
	printf("  normal    0° - 720x280 framebuffer (default)\n");
	printf("  left      90° - 280x720 framebuffer\n");
	printf("  inverted  180° - 720x280 framebuffer, flipped\n");
	printf("  right     270° - 280x720 framebuffer, flipped\n");
	printf("\n");
	printf("Options:\n");
	printf("  --list          List connectors, modes, and planes\n");
	printf("  --pattern NAME  Fill with pattern (white, black, checker, vstripes, hstripes, arrow)\n");
	printf("  --stdin         Read raw XRGB8888 data from stdin\n");
	printf("  --help          Show this help\n");
}

static int read_stdin_data(int expected_width, int expected_height)
{
	size_t expected = expected_width * expected_height * 4;
	g_stdin_data = malloc(expected);
	if (!g_stdin_data) {
		fprintf(stderr, "Failed to allocate %zu bytes\n", expected);
		return -1;
	}
	
	size_t total = 0;
	while (total < expected) {
		ssize_t n = read(STDIN_FILENO, g_stdin_data + total, expected - total);
		if (n <= 0) {
			if (n < 0)
				fprintf(stderr, "Read error: %s\n", strerror(errno));
			break;
		}
		total += n;
	}
	
	if (total < expected) {
		fprintf(stderr, "Warning: read %zu bytes, expected %zu\n", total, expected);
	}
	g_stdin_size = total;
	return 0;
}

int main(int argc, char **argv)
{
	int fd;
	uint32_t rotation;
	const char *pattern = NULL;

	if (argc < 2) {
		usage(argv[0]);
		return 1;
	}

	if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
		usage(argv[0]);
		return 0;
	}

	fd = find_p4_device();
	if (fd < 0) {
		fprintf(stderr, "P4 display not found\n");
		return 1;
	}

	if (strcmp(argv[1], "--list") == 0) {
		list_info(fd);
		close(fd);
		return 0;
	}

	if (strcmp(argv[1], "normal") == 0) {
		rotation = DRM_MODE_ROTATE_0;
	} else if (strcmp(argv[1], "left") == 0) {
		rotation = DRM_MODE_ROTATE_90;
	} else if (strcmp(argv[1], "inverted") == 0) {
		rotation = DRM_MODE_ROTATE_180;
	} else if (strcmp(argv[1], "right") == 0) {
		rotation = DRM_MODE_ROTATE_270;
	} else {
		fprintf(stderr, "Unknown rotation: %s\n", argv[1]);
		fprintf(stderr, "Use: normal, left, inverted, right\n");
		close(fd);
		return 1;
	}

	/* Check for --pattern or --stdin */
	int use_stdin = 0;
	for (int i = 2; i < argc; i++) {
		if (strcmp(argv[i], "--pattern") == 0 && i + 1 < argc) {
			pattern = argv[i + 1];
			i++;
		} else if (strcmp(argv[i], "--stdin") == 0) {
			use_stdin = 1;
		}
	}

	/* Read stdin data if requested */
	if (use_stdin) {
		int w = (rotation == DRM_MODE_ROTATE_0 || rotation == DRM_MODE_ROTATE_180) ? P4_WIDTH : P4_HEIGHT;
		int h = (rotation == DRM_MODE_ROTATE_0 || rotation == DRM_MODE_ROTATE_180) ? P4_HEIGHT : P4_WIDTH;
		if (read_stdin_data(w, h) < 0) {
			close(fd);
			return 1;
		}
	}

	int ret = set_rotation(fd, rotation, pattern);
	
	if (g_stdin_data)
		free(g_stdin_data);
	
	close(fd);
	return ret ? 1 : 0;
}
