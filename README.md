# RealSense Viewer for Apple Silicon Macs

Native **RealSense Viewer** (and Depth Quality Tool + command line tools) for **Apple Silicon** Macs running
**macOS 27 "Golden Gate" or newer**, for Intel® RealSense™ depth cameras such as the **D455** connected
directly to the Mac with a USB‑C cable.

It is the official [librealsense](https://github.com/realsenseai/librealsense) **v2.58.4** viewer — the same
application and the same features as on Windows/Linux — built natively for `arm64` with a set of macOS fixes.
Upstream librealsense states that on macOS *"RealSense Viewer is not supported"* and *"Motion sensors (IMU)
are disabled"*; this project makes both work.

> 🇵🇱 **Po polsku:** natywna wersja RealSense Viewer dla Maców z procesorem Apple Silicon i macOS 27
> (Golden Gate) lub nowszym, dla kamer Intel RealSense (testowana z D455 podłączoną kablem USB‑C bezpośrednio
> do Maca). Ma te same funkcje co oryginalny Viewer: podgląd głębi/IR/RGB w 2D i 3D, chmura punktów, IMU
> (akcelerometr + żyroskop), nagrywanie i odtwarzanie, eksport PLY/PNG/CSV, filtry post‑processingu,
> presety, kalibracja on‑chip, aktualizacja firmware. Instalacja: otwórz plik DMG i przeciągnij
> „RealSense Viewer” do Aplikacji. Przy każdym uruchomieniu macOS poprosi o hasło administratora — bez tego
> system nie pozwala przejąć kamery od wbudowanego sterownika USB wideo.

## Features

Everything RealSense Viewer offers, running natively on Apple Silicon:

| | |
|---|---|
| Streams | Depth, Infrared (left/right), RGB, **IMU** (accelerometer, gyroscope) — all simultaneously |
| Views | 2D and 3D (point cloud with texture), measurements, metadata overlay, histograms |
| Controls | All sensor options (exposure, laser power, gain, presets, advanced mode, HDR, …) |
| Processing | Decimation, threshold, spatial/temporal filters, hole filling, disparity, alignment |
| Recording | Record to `.db3`/`.bag`, playback, export snapshots (PNG, RAW, CSV) and point clouds (PLY) |
| Device | Firmware info & update, hardware reset, on‑chip / focal‑length / tare calibration, firmware logs |
| Extras | RealSense Depth Quality Tool, `rs-*` command line tools, `rs-macos-selftest` hardware self‑test |

Tested on a MacBook Pro M4 Pro, macOS 27.0.1, RealSense **D455** (firmware 5.16.0.1) over USB 3.2:
depth + IR + RGB at 30 fps and IMU at 400 Hz, simultaneously.

## Install

1. Download `RealSense-Viewer-<version>-macOS-arm64.dmg` from
   [Releases](../../releases) (or build it yourself, see below).
2. Open the DMG and drag **RealSense Viewer** (and optionally **RealSense Depth Quality Tool**) to
   *Applications*.
3. The apps are signed ad‑hoc (no paid Apple developer certificate). On first launch macOS may refuse to
   open a downloaded copy — go to *System Settings → Privacy & Security* and click **Open Anyway**, or run
   `xattr -dr com.apple.quarantine "/Applications/RealSense Viewer.app"`.
4. Connect the camera directly with a USB‑C (USB 3) cable and start **RealSense Viewer**.

### Why does it ask for the administrator password?

macOS attaches its own drivers to RealSense cameras (`UVCAssistant` for the video interfaces and the HID
driver for the IMU). The SDK talks to the camera through libusb, which has to *capture* the USB device from
those drivers — and macOS only allows that for root (or for apps holding a restricted Apple entitlement).
The app therefore asks for your password with the standard macOS dialog, runs the viewer with administrator
rights in your session, and when you quit:

* gives the camera back to macOS (so the RGB camera works again as a webcam in FaceTime, Zoom, …),
* returns ownership of the settings file and of files saved to *Documents* to your user.

## Command line tools

`RealSense Tools/` in the DMG contains `rs-enumerate-devices`, `rs-fw-update`, `rs-data-collect`,
`rs-convert`, `rs-record`, the examples and two macOS helpers:

```bash
sudo "RealSense Tools/bin/rs-macos-selftest"     # checks enumeration, options, depth/IR/RGB/IMU streaming
sudo "RealSense Tools/bin/rs-macos-release"      # gives the camera back to macOS after using a CLI tool
```

## Build from source

Requirements: Apple Silicon Mac, macOS 27+, Xcode 27 or the Command Line Tools (`xcode-select --install`),
CMake (`brew install cmake`).

```bash
git clone --recurse-submodules https://github.com/dolegadolegowski/realsense-viewer-macos.git
cd realsense-viewer-macos
scripts/build.sh      # applies patches/ to librealsense v2.58.4 and builds everything into build/
scripts/package.sh    # creates dist/*.app, dist/RealSense Tools and the DMG
```

`VIEWER_TESTS=ON scripts/build.sh` additionally builds `realsense-viewer-tests`, the upstream GUI test suite
(ImGui Test Engine) that drives the real viewer UI: `sudo build/Release/realsense-viewer-tests --auto`.

## What was changed in librealsense

All changes are in [`patches/0001-macos-apple-silicon-support.patch`](patches/0001-macos-apple-silicon-support.patch):

* **Viewer enabled on macOS** — `realsense-viewer` was excluded from macOS builds.
* **USB device capture** — libusb on macOS captures the *whole* device and, with auto‑detach, re‑attaches the
  system drivers (re‑enumerating the device) whenever *any* interface is released, killing the streams of the
  other sensors. The camera is now captured once, explicitly, for the lifetime of the process, and a clear
  error is reported when administrator rights are missing.
* **IMU / motion module** — the half‑finished hidapi code path is replaced by the regular libusb HID backend
  (the same one used on Linux with the RSUSB backend); accelerometer and gyroscope stream at 200/400 Hz.
* **libusb 1.0.30** (statically linked; needs the Security framework on macOS).
* **GLFW as a shared library** on macOS — it was linked statically into both `librealsense2-gl` and the
  tools, duplicating its Objective‑C classes (two GLFW states in one process).
* **Dear ImGui OpenGL backend on legacy OpenGL 2.1** (the context type macOS gives the fixed‑function
  viewer): GLSL 1.20 shaders, no core‑profile VAOs, no GL 3.0‑only queries. Before, the UI shaders failed to
  compile and every frame raised `GL_INVALID_OPERATION`, which the viewer showed as a blocking error.
* **Clean shutdown on SIGINT/SIGTERM**, so devices are stopped and released when the app is quit from outside.
* libusb error names in transfer warnings (the logs printed an unrelated `errno`).

macOS‑specific additions in this repository: the app launcher (`macos/launcher`), `rs-macos-release`
(re‑enumerates the camera to hand it back to macOS) and `rs-macos-selftest`.

## Known limitations

* An administrator password is needed every time the viewer starts (macOS USB security, see above).
* Toggling the RGB stream while depth is streaming makes the camera firmware restart the depth stream
  briefly (frame counters reset) — this is camera behaviour, not specific to macOS.
* "Use GLSL for rendering/processing" options are not available on macOS (upstream limitation of the
  legacy OpenGL context).
* Files saved outside *Documents* while the viewer runs are owned by root (readable by you; you can delete
  them, or `sudo chown` them).
* Do not force‑quit (`kill -9`) the viewer while it talks to the camera: the camera firmware can hang and
  then needs to be re‑plugged.

## License

Apache License 2.0, like librealsense. RealSense is a trademark of RealSense, Inc.; Intel is a trademark of
Intel Corporation. This is an unofficial community build.
