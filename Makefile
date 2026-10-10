# Builds everything that targets the machine you are on (modern Linux):
# the hub, the X11 agent and the uinput agent.  The Windows, classic Mac and
# IRIX agents are built on or for those systems; see README.md.
CC     ?= cc
CFLAGS ?= -O2 -Wall -Wextra

all: hub build/rkm-x11 build/rkm-uinput

hub:
	$(MAKE) -C hub

build/rkm-x11: agents/x11/rkm_x11.c agents/x11/buddy_x11.h common/rkm_proto.c common/rkm_proto.h
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ agents/x11/rkm_x11.c common/rkm_proto.c -lXtst -lX11 -lm

build/rkm-uinput: agents/linux/rkm_uinput.c hub/uinput.c hub/keymap.c common/rkm_proto.c
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ agents/linux/rkm_uinput.c hub/uinput.c hub/keymap.c common/rkm_proto.c

build/xq: tests/xq.c
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ tests/xq.c -lX11

# Needs Xvfb.  Starts the hub, two X11 agents, simulated Mac and injector
# agents and a simulated Extron switcher, then checks the whole path.
test: all build/xq
	python3 tests/e2e.py

clean:
	$(MAKE) -C hub clean
	rm -rf build

.PHONY: all hub test clean
