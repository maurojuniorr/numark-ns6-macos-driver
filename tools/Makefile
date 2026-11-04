SDK := $(shell xcrun --show-sdk-path)
ARCH ?= x86_64
MACOSX_DEPLOYMENT_TARGET ?= 10.15
CFLAGS := -Wall -Wextra -Werror -arch $(ARCH) -isysroot $(SDK) -mmacosx-version-min=$(MACOSX_DEPLOYMENT_TARGET)
LDFLAGS := -framework IOKit -framework CoreFoundation
LIBUSB_CFLAGS := $(shell pkg-config --cflags libusb-1.0)
LIBUSB_LIBS := $(shell pkg-config --libs libusb-1.0)

ns6-probe: ns6-probe.c
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

ns6-libusb-probe: ns6-libusb-probe.c
	$(CC) $(CFLAGS) $(LIBUSB_CFLAGS) $< -o $@ $(LIBUSB_LIBS)

ns6-isoc-schedule-probe: ns6-isoc-schedule-probe.c
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

ns6-native-tone: ns6-native-tone.c
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS) -lm

clean:
	rm -f ns6-probe ns6-libusb-probe ns6-isoc-schedule-probe ns6-native-tone
