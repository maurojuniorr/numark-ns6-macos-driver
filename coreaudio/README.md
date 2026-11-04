# CoreAudio implementation

The HAL driver, persistent USB device service, CoreMIDI bridge, and Status app are built from this directory. Project overview, audio behavior, known limitations, and the Intel test package are documented in the [root README](../README.md).

Build an experimental package for the current Mac architecture by selecting it explicitly:

```sh
make NS6_ARCH=arm64 EXPERIMENTAL_VERSION=1.0.13 experimental-package
```

For Intel, set `NS6_ARCH=x86_64`. Each build produces an architecture-specific package; this target does not combine both slices into a single universal binary. The package target runs the capture timeline, fanout, HAL boundary, service lifecycle, and mapping checks before packaging the driver and supporting services.
