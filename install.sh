#!/bin/bash
# Installs the KeDei 3.5" v3 LCD driver on Raspberry Pi OS (Trixie).
#
#   sudo ./install.sh desktop    driver + Raspberry Pi desktop (X11) on the LCD
#   sudo ./install.sh console    driver only: the text console on the LCD
#   sudo ./install.sh uninstall  remove everything (calibration is kept)
#
# After installing, reboot and calibrate the touchscreen:
#   sudo systemctl stop kedei3-lcd && sudo kedei3-lcd calibrate && sudo systemctl start kedei3-lcd
set -euo pipefail

MODE=${1:-desktop}
SRC=$(cd "$(dirname "$0")" && pwd)
VIDEO_ARG="video=HDMI-A-1:480x320M@60D"

BOOT=/boot/firmware
[ -d "$BOOT" ] || BOOT=/boot
CONFIG_TXT=$BOOT/config.txt
CMDLINE_TXT=$BOOT/cmdline.txt
XCONF=/etc/X11/xorg.conf.d
NOGLAMOR_CONF=/usr/share/X11/xorg.conf.d/20-noglamor.conf

REBOOT=0

log() { echo "==> $*"; }

need_root() {
	if [ "$(id -u)" -ne 0 ]; then
		echo "run as root: sudo $0 $MODE" >&2
		exit 1
	fi
}

# The driver drives SPI0 directly, so the kernel SPI driver must stay off.
disable_kernel_spi() {
	if grep -qE '^\s*dtparam=spi=on' "$CONFIG_TXT"; then
		log "disabling kernel SPI in $CONFIG_TXT"
		sed -i -E 's/^(\s*dtparam=spi=on)/#\1  # disabled by kedei3-lcd/' "$CONFIG_TXT"
		REBOOT=1
	fi
}

# Without a monitor vc4 creates no framebuffer; force a 480x320 one to mirror.
cmdline_add_video() {
	if ! grep -q "$VIDEO_ARG" "$CMDLINE_TXT"; then
		log "adding $VIDEO_ARG to $CMDLINE_TXT"
		sed -i "1s|\$| $VIDEO_ARG|" "$CMDLINE_TXT"
		REBOOT=1
	fi
}

cmdline_remove_video() {
	if grep -q "$VIDEO_ARG" "$CMDLINE_TXT"; then
		log "removing $VIDEO_ARG from $CMDLINE_TXT"
		sed -i "s| $VIDEO_ARG||" "$CMDLINE_TXT"
		REBOOT=1
	fi
}

install_driver() {
	log "building kedei3-lcd"
	if ! command -v gcc >/dev/null; then
		DEBIAN_FRONTEND=noninteractive apt-get install -y -qq gcc libc6-dev >/dev/null
	fi
	gcc -O2 -Wall -Wextra -o /usr/local/bin/kedei3-lcd "$SRC/src/kedei3-lcd.c"
	install -m 644 "$SRC/systemd/kedei3-lcd.service" /etc/systemd/system/
	disable_kernel_spi
	cmdline_add_video
	systemctl daemon-reload
	systemctl enable kedei3-lcd.service
}

# X has to render into /dev/fb0 with the fbdev driver. On Pi 0-3,
# glamor-test.service writes 20-noglamor.conf at every boot; its modesetting
# Device gets attached to our screen as a GPU and X refuses to start ("Cannot
# run in framebuffer mode"). 99-v3d.conf (rewritten by rp1-test.service) is
# harmless next to the explicit layout in 99-kedei.conf.
install_desktop() {
	if [ ! -x /usr/bin/raspi-config ] || [ ! -e /usr/share/xsessions/rpd-x.desktop ]; then
		echo "desktop mode needs Raspberry Pi OS with desktop (rpd-x session not found)" >&2
		exit 1
	fi
	DEBIAN_FRONTEND=noninteractive apt-get install -y -qq xserver-xorg-video-fbdev >/dev/null
	install -d "$XCONF"
	install -m 644 "$SRC/x11/99-kedei.conf" "$XCONF/"
	systemctl mask glamor-test.service >/dev/null 2>&1 || true
	rm -f "$NOGLAMOR_CONF"
	log "switching the desktop to X11"
	raspi-config nonint do_wayland W1
	REBOOT=1
}

remove_desktop() {
	rm -f "$XCONF/99-kedei.conf"
	systemctl unmask glamor-test.service >/dev/null 2>&1 || true
}

mode_uninstall() {
	systemctl disable --now kedei3-lcd.service 2>/dev/null || true
	rm -f /etc/systemd/system/kedei3-lcd.service /usr/local/bin/kedei3-lcd
	systemctl daemon-reload
	remove_desktop
	cmdline_remove_video
	log "removed (calibration kept in /etc/kedei-touch.conf)"
	log "the desktop still runs on X11; switch back with: sudo raspi-config nonint do_wayland W2"
}

need_root
case "$MODE" in
desktop)
	install_driver
	install_desktop
	;;
console)
	install_driver
	;;
uninstall) mode_uninstall ;;
*)
	echo "usage: $0 [desktop|console|uninstall]" >&2
	exit 2
	;;
esac

if [ "$REBOOT" -eq 1 ]; then
	log "done: reboot to apply (sudo reboot), then calibrate the touchscreen"
else
	systemctl restart kedei3-lcd.service 2>/dev/null || true
	log "done"
fi
