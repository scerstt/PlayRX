# Patches to the third-party code

`third_party/kepyur` is a git submodule (kepyur, with its own submodules wifi-sdr, devourer and libusb). Git
records only which upstream commit each one is at; this project's changes to them live here. Apply them to a
fresh checkout (after `git submodule update --init --recursive`):

| Patch | Apply in | What |
|---|---|---|
| `kepyur-0001-comb-luma.patch` … `kepyur-0006-log-format.patch` (in order) | `third_party/kepyur` | video decoder: comb filter, sparkle repair, adaptive comb, smooth lines, low latency (line sink, pictures built on their own thread); log format checking on MinGW |
| `wifi-sdr-0001-windows.patch` | `third_party/kepyur/third_party/wifi-sdr` | Windows streaming, late banks behind a USB hub, centre hint, firmware table offload (fast bring-up), late banks dropped mid-run instead of a restart |
| `devourer-0001-windows-usb.patch` | `third_party/kepyur/third_party/wifi-sdr/external/devourer` | synchronous EP0 writes, retries, circuit breaker |
| `libusb-0001-winusb-raw-io.patch` | `third_party/kepyur/third_party/libusb` | WinUSB RAW_IO (Windows only) |
| `kepyur-0007-comb-top-line.patch`, `kepyur-0008-sparkle-repair-clicks.patch` (after kepyur 0006, in order) | `third_party/kepyur` | from [Auburn/fpvDROID](https://github.com/Auburn/fpvDROID): the first picture line in colour with the comb, and sparkle repair's click test |

    cd third_party/kepyur && git apply ../../patches/kepyur-000*.patch
    cd third_party/wifi-sdr && git apply ../../../../patches/wifi-sdr-0001-windows.patch
    cd external/devourer && git apply ../../../../../../patches/devourer-0001-windows-usb.patch
    cd ../../../libusb && git apply ../../../../patches/libusb-0001-winusb-raw-io.patch

Regenerate one after changing that code: `git -C <submodule> diff --ignore-submodules > patches/<name>.patch`.
