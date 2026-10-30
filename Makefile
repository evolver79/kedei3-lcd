CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra

.PHONY: all clean

all: kedei3-lcd kprobe ktouch demoscene

kedei3-lcd: src/kedei3-lcd.c
	$(CC) $(CFLAGS) -o $@ $<

kprobe: tools/kprobe.c
	$(CC) $(CFLAGS) -o $@ $<

ktouch: tools/ktouch.c
	$(CC) $(CFLAGS) -o $@ $<

# Includes the driver source; its unused modes are expected.
demoscene: demos/demoscene.c src/kedei3-lcd.c
	$(CC) $(CFLAGS) -Wno-unused-function -o $@ $< -lm

clean:
	rm -f kedei3-lcd kprobe ktouch demoscene
