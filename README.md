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
> administratorowi (root), dlatego przy pierwszym uruchomieniu Viewer sam instaluje **usługę kamery RealSense**
> (*realsense-usbd*, mały LaunchDaemon): macOS pyta o nazwę i hasło administratora — jeden raz na danym Macu.
> Od tej chwili Viewer, Depth Quality Tool, narzędzia `rs-*` i inne aplikacje RealSense, na przykład
> [IntelFaceScan](https://github.com/dolegadolegowski/IntelFaceScan), działają **bez hasła, także na kontach
> standardowych** (tylko użytkownik zalogowany przy ekranie Maca i root). Usługa jest wspólna dla wszystkich
> aplikacji i nie jest związana z plikiem DMG ani z wersją aplikacji: nowe wersje aplikacji nie wymagają jej
> aktualizacji, a jeśli zainstalowała ją wcześniej inna aplikacja, Viewer korzysta z niej bez pytania o hasło.
> **Prywatność:** obraz z kamery przez tę usługę nie zapala wskaźnika kamery macOS i nie wywołuje pytania o
> zgodę; respektowany jest tylko zakaz kamery z MDM/Czasu przed ekranem (`allowCamera`). **Bezpieczeństwo:**
> usługa nie sprawdza, jaki program się z nią łączy — z kamer RealSense może przez nią korzystać każdy program
> działający jako użytkownik zalogowany przy ekranie Maca (administrator może to ograniczyć listą
> `AllowedCDHashes`). Usługa otwiera wyłącznie kamery RealSense, sprawdza każde żądanie i odrzuca nieodwracalne
> polecenia pamięci flash. Po anulowaniu instalacji pozostaje dotychczasowe „Run once as administrator” (hasło
> przy każdym uruchomieniu). Odinstalowanie:
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
5. At its first start the viewer installs the **RealSense camera service**: macOS asks for an administrator's
   name and password once (this works from a standard account too). From then on the viewer starts without any
   password, for every user of the Mac, and so does every other RealSense application that uses the service.
   When the service is already on the Mac (another application installed it), nothing is asked. See
   [Camera access](#camera-access-no-password).

### Why is an administrator needed?

macOS attaches its own drivers to RealSense cameras (`UVCAssistant` for the video interfaces and the HID
driver for the IMU). The SDK talks to the camera through libusb, which has to *capture* the USB device from
those drivers — and macOS only allows that for root (or for apps holding a restricted Apple entitlement).
There are two ways to give the viewer that right:

* **The RealSense camera service** (the default): a small root service, installed once per Mac, does the USB
  work for the viewer and for every other RealSense application; the applications themselves run as you.
* **Run once as administrator**: the app asks for an administrator password with the standard macOS dialog,
  runs the viewer with administrator rights in your session, and when you quit gives the camera back to macOS
  (so the RGB camera works again as a webcam in FaceTime, Zoom, …) and returns ownership of the settings file and
  of files saved to *Documents* to your user. The app offers this when the installation of the service was
  cancelled or failed, or when the service cannot be used.

## Camera access (no password)

`realsense-usbd` is the **RealSense camera service**: a root LaunchDaemon
(`io.github.dolegadolegowski.realsense-usbd`) that captures RealSense cameras from macOS and forwards librealsense's
USB requests to them. There is one per Mac, and it is shared: it serves every program of the user logged in at the
Mac's screen and does not check which program is asking. RealSense Viewer, the Depth Quality Tool, the `rs-*` tools,
[IntelFaceScan](https://github.com/dolegadolegowski/IntelFaceScan) and applications written later all use the same
installed service, so it is not tied to a DMG or to the version of an application. librealsense asks it first and
falls back to its normal in‑process libusb path (which needs root) when it is not installed, switched off, or does
not admit the program. Design: [`macos/broker/DESIGN.md`](macos/broker/DESIGN.md).

Status: the shared service (the second generation of `realsense-usbd`; the first admitted only the programs of one
DMG) is new in this version. First run on hardware, 2026-11-10 (M3 Ultra, macOS 27.2, D455 with firmware 5.16.0.1
on USB 3.2, an administrator account at the screen): the service was installed by IntelFaceScan's copy of the
installer after the administrator prompt; RealSense Viewer from this repository, which carries another build of the
same service version, then started without any prompt and opened the camera as the user; `rs-enumerate-devices` and
`rs-macos-selftest` ran without `sudo`, the self-test passing all 24 checks (enumeration, hardware monitor, options,
the 6 visual presets, depth + 2×IR, RGB, accelerometer and gyroscope at 400 Hz, all sensors at once, recording to
`.db3` and playback, repeated start/stop of RGB/IMU while depth streams); five seconds after the last program closed
the camera the service handed it back to macOS. `rs-macos-selftest --stress` also passed without `sudo` (2026-11-11,
36 checks): 20 depth start/stop cycles (slowest stop 0.06 s), 30 recording start/stops while depth + RGB stream, the
presets and options ×3 while streaming, every sensor for 60 s (depth, infrared and RGB at 30.0 fps, IMU throughout,
memory 302.3 → 302.5 MB) and a hardware reset after which the camera streamed again. The installer of this
repository's package, run as root over the service that IntelFaceScan had installed (the same service version from
another build), reported that the service was already installed and answering and replaced nothing: the daemon's
code hash and the dates of the daemon and of its configuration were unchanged. Half an hour of continuous depth and
color streaming through the service (IntelFaceScan's helper as the user, five runs back to back) ended without an
error, the daemon at 8.5–9.3 MB and 1–3 % of a core. Still to be done from DESIGN.md 9.2: the one-hour soak test of
all sensors, a standard account, a second user session, the Login Items switch, an update from an older service, a
restart of the Mac, MDM camera restrictions, firmware update, two cameras, and the launcher's dialogs for a missing
service. The *Verified* list above was made with the *Run once as administrator* path.

**Install** (once per Mac, with an administrator's name and password):

* start **RealSense Viewer** from */Applications*. When the service is not installed, is too old for the app or does
  not answer, the app goes straight to the macOS administrator prompt, installs the service and then starts the
  viewer. If the prompt is cancelled or the installation fails, a dialog offers **Install camera service**, **Run
  once as administrator** and **Cancel**. The app installs only from a copy that, together with every folder above
  it, can be changed only by root or an administrator (so for a standard user not in `~/Applications` or
  `~/Downloads`, and not a translocated or quarantined copy run from the DMG); otherwise it says that an
  administrator must move it to */Applications*; or
* in Terminal, as the user logged in at the Mac's screen:
  `"/Applications/RealSense Viewer.app/Contents/Resources/realsense-usbd/ensure-camera-access.sh"` (the same script
  is in `RealSense Tools/camera-access/`). It prints `ok <service version>` when the service answers; otherwise it
  shows the same administrator prompt and installs it. With `--check` it only reports (exit status 0: answering,
  10: not installed); or
* as root, for example over SSH or from a management tool:
  `sudo "RealSense Tools/camera-access/install-camera-access.sh" --source "RealSense Tools/camera-access"`.

The installer refuses files that a non‑administrator could have changed, checks the daemon's code signature against
the manifest it ships with (`service.plist`), never replaces a newer service or a working one of the same version,
and writes:

| file | |
|---|---|
| `/Library/PrivilegedHelperTools/io.github.dolegadolegowski.realsense-usbd` | the daemon |
| `/Library/LaunchDaemons/io.github.dolegadolegowski.realsense-usbd.plist` | its launchd job (started on demand, exits when idle) |
| `/Library/Application Support/io.github.dolegadolegowski.realsense-usbd/usbd.plist` | service and protocol versions, optional administrator settings |
| `/Library/Application Support/RealSense Viewer/usbd.plist` | a marker for apps of the first generation (up to release `v2.58.4-macos.3`), so that they do not install their old daemon over this one |

macOS then shows *"Background Items Added"*; the entry appears in *System Settings → General → Login Items &
Extensions*. An administrator can switch it off there (the app then offers *Open Login Items Settings*); a
standard user cannot.

**Standard accounts:** after the installation every user can use the camera without a password, but only the
user logged in at the Mac's screen (and root) — not a second, fast‑user‑switched session in the background and
not an SSH login.

**Other applications:** whichever application starts first installs the service; the others find it and use it
without asking. Every application carries its own copy of the daemon and of the installer, so none depends on
another being installed. See [Using the camera service from another
application](#using-the-camera-service-from-another-application).

**Updates:** a new version of the viewer, or of any other application, needs no update of the service, because the
service does not know which programs use it. It is updated only when an application needs a newer service than the
Mac has: a first‑generation installation (from release `v2.58.4-macos.3`), or a service that does not speak the
application's protocol version. The app then shows the administrator prompt as at the first installation. The
service is never downgraded: an installer that carries an older service than the installed one keeps the installed
one and only makes sure that it runs, and a service of the same version that is installed and answering is left as
it is, so two applications never replace and restart each other's copy (`--allow-downgrade` overrides both).
`realsense-usbd --version`, in an app's
`Contents/MacOS` or at the installed path, prints the service and protocol versions. Apps of the first generation
do not recognize the shared service and report that camera access is newer than the app: update them as well.

**Uninstall:** `sudo "/Applications/RealSense Viewer.app/Contents/Resources/realsense-usbd/uninstall-camera-access.sh"`
(or the same script in `RealSense Tools/camera-access/`). It stops the daemon, gives every camera it captured back
to macOS and removes the files above. This removes the service for every application that uses it; each of them
shows the administrator prompt again at its next start.

**Administrator settings** (optional keys in the installed `usbd.plist`, kept by updates): `HandBackGraceSeconds`
(0–600, default 5: how long a camera stays captured after the last program closed it), `IdleExitSeconds` (10–3600,
default 60), `DeniedHWMOpcodes` (array of hardware‑monitor opcodes to refuse) and `ProtectedFlashRanges` (array
of `{Start, Length}` flash byte ranges whose erase/write is refused). For D585S safety cameras, D500
`SAFETY_PRESET_WRITE` (`0x95`) and `CALIBRESTOREEPROM` (`0xA8`) are candidates for `DeniedHWMOpcodes`.
`AllowedCDHashes` (array of up to 256 code directory hashes, 40 lowercase hex digits each, the `CDHash` line of
`codesign -dvvv --arch arm64 <program>`) restricts the service to the listed programs: every other program is
turned away and falls back to the root‑only path within about a second. List every program that opens a camera and
each application's own `realsense-usbd`, which the application runs to ask whether the service admits it. The hash
of a program changes with every build, so this list has to be maintained with every update of an application.
Restart the daemon after a change: `sudo launchctl kickstart -k system/io.github.dolegadolegowski.realsense-usbd`.

### Security model

* The daemon runs as root. It admits a program that runs as root or as the user logged in at the Mac's screen in a
  local graphical session, **whatever program it is**. Any program running as that user can therefore use RealSense
  cameras through the daemon and reach its message parser — a program the user never meant to give a camera to, and
  malware running as the user, included. This is what lets applications written later use the service without
  updating it. Through the daemon a program reaches nothing but RealSense cameras (no files, no network, no other
  devices).
* Only the console user and root: a second, fast‑user‑switched session in the background, an SSH login and system
  accounts are refused, and a session loses its cameras when its user stops being the one at the screen.
* Only RealSense devices are ever opened (vendor `0x38E5`, and `0x8086` only with a RealSense product ID from
  librealsense), never a device with an audio, mass‑storage, hub, network, wireless or boot keyboard/mouse
  interface. Every request is validated; a program can only use interfaces it opened, and only class requests.
* Erasing the whole flash and permanently locking or unlocking it (hardware‑monitor opcodes `FEF`, `FSRU`,
  `FPLOCK`) are always refused. Everything else librealsense does is allowed — including firmware updates and
  writing calibration, so any program of the logged‑in user can make a camera need recovery unless
  `ProtectedFlashRanges` / `DeniedHWMOpcodes` are configured. The same filter applies to the raw hardware‑monitor
  commands of `rs-terminal`.
* The camera ban of an MDM profile or Screen Time (`com.apple.applicationaccess` `allowCamera = false`) blocks
  the daemon for everyone, root included, and ends running sessions.
* `AllowedCDHashes` (see *Administrator settings*) brings back the first generation's restriction to listed
  programs. It is **version pinning, not code containment**: anything running as the logged‑in user can start a
  listed program, or copy an app and swap its (ad‑hoc signed) libraries — the cdhash covers only the main
  executable.
* The installer only takes files from folders that only root/administrators can change, never from a
  translocated app, and generates the launchd job and settings itself instead of copying them. The daemon it
  installs is the one the application carries: as with any installer, the administrator who enters the password
  trusts the application that asks.

### Privacy

* Depth, infrared and RGB streamed through the daemon do **not** light up the macOS camera indicator and do not
  trigger the camera permission prompt: macOS does not see a camera being used, only a USB device. Together with the
  admission rule above this means that any program running as the user at the Mac's screen can read images from a
  connected RealSense camera without macOS asking or showing anything. Privacy (PPPC) profiles cannot target it;
  only the `allowCamera` ban above is honoured. Where that is not acceptable, unplug the camera when it is not in
  use, restrict the service with `AllowedCDHashes`, switch it off in *Login Items & Extensions* or uninstall it.
* The daemon logs to the unified log (subsystem `io.github.dolegadolegowski.realsense-usbd`): admitted and
  rejected programs (pid, uid, path), camera capture and hand‑back, device arrival/removal, refused commands —
  never image data or command payloads. `log show --last 1h --predicate 'subsystem ==
  "io.github.dolegadolegowski.realsense-usbd"'` shows them, and with them which programs used a camera.
* Non‑RealSense webcams: the root path of the viewer also lists third‑party UVC webcams; through the camera
  service they are not shown, because the daemon never captures devices other than RealSense cameras.

## Using the camera service from another application

[`macos/broker`](macos/broker) is a self‑contained kit that other projects copy unchanged (IntelFaceScan has it as
`third_party/realsense-usbd`): the daemon and its build script, the installer scripts, the librealsense client
patch and `client/RealSenseCameraAccess.swift`, one Swift file that tells an application whether the service is
installed and answering and installs it when it is not. An application builds its librealsense with the patch,
puts `realsense-usbd`, its manifest and the scripts into its bundle, and asks for the service's state at start;
applications in other languages run the bundled `ensure-camera-access.sh` instead. Because the service does not
check which program connects, a new application needs no change to an installed service.

The integration guide is [`macos/broker/README.md`](macos/broker/README.md). In this repository
[`macos/launcher/main.swift`](macos/launcher/main.swift) and the camera service part of
[`scripts/package.sh`](scripts/package.sh) are a complete example.

## Deploying to many Macs

`deploy-macs.sh` installs or updates the apps **and** camera access on many Macs at once over SSH. It runs on your
own Mac as your normal user (no sudo). For every target it logs in over SSH as an administrator account, and there,
with `sudo`, `remote-install.sh` downloads the release DMG from GitHub, checks it and installs it. Afterwards every
user of those Macs starts RealSense Viewer from *Launchpad* or *Applications* without a password.

The scripts are pinned to release `v2.58.4-macos.3`, and this section describes that release. Its camera access is
the first generation, which admits only the programs of that DMG, not the shared service described above. On a Mac
that already has the shared service, a run that installs or updates (the default, also with `--tools`) reports that
the installed camera access is newer, changes nothing and counts the Mac as `FAILED`. Do not pass
`--allow-downgrade` there: the old installer would put its own daemon in place of the shared one, and other
applications would lose the camera until one of them installs the service again. Two options do change such a Mac:

* `--uninstall` runs the uninstaller of the apps installed there, which removes the shared service for every
  application that uses it, not only for RealSense Viewer. Each of those applications then asks for an administrator
  again. The uninstaller of a first‑generation app also leaves the shared service's configuration folder behind
  (`/Library/Application Support/io.github.dolegadolegowski.realsense-usbd`).
* `--apps-only` skips the check and installs the apps of the pinned release. They do not know the shared service:
  each start says that camera access is newer than the app and offers only *Run once as administrator*.

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
temporary folder, a fake `ssh`, `sudo` and `curl`). It needs the DMG of the pinned release: pass its path as the
first argument, because `scripts/package.sh` replaces everything in `dist/`, where the test looks by default.
Maintainers: attach `dist/rs-deploy-<tag>.tar.gz` from `scripts/deploy/make-bundle.sh` to the release next to the
DMG; the test fails when the hash in this README no longer matches the scripts.

Maintainers, when a release with the shared camera service is published: adapt the deployment scripts before
re-pinning them. `remote-install.sh` compares `BuildNumber` from the DMG's
`Contents/Resources/realsense-usbd/allowlist.plist` with `BuildNumber` in
`/Library/Application Support/RealSense Viewer/usbd.plist` to refuse a release older than what is installed. A DMG
with the shared service has `service.plist` with `ServiceVersion` instead, the installed configuration is
`/Library/Application Support/io.github.dolegadolegowski.realsense-usbd/usbd.plist`, and its installer keeps a newer
installed service by itself and exits 0, so that case no longer has to fail the Mac. The messages that speak of a
camera access build and of tools that only work with the same build change with it, and so does the *newer camera
access installed* case of `scripts/test-deploy.sh`, which writes a `BuildNumber`. Then re-pin: `PINNED_TAG`,
`PINNED_ASSET` and `PINNED_SHA256` in `deploy-macs.sh` and `remote-install.sh`, `SHA` and the default DMG in
`scripts/test-deploy.sh`, and, after `make-bundle.sh`, the bundle's name and SHA-256 in the two one-line commands
above (English and Polish) and in *Security notes*.

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
> który już zaczął — wystarczy uruchomić polecenie ponownie, żeby zobaczyć stan. Skrypty instalują wydanie
> `v2.58.4-macos.3`, czyli pierwszą generację dostępu do kamery (tylko programy z tego DMG), a nie opisaną wyżej
> wspólną usługę kamery. Na Macu, który ma już wspólną usługę, zwykła instalacja lub aktualizacja niczego nie
> zmienia i zgłasza FAILED; nie używaj tam `--allow-downgrade`, bo stary instalator zastąpiłby wspólną usługę
> swoją. Dwie opcje taki komputer jednak zmieniają: `--uninstall` usuwa wspólną usługę dla wszystkich aplikacji,
> które z niej korzystają (każda z nich znów poprosi o administratora), a `--apps-only` instaluje aplikacje
> pierwszej generacji, które przy wspólnej usłudze uruchamiają się tylko z hasłem administratora.

## Command line tools

`RealSense Tools/` in the DMG contains `rs-enumerate-devices`, `rs-fw-update`, `rs-data-collect`,
`rs-convert`, `rs-record`, the examples and two macOS helpers:

```bash
"RealSense Tools/bin/rs-macos-selftest"          # checks enumeration, options, depth/IR/RGB/IMU streaming
"RealSense Tools/bin/rs-macos-usbdump"           # what librealsense's USB layer sees (opens nothing)
```

With the camera service installed all of them run without sudo, `rs-terminal` included: the service admits every
program of the user at the Mac's screen. `"RealSense Tools/camera-access/ensure-camera-access.sh"` installs the
service when it is missing (the administrator prompt) and `--check` only reports. Without the service, run the tools
with `sudo`; afterwards `sudo "RealSense Tools/bin/rs-macos-release"` gives the camera back to macOS.

## Build from source

Requirements: Apple Silicon Mac, macOS 27+, Xcode 27 or the Command Line Tools (`xcode-select --install`),
CMake (`brew install cmake`).

```bash
git clone --recurse-submodules https://github.com/dolegadolegowski/realsense-viewer-macos.git
cd realsense-viewer-macos
scripts/build.sh      # patches librealsense v2.58.4 (patches/ and the camera service client), builds into build/
scripts/package.sh    # creates dist/*.app, dist/RealSense Tools and the DMG
```

The GitHub Actions workflow (`.github/workflows/build.yml`, macOS 27 runner) builds the DMG on demand
(*Actions → build → Run workflow*), runs the camera service tests below, and optionally attaches the DMG to an
existing release.

The binaries in `build/Release` reach the camera through the camera service like any other program when it is
installed on the Mac; without it, run them with `sudo`. Tests that need no camera and no root:

```bash
# the daemon on a mock USB backend, and librealsense's client against that daemon
build/usbd/rs-usbd-tests
# installer, uninstaller, manifest and ensure-camera-access.sh --check, as dry runs into a temporary folder
macos/broker/tests/test-camera-access.sh build/Release/realsense-usbd
```

`scripts/build.sh --asan|--tsan` adds sanitizer builds of the daemon tests (`build/usbd/asan`, `build/usbd/tsan`);
the fuzzer `build/usbd/rs-usbd-fuzz` is always built.

`VIEWER_TESTS=ON scripts/build.sh` additionally builds `realsense-viewer-tests`, the upstream GUI test suite
(ImGui Test Engine) that drives the real viewer UI: `sudo build/Release/realsense-viewer-tests --auto`.

## What was changed in librealsense

The camera service client (librealsense asks `realsense-usbd` first, then falls back to in‑process libusb) is
[`macos/broker/librealsense/macos-usb-broker.patch`](macos/broker/librealsense/macos-usb-broker.patch). It is part
of the kit in `macos/broker` because every application that uses the service builds its librealsense with it;
`scripts/apply-patches.sh` applies it after the patches in `patches/`. All other changes are in
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

macOS‑specific additions in this repository: the camera service daemon, its installer and the client file for
applications (`macos/broker`), the app launcher (`macos/launcher`), `rs-macos-usbdump`, `rs-macos-release`
(re‑enumerates the camera to hand it back to macOS), `rs-macos-handback` (returns ownership of the files the
viewer created) and `rs-macos-selftest` (`--stress` adds start/stop cycles, recording toggled while streaming,
presets changed while streaming, a long run of every sensor and a hardware reset).

## Known limitations

* Without the camera service an administrator password is needed every time the viewer starts (macOS USB
  security, see above). Installing the service needs an administrator once per Mac, and again only when an
  application needs a newer service than the installed one.
* The camera service does not tell programs apart: any program running as the user at the Mac's screen can use a
  connected RealSense camera through it (see *Security model*).
* Through the camera service the macOS camera indicator stays off and third‑party webcams are not listed (see
  *Privacy*).
* The shared camera service has had its first run on a D455, not yet the full hardware test plan (see *Camera
  access*, Status).
* Apps of the first generation (up to release `v2.58.4-macos.3`) do not start without a password once the shared
  service is installed; they report that camera access is newer than the app. Install this version of the apps.
* The deployment scripts still install release `v2.58.4-macos.3` (see *Deploying to many Macs*).
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
