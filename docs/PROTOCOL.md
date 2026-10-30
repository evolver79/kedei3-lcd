# KeDei 3.5" SPI TFTLCD Version 3.0 (2015/12/1) protocol

Found by probing the board with `tools/kprobe` and `tools/ktouch` on a
Raspberry Pi 3 Model B running Raspberry Pi OS Trixie (kernel 6.18). KeDei
never published source for this revision; the only vendor driver is a patched
2015 Raspbian kernel.

## Board

Same "classic KeDei" design as the v1.1 board (see the reverse-engineered
schematic in [saper-2/rpi-spi-lcd35-kedei](https://github.com/saper-2/rpi-spi-lcd35-kedei/tree/master/lcd-module-info/v11)):

- 3 × 74HC595 shift registers (24 bits) drive the LCD's parallel bus. Their
  latch (STCP) is wired to CE1.
- XPT2046 resistive touch controller.
- Only the SPI pins of the 26-pin header are used:

| Signal | Header pin | BCM GPIO | Use |
|---|---|---|---|
| MOSI | 19 | 10 | LCD shift registers and touch controller |
| MISO | 21 | 9 | touch controller replies |
| SCLK | 23 | 11 | clock |
| CE0 | 24 | 8 | touch controller chip select |
| CE1 | 26 | 7 | LCD shift register latch |
| PENIRQ | 22 | 25 | low while the screen is pressed |

The backlight is always on.

## LCD frames

SPI0 **mode 3**. Every 16-bit word (command, parameter or pixel) is sent as
two 4-byte frames, each in its own CE1 assertion; CE1 going high latches the
frame into the 595s:

```
00 <hi> <lo> <ctl>
00 <hi> <lo> <ctl | 0x0A>
```

The first byte falls off the end of the 24-bit chain. Control byte bits:

| Bit | Meaning |
|---|---|
| 0 | RESET, active low |
| 1, 3 | WR strobe: low in the first frame, high in the second, which stores the word |
| 2 | D/C: 0 = command, 1 = data |
| 4 | always 1 |

So a command is `ctl = 0x11`, a parameter or pixel is `ctl = 0x15`, and reset
is a single frame with `ctl = 0x00`, released with `ctl = 0x01`.

Mode 0 does not work. The v6.3 framing (`00 <ctl> <hi> <lo>`, latched by a
pulse on CE0) does not work either.

### Clock

25 MHz (CDIV 16 at the Pi 3's 400 MHz core clock) is the driver's default. A
full 480×320 frame then takes 0.52 s. 50 MHz (CDIV 8) takes 0.29 s but has
not been checked for corrupted frames; set `KEDEI_SPI_CDIV` to try it.

## LCD init

The v1.1 board's sequence (ILI9481-style plus ILI9486-style gamma) works but
leaves the picture pale with a visible line pattern. The sequence from KeDei's
v6.3 kernel gives clearly better contrast and is the default:

```
reset asserted, 50 ms
reset released, 150 ms
CMD 00                                  10 ms
CMD FF × 6                              15 ms
CMD 11 (sleep out)                      150 ms
CMD B0 00
CMD B3 02 00 00 00
CMD B9 01 00 0F 0F
CMD C0 13 3B 00 02 00 01 00 43
CMD C1 08 0F 08 08
CMD C4 11 07 03 04
CMD C6 00
CMD C8 03 03 13 5C 03 07 14 08 00 21 08 14 07 53 0C 13 03 03 21 00
CMD 35 00
CMD 36 28                               landscape 480×320, BGR
CMD 3A 55                               RGB565
CMD 44 00 01
CMD D0 07 07 1D 03
CMD D1 03 30 10
CMD D2 03 14 04
CMD 29 (display on)                     30 ms
CMD B4 00
```

## Drawing

```
CMD 2B  y_start(16 bit)  0x013F
CMD 2A  x_start(16 bit)  0x01DF
CMD 2C
pixels…
```

Each 16-bit window value is sent as two parameter words (high byte, low
byte). The driver re-sends the window for every row it updates.

## Touch

XPT2046 on CE0, SPI **mode 0**, at most 2.5 MHz (the driver uses 2 MHz). A
conversion is `<cmd> 00 00`, and the 12-bit result is
`(rx[1] << 5) | (rx[2] >> 3)`:

| Command | Value |
|---|---|
| `D0` | X |
| `90` | Y |
| `B0` | Z1 (pressure) |

PD bits 00 keep PENIRQ enabled between conversions. A press is PENIRQ low
and Z1 ≥ 80. Each value is the mean of 6 conversions without the minimum and
maximum.

The LCD's shift registers see the touch bytes on MOSI too, but nothing is
latched while CE1 stays high, so no parking is needed (unlike v6.3).

On the tested board the touch axes are swapped relative to the landscape
screen (raw Y follows screen X, raw X decreases from top to bottom);
`kedei3-lcd calibrate` detects both.
