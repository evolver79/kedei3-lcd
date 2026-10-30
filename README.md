# kedei3-lcd

Driver for the **KeDei 3.5" SPI TFTLCD 480×320, Version 3.0 (2015/12/1)**
touchscreen on current Raspberry Pi OS (Debian 13 "Trixie", 64-bit).

KeDei only ever shipped this board with a patched 2015 Raspbian kernel, and
none of the usual drivers (`piscreen`, `waveshare35a`, LCD-show, fbtft
ILI9486) drive it: the screen stays white. `kedei3-lcd` is a small userspace
driver that talks to the board directly, so it needs no kernel module and
survives kernel updates.

- Mirrors `/dev/fb0` to the LCD, pushing only the rows that change: the boot
  console, and in desktop mode the Raspberry Pi desktop (X11).
- Exposes the resistive touchscreen as the input device "KeDei Touch", with
  on-screen calibration.

Tested on a Raspberry Pi 3 Model B with Raspberry Pi OS Trixie (2026-10-06,
kernel 6.18). Other KeDei revisions use different protocols; see
[Other KeDei boards](#other-kedei-boards).

## Identify your board

The back of the PCB says `3.5 inch SPI TFTLCD 480*320` and `Version 3.0
2015/12/1`. The board plugs onto the first 26 GPIO pins.

## Install

On Raspberry Pi OS **with desktop**:

```sh
git clone https://github.com/evolver79/kedei3-lcd.git
cd kedei3-lcd
sudo ./install.sh desktop    # or "console" for the text console only
sudo reboot
```

Then calibrate the touchscreen (tap the four crosses):

```sh
sudo systemctl stop kedei3-lcd && sudo kedei3-lcd calibrate && sudo systemctl start kedei3-lcd
```

`sudo ./install.sh uninstall` removes everything again.

### What the installer changes

- Builds `/usr/local/bin/kedei3-lcd` and enables `kedei3-lcd.service`.
- Comments out `dtparam=spi=on` if set: the driver uses SPI0 directly.
- Adds `video=HDMI-A-1:480x320M@60D` to `cmdline.txt`, so the Pi creates a
  480×320 framebuffer even without a monitor.
- Desktop mode only: switches the desktop from Wayland to X11
  (`raspi-config nonint do_wayland W1`), installs
  `/etc/X11/xorg.conf.d/99-kedei.conf` so X renders into `/dev/fb0`, and masks
  `glamor-test.service`, whose `20-noglamor.conf` stops X from starting on the
  framebuffer. Don't switch back to Wayland: the compositor would bypass
  `/dev/fb0` and the LCD would only show the console.

## Performance

A full-screen redraw takes about 0.5 s at the default 25 MHz SPI clock;
typical desktop updates (menus, terminal text) only redraw the changed rows
and feel responsive. Video and games are not realistic. To try a faster
clock, add `Environment=KEDEI_SPI_CDIV=8` (50 MHz) with
`sudo systemctl edit kedei3-lcd` and check the picture for glitches.

## Usage

```
kedei3-lcd test             init and draw colour bars
kedei3-lcd daemon <src>     mirror a framebuffer device or raw 480x320 RGB565 file
kedei3-lcd calibrate        touch calibration, saved to /etc/kedei-touch.conf
```

Environment: `KEDEI_SPI_CDIV` (SPI clock divider, default 16) and
`KEDEI_INIT` (`v63` panel settings, the default, or `v11`).

Stop the service before running `test` or `calibrate`: only one program may
drive SPI0 at a time.

## Other KeDei boards

- **v6.3 (2018/4/9)**: use [kolomiichenko/kedei-lcd](https://github.com/kolomiichenko/kedei-lcd).
- **Anything else**: `tools/kprobe` tries the known bus framings and draws a
  different number of white bars for each, so you can see which one your board
  speaks; `tools/ktouch` finds the touch controller wiring. Build them with
  `make` and stop the service first. Issues and pull requests with results
  for other revisions are welcome.

The protocol is documented in [docs/PROTOCOL.md](docs/PROTOCOL.md).

## Credits

- [kolomiichenko/kedei-lcd](https://github.com/kolomiichenko/kedei-lcd): the
  v6.3 userspace driver this one is based on (register-level SPI, touch,
  uinput, X11 setup) and its panel init sequence.
- [saper-2/rpi-spi-lcd35-kedei](https://github.com/saper-2/rpi-spi-lcd35-kedei):
  the v1.1 schematic and bus framing that the v3 board turned out to share.

## License

GPL-2.0-only, see [LICENSE](LICENSE).
