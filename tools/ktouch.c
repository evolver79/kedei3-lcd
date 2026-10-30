// SPDX-License-Identifier: GPL-2.0-only
//
// ktouch: find the touch controller wiring on a KeDei board.
// Reads an XPT2046-style controller on SPI0 CE0 in SPI modes 0 and 3 and
// watches PENIRQ (GPIO25) plus the other header GPIOs, printing press and
// release transitions for the given number of seconds.
// Usage: ktouch [seconds]. Stop kedei3-lcd first: both drive SPI0 directly.

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define CS_CPHA (1u << 2)
#define CS_CPOL (1u << 3)
#define CS_CLEAR_TX (1u << 4)
#define CS_CLEAR_RX (1u << 5)
#define CS_TA (1u << 7)
#define CS_DONE (1u << 16)
#define CS_TXD (1u << 18)

static volatile uint32_t *gpio, *spi;

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

static int conv(int mode3, uint8_t c)
{
	uint32_t cs = mode3 ? CS_CPOL | CS_CPHA : 0; // CE0
	uint8_t r[3];

	spi[0] = cs | CS_TA | CS_CLEAR_TX | CS_CLEAR_RX;
	while (!(spi[0] & CS_TXD))
		;
	spi[1] = c;
	spi[1] = 0;
	spi[1] = 0;
	while (!(spi[0] & CS_DONE))
		;
	for (int i = 0; i < 3; i++)
		r[i] = spi[1];
	spi[0] = cs;
	return (r[1] << 5) | (r[2] >> 3);
}

int main(int argc, char **argv)
{
	int secs = argc > 1 ? atoi(argv[1]) : 20;
	int fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0) {
		perror("/dev/mem");
		return 1;
	}
	uint32_t base = peripheral_base();
	gpio = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, base + 0x200000u);
	spi = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, base + 0x204000u);
	close(fd);
	// SPI0 pins (GPIO 7-11) to ALT0, in case kedei3-lcd never ran.
	for (int pin = 7; pin <= 11; pin++)
		gpio[pin / 10] = (gpio[pin / 10] & ~(7u << (pin % 10) * 3)) | 4u << (pin % 10) * 3;
	spi[2] = 200; // 2 MHz at a 400 MHz core

	// Only header pins 1-26 are connected (GPIO 2-27); skip the SPI pins.
	uint32_t watch = 0x0FFFFFFCu & ~(0x1Fu << 7);
	uint32_t base_lev = gpio[13] & watch;
	printf("idle GPIO levels: 0x%08x\n", base_lev);
	fflush(stdout);

	// Print only press/release transitions plus a heartbeat every 10 s.
	time_t end = time(NULL) + secs, beat = 0;
	int was_down = -1;
	while (time(NULL) < end) {
		uint32_t lev = gpio[13] & watch;
		int x0 = conv(0, 0xD0), y0 = conv(0, 0x90), z0 = conv(0, 0xB0);
		int x3 = conv(1, 0xD0), y3 = conv(1, 0x90), z3 = conv(1, 0xB0);
		conv(0, 0x80); // power down, PENIRQ enabled
		int irq = !(lev & (1u << 25));
		int down = irq || z0 > 200 || z3 > 200;
		if (down != was_down || (down && time(NULL) != beat)) {
			printf("%s irq=%d other_gpio=0x%08x  mode0 x=%4d y=%4d z=%4d  mode3 x=%4d y=%4d z=%4d\n",
			       down ? "DOWN" : "UP  ", irq, (lev ^ base_lev) & ~(1u << 25),
			       x0, y0, z0, x3, y3, z3);
			fflush(stdout);
			was_down = down;
			beat = time(NULL);
		} else if (time(NULL) - beat >= 10) {
			printf("waiting... irq=%d\n", irq);
			fflush(stdout);
			beat = time(NULL);
		}
		usleep(50 * 1000);
	}
	return 0;
}
