# Patches to the third-party code

`third_party/kepyur` is a git submodule (kepyur, with its own submodules wifi-sdr, devourer and libusb). Git
records only which upstream commit each one is at; this project's changes to them live here. Apply them to a
fresh checkout (after `git submodule update --init --recursive`):

| Patch | Apply in | What |
|---|---|---|
| `kepyur-0001-comb-luma.patch` … `kepyur-0007-hd-picture.patch` (in order) | `third_party/kepyur` | video decoder: comb filter, sparkle repair, adaptive comb, smooth lines, low latency (line sink, pictures built on their own thread); log format checking on MinGW; HD / Sharp OSD picture per front end (PictureOpts: steady-picture denoise, channel filter, clean OSD colour, still weave); weave field order fixed |
| `wifi-sdr-0001-windows.patch` | `third_party/kepyur/third_party/wifi-sdr` | Windows streaming, late banks behind a USB hub, centre hint, firmware table offload (fast bring-up), late banks dropped mid-run instead of a restart; from fpvDROID (Auburn/fpvDROID#7): USB 3 C2H wedge freed in place by the onboard program, freed packets put back together, late banks skipped, 10 ms restart pause; Play League: `set_usb3_recovery()` turns the in-place recovery on for every platform (default on). **Applies to wifi-sdr 9433c1f** (check it out first) |
| `devourer-0001-windows-usb.patch` | `third_party/kepyur/third_party/wifi-sdr/external/devourer` | synchronous EP0 writes, retries, circuit breaker |
| `libusb-0001-winusb-raw-io.patch` | `third_party/kepyur/third_party/libusb` | WinUSB RAW_IO (Windows only) |

    cd third_party/kepyur && git apply ../../patches/kepyur-000*.patch
    cd third_party/wifi-sdr && git checkout 9433c1f && git apply ../../../../patches/wifi-sdr-0001-windows.patch
    cd external/devourer && git apply ../../../../../../patches/devourer-0001-windows-usb.patch
    cd ../../../libusb && git apply ../../../../patches/libusb-0001-winusb-raw-io.patch

Regenerate one after changing that code: `git -C <submodule> diff --ignore-submodules > patches/<name>.patch`.
