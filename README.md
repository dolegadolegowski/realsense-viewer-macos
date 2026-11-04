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
> „RealSense Viewer” do Aplikacji. macOS pozwala przejąć kamerę od wbudowanego sterownika USB wideo tylko
> administratorowi (root), dlatego przy pierwszym uruchomieniu Viewer proponuje **„Install camera access for all
> users”**: administrator raz podaje nazwę i hasło, a instalowana jest mała usługa systemowa *realsense-usbd*
> (LaunchDaemon). Od tej chwili Viewer, Depth Quality Tool i narzędzia `rs-*` z tego samego DMG działają **bez
> hasła, także na kontach standardowych** (tylko użytkownik zalogowany przy ekranie Maca i root).
> **Prywatność:** obraz z kamery przez tę usługę nie zapala wskaźnika kamery macOS i nie wywołuje pytania o
> zgodę; respektowany jest tylko zakaz kamery z MDM/Czasu przed ekranem (`allowCamera`). **Bezpieczeństwo:**
> usługa wpuszcza tylko programy z tej samej kompilacji (lista skrótów cdhash) — to przypięcie wersji, nie
> izolacja przed złośliwym kodem działającym jako zalogowany użytkownik. Bez instalacji pozostaje dotychczasowe
> „Run once as administrator” (hasło przy każdym uruchomieniu). Odinstalowanie:
> `sudo "/Applications/RealSense Viewer.app/Contents/Resources/realsense-usbd/uninstall-camera-access.sh"`.

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

### Verified

On a MacBook Pro M4 Pro, macOS 27.0.1 (Golden Gate), RealSense **D455** (firmware 5.16.0.1) connected
directly over USB‑C (USB 3.2):

* the packaged **RealSense Viewer.app**: live depth/RGB in 2D and the textured point cloud in 3D;
* the upstream **realsense-viewer GUI test suite** (ImGui Test Engine driving the real UI) — **9/9 passed**:
  device detection, hardware reset, exposure, options filter, post‑processing, resolution selection,
  streaming all sensors, streaming each sensor, memory‑leak soak test;
* `rs-macos-selftest` — all checks pass: enumeration, hardware monitor, depth/RGB options, all 6 visual
  presets (~350 ms each), parallel access to all sensors, depth + 2×IR at 30 fps, RGB 1280×720 at 30 fps,
  accelerometer + gyroscope at 400 Hz, all sensors at once, recording to `.db3` and playback, repeated
  start/stop of RGB/IMU while depth streams;
* `rs-macos-selftest --stress` — all checks pass: 20 depth start/stop cycles (stop in 0.06 s), 30 recording
  start/stops while depth + RGB stream, all presets ×3 with laser power changes while streaming, every sensor for
  60 s (depth / IR / RGB at 30.0 fps, IMU at 100/200 Hz, no memory growth), context teardown in < 0.01 s, and a
  hardware reset after which the camera streams again;
* after quitting, the camera is handed back to macOS and works as a webcam again.

Not tested on purpose: flashing firmware and writing calibration to the camera.

## Install

1. Download `RealSense-Viewer-<version>-macOS-arm64.dmg` from
   [Releases](../../releases) (or build it yourself, see below).
2. Open the DMG and drag **RealSense Viewer** (and optionally **RealSense Depth Quality Tool**) to
   *Applications*.
3. The apps are signed ad‑hoc (no paid Apple developer certificate). On first launch macOS may refuse to
   open a downloaded copy — go to *System Settings → Privacy & Security* and click **Open Anyway**, or run
   `xattr -dr com.apple.quarantine "/Applications/RealSense Viewer.app"`.
4. Connect the camera directly with a USB‑C (USB 3) cable and start **RealSense Viewer**.
5. The first start offers **Install camera access for all users**: an administrator enters their name and
   password once (this works from a standard account too). From then on the viewer starts without any
   password, for every user of the Mac. See [Camera access](#camera-access-no-password).

### Why is an administrator needed?

macOS attaches its own drivers to RealSense cameras (`UVCAssistant` for the video interfaces and the HID
driver for the IMU). The SDK talks to the camera through libusb, which has to *capture* the USB device from
those drivers — and macOS only allows that for root (or for apps holding a restricted Apple entitlement).
There are two ways to give the viewer that right:

* **Camera access** (recommended): a small root service, installed once by an administrator, does the USB work
  for the viewer, which itself runs as you.
* **Run once as administrator**: the app asks for an administrator password with the standard macOS dialog,
  runs the viewer with administrator rights in your session, and when you quit gives the camera back to macOS
  (so the RGB camera works again as a webcam in FaceTime, Zoom, …) and returns ownership of the settings file and
  of files saved to *Documents* to your user. The app offers this when camera access is not installed or not
  usable.

## Camera access (no password)

`realsense-usbd` is a root LaunchDaemon (`io.github.dolegadolegowski.realsense-usbd`) that captures RealSense
cameras from macOS and forwards librealsense's USB requests to them for the programs of one build of this
project. librealsense asks it first and falls back to its normal in‑process libusb path (which needs root) when
it is not installed, switched off, or does not admit the program. Design: [`macos/broker/DESIGN.md`](macos/broker/DESIGN.md).

Status: new in this version. Its hardware checks (DESIGN.md 9.2) on the D455 are still to be done; the
*Verified* list above covers the *Run once as administrator* path.

**Install** (an administrator, once per Mac and per DMG version):

* start **RealSense Viewer** from */Applications* and click **Install camera access for all users** — the
  button is only offered when the app and every folder above it can be changed only by root or an administrator
  (so for a standard user not in `~/Applications` or `~/Downloads`, and not a translocated or quarantined copy run
  from the DMG); or
* from the DMG's Tools folder:
  `sudo "RealSense Tools/camera-access/install-camera-access.sh" --source "RealSense Tools/camera-access"`.

The installer refuses files that a non‑administrator could have changed, checks the daemon's code signature
against the allowlist, and writes:

| file | |
|---|---|
| `/Library/PrivilegedHelperTools/io.github.dolegadolegowski.realsense-usbd` | the daemon |
| `/Library/LaunchDaemons/io.github.dolegadolegowski.realsense-usbd.plist` | its launchd job (started on demand, exits when idle) |
| `/Library/Application Support/RealSense Viewer/usbd.plist` | allowlist of this build and optional administrator settings |

macOS then shows *"Background Items Added"*; the entry appears in *System Settings → General → Login Items &
Extensions*. An administrator can switch it off there (the app then offers *Open Login Items settings*); a
standard user cannot.

**Standard accounts:** after the installation every user can use the camera without a password, but only the
user logged in at the Mac's screen (and root) — not a second, fast‑user‑switched session in the background and
not an SSH login.

**Updates:** camera access admits exactly the programs of the DMG it was installed from (their code hashes).
After installing the apps from a newer DMG the app offers **Update camera access** (an administrator again).
Install the viewer, the Depth Quality Tool and the command line tools from the same DMG. Camera access is never
silently downgraded: an older app reports that camera access is newer than the app.

**Uninstall:** `sudo "/Applications/RealSense Viewer.app/Contents/Resources/realsense-usbd/uninstall-camera-access.sh"`
(or the same script in `RealSense Tools/camera-access/`). It stops the daemon, gives every camera it captured back
to macOS and removes the three files; the app then asks again at the next start.

**Administrator settings** (optional keys in `usbd.plist`, kept by updates): `HandBackGraceSeconds` (0–600,
default 5: how long a camera stays captured after the last program closed it), `IdleExitSeconds` (10–3600,
default 60), `DeniedHWMOpcodes` (array of hardware‑monitor opcodes to refuse) and `ProtectedFlashRanges` (array
of `{Start, Length}` flash byte ranges whose erase/write is refused). For D585S safety cameras, D500
`SAFETY_PRESET_WRITE` (`0x95`) and `CALIBRESTOREEPROM` (`0xA8`) are candidates for `DeniedHWMOpcodes`. Restart the
daemon after a change: `sudo launchctl kickstart -k system/io.github.dolegadolegowski.realsense-usbd`.

### Security model

* The daemon runs as root. It admits a program only if its code hash (cdhash) is in the allowlist of the
  installed build **and** it runs as root or as the user logged in at the Mac's screen in a local graphical
  session. Programs of other builds fall back to the root‑only in‑process path within about a second.
* The allowlist is **version pinning, not code containment**: anything running as the logged‑in user can start
  an allowlisted program, or copy an app and swap its (ad‑hoc signed) libraries — the cdhash covers only the
  main executable. Malware running as you can therefore use the camera through the daemon and reach its message
  parser; through the daemon it reaches nothing but RealSense cameras (no files, no network, no other devices).
* Only RealSense devices are ever opened (vendor `0x38E5`, and `0x8086` only with a RealSense product ID from
  librealsense), never a device with an audio, mass‑storage, hub, network, wireless or boot keyboard/mouse
  interface. Every request is validated; a program can only use interfaces it opened, and only class requests.
* Erasing the whole flash and permanently locking or unlocking it (hardware‑monitor opcodes `FEF`, `FSRU`,
  `FPLOCK`) are always refused. Everything else librealsense does is allowed — including firmware updates and
  writing calibration, so a standard user can make a camera need recovery unless `ProtectedFlashRanges` /
  `DeniedHWMOpcodes` are configured.
* The camera ban of an MDM profile or Screen Time (`com.apple.applicationaccess` `allowCamera = false`) blocks
  the daemon for everyone, root included, and ends running sessions.
* Command line tools that send raw hardware‑monitor commands (`rs-terminal`) or open no camera, and the build tree
  (`build/Release`), are not allowlisted: they need sudo as before.
* The installer only takes files from folders that only root/administrators can change, never from a
  translocated app, and generates the launchd job and settings itself instead of copying them.

### Privacy

* Depth, infrared and RGB streamed through the daemon do **not** light up the macOS camera indicator and do not
  trigger the camera permission prompt: macOS does not see a camera being used, only a USB device. Privacy
  (PPPC) profiles cannot target it; only the `allowCamera` ban above is honoured.
* The daemon logs to the unified log (subsystem `io.github.dolegadolegowski.realsense-usbd`): admitted and
  rejected programs (pid, uid, path), camera capture and hand‑back, device arrival/removal, refused commands —
  never image data or command payloads. `log show --last 1h --predicate 'subsystem ==
  "io.github.dolegadolegowski.realsense-usbd"'` shows them.
* Non‑RealSense webcams: the root path of the viewer also lists third‑party UVC webcams; through camera access
  they are not shown, because the daemon never captures devices other than RealSense cameras.

## Deploying to many Macs

`deploy-macs.sh` installs or updates the apps **and** camera access on many Macs at once over SSH. It runs on your
own Mac as your normal user (no sudo). For every target it logs in over SSH as an administrator account, and there,
with `sudo`, `remote-install.sh` downloads the release DMG from GitHub, checks it and installs it. Afterwards every
user of those Macs starts RealSense Viewer from *Launchpad* or *Applications* without a password.

**Prerequisites on every target Mac**

* Apple Silicon, macOS 27 or newer, internet access to `github.com` (or a DMG copied there, see `--dmg`);
* an administrator account that may log in over SSH: *System Settings → General → Sharing → Remote Login* on
  (*Allow access for* that account);
* on your Mac: the targets' SSH host keys accepted once (`ssh admin@mac-01.local true`), ideally an SSH key
  (`ssh-copy-id admin@mac-01.local`) so that only the sudo password is asked for.

**One command** (paste into Terminal on your Mac; replace `admin` and the host names). It downloads the deployment
scripts of this release (`rs-deploy-v2.58.4-macos.3.tar.gz`, a release asset next to the DMG) into a new private
temporary folder, checks their SHA-256 and runs them:

```bash
d="$(mktemp -d)" && curl -fsSL --proto '=https' --proto-redir '=https' -o "$d/rs-deploy.tar.gz" https://github.com/dolegadolegowski/realsense-viewer-macos/releases/download/v2.58.4-macos.3/rs-deploy-v2.58.4-macos.3.tar.gz && echo "be4370814c7827fe2733ad049d144d65244da3510c747b908acd70ea437c4db8  $d/rs-deploy.tar.gz" | shasum -a 256 -c - && tar -xzf "$d/rs-deploy.tar.gz" -C "$d" && "$d/rs-deploy/deploy-macs.sh" --ask-pass -l admin mac-01.local mac-02.local
```

`--ask-pass` asks for the administrator's sudo password once and uses it on every Mac; without it, `sudo` asks on each
Mac in turn. If the first Mac rejects the password, the others are skipped, so a typo is not tried everywhere. At
the end a summary shows `OK`, `FAILED` or `SKIPPED` per Mac (exit status 1 unless all are OK); the full output of
each Mac is in the log folder it prints.

More examples (`deploy-macs.sh` is `"$d/rs-deploy/deploy-macs.sh"` in the same Terminal window as the command
above, or `scripts/deploy-macs.sh` in a clone of this repository):

```bash
# hosts from a file: one [user@]host per line, "#" comments; 4 Macs at a time (needs SSH keys)
deploy-macs.sh --ask-pass -j 4 -l admin -f lab-macs.txt
# also install the command line tools to "/Applications/RealSense Tools"
deploy-macs.sh --ask-pass --tools -l admin -f lab-macs.txt
# only the apps, camera access unchanged
deploy-macs.sh --ask-pass --apps-only -l admin -f lab-macs.txt
# remove camera access; with --remove-apps also the apps and the tools
deploy-macs.sh --ask-pass --uninstall --remove-apps -l admin -f lab-macs.txt
# the password from the login keychain instead of typing it (store it once with:
#   security add-generic-password -s rs-deploy -a admin -w     which asks for it)
security find-generic-password -s rs-deploy -w | deploy-macs.sh --ask-pass -l admin -f lab-macs.txt
```

Other options: `--ssh-opt <opt>` (passed as `ssh -o <opt>`, e.g. `Port=2222`), `--tag <tag> --sha256 <hex>` (another
release), `--dmg <path on the targets>` (e.g. for Macs that reach the internet only through an HTTP proxy: the
download runs as root through `sudo`, which does not use the macOS proxy settings), `--allow-downgrade`,
`--log-dir <dir>` (a new private folder is created inside it); `deploy-macs.sh --help` lists all. On a single Mac the
same script also works by hand: `sudo bash remote-install.sh [--tools] [--apps-only]` or
`sudo bash remote-install.sh --uninstall [--remove-apps]`.

**What happens on each target:** the DMG of the pinned release is downloaded to a root-only temporary folder and its
SHA-256 is checked before anything is used; it is attached read-only and invisibly; the code signatures and bundle IDs
of both apps are checked; each app is copied into a new root-only folder inside `/Applications`, made `root:wheel`,
stripped of ACLs and quarantine, made read-only for others, checked again and only then renamed into place, replacing
the old version (with `--tools`, or when `/Applications/RealSense Tools` already exists, the same for the tools); then
the **installed** app's `install-camera-access.sh` installs or updates the `realsense-usbd` LaunchDaemon (see
[Camera access](#camera-access-no-password)). The user logged in at the screen gets the macOS notification
*"Background Items Added"*. If someone is using a RealSense program during an update, their camera stops and they
have to restart the app (the script warns). Running it again updates; a Mac whose camera access is newer than the
release is left unchanged and reported as `FAILED` (unless `--allow-downgrade`).

**Interrupted runs:** once a Mac has started, it finishes (or rolls back) on its own even if the SSH connection drops
or you press Ctrl-C (`--ask-pass` mode); camera access is never left half-installed. Its output after the drop is lost:
run the same command again to see the state of every Mac (re-running is safe). If a Mac reports a stale lock (only
after a crash or `kill -9`), the message names the `sudo rm -rf …` command that removes it; a restart also clears it.

**App Management:** if a Mac reports *Operation not permitted* while replacing or removing an app in
`/Applications`, macOS's *App Management* protection blocked the SSH session. Allow it once on that Mac in *System
Settings → Privacy & Security → App Management* (the entry `sshd-keygen-wrapper`), or delete the old app in the
Finder, and run the command again.

**Security notes**

* The release is pinned: tag `v2.58.4-macos.3`, asset `RealSense-Viewer-2.58.4-macOS-arm64.dmg`, SHA-256
  `2aed32042f8ded5c4a66a9a0c64b356f22579fe98db3af0e3493c680b8020471` (in both scripts). A DMG with another hash is
  never attached, also with `--dmg`. Downloads use https only (`curl --proto =https --proto-redir =https --tlsv1.2`,
  without reading any `.curlrc`).
* The deployment scripts are pinned too: the one-line command checks the SHA-256 of `rs-deploy-v2.58.4-macos.3.tar.gz`
  before extracting it into a new folder only you can write to. The archive is reproducible:
  `scripts/deploy/make-bundle.sh` rebuilds it byte for byte from `scripts/deploy-macs.sh` and
  `scripts/deploy/remote-install.sh` and prints its hash.
* The sudo password goes to `sudo -S` through the SSH connection's standard input only: never on a command line (so
  not in `ps` or in a shell history), never in a file or a log. The remote script itself travels as an argument of
  `bash -c` and starts by closing its standard input, so nothing else can read a password that sudo did not need.
* On a target, root reads nothing that another user could change: the DMG and its mount point are in a root-only
  folder, and the copies are prepared in a root-only folder until they are final, so no other user can open, change
  or add an ACL to any file of them; the camera access installer then applies its own checks (trust rule,
  allowlist, daemon signature).
* `--dry-run --root <dir>` is a test mode for `scripts/test-deploy.sh` only (it needs that script's fake `sudo`).

`scripts/test-deploy.sh` tests both scripts and the one-line command without root, SSH or network (dry runs into a
temporary folder, a fake `ssh`, `sudo` and `curl`). Maintainers: attach `dist/rs-deploy-<tag>.tar.gz` from
`scripts/deploy/make-bundle.sh` to the release next to the DMG; the test fails when the hash in this README no
longer matches the scripts.

> 🇵🇱 **Po polsku:** `deploy-macs.sh` instaluje lub aktualizuje RealSense Viewer, Depth Quality Tool i dostęp do
> kamery (*realsense-usbd*) na wielu Macach naraz przez SSH. Uruchamiasz go na swoim Macu jako zwykły użytkownik
> (bez sudo); na każdym komputerze loguje się przez SSH na konto administratora, pobiera z GitHuba DMG tego wydania
> (przypięty tag i skrót SHA‑256, tylko https), sprawdza podpisy, kopiuje aplikacje do */Applications* i instaluje
> dostęp do kamery — potem każdy użytkownik uruchamia Viewer z Launchpada/Aplikacji bez hasła.
> Wymagania na każdym Macu: Apple Silicon, macOS 27+, dostęp do github.com, konto administratora z włączonym
> *Ustawienia systemowe → Ogólne → Udostępnianie → Logowanie zdalne*. Polecenie do Terminala na Twoim Macu (podmień
> konto `admin` i nazwy komputerów; pobiera skrypty tego wydania do nowego prywatnego folderu i sprawdza ich SHA‑256):
> `d="$(mktemp -d)" && curl -fsSL --proto '=https' --proto-redir '=https' -o "$d/rs-deploy.tar.gz" https://github.com/dolegadolegowski/realsense-viewer-macos/releases/download/v2.58.4-macos.3/rs-deploy-v2.58.4-macos.3.tar.gz && echo "be4370814c7827fe2733ad049d144d65244da3510c747b908acd70ea437c4db8  $d/rs-deploy.tar.gz" | shasum -a 256 -c - && tar -xzf "$d/rs-deploy.tar.gz" -C "$d" && "$d/rs-deploy/deploy-macs.sh" --ask-pass -l admin mac-01.local mac-02.local`
> `--ask-pass` pyta o hasło sudo raz i przekazuje je tylko przez standardowe wejście połączenia SSH (nigdy w
> wierszu poleceń ani w pliku); jeśli pierwszy Mac odrzuci hasło, pozostałe są pomijane. `-f plik.txt` czyta listę
> komputerów (jeden na linię), `--tools` dodaje narzędzia `rs-*`, `--uninstall --remove-apps` usuwa wszystko. Na
> końcu jest podsumowanie OK/FAILED/SKIPPED dla każdego Maca. Zerwane połączenie nie przerywa instalacji na Macu,
> który już zaczął — wystarczy uruchomić polecenie ponownie, żeby zobaczyć stan.

## Command line tools

`RealSense Tools/` in the DMG contains `rs-enumerate-devices`, `rs-fw-update`, `rs-data-collect`,
`rs-convert`, `rs-record`, the examples and two macOS helpers:

```bash
"RealSense Tools/bin/rs-macos-selftest"          # checks enumeration, options, depth/IR/RGB/IMU streaming
"RealSense Tools/bin/rs-macos-usbdump"           # what librealsense's USB layer sees (opens nothing)
```

With camera access installed they run without sudo (except `rs-terminal`). Without it, run them with `sudo`;
afterwards `sudo "RealSense Tools/bin/rs-macos-release"` gives the camera back to macOS.

## Build from source

Requirements: Apple Silicon Mac, macOS 27+, Xcode 27 or the Command Line Tools (`xcode-select --install`),
CMake (`brew install cmake`).

```bash
git clone --recurse-submodules https://github.com/dolegadolegowski/realsense-viewer-macos.git
cd realsense-viewer-macos
scripts/build.sh      # applies patches/ to librealsense v2.58.4 and builds everything into build/
scripts/package.sh    # creates dist/*.app, dist/RealSense Tools and the DMG
```

The GitHub Actions workflow (`.github/workflows/build.yml`, macOS 27 runner) builds the DMG on demand
(*Actions → build → Run workflow*), optionally attaching it to an existing release.

The binaries in `build/Release` are not allowlisted for camera access; run them with `sudo`, or use the
packaged apps and tools. `scripts/test-camera-access.sh` tests the camera access installer and uninstaller
without root (dry runs into a temporary folder); `scripts/build.sh --asan|--tsan|--fuzz` adds sanitizer and
fuzzer builds of the daemon tests.

`VIEWER_TESTS=ON scripts/build.sh` additionally builds `realsense-viewer-tests`, the upstream GUI test suite
(ImGui Test Engine) that drives the real viewer UI: `sudo build/Release/realsense-viewer-tests --auto`.

## What was changed in librealsense

The camera access client (librealsense asks `realsense-usbd` first, then falls back to in‑process libusb) is
[`patches/0002-macos-usb-broker.patch`](patches/0002-macos-usb-broker.patch). All other changes are in
[`patches/0001-macos-apple-silicon-support.patch`](patches/0001-macos-apple-silicon-support.patch):

* **Sensor power accounting (libc++)** — `rsutils::deferred` relied on the defaulted move of
  `std::function`; libc++ keeps a moved‑from small `std::function`, so `x = sensor->bulk_operation()` released
  the sensor's power twice. The depth and RGB power counters went negative and the sensors could never be
  powered again (streams did not start, *"Device must be powered to query supported profiles!"*, preset
  changes froze the viewer) while the IMU kept working. Moves now empty the source.
* **No more infinite USB waits** — processing‑unit and probe/commit requests used timeout 0 (wait forever);
  they now use 5 s like the Linux `uvcvideo` driver.
* **Thread‑safe USB handles on macOS** — libusb's macOS backend keeps non‑thread‑safe per‑device state and
  re‑opens the device when capturing it, so librealsense now opens/closes its USB handles one at a time.

* **Viewer enabled on macOS** — `realsense-viewer` was excluded from macOS builds.
* **USB device capture** — libusb on macOS captures the *whole* device and, with auto‑detach, re‑attaches the
  system drivers (re‑enumerating the device) whenever *any* interface is released, killing the streams of the
  other sensors. The camera is now captured once, explicitly, for the lifetime of the process, and a clear
  error is reported when administrator rights are missing.
* **IMU / motion module** — the half‑finished hidapi code path is replaced by the regular libusb HID backend
  (the same one used on Linux with the RSUSB backend); accelerometer and gyroscope stream at up to 400 Hz.
* **libusb 1.0.30** (statically linked; needs the Security framework on macOS).
* **GLFW as a shared library** on macOS — it was linked statically into both `librealsense2-gl` and the
  tools, duplicating its Objective‑C classes (two GLFW states in one process).
* **Dear ImGui OpenGL backend on legacy OpenGL 2.1** (the context type macOS gives the fixed‑function
  viewer): GLSL 1.20 shaders, no core‑profile VAOs, no GL 3.0‑only queries. Before, the UI shaders failed to
  compile and every frame raised `GL_INVALID_OPERATION`, which the viewer showed as a blocking error.
* **Clean shutdown on SIGINT/SIGTERM**, so devices are stopped and released when the app is quit from outside.
* libusb error names in transfer warnings (the logs printed an unrelated `errno`).

Hangs and crashes found by a stability review (static analysis, ASan/UBSan/TSan builds, soak tests) and fixed:

* **Device command queue** (`rsutils` dispatcher, which serialises every USB command of a device): a command that
  threw (a failed stream start, an option the camera rejected) left its caller waiting forever, which froze the
  viewer. A caller that gave up early let the command write into its finished stack frame, and several waiting
  threads could lose their wake‑ups. Errors now reach the caller, dropped commands release it, and stopping a
  dispatcher wakes sleeping commands (every context teardown and app exit used to wait ~2 s).
* **USB requests** whose cancelled transfer completes late keep their buffer until libusb is done with it
  (use‑after‑free); a failed interface claim releases the handle and the interfaces already claimed (the
  sensor could not start again until re‑plugged); submit errors report libusb's status instead of a stale
  `errno`; probe/commit no longer retries a timeout 6× (a hung camera blocked *Start* for 30 s).
* **IMU start and firmware update** fail with an error instead of crashing when the HID/DFU interface can't be
  opened; IMU samples no longer copy 38 bytes out of a 12‑byte value.
* **Recording toggled while streaming** — frame callbacks are swapped atomically and the recorder's wrapper
  checks that the recorder is still alive (segfault).
* **Quitting the viewer while streaming or during the splash screen** waits for the background sensor stops
  and the device loader before the models they use are destroyed (use‑after‑free).
* **NEON point cloud / align** (always used on Apple Silicon) fall back to the scalar code for frame sizes their
  vector loops can't handle (heap overflow on such recordings); **spatial filter hole filling** read one pixel
  past every row; the viewer's post‑processing queue map is locked against the sensor threads.
* **Running as root safely** — files are handed back to you only if root created them during the session, through
  descriptors that don't follow links (a planted symlink or hard link could make root give away a system file),
  and the log file named in the user‑writable settings is opened the same way.

macOS‑specific additions in this repository: the camera access daemon and its installer (`macos/broker`), the
app launcher (`macos/launcher`), `rs-macos-usbdump`, `rs-macos-release`
(re‑enumerates the camera to hand it back to macOS), `rs-macos-handback` (returns ownership of the files the
viewer created) and `rs-macos-selftest` (`--stress` adds start/stop cycles, recording toggled while streaming,
presets changed while streaming, a long run of every sensor and a hardware reset).

## Known limitations

* Without camera access an administrator password is needed every time the viewer starts (macOS USB
  security, see above); with it, every daemon update needs an administrator once.
* Through camera access the macOS camera indicator stays off and third‑party webcams are not listed (see
  *Privacy*).
* Toggling the RGB stream while depth is streaming makes the camera firmware restart the depth stream
  briefly (frame counters reset) — this is camera behaviour, not specific to macOS.
* "Use GLSL for rendering/processing" options are not available on macOS (upstream limitation of the
  legacy OpenGL context).
* After *Run once as administrator*, files saved outside *Documents* while the viewer ran are owned by root (readable by you; you can delete
  them, or `sudo chown` them).
* Quit the viewer normally (window close / ⌘Q). Killing it (`kill -9`) in the middle of a camera command can
  leave the camera firmware unresponsive until it is re‑plugged.
* A sporadic `control_transfer returned error … LIBUSB_ERROR_PIPE` warning in the log comes from the
  firmware's error‑reporting control and is harmless (it also appears with the libusb backend on Linux).

## License

Apache License 2.0, like librealsense. RealSense is a trademark of RealSense, Inc.; Intel is a trademark of
Intel Corporation. This is an unofficial community build.
