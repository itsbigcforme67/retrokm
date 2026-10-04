# RetroKM hub.  Needs only a C compiler and Linux headers.
CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra
OBJS    = main.o config.o keymap.o evdev.o uinput.o extron.o text.o rkm_proto.o

retrokm-hub: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) -lm

rkm_proto.o: ../common/rkm_proto.c ../common/rkm_proto.h
	$(CC) $(CFLAGS) -c -o $@ ../common/rkm_proto.c

%.o: %.c hub.h ../common/rkm_proto.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) retrokm-hub

.PHONY: clean
