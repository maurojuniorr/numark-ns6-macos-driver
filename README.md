# Numark NS6 macOS Driver

An independent community driver for the original Numark NS6. This repository brings the Intel (`x86_64`) and Apple Silicon (`arm64`) implementations together in one source tree, including CoreAudio, CoreMIDI, the persistent USB service, the MIDI bridge, and the Status app. It is not an official Numark or Ploytec product.

The build selects one architecture at a time. The repository is unified, but the installer linked below is an Intel test package; it is not a fat, universal binary and is not intended for Apple Silicon.

## What works

- **Audio output:** 44.1 kHz, four channels: master on channels 1–2 and headphones on 3–4. CoreAudio clients choose their buffer period; the HAL reports the active period instead of forcing a fixed frame size.
- **Output clock and buffering:** asynchronous USB output follows the NS6 feedback endpoint with fractional frame scheduling. A bounded queue-occupancy correction bridges the hardware clock to CoreAudio's nominal 44.1 kHz clock. The USB worker maintains eight queued transfers, and a 4,096-frame queue target absorbs timing variation. These are internal transport settings, not the client's CoreAudio buffer period.
- **Audio input:** the NS6 supplies its own two-channel master/record mix over USB. The driver publishes it as an input device; it does not create the signal by copying the driver's output. Eight ordered asynchronous reads feed a shared sample timeline. The timeline waits for a 2,048-frame reserve, aligns reads to CoreAudio sample time, and applies bounded rate correction so multiple input clients read matching audio intervals.
- **MIDI and hardware control:** bidirectional MIDI, controller initialization, and communication recovery after USB interruptions. The package also includes the user-session MIDI bridge and the Status app.
- **Keep-alive and startup:** a persistent system service owns the USB connection and keeps the hardware clock active when audio clients stop. It sends digital silence while idle, so the NS6 can remain initialized without sending audible content. The MIDI bridge starts in the logged-in user session.
- **Compatibility observed in testing:** the experimental driver has been recognized by Mixxx and, in recorded tests, by Serato and VirtualDJ. Application and macOS-version coverage is not complete, so this is not a claim of official support.

## Current test build

The current experimental source is version **1.0.13**. The Intel installer is included for reproducible testing:

[Download Numark NS6 1.0.13 Intel experimental](packages/NumarkNS6-1.0.13-Intel-experimental.pkg)

SHA-256: `63224e77fa7bcba7c37e55444699b4da4b1e44d2ad3712db87ac8a2c4a45be36`

This local ad-hoc-signed package is not notarized. Installing it replaces existing driver components and restarts CoreAudio. An older Numark kernel extension can claim the controller's USB interface and block this driver; remove that conflict before testing.

## Known limitation

Opening or switching media in Opera has triggered CoreAudio input reconfiguration. The input capture can pause for roughly 400–450 ms during that reconfiguration. The shared timeline and reserve reduce discontinuities caused by scheduling variation, but they cannot prevent CoreAudio from stopping or reconfiguring an input stream. This has been observed with Opera; it is a CoreAudio/device-flow interaction, not a TikTok-specific behavior. Whether output artifacts occur in every such reconfiguration is still unconfirmed.

The 1.0.13 timeline was exercised with synthetic tests and a clean capture on Apple Silicon. Earlier Intel experiments verified playback, input, MIDI, and Serato recognition on macOS Monterey 12.7.6, but long-duration input capture and recovery have not been fully validated on Intel.

## Build

Build on a Mac with Xcode and the command-line tools. Select the target architecture explicitly:

```sh
cd coreaudio
make NS6_ARCH=arm64 EXPERIMENTAL_VERSION=1.0.13 experimental-package
```

For an Intel package, use `NS6_ARCH=x86_64`. The generated package is under `coreaudio/build-experimental/dist-1.0.13/`. The package target runs the capture timeline, fanout, HAL boundary, service lifecycle, and mapping checks before packaging the HAL driver, USB device service, MIDI bridge, Status app, and launch configuration.

## Project background

This work builds on the macOS Numark NS6 driver, Linux NS6 protocol research, hardware analysis, and community testing. Thanks to [Gregory Senay](https://github.com/GregorySenay) for independent protocol research.

When reporting a problem, include the Mac model, macOS version, driver version, application, selected input/output devices, buffer settings, and approximate time of the event.
