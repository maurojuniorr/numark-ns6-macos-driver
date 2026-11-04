ARCH ?= x86_64
MACOSX_DEPLOYMENT_TARGET ?= 10.15
LIBUSB_CFLAGS := $(shell pkg-config --cflags libusb-1.0)
LIBUSB_LIBS := $(shell pkg-config --libs libusb-1.0)
CFLAGS := -Wall -Wextra -Werror -arch $(ARCH) -mmacosx-version-min=$(MACOSX_DEPLOYMENT_TARGET) $(LIBUSB_CFLAGS)

ns6-clock-probe: ns6-clock-probe.c
	$(CC) $(CFLAGS) $< -o $@ $(LIBUSB_LIBS) -lm

clean:
	rm -f ns6-clock-probe
