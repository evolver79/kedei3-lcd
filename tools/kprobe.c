// SPDX-License-Identifier: GPL-2.0-only
//
// kprobe: find the bus framing a KeDei 3.5" board speaks.
//
// Every KeDei board feeds a parallel LCD bus from a chain of 74HC595s on SPI0,
// with a control byte carrying RESET (bit 0), D/C (bit 2) and a WR strobe.
// Known boards differ in where that control byte sits in the 4-byte frame,
// how WR is strobed and how the frame is latched:
//   v1.1 (2015): mode 0, 00 hi lo ctl, two frames per word (ctl then ctl|0x0A),
//                latched by CE1 going high after each frame
//   v6.3 (2018): mode 3, 00 ctl hi lo, one frame per word, latched by a pulse
//                on CE0 (GPIO8) while CE1 stays asserted
//
// Usage: kprobe <variant 1-8> [cdiv] [colors]
// Draws black with <variant> white bars, so a working variant identifies
// itself by the number of bars even if colours or orientation are off.

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define W 480
#define H 320

#define CS_SEL1 1u
#define CS_CPHA (1u << 2)
#define CS_CPOL (1u << 3)
#define CS_CLEAR_TX (1u << 4)
#define CS_CLEAR_RX (1u << 5)
#define CS_TA (1u << 7)
#define CS_DONE (1u << 16)
#define CS_TXD (1u << 18)

#define CTL_CMD 0x11
#define CTL_DATA 0x15
#define CTL_WR_HIGH 0x0A

static const struct variant {
	const char *name;
	int mode3, ctl_last, two_frames, latch_ce0, v11_init;
} variants[] = {
	{"v6.3 exact", 1, 0, 0, 1, 0},
	{"v1.1 exact", 0, 1, 1, 0, 1},
	{"v1.1 framing, mode 3", 1, 1, 1, 0, 1},
	{"v6.3 framing, mode 0", 0, 0, 0, 1, 0},
	{"v6.3 framing, latched by CE1", 1, 0, 0, 0, 0},
	{"v6.3 framing, two frames, CE1 latch, mode 0", 0, 0, 1, 0, 1},
	{"ctl last, one frame, CE0 pulse, mode 3", 1, 1, 0, 1, 0},
	{"ctl last, one frame, CE1 latch, mode 0", 0, 1, 0, 0, 1},
};

static const struct variant *v;
static volatile uint32_t *gpio, *spi;

static void gpio_mode(int pin, int mode)
{
	volatile uint32_t *reg = gpio + pin / 10;
	int shift = (pin % 10) * 3;
	*reg = (*reg & ~(7u << shift)) | ((uint32_t)mode << shift);
}

static void msleep(int ms) { usleep(ms * 1000); }

static void xfer(uint8_t b0, uint8_t b1, uint8_t b2, uint8_t b3)
{
	uint32_t cs = CS_SEL1 | (v->mode3 ? CS_CPOL | CS_CPHA : 0);

	spi[0] = cs | CS_TA | CS_CLEAR_TX | CS_CLEAR_RX;
	while (!(spi[0] & CS_TXD))
		;
	spi[1] = b0;
	spi[1] = b1;
	spi[1] = b2;
	spi[1] = b3;
	while (!(spi[0] & CS_DONE))
		;
	if (v->latch_ce0) {
		gpio[10] = 1u << 8;
		__sync_synchronize();
		gpio[10] = 1u << 8;
		gpio[7] = 1u << 8;
	} else {
		spi[0] = cs; // TA=0: CE1 goes high and latches the 595s
	}
}

static void raw(uint8_t ctl, uint8_t hi, uint8_t lo)
{
	if (v->ctl_last)
		xfer(0, hi, lo, ctl);
	else
		xfer(0, ctl, hi, lo);
}

static void word(uint8_t ctl, uint8_t hi, uint8_t lo)
{
	raw(ctl, hi, lo);
	if (v->two_frames)
		raw(ctl | CTL_WR_HIGH, hi, lo);
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

static uint32_t peripheral_base(void)
{
	uint8_t r[12];
	uint32_t base = 0x3F000000u;
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

static void *map(int fd, uint32_t base)
{
	void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, base);
	if (p == MAP_FAILED) {
		perror("mmap");
		exit(1);
	}
	return p;
}

static void touch_frame(uint8_t c)
{
	spi[0] = CS_CPOL | CS_CPHA | CS_TA | CS_CLEAR_TX | CS_CLEAR_RX;
	while (!(spi[0] & CS_TXD))
		;
	spi[1] = 0;
	spi[1] = c;
	spi[1] = 0;
	spi[1] = 0;
	while (!(spi[0] & CS_DONE))
		;
	spi[0] = CS_CPOL | CS_CPHA;
}

// v6.3 boards only answer after the touch controller has been addressed.
static void touch_wakeup(uint32_t cdiv)
{
	gpio_mode(8, 4);
	spi[2] = 32;
	for (int i = 0; i < 21; i++)
		touch_frame(0xE7);
	msleep(50);
	for (int i = 0; i < 21; i++)
		touch_frame(0xE7);
	touch_frame(0x80);
	gpio_mode(8, 1);
	gpio[7] = 1u << 8;
	spi[2] = cdiv;
	msleep(10);
}

static void init_v63(void)
{
	cmd(0x00);
	msleep(10);
	for (int i = 0; i < 6; i++)
		cmd(0xFF);
	msleep(15);
	cmd(0x11);
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
	CMD(0x36, 0x60);
	CMD(0x3A, 0x55);
	CMD(0x44, 0x00, 0x01);
	CMD(0xD0, 0x07, 0x07, 0x1D, 0x03);
	CMD(0xD1, 0x03, 0x30, 0x10);
	CMD(0xD2, 0x03, 0x14, 0x04);
	cmd(0x29);
	msleep(30);
	CMD(0xB4, 0x00);
}

static void init_v11(void)
{
	cmd(0x00);
	msleep(1);
	CMD(0xB0, 0x00);
	cmd(0x11);
	msleep(50);
	CMD(0xB3, 0x02, 0x00, 0x00, 0x00);
	CMD(0xC0, 0x10, 0x3B, 0x00, 0x02, 0x00, 0x01, 0x00, 0x43);
	CMD(0xC1, 0x08, 0x16, 0x08, 0x08);
	CMD(0xC4, 0x11, 0x07, 0x03, 0x03);
	CMD(0xC6, 0x00);
	CMD(0xC8, 0x03, 0x03, 0x13, 0x5C, 0x03, 0x07, 0x14, 0x08, 0x00, 0x21,
	    0x08, 0x14, 0x07, 0x53, 0x0C, 0x13, 0x03, 0x03, 0x21, 0x00);
	CMD(0x35, 0x00);
	CMD(0x36, 0x28);
	CMD(0x3A, 0x55);
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
	cmd(0x29);
	msleep(20);
	CMD(0xB4, 0x00);
	msleep(20);
}

static void draw_bars(int n)
{
	int bar = H / (2 * n + 1);

	for (int y = 0; y < H; y++) {
		CMD(0x2B, y >> 8, y & 0xFF, (H - 1) >> 8, (H - 1) & 0xFF);
		CMD(0x2A, 0, 0, (W - 1) >> 8, (W - 1) & 0xFF);
		cmd(0x2C);
		int on = (y / bar) % 2 == 1 && y / bar < 2 * n;
		for (int x = 0; x < W; x++)
			word(CTL_DATA, on ? 0xFF : 0, on ? 0xFF : 0);
	}
}

// 8 vertical bars: red green blue white black yellow cyan magenta, with a
// white square in the top-left corner to show orientation.
static void draw_colors(void)
{
	static const uint16_t colors[] = {0xF800, 0x07E0, 0x001F, 0xFFFF,
					  0x0000, 0xFFE0, 0x07FF, 0xF81F};

	for (int y = 0; y < H; y++) {
		CMD(0x2B, y >> 8, y & 0xFF, (H - 1) >> 8, (H - 1) & 0xFF);
		CMD(0x2A, 0, 0, (W - 1) >> 8, (W - 1) & 0xFF);
		cmd(0x2C);
		for (int x = 0; x < W; x++) {
			uint16_t c = x < 40 && y < 40 ? 0xFFFF : colors[x * 8 / W];
			word(CTL_DATA, c >> 8, c & 0xFF);
		}
	}
}

int main(int argc, char **argv)
{
	int n = argc > 1 ? atoi(argv[1]) : 0;
	uint32_t cdiv = argc > 2 ? (uint32_t)atoi(argv[2]) & ~1u : 32;

	if (n < 1 || n > (int)(sizeof(variants) / sizeof(variants[0]))) {
		fprintf(stderr, "usage: %s <variant 1-%zu> [cdiv]\n", argv[0],
			sizeof(variants) / sizeof(variants[0]));
		return 2;
	}
	v = &variants[n - 1];

	int fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0) {
		perror("/dev/mem");
		return 1;
	}
	uint32_t base = peripheral_base();
	gpio = map(fd, base + 0x200000u);
	spi = map(fd, base + 0x204000u);
	close(fd);

	gpio_mode(9, 4);
	gpio_mode(10, 4);
	gpio_mode(11, 4);
	gpio_mode(7, 4);
	gpio_mode(8, 1);
	gpio[7] = 1u << 8;
	spi[2] = cdiv;

	printf("variant %d: %s (cdiv %u)\n", n, v->name, cdiv);
	touch_wakeup(cdiv);

	raw(0x01, 0, 0);
	msleep(50);
	raw(0x00, 0, 0);
	msleep(100);
	raw(0x01, 0, 0);
	msleep(50);

	if (v->v11_init)
		init_v11();
	else
		init_v63();
	if (argc > 3 && !strcmp(argv[3], "colors"))
		draw_colors();
	else
		draw_bars(n);
	return 0;
}
