// SPDX-License-Identifier: GPL-2.0-only
//
// demoscene: a small demo show for the KeDei v3 screen: starfield with a sine
// scroller, a hardware-scrolled landscape, sine waves, raster bars, plasma and
// blobs.
//
// Every frame is drawn in full into a buffer and only the pixels that differ
// from the previous frame are sent, so effects that change few pixels run
// full-screen, and plasma and blobs, which change every pixel, run in a box.
//
// Usage: demoscene [runs]   (default 1, 0 = forever). Stop kedei3-lcd first.

#define KEDEI3_NO_MAIN
#include "../src/kedei3-lcd.c"

#include <math.h>

#define FONT_PATH "/usr/share/consolefonts/Lat15-TerminusBold32x16.psf.gz"
#define FW 16
#define FH 32

static uint8_t font[128][FH * FW / 8];
static int have_font;

// Loads the ASCII glyphs of a gzipped PSF2 console font, using its unicode
// table to find them.
static void load_font(void)
{
	static uint8_t buf[1 << 17];
	FILE *f = popen("zcat " FONT_PATH " 2>/dev/null", "r");
	if (!f)
		return;
	size_t len = fread(buf, 1, sizeof(buf), f);
	pclose(f);
	uint32_t *hdr = (uint32_t *)buf;
	if (len < 32 || hdr[0] != 0x864ab572 || hdr[6] != FH || hdr[7] != FW)
		return;
	uint32_t headersize = hdr[2], flags = hdr[3], count = hdr[4], charsize = hdr[5];
	const uint8_t *glyphs = buf + headersize;
	if (!(flags & 1)) {
		for (int c = 32; c < 128 && (uint32_t)c < count; c++)
			memcpy(font[c], glyphs + c * charsize, charsize);
		have_font = 1;
		return;
	}
	const uint8_t *p = glyphs + count * charsize, *end = buf + len;
	for (uint32_t g = 0; g < count && p < end; g++) {
		int in_sequence = 0;
		for (; p < end && *p != 0xFF; p++) {
			if (*p == 0xFE)
				in_sequence = 1;
			else if (!in_sequence && *p < 0x80)
				memcpy(font[*p], glyphs + g * charsize, charsize);
		}
		p++;
	}
	have_font = 1;
}

static int glyph_bit(int c, int x, int y)
{
	return c >= 0 && c < 128 && (font[c][y * 2 + x / 8] & (0x80 >> (x % 8)));
}

static void draw_text(const char *s, int x, int y, uint16_t color)
{
	for (; *s; s++, x += FW)
		for (int j = 0; j < FH; j++)
			for (int i = 0; i < FW; i++)
				if (x + i >= 0 && x + i < W && glyph_bit(*s, i, j))
					fb[(y + j) * W + x + i] = color;
}

static uint16_t rgbf(double r, double g, double b)
{
	r = r < 0 ? 0 : r > 1 ? 1 : r;
	g = g < 0 ? 0 : g > 1 ? 1 : g;
	b = b < 0 ? 0 : b > 1 ? 1 : b;
	return rgb(r * 255, g * 255, b * 255);
}

static void fill(int x0, int y0, int x1, int y1, uint16_t c)
{
	for (int y = y0; y <= y1; y++)
		for (int x = x0; x <= x1; x++)
			fb[y * W + x] = c;
}

// The FPS counter in the top-right corner.
static double fps_at;
static int fps_frames, fps_shown, total_frames;

static void count_frame(void)
{
	double t = now();
	total_frames++;
	fps_frames++;
	if (t - fps_at >= 0.5) {
		fps_shown = fps_frames / (t - fps_at);
		fps_shown = fps_shown > 99 ? 99 : fps_shown;
		fps_at = t;
		fps_frames = 0;
	}
	draw_digit(fb, fps_shown / 10, W - 44, 6, 0x7BEF);
	draw_digit(fb, fps_shown % 10, W - 24, 6, 0x7BEF);
}

static void show(void)
{
	count_frame();
	push_diff(fb, shadow);
}

// Like show(), but sends only every other row, alternating between frames:
// twice the frame rate for effects that change most of their area.
static void show_interlaced(void)
{
	static int field;
	count_frame();
	push_diff_rows(fb, shadow, field, 2);
	field ^= 1;
}

// --- Starfield with a sine scroller -------------------------------------

#define STARS 220

static const char *scroll_text =
	"                              "
	"KEDEI 3.5\" V3.0 FROM 2015 ... ALIVE AGAIN ON RASPBERRY PI OS TRIXIE ... "
	"480 X 320 PIXELS SHIFTED OUT AT 33 MHZ, TWO TRANSFERS PER PIXEL ... "
	"ONLY THE PIXELS THAT CHANGE ARE SENT ... "
	"GREETINGS TO SAPER-2 AND KOLOMIICHENKO ... "
	"CODE BY CLAUDE FOR EVOLVER79 ...                              ";

static void part_starfield(double seconds)
{
	double sx[STARS], sy[STARS], sz[STARS];
	int len = strlen(scroll_text);

	for (int i = 0; i < STARS; i++) {
		sx[i] = (rand() % 2000 - 1000) / 4.0;
		sy[i] = (rand() % 2000 - 1000) / 6.0;
		sz[i] = 1 + rand() % 255;
	}
	double start = now(), last = start;
	while (now() - start < seconds) {
		double t = now(), dt = t - last, tt = t - start;
		last = t;
		memset(fb, 0, sizeof(fb));
		for (int i = 0; i < STARS; i++) {
			sz[i] -= 90 * dt;
			int px = W / 2 + sx[i] * 128 / sz[i], py = H / 2 + sy[i] * 128 / sz[i];
			if (sz[i] < 1 || px < 0 || px >= W - 1 || py < 0 || py >= H - 1) {
				sx[i] = (rand() % 2000 - 1000) / 4.0;
				sy[i] = (rand() % 2000 - 1000) / 6.0;
				sz[i] = 255;
				continue;
			}
			double b = 1 - sz[i] / 255;
			uint16_t c = rgbf(b, b, b * 1.2 + 0.1);
			fb[py * W + px] = c;
			if (sz[i] < 90) {
				fb[py * W + px + 1] = c;
				fb[(py + 1) * W + px] = c;
				fb[(py + 1) * W + px + 1] = c;
			}
		}
		if (have_font) {
			int scroll = tt * 140;
			for (int x = 0; x < W; x++) {
				int p = (scroll + x) % (len * FW);
				int c = scroll_text[p / FW];
				int y0 = 220 + 26 * sin(tt * 3 + x * 0.018);
				for (int j = 0; j < FH; j++)
					if (glyph_bit(c, p % FW, j)) {
						double h = (double)j / FH;
						fb[(y0 + j) * W + x] = rgbf(1, 0.4 + 0.6 * h, 1 - h);
					}
			}
		}
		show();
	}
}

// --- Sine waves ----------------------------------------------------------

static void part_sines(double seconds)
{
	static const double col[3][3] = {{0.2, 1, 1}, {1, 0.3, 1}, {1, 1, 0.2}};
	double start = now();
	while (now() - start < seconds) {
		double t = now() - start;
		memset(fb, 0, sizeof(fb));
		for (int y = 0; y < H; y += 40)
			fill(0, y, W - 1, y, rgb(0, 0, 70));
		for (int x = 0; x < W; x += 40)
			fill(x, 0, x, H - 1, rgb(0, 0, 70));
		for (int k = 0; k < 3; k++) {
			double amp = 70 + 50 * sin(t * 0.7 + k * 2);
			int prev = -1;
			for (int x = 0; x < W; x++) {
				int y = H / 2 + amp * sin(x * (0.012 + 0.006 * k) + t * (2.0 + k * 0.7) + k);
				int a = prev < 0 ? y : prev < y ? prev : y, b = prev > y ? prev : y;
				fill(x, a - 1, x, b + 1, rgbf(col[k][0], col[k][1], col[k][2]));
				prev = y;
			}
		}
		if (have_font)
			draw_text("SINE WAVES", 8, 6, 0xFFFF);
		show();
	}
}

// --- Raster bars ---------------------------------------------------------

static void part_rasterbars(double seconds)
{
	enum { BARS = 6, BH = 18 };
	static const double col[BARS][3] = {{1, 0.2, 0.2}, {1, 0.6, 0.1}, {1, 1, 0.2},
					    {0.2, 1, 0.3}, {0.2, 0.6, 1}, {0.7, 0.3, 1}};
	double start = now();
	while (now() - start < seconds) {
		double t = now() - start;
		memset(fb, 0, sizeof(fb));
		// Draw back to front: bars whose cosine is negative are behind.
		int order[BARS];
		double z[BARS];
		for (int i = 0; i < BARS; i++) {
			order[i] = i;
			z[i] = cos(t * 1.6 + i * 0.5);
		}
		for (int i = 0; i < BARS; i++)
			for (int j = i + 1; j < BARS; j++)
				if (z[order[j]] < z[order[i]]) {
					int tmp = order[i];
					order[i] = order[j];
					order[j] = tmp;
				}
		for (int n = 0; n < BARS; n++) {
			int i = order[n];
			int yc = 175 + 110 * sin(t * 1.6 + i * 0.5);
			for (int j = -BH / 2; j <= BH / 2; j++) {
				double v = 1 - fabs((double)j) / (BH / 2 + 1);
				double hl = v > 0.85 ? (v - 0.85) * 4 : 0;
				uint16_t c = rgbf(col[i][0] * v + hl, col[i][1] * v + hl, col[i][2] * v + hl);
				int y = yc + j;
				if (y >= 40 && y < H)
					fill(40, y, W - 41, y, c);
			}
		}
		if (have_font)
			draw_text("RASTER BARS", 8, 6, 0xFFFF);
		show_interlaced();
	}
}

// --- Hardware scrolling landscape -----------------------------------------
//
// The controller's vertical scrolling (0x33/0x37) runs along the panel's
// native 480-line axis, which is our horizontal. Moving the scroll start
// shifts the whole picture for the cost of one command, so each frame only
// draws the two columns that come in on the right.

static const char *hw_text =
	"          HARDWARE SCROLLING ... THE PANEL MOVES THE PICTURE BY ITSELF, "
	"WE ONLY DRAW TWO NEW COLUMNS PER FRAME ...          ";

static uint32_t hash2(uint32_t a, uint32_t b)
{
	uint32_t h = a * 374761393u + b * 668265263u;
	h = (h ^ (h >> 13)) * 1274126177u;
	return h ^ (h >> 16);
}

static void scene_column(long pos, uint16_t *col)
{
	int far = 175 - 30 * (0.5 + 0.5 * sin(pos * 0.0045)) - 18 * sin(pos * 0.013 + 1);
	int near = 262 - 18 * sin(pos * 0.021) - 12 * sin(pos * 0.0071 + 2);
	long moon_x = pos / 1400 * 1400 + 900;
	long tp = pos % ((long)strlen(hw_text) * FW * 2);
	int ch = hw_text[tp / (FW * 2)], gx = tp % (FW * 2) / 2;
	int ty = 28 + 20 * sin(pos * 0.012);

	for (int y = 0; y < H; y++) {
		uint16_t c;
		if (y < far) {
			double h = y / 200.0;
			long dx = pos - moon_x, dy = y - 75;
			if (dx * dx + dy * dy < 30 * 30)
				c = rgb(240, 230, 190);
			else if (hash2(pos, y) % 700 == 0)
				c = 0xFFFF;
			else
				c = rgbf(0.03 + 0.4 * h, 0.03 + 0.12 * h, 0.12 + 0.25 * h);
		} else if (y < near) {
			c = rgbf(0.22 - (y - far) * 0.001, 0.1, 0.3);
		} else {
			c = (y - near) % 12 < 2 ? rgb(10, 80, 70) : rgb(5, 45, 45);
		}
		if (have_font && y >= ty && y < ty + 2 * FH && glyph_bit(ch, gx, (y - ty) / 2)) {
			double h = (double)(y - ty) / (2 * FH);
			c = rgbf(1, 0.45 + 0.55 * h, 0.15 + 0.6 * (1 - h));
		}
		col[y] = c;
	}
}

static void push_column(int x, const uint16_t *col)
{
	window(x, 0, x, H - 1);
	for (int y = 0; y < H; y++)
		word(CTL_DATA, col[y] >> 8, col[y] & 0xFF);
}

static void part_hwscroll(double seconds)
{
	uint16_t col[H];
	long pos = 0;
	int v = 0;

	CMD(0x33, 0x00, 0x00, W >> 8, W & 0xFF, 0x00, 0x00); // all 480 lines scroll
	CMD(0x37, 0x00, 0x00);
	for (int x = 0; x < W; x++, pos++) {
		scene_column(pos, col);
		push_column(x, col);
	}
	double start = now(), next = start;
	while (now() - start < seconds) {
		// Redraw the columns at the left edge; after the scroll they are
		// the new ones on the right.
		for (int i = 0; i < 2; i++, pos++) {
			scene_column(pos, col);
			push_column(v, col);
			v = (v + 1) % W;
		}
		CMD(0x37, v >> 8, v & 0xFF);
		total_frames++;
		next += 1.0 / 60;
		double wait = next - now();
		if (wait > 0)
			usleep(wait * 1e6);
		else
			next = now();
	}
	CMD(0x37, 0x00, 0x00);
	// The panel memory is now rotated against the screen: restart from black.
	memset(fb, 0, sizeof(fb));
	memset(shadow, 0, sizeof(shadow));
	push_rows(fb, 0, H - 1);
}

// --- Plasma and blobs, in a box ------------------------------------------

#define BX 120
#define BY 70
#define BW 240
#define BHT 160

static void box_frame(const char *title)
{
	memset(fb, 0, sizeof(fb));
	fill(BX - 3, BY - 3, BX + BW + 2, BY - 2, 0xFFFF);
	fill(BX - 3, BY + BHT + 1, BX + BW + 2, BY + BHT + 2, 0xFFFF);
	fill(BX - 3, BY - 3, BX - 2, BY + BHT + 2, 0xFFFF);
	fill(BX + BW + 1, BY - 3, BX + BW + 2, BY + BHT + 2, 0xFFFF);
	if (have_font)
		draw_text(title, (W - (int)strlen(title) * FW) / 2, BY - 44, 0xFFFF);
}

static void part_plasma(double seconds)
{
	uint16_t pal[256];
	for (int i = 0; i < 256; i++) {
		double a = 2 * M_PI * i / 256;
		pal[i] = rgbf(0.5 + 0.5 * sin(a), 0.5 + 0.5 * sin(a + 2.1), 0.5 + 0.5 * sin(a + 4.2));
	}
	double start = now();
	while (now() - start < seconds) {
		double t = now() - start;
		box_frame("PLASMA");
		double cx = BW / 2 + BW / 2 * sin(t * 0.9), cy = BHT / 2 + BHT / 2 * cos(t * 1.3);
		for (int y = 0; y < BHT; y++)
			for (int x = 0; x < BW; x++) {
				double dx = x - cx, dy = y - cy;
				double v = sin(x * 0.045 + t * 1.5) + sin(y * 0.06 - t * 1.1) +
					   sin((x + y) * 0.03 + t * 0.8) + sin(sqrt(dx * dx + dy * dy) * 0.07 - t * 2);
				// 16 colour bands: only pixels at moving band edges change
				fb[(BY + y) * W + BX + x] = pal[(int)((v + 4) * 32 + t * 40) & 0xF0];
			}
		show();
	}
}

static void part_blobs(double seconds)
{
	enum { N = 4 };
	uint16_t pal[256];
	for (int i = 0; i < 256; i++) {
		double v = i / 255.0;
		pal[i] = rgbf(v * 3, v * 3 - 1, v * 3 - 2); // black, red, yellow, white
	}
	double start = now();
	while (now() - start < seconds) {
		double t = now() - start;
		box_frame("BLOBS");
		double bx[N], by[N];
		for (int i = 0; i < N; i++) {
			bx[i] = BW / 2 + (BW / 2 - 25) * sin(t * (0.8 + i * 0.3) + i * 1.7);
			by[i] = BHT / 2 + (BHT / 2 - 20) * cos(t * (1.1 + i * 0.2) + i * 2.3);
		}
		for (int y = 0; y < BHT; y++)
			for (int x = 0; x < BW; x++) {
				double f = 0;
				for (int i = 0; i < N; i++) {
					double dx = x - bx[i], dy = y - by[i];
					f += 500 / (dx * dx + dy * dy + 1);
				}
				int c = f * 128;
				fb[(BY + y) * W + BX + x] = pal[c > 255 ? 255 : c];
			}
		show();
	}
}

int main(int argc, char **argv)
{
	int runs = argc > 1 ? atoi(argv[1]) : 1;
	const char *cdiv = getenv("KEDEI_SPI_CDIV");
	if (cdiv && atoi(cdiv) >= 2)
		spi_cdiv = atoi(cdiv) & ~1u;
	load_font();
	if (!have_font)
		fprintf(stderr, "no font at %s, running without text\n", FONT_PATH);
	hw_init();
	lcd_init();
	memset(fb, 0, sizeof(fb));
	memset(shadow, 0, sizeof(shadow));
	push_rows(fb, 0, H - 1);
	fps_at = now();
	static const struct {
		const char *name;
		void (*run)(double);
		double seconds;
	} parts[] = {{"starfield + scroller", part_starfield, 20}, {"hardware scrolling", part_hwscroll, 20},
		     {"sine waves", part_sines, 10}, {"raster bars", part_rasterbars, 12},
		     {"plasma", part_plasma, 12}, {"blobs", part_blobs, 12}};
	for (int run = 0; runs == 0 || run < runs; run++)
		for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
			int frames = total_frames;
			double t = now();
			parts[i].run(parts[i].seconds);
			printf("%-22s %5.1f fps\n", parts[i].name, (total_frames - frames) / (now() - t));
			fflush(stdout);
		}
	return 0;
}
