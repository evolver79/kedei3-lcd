// SPDX-License-Identifier: GPL-2.0-only
//
// kedei3-lcd: userspace driver for the KeDei 3.5" SPI TFTLCD Version 3.0
// (2015/12/1) on Raspberry Pi. Based on kedei-lcd for the v6.3 board
// (https://github.com/kolomiichenko/kedei-lcd), with the v3 bus framing found
// by probing the board:
//   - SPI0 mode 3, LCD on CE1, touch controller on CE0
//   - a chain of 74HC595s feeds the LCD's parallel bus; every word is sent as
//     two 4-byte frames 00 <hi> <lo> <ctl> and 00 <hi> <lo> <ctl|0x0A>, the
//     second one raising WR so the LCD stores the word
//   - ctl 0x11 = command, 0x15 = data, bit 0 = RESET (active low)
//   - each frame is latched into the 595s by CE1 going high (TA cleared)
// Kernel SPI must be disabled (no dtparam=spi=on): SPI0 and GPIO are driven
// directly through /dev/mem, so this must run as root.
//
// Usage:
//   kedei3-lcd test             init and draw colour bars
//   kedei3-lcd daemon <src>     init, then keep the screen in sync with <src>:
//                               a framebuffer device (/dev/fb0, 16 or 32 bpp,
//                               top-left 480x320 is shown) or a raw 480x320
//                               RGB565 little-endian file; the touchscreen is
//                               exposed as the uinput device "KeDei Touch"
//   kedei3-lcd calibrate        show four targets, write the touch calibration
//
// Touch: XPT2046 on CE0 (SPI mode 0), PENIRQ on GPIO25. Calibration lives in
// /etc/kedei-touch.conf as "xmin xmax ymin ymax swap_xy": the raw readings at
// the left/right and top/bottom screen edges.
//
// Environment:
//   KEDEI_SPI_CDIV   SPI clock divider (default 16, 25 MHz at a 400 MHz core)
//   KEDEI_INIT       panel settings: "v63" (default, KeDei's v6.3 kernel) or
//                    "v11" (the v1.1 board's, paler with visible lines here)

#include <fcntl.h>
#include <linux/uinput.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define GPIO_OFFSET 0x200000u
#define SPI0_OFFSET 0x204000u

#define W 480
#define H 320

// SPI0 CS register bits
#define CS_SEL1 1u
#define CS_CPHA (1u << 2)
#define CS_CPOL (1u << 3)
#define CS_CLEAR_TX (1u << 4)
#define CS_CLEAR_RX (1u << 5)
#define CS_TA (1u << 7)
#define CS_DONE (1u << 16)
#define CS_TXD (1u << 18)
#define CS_LCD (CS_SEL1 | CS_CPOL | CS_CPHA)

#define CTL_CMD 0x11
#define CTL_DATA 0x15
#define CTL_WR_HIGH 0x0A

#define SPI_CDIV_DEFAULT 16

static volatile uint32_t *gpio;
static volatile uint32_t *spi; // [0]=CS [1]=FIFO [2]=CLK
static uint32_t spi_cdiv = SPI_CDIV_DEFAULT;

static void gpio_mode(int pin, int mode)
{
	volatile uint32_t *reg = gpio + pin / 10;
	int shift = (pin % 10) * 3;
	*reg = (*reg & ~(7u << shift)) | ((uint32_t)mode << shift);
}

static void msleep(int ms) { usleep(ms * 1000); }

static void frame(uint8_t hi, uint8_t lo, uint8_t ctl)
{
	spi[0] = CS_LCD | CS_TA | CS_CLEAR_TX | CS_CLEAR_RX;
	while (!(spi[0] & CS_TXD))
		;
	spi[1] = 0x00;
	spi[1] = hi;
	spi[1] = lo;
	spi[1] = ctl;
	while (!(spi[0] & CS_DONE))
		;
	spi[0] = CS_LCD; // CE1 high: latch
}

static void word(uint8_t ctl, uint8_t hi, uint8_t lo)
{
	frame(hi, lo, ctl);
	frame(hi, lo, ctl | CTL_WR_HIGH);
}

static void cmd(uint8_t c) { word(CTL_CMD, 0, c); }
static void dat(uint8_t d) { word(CTL_DATA, 0, d); }

static void cmdv(uint8_t c, int n, const uint8_t *d)
{
	cmd(c);
	for (int i = 0; i < n; i++)
		dat(d[i]);
}
#define CMD(c, ...)                                                            \
	do {                                                                   \
		const uint8_t d_[] = {__VA_ARGS__};                            \
		cmdv(c, sizeof(d_), d_);                                       \
	} while (0)

static void *map(int fd, uint32_t base)
{
	void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, base);
	if (p == MAP_FAILED) {
		perror("mmap");
		exit(1);
	}
	return p;
}

// Same logic as bcm_host_get_peripheral_address(): the SoC "ranges" property
// maps the bus address 0x7e000000 to the CPU physical peripheral base.
static uint32_t peripheral_base(void)
{
	uint8_t r[12];
	uint32_t base = 0x3F000000u; // Pi 2/3 fallback
	FILE *f = fopen("/proc/device-tree/soc/ranges", "rb");

	if (!f)
		return base;
	if (fread(r, 1, sizeof(r), f) == sizeof(r)) {
		uint32_t a = (uint32_t)r[4] << 24 | r[5] << 16 | r[6] << 8 | r[7];
		uint32_t b = (uint32_t)r[8] << 24 | r[9] << 16 | r[10] << 8 | r[11];
		base = a ? a : b;
	}
	fclose(f);
	return base;
}

static void hw_init(void)
{
	uint32_t base = peripheral_base();

	int fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0) {
		perror("/dev/mem");
		exit(1);
	}
	gpio = map(fd, base + GPIO_OFFSET);
	spi = map(fd, base + SPI0_OFFSET);
	close(fd);

	gpio_mode(7, 4);  // ALT0: CE1 (LCD)
	gpio_mode(8, 4);  // ALT0: CE0 (touch)
	gpio_mode(9, 4);  // ALT0: MISO
	gpio_mode(10, 4); // ALT0: MOSI
	gpio_mode(11, 4); // ALT0: SCLK
	spi[2] = spi_cdiv;
}

// Panel settings from KeDei's v6.3 kernel: better contrast and no visible
// line pattern on the v3 board than the v1.1 ones in lcd_init().
static void lcd_init_v63(void)
{
	cmd(0x00);
	msleep(10);
	for (int i = 0; i < 6; i++)
		cmd(0xFF);
	msleep(15);
	cmd(0x11); // sleep out
	msleep(150);
	CMD(0xB0, 0x00);
	CMD(0xB3, 0x02, 0x00, 0x00, 0x00);
	CMD(0xB9, 0x01, 0x00, 0x0F, 0x0F);
	CMD(0xC0, 0x13, 0x3B, 0x00, 0x02, 0x00, 0x01, 0x00, 0x43);
	CMD(0xC1, 0x08, 0x0F, 0x08, 0x08);
	CMD(0xC4, 0x11, 0x07, 0x03, 0x04);
	CMD(0xC6, 0x00);
	CMD(0xC8, 0x03, 0x03, 0x13, 0x5C, 0x03, 0x07, 0x14, 0x08, 0x00, 0x21,
	    0x08, 0x14, 0x07, 0x53, 0x0C, 0x13, 0x03, 0x03, 0x21, 0x00);
	CMD(0x35, 0x00);
	CMD(0x36, 0x28); // landscape 480x320, BGR
	CMD(0x3A, 0x55); // RGB565
	CMD(0x44, 0x00, 0x01);
	CMD(0xD0, 0x07, 0x07, 0x1D, 0x03);
	CMD(0xD1, 0x03, 0x30, 0x10);
	CMD(0xD2, 0x03, 0x14, 0x04);
	cmd(0x29); // display on
	msleep(30);
	CMD(0xB4, 0x00);
}

static void lcd_init(void)
{
	frame(0, 0, 0x00); // reset asserted
	msleep(50);
	frame(0, 0, 0x01);
	msleep(150);

	const char *init = getenv("KEDEI_INIT");
	if (!init || strcmp(init, "v11")) {
		lcd_init_v63();
		return;
	}
	cmd(0x00);
	msleep(1);
	CMD(0xB0, 0x00);
	cmd(0x11); // sleep out
	msleep(50);
	CMD(0xB3, 0x02, 0x00, 0x00, 0x00);
	CMD(0xC0, 0x10, 0x3B, 0x00, 0x02, 0x00, 0x01, 0x00, 0x43);
	CMD(0xC1, 0x08, 0x16, 0x08, 0x08);
	CMD(0xC4, 0x11, 0x07, 0x03, 0x03);
	CMD(0xC6, 0x00);
	CMD(0xC8, 0x03, 0x03, 0x13, 0x5C, 0x03, 0x07, 0x14, 0x08, 0x00, 0x21,
	    0x08, 0x14, 0x07, 0x53, 0x0C, 0x13, 0x03, 0x03, 0x21, 0x00);
	CMD(0x35, 0x00);
	CMD(0x36, 0x28); // landscape 480x320, BGR
	CMD(0x3A, 0x55); // RGB565
	CMD(0x44, 0x00, 0x01);
	CMD(0xB6, 0x00, 0x02, 0x3B);
	CMD(0xD0, 0x07, 0x07, 0x1D);
	CMD(0xD1, 0x00, 0x03, 0x00);
	CMD(0xD2, 0x03, 0x14, 0x04);
	CMD(0xE0, 0x1F, 0x2C, 0x2C, 0x0B, 0x0C, 0x04, 0x4C, 0x64, 0x36, 0x03,
	    0x0E, 0x01, 0x10, 0x01, 0x00);
	CMD(0xE1, 0x1F, 0x3F, 0x3F, 0x0F, 0x1F, 0x0F, 0x7F, 0x32, 0x36, 0x04,
	    0x0B, 0x00, 0x19, 0x14, 0x0F);
	CMD(0xE2, 0x0F, 0x0F, 0x0F);
	CMD(0xE3, 0x0F, 0x0F, 0x0F);
	cmd(0x13);
	cmd(0x29); // display on
	msleep(20);
	CMD(0xB4, 0x00);
	msleep(20);
}

static void window(int x0, int y0, int x1, int y1)
{
	CMD(0x2B, y0 >> 8, y0 & 0xFF, y1 >> 8, y1 & 0xFF);
	CMD(0x2A, x0 >> 8, x0 & 0xFF, x1 >> 8, x1 & 0xFF);
	cmd(0x2C);
}

static void push_rows(const uint16_t *fb, int y0, int y1)
{
	for (int y = y0; y <= y1; y++) {
		window(0, y, W - 1, H - 1);
		for (int i = y * W; i < (y + 1) * W; i++)
			word(CTL_DATA, fb[i] >> 8, fb[i] & 0xFF);
	}
}

#define PIN_PENIRQ 25  // low while the screen is pressed
#define TOUCH_CDIV 200 // 2 MHz at a 400 MHz core, XPT2046 limit is 2.5 MHz
#define CS_TOUCH 0u    // CE0, mode 0
#define Z_MIN 80       // lighter presses give unstable positions

// One 12-bit XPT2046 conversion. PD bits 00 leave PENIRQ enabled afterwards.
// The LCD's 595s see these bytes too, but nothing is latched while CE1 is high.
static int touch_conv(uint8_t c)
{
	uint8_t r[3];

	spi[0] = CS_TOUCH | CS_TA | CS_CLEAR_TX | CS_CLEAR_RX;
	while (!(spi[0] & CS_TXD))
		;
	spi[1] = c;
	spi[1] = 0;
	spi[1] = 0;
	while (!(spi[0] & CS_DONE))
		;
	for (int i = 0; i < 3; i++)
		r[i] = spi[1];
	spi[0] = CS_TOUCH;
	return (r[1] << 5) | (r[2] >> 3);
}

// Trimmed mean of 6 conversions, as KeDei's own driver does.
static int touch_sample(uint8_t c)
{
	int sum = 0, lo = 4096, hi = -1;

	for (int i = 0; i < 6; i++) {
		int v = touch_conv(c);
		sum += v;
		if (v < lo)
			lo = v;
		if (v > hi)
			hi = v;
	}
	return (sum - lo - hi) / 4;
}

// Returns 1 with raw x/y while the screen is pressed firmly enough.
static int touch_read(int *x, int *y)
{
	int pressed = 0;

	if (gpio[13] & (1u << PIN_PENIRQ))
		return 0;
	spi[2] = TOUCH_CDIV;
	if (touch_sample(0xB0) >= Z_MIN) {
		*x = touch_sample(0xD0);
		*y = touch_sample(0x90);
		// Still pressed after the conversions, so x/y are not from a release.
		pressed = touch_sample(0xB0) >= Z_MIN;
	}
	spi[2] = spi_cdiv;
	return pressed;
}

static struct {
	int xmin, xmax, ymin, ymax, swap;
} cal = {200, 3900, 200, 3900, 0};

#define CAL_PATH "/etc/kedei-touch.conf"

// Re-reads the calibration whenever the file changes.
static void load_calibration(void)
{
	static struct timespec mtime;
	struct stat st;

	if (stat(CAL_PATH, &st) || (st.st_mtim.tv_sec == mtime.tv_sec &&
				    st.st_mtim.tv_nsec == mtime.tv_nsec))
		return;
	mtime = st.st_mtim;
	FILE *f = fopen(CAL_PATH, "r");
	if (!f)
		return;
	if (fscanf(f, "%d %d %d %d %d", &cal.xmin, &cal.xmax, &cal.ymin,
		   &cal.ymax, &cal.swap) != 5)
		fprintf(stderr, "bad " CAL_PATH ", using defaults\n");
	fclose(f);
}

static int scale(int v, int from, int to, int size)
{
	if (to == from)
		return 0;
	int r = (v - from) * (size - 1) / (to - from);
	return r < 0 ? 0 : r > size - 1 ? size - 1 : r;
}

static int uinput_open(void)
{
	int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
	if (fd < 0) {
		perror("/dev/uinput");
		return -1;
	}
	ioctl(fd, UI_SET_EVBIT, EV_KEY);
	ioctl(fd, UI_SET_KEYBIT, BTN_TOUCH);
	ioctl(fd, UI_SET_EVBIT, EV_ABS);
	ioctl(fd, UI_SET_ABSBIT, ABS_X);
	ioctl(fd, UI_SET_ABSBIT, ABS_Y);
	ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT);
	struct uinput_abs_setup ax = {.code = ABS_X, .absinfo = {.maximum = W - 1}};
	struct uinput_abs_setup ay = {.code = ABS_Y, .absinfo = {.maximum = H - 1}};
	ioctl(fd, UI_ABS_SETUP, &ax);
	ioctl(fd, UI_ABS_SETUP, &ay);
	struct uinput_setup us = {.id = {.bustype = BUS_VIRTUAL, .vendor = 0x4b44, .product = 0x0335}};
	strcpy(us.name, "KeDei Touch");
	ioctl(fd, UI_DEV_SETUP, &us);
	ioctl(fd, UI_DEV_CREATE);
	return fd;
}

static void emit(int fd, int type, int code, int value)
{
	struct input_event ev = {.type = type, .code = code, .value = value};
	if (fd >= 0 && write(fd, &ev, sizeof(ev)) < 0)
		perror("uinput write");
}

static void touch_poll(int ufd, int *down)
{
	int rx, ry;

	if (touch_read(&rx, &ry)) {
		int ax = cal.swap ? ry : rx, ay = cal.swap ? rx : ry;
		emit(ufd, EV_ABS, ABS_X, scale(ax, cal.xmin, cal.xmax, W));
		emit(ufd, EV_ABS, ABS_Y, scale(ay, cal.ymin, cal.ymax, H));
		if (!*down)
			emit(ufd, EV_KEY, BTN_TOUCH, 1);
		emit(ufd, EV_SYN, SYN_REPORT, 0);
		*down = 1;
	} else if (*down) {
		emit(ufd, EV_KEY, BTN_TOUCH, 0);
		emit(ufd, EV_SYN, SYN_REPORT, 0);
		*down = 0;
	}
}

static uint16_t fb[W * H], shadow[W * H];

static void test(void)
{
	static const uint16_t colors[] = {0xF800, 0x07E0, 0x001F, 0xFFFF,
					  0x0000, 0xFFE0, 0x07FF, 0xF81F};
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++)
			fb[y * W + x] = colors[x * 8 / W];
	// Mark the top-left corner so orientation is obvious.
	for (int y = 0; y < 40; y++)
		for (int x = 0; x < 40; x++)
			fb[y * W + x] = 0xFFFF;
	struct timespec a, b;
	clock_gettime(CLOCK_MONOTONIC, &a);
	push_rows(fb, 0, H - 1);
	clock_gettime(CLOCK_MONOTONIC, &b);
	printf("full frame: %.2fs\n",
	       (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) / 1e9);
}

// A framebuffer device, mapped once; its geometry comes from sysfs.
static struct {
	const uint8_t *mem;
	int bpp, stride;
} src_fb;

static int sysfs_int(const char *dev, const char *attr)
{
	char path[128];
	int v = 0;
	snprintf(path, sizeof(path), "/sys/class/graphics/%s/%s", dev, attr);
	FILE *f = fopen(path, "r");
	if (f) {
		if (fscanf(f, "%d", &v) != 1)
			v = 0;
		fclose(f);
	}
	return v;
}

static int open_fb(const char *path)
{
	const char *dev = strrchr(path, '/') + 1;
	int fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	src_fb.bpp = sysfs_int(dev, "bits_per_pixel");
	src_fb.stride = sysfs_int(dev, "stride");
	char vpath[128];
	int vw = 0, vh = 0;
	snprintf(vpath, sizeof(vpath), "/sys/class/graphics/%s/virtual_size", dev);
	FILE *f = fopen(vpath, "r");
	if (f) {
		if (fscanf(f, "%d,%d", &vw, &vh) != 2)
			vw = vh = 0;
		fclose(f);
	}
	if ((src_fb.bpp != 16 && src_fb.bpp != 32) || vw < W || vh < H) {
		fprintf(stderr, "%s: need >= %dx%d at 16/32 bpp, got %dx%d at %d bpp\n",
			path, W, H, vw, vh, src_fb.bpp);
		close(fd);
		return -1;
	}
	src_fb.mem = mmap(NULL, (size_t)src_fb.stride * H, PROT_READ, MAP_SHARED, fd, 0);
	close(fd);
	if (src_fb.mem == MAP_FAILED) {
		src_fb.mem = NULL;
		return -1;
	}
	fprintf(stderr, "mirroring %s: %d bpp, stride %d\n", path, src_fb.bpp,
		src_fb.stride);
	return 0;
}

static void read_fb(void)
{
	for (int y = 0; y < H; y++) {
		const uint8_t *row = src_fb.mem + (size_t)y * src_fb.stride;
		uint16_t *out = fb + y * W;
		if (src_fb.bpp == 16) {
			memcpy(out, row, W * 2);
		} else {
			const uint32_t *p = (const uint32_t *)row;
			for (int x = 0; x < W; x++) {
				uint32_t c = p[x];
				out[x] = (c >> 8 & 0xF800) | (c >> 5 & 0x07E0) | (c >> 3 & 0x001F);
			}
		}
	}
}

static int read_raw(const char *path, struct timespec *last)
{
	struct stat st;
	if (stat(path, &st) || (st.st_mtim.tv_sec == last->tv_sec &&
				 st.st_mtim.tv_nsec == last->tv_nsec))
		return 0;
	FILE *f = fopen(path, "rb");
	if (!f)
		return 0;
	int ok = fread(fb, sizeof(fb), 1, f) == 1;
	fclose(f);
	if (ok)
		*last = st.st_mtim;
	return ok;
}

// Push only the rows that changed since the last sync.
static void sync_rows(void)
{
	for (int y = 0; y < H; y++)
		if (memcmp(fb + y * W, shadow + y * W, W * 2)) {
			push_rows(fb, y, y);
			memcpy(shadow + y * W, fb + y * W, W * 2);
		}
}

// Draws a cross at (cx, cy) on black and waits for a press on it; returns the
// averaged raw reading, or 0 if nobody pressed within 30 s.
static int calibrate_point(int cx, int cy, int *rx, int *ry)
{
	memset(fb, 0, sizeof(fb));
	for (int d = -12; d <= 12; d++) {
		fb[cy * W + cx + d] = 0xFFFF;
		fb[(cy + d) * W + cx] = 0xFFFF;
	}
	push_rows(fb, 0, H - 1);

	long sx = 0, sy = 0;
	int n = 0, x, y;
	for (int t = 0; t < 1500 && n < 15; t++) {
		if (touch_read(&x, &y)) {
			sx += x;
			sy += y;
			n++;
		}
		msleep(20);
	}
	if (n < 15)
		return 0;
	*rx = sx / n;
	*ry = sy / n;
	// Wait for the release so the next target does not take this press.
	for (int up = 0; up < 10; msleep(20))
		up = touch_read(&x, &y) ? 0 : up + 1;
	return 1;
}

static void calibrate(void)
{
	enum { M = 30 }; // target distance from the edges
	static const int pts[4][2] = {{M, M}, {W - 1 - M, M}, {W - 1 - M, H - 1 - M}, {M, H - 1 - M}};
	int rx[4], ry[4];

	for (int i = 0; i < 4; i++) {
		printf("target %d/4 at %d,%d: press and hold it\n", i + 1, pts[i][0], pts[i][1]);
		fflush(stdout);
		if (!calibrate_point(pts[i][0], pts[i][1], &rx[i], &ry[i])) {
			printf("no press on target %d, calibration not saved\n", i + 1);
			return;
		}
		printf("  raw %d,%d\n", rx[i], ry[i]);
		fflush(stdout);
	}
	// Moving right on the screen (0->1, 3->2) changes raw x or raw y?
	int dx_rawx = abs(rx[1] - rx[0]) + abs(rx[2] - rx[3]);
	int dx_rawy = abs(ry[1] - ry[0]) + abs(ry[2] - ry[3]);
	int swap = dx_rawy > dx_rawx;
	int *hx = swap ? ry : rx, *hy = swap ? rx : ry;
	double left = (hx[0] + hx[3]) / 2.0, right = (hx[1] + hx[2]) / 2.0;
	double top = (hy[0] + hy[1]) / 2.0, bottom = (hy[2] + hy[3]) / 2.0;
	// Extrapolate from the targets to the screen edges.
	double px = (right - left) / (W - 1 - 2 * M), py = (bottom - top) / (H - 1 - 2 * M);
	int xmin = left - M * px, xmax = right + M * px;
	int ymin = top - M * py, ymax = bottom + M * py;

	FILE *f = fopen(CAL_PATH, "w");
	if (!f) {
		perror(CAL_PATH);
		return;
	}
	fprintf(f, "%d %d %d %d %d\n", xmin, xmax, ymin, ymax, swap);
	fclose(f);
	printf("saved %s: %d %d %d %d %d\n", CAL_PATH, xmin, xmax, ymin, ymax, swap);
	for (int i = 0; i < W * H; i++)
		fb[i] = 0x07E0;
	push_rows(fb, 0, H - 1);
}

static void daemon_loop(const char *path)
{
	struct timespec last = {0};
	int is_fb = !strncmp(path, "/dev/fb", 7);
	int ufd = uinput_open(), down = 0;

	memset(shadow, 0, sizeof(shadow));
	push_rows(shadow, 0, H - 1); // clear to black
	for (int tick = 0;; tick++) {
		touch_poll(ufd, &down);
		if (tick % 50 == 0)
			load_calibration();
		if (tick % 2 == 0) {
			if (is_fb) {
				// The framebuffer may appear after we start (vc4 loads
				// late), so keep retrying every 2 s until it can be mapped.
				if (src_fb.mem || (tick % 100 == 0 && open_fb(path) == 0)) {
					read_fb();
					sync_rows();
				}
			} else if (read_raw(path, &last)) {
				sync_rows();
			}
		}
		usleep(20 * 1000);
	}
}

int main(int argc, char **argv)
{
	if (argc < 2 || (strcmp(argv[1], "test") && strcmp(argv[1], "calibrate") && argc < 3)) {
		fprintf(stderr, "usage: %s test | calibrate | daemon <src>\n", argv[0]);
		return 2;
	}
	const char *cdiv = getenv("KEDEI_SPI_CDIV");
	if (cdiv && atoi(cdiv) >= 2)
		spi_cdiv = atoi(cdiv) & ~1u; // the divider must be even
	hw_init();
	lcd_init();
	if (!strcmp(argv[1], "test"))
		test();
	else if (!strcmp(argv[1], "calibrate"))
		calibrate();
	else
		daemon_loop(argv[2]);
	return 0;
}
