CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra

.PHONY: all clean

all: kedei3-lcd kprobe ktouch

kedei3-lcd: src/kedei3-lcd.c
	$(CC) $(CFLAGS) -o $@ $<

kprobe: tools/kprobe.c
	$(CC) $(CFLAGS) -o $@ $<

ktouch: tools/ktouch.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f kedei3-lcd kprobe ktouch
