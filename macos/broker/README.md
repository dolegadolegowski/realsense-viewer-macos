# RealSense camera service for macOS (`realsense-usbd`)

A guide for people who build a macOS application that uses an Intel RealSense camera through librealsense.
`DESIGN.md` is the implementation spec (section 13 describes the shared service), `CONTRACT.md` describes what the
librealsense side must do.

> 🇵🇱 Krótkie streszczenie po polsku jest na końcu tego pliku.

## What it is and why it exists

A RealSense camera is a USB video device. macOS attaches its own USB video driver to it, and only a process running
as root may take the device away from that driver, which librealsense has to do to reach depth, infrared, the IMU
and the camera's controls. Without help, every program that uses the camera has to run as root, so the user types
an administrator password at every start, and a standard account cannot use the camera at all.

`realsense-usbd` is a small root LaunchDaemon that does this one thing for everybody. It owns the camera over libusb
and serves librealsense clients over XPC. librealsense, built with the patch in `librealsense/`, asks the service
first and talks to the camera through it as the logged-in user; no part of the application runs as root. A few
seconds after the last program let go of a camera, the service gives it back to macOS.

## One service for every application

- There is one installed service per Mac. It belongs to no application and to no build: it admits every program of
  the user logged in at the Mac's screen (and root). An application written next year uses the service that is
  installed today, without any update of the service.
- Every application carries the service files, checks at start whether the service answers, and installs it when it
  does not. Installing needs an administrator's name and password once per Mac, not once per application.
- The installer never replaces a newer service with an older one, and it leaves a working service of the same
  version alone, so any application may run it at any time.

## What to copy into your application

Copy this whole folder into your repository unchanged (for example as `third_party/realsense-usbd/`). It refers to
nothing outside itself; what it needs comes as arguments.

| path | what |
|---|---|
| `daemon/`, `build-daemon.sh`, `gen-usbd-pids.sh`, `rs-usb-broker-protocol.h` | the daemon and its build |
| `make-service-manifest.sh` | writes `service.plist` for a signed daemon |
| `install-camera-access.sh`, `uninstall-camera-access.sh`, `ensure-camera-access.sh` | shipped inside the application |
| `client/RealSenseCameraAccess.swift` | the same checks and the installation for Swift applications (one file) |
| `librealsense/macos-usb-broker.patch` | the client inside librealsense |
| `tests/` | host tests (no root, no camera) |

### Build

The daemon (arm64, C++17, static libusb, system libraries only):

```sh
KIT=third_party/realsense-usbd
"$KIT/build-daemon.sh" build/usbd --libusb <libusb prefix>/lib/libusb-1.0.a \
    --librealsense-src <librealsense sources> --daemon-only
# -> build/usbd/realsense-usbd
```

`--libusb` is a static libusb 1.0 (developed with 1.0.30); its headers are taken from `../include/libusb-1.0` next
to the archive. `--librealsense-src` is read for the list of RealSense product IDs. Without `--daemon-only` the
script also builds the host tests (`rs-usbd-tests`, `rs-usbd-testd`, `rs-usbd-fuzz`); `--deployment-target` sets the
minimum macOS version (default 27.0).

librealsense (v2.58.4) needs two patches, applied in this order:

1. `patches/0001-macos-apple-silicon-support.patch` of
   [realsense-viewer-macos](https://github.com/dolegadolegowski/realsense-viewer-macos): the macOS port itself
   (build it with `-DFORCE_RSUSB_BACKEND=ON`);
2. `librealsense/macos-usb-broker.patch` of this folder: the client of the service.

A librealsense built this way decides once per process, at its first device query: when the service admits the
process, all USB traffic goes through the service; otherwise it uses libusb in the process, which works only as
root. So make sure the service is ready (next section) before the process that uses librealsense looks for
devices, or start that process afterwards.

### Bundle layout

```
<App>.app/Contents/MacOS/realsense-usbd                      (or Contents/Helpers/realsense-usbd)
<App>.app/Contents/Resources/realsense-usbd/service.plist
<App>.app/Contents/Resources/realsense-usbd/install-camera-access.sh
<App>.app/Contents/Resources/realsense-usbd/uninstall-camera-access.sh
<App>.app/Contents/Resources/realsense-usbd/ensure-camera-access.sh
```

The application's copy of `realsense-usbd` has two jobs: the installer installs it as the daemon, and the
application runs it as `realsense-usbd --probe` to ask whether the installed service admits it.

### Packaging order

1. Copy `realsense-usbd` and the three scripts into the bundle (the scripts with mode 755).
2. Sign the daemon:
   ```sh
   codesign --force --sign - -o runtime -i io.github.dolegadolegowski.realsense-usbd \
       "$APP/Contents/MacOS/realsense-usbd"
   ```
3. Write the manifest:
   ```sh
   "$KIT/make-service-manifest.sh" "$APP/Contents/MacOS/realsense-usbd" \
       "$APP/Contents/Resources/realsense-usbd/service.plist"
   ```
4. Sign the bundle. Its signature seals `Resources`, so the manifest must exist by now.

The manifest holds the cdhash of the signed daemon, and the installer refuses a daemon that does not match it. Do
not sign the daemon again after step 3 (`codesign --deep` on the bundle would).

`service.plist` keys: `Version` (2), `ServiceVersion`, `ProtocolMin`, `ProtocolMax` (from
`realsense-usbd --version`) and `DaemonCDHash`.

## What the application does at start

1. Ask for the state of the service.
2. Ready: use the camera as the user. No dialog, no password.
3. Not installed, too old, or installed but not answering: install it. Go straight to the macOS administrator
   prompt; its text should say what is installed and that this is asked once. Then continue as in 2. When the
   prompt was cancelled or the installation failed, offer to try again, your application's own one-time
   administrator path if it has one (running the camera code as root for one session), and cancel.
4. Switched off by an administrator in System Settings > General > Login Items & Extensions: say so, offer to
   open that page, and the one-time administrator path if there is one.
5. The user is not the one logged in at the Mac's screen (SSH, a fast-user-switched session in the background): say
   so. The service does not serve that user; the one-time administrator path is the only other way.
6. Camera use is disabled by MDM or Screen Time, or too many programs are connected: say so. Offer no
   administrator path for these: it would get around the camera ban, or clash with the devices the daemon holds.
7. The installer refuses the application's files as untrusted: an administrator must move the application to
   `/Applications`. The installer only installs from folders that only root or administrators can change, and
   never from a translocated copy (an application opened straight from a download).

A process running as root is admitted as well, so an application's own root path also goes through the service
whenever the service answers. Such a path must then leave capturing the camera and giving it back to the service.

Swift, with `client/RealSenseCameraAccess.swift` added to the application's sources (Foundation and
ServiceManagement only). Both calls block, so run this off the main thread:

```swift
guard let kit = RealSenseCameraAccess.Kit.inBundle(Bundle.main.bundleURL) else {
    fatalError("the camera service files are missing from the bundle")
}
var state = RealSenseCameraAccess.state(kit: kit)
if state.installationHelps {
    do {
        try RealSenseCameraAccess.install(
            kit: kit,
            prompt: "MyApp installs the RealSense camera service, so that the camera works without an "
                + "administrator password. This is asked once on this Mac.")
        state = RealSenseCameraAccess.state(kit: kit)
    } catch RealSenseCameraAccess.InstallError.cancelled {
        // offer: install again / one-time administrator path / cancel
    } catch RealSenseCameraAccess.InstallError.untrusted(let why) {
        // an administrator must move the application to /Applications; `why` names the folder
    } catch {
        // show the installer's message (String(describing: error)); offer the same choices
    }
}
switch state {
case .ready: break                                  // start librealsense as the user
case .switchedOff: RealSenseCameraAccess.openLoginItemsSettings()
case .notConsoleUser, .cameraDisabled, .tooManyClients: break   // explain; see the list above
default: break                                      // installation was cancelled or failed
}
```

The same from a shell script or any other language, with `ensure-camera-access.sh` (run as the user, never as
root):

```sh
kit="/Applications/MyApp.app/Contents/Resources/realsense-usbd"
"$kit/ensure-camera-access.sh" --check        # only ask: prints "ok <service version>" and exits 0 when ready
"$kit/ensure-camera-access.sh" --prompt "MyApp installs the RealSense camera service. This is asked once."
```

| exit status | meaning |
|---|---|
| 0 | the service answers and admits this user (stdout: `ok <service version>`) |
| 2 | not reachable: switched off, not answering, or this program is not on the administrator's list |
| 3 | only the user logged in at the Mac's screen (and root) can use the service |
| 4 | camera use is disabled on this Mac by its administrator (MDM / Screen Time) |
| 5 | the installed service does not speak this application's protocol version |
| 6 | too many programs are connected |
| 10 | `--check` only: not installed |
| 20 | the administrator prompt was cancelled |
| 21 | the installer failed; its message is on stderr |

Without `--check` the script installs for every state but 0, 3, 4 and 6 and then exits with the status of a new
probe. The script cannot tell "switched off in Login Items" from "not answering": in that case the installer fails
(21) and its message names the Login Items page.

Root has no prompt to answer and runs the installer itself, for example over SSH, from a management tool or from a
package script:

```sh
app="/Applications/MyApp.app/Contents"
kit="$app/Resources/realsense-usbd"
sudo "$kit/install-camera-access.sh" --source "$app/MacOS" --source "$kit"
sudo "$kit/install-camera-access.sh" --source "$app/MacOS" --source "$kit" --allow-downgrade
sudo "$kit/uninstall-camera-access.sh"
"$kit/install-camera-access.sh" --dry-run --root "$HOME/camera-service-test" --source "$app/MacOS" --source "$kit"
```

The `--source` folders together hold `realsense-usbd` and `service.plist`: `Contents/Helpers` in place of
`Contents/MacOS` when the daemon is there, one `--source` when both files are in one folder. Exit status: 0
installed, or the installed service kept (see the compatibility rules below); 1 error, with the reason on stderr;
2 wrong arguments. `--allow-downgrade` installs the application's service whatever is installed, also over a newer
version. The third line removes the service ([Uninstall](#uninstall)). The last line is the dry run, which is not
run as root: it writes the files a real installation would write under the given folder, runs neither `launchctl`
nor the probe, and reports what the installer's trust rule (point 7 above) would refuse instead of stopping there;
its exit status is then 3. `uninstall-camera-access.sh` takes `--dry-run --root <dir>` in the same way.

## Compatibility rules

- **Protocol.** A client speaks one protocol version (`RSUSB_PROTOCOL_VERSION` of the header it was built with). A
  daemon serves every version from `ProtocolMin` to `ProtocolMax`. The protocol only grows: a change that an
  existing client or daemon could misread gets a new version number, and the daemon keeps serving the older ones.
  A newer daemon therefore works with every older application.
- **Service version.** `ServiceVersion` (`daemon/version.h`, printed by `realsense-usbd --version`) is an integer
  that grows with every release of the daemon. 1 stands for the first generation, which admitted only the programs
  of one RealSense Viewer build.
- **The installer keeps a newer service.** When the installed `ServiceVersion` is larger than the application's,
  nothing is replaced; the installer only makes sure the installed service is loaded and answers, and fails when it
  does not.
- **The installer keeps a working service of the same version.** Two applications may carry different builds of one
  service version. If each installed its own, they would replace and restart each other's daemon, and a restart
  ends every running camera session; with `AllowedCDHashes` they would also take turns at whose bundled probe is
  admitted. So when the installed `ServiceVersion` equals the application's, the installer loads the service if it
  was not loaded and asks the installed daemon. When it answers, nothing is replaced, the installer says that this
  version is already installed and exits 0. When it does not, the installer goes on to install the application's
  build: that is the repair.
- **Everything else is replaced** by the application's service: an older version, the first generation, and an
  installation that is not whole (the daemon or its job is missing, or the daemon is not the one the installed
  configuration names), whatever version it claims. `--allow-downgrade` skips both keep rules: the application's
  service is installed also over a newer one, and over a working one of the same version.
- **An application that needs a newer protocol** than the installed service speaks gets probe status 5, installs
  its own newer daemon, and the older applications keep working with it.
- **First-generation applications** (RealSense Viewer builds that shipped `allowlist.plist`) do not know the shared
  service. They find a marker in their old configuration path and report that camera access is newer than the app;
  their installers refuse to put the old daemon back. Update those applications.
- **Lists of other applications** (`usbd.d` and `camera-access-allowlist.sh`, a variant that was never released)
  are withdrawn: every new application and every new build of one needed an administrator again. The installer and
  the uninstaller remove those files where they find them.

## Installed files

```
/Library/PrivilegedHelperTools/io.github.dolegadolegowski.realsense-usbd                   daemon (root:wheel 755)
/Library/LaunchDaemons/io.github.dolegadolegowski.realsense-usbd.plist                     job (root:wheel 644)
/Library/Application Support/io.github.dolegadolegowski.realsense-usbd/usbd.plist          configuration (644)
/Library/Application Support/RealSense Viewer/usbd.plist                                   marker for generation 1
```

The job and the configuration are generated by the installer, never copied from an application. The configuration
is world-readable: `ServiceVersion`, `ProtocolMin`, `ProtocolMax`, `DaemonCDHash` of the installed daemon, plus the
administrator's settings.

## Administrator settings

Optional keys in the installed `usbd.plist`. The installer keeps them across installations when they are valid
(and takes them over from a first-generation configuration). The daemon reads the file when it starts; after a
change restart it with `sudo launchctl kickstart -k system/io.github.dolegadolegowski.realsense-usbd`. Check an
edited file with `plutil -lint` and keep it owned by root: the daemon does not start with a file it cannot read,
and the installer cannot keep settings from one (see below the table).

| key | values | meaning |
|---|---|---|
| `HandBackGraceSeconds` | 0..600, default 5 | how long a camera stays captured after the last program closed it |
| `IdleExitSeconds` | 10..3600, default 60 | the daemon exits when idle this long; launchd starts it again on demand |
| `DeniedHWMOpcodes` | array of 0..255 | hardware-monitor opcodes to refuse, besides the fixed three (below) |
| `ProtectedFlashRanges` | array of `{Start, Length}` | flash byte ranges whose erase and write are refused |
| `AllowedCDHashes` | array of up to 256 cdhashes (40 lowercase hex) | only these programs may use the service |

`AllowedCDHashes` is absent by default: no restriction to particular programs. When an administrator sets it, the
list has to name every program that opens a camera (`CDHash` of `codesign -dvvv --arch arm64 <program>`) and each
application's own `realsense-usbd`, which the application runs as its probe. The daemon admits one program on top of
the list: its own binary, so the installed `realsense-usbd --probe`, with which the installer checks its work,
always gets an answer. An application's copy of `realsense-usbd` from another build of the daemon is a different
program and must be listed. A cdhash changes with every build, so the list needs care at every update of an
application.

What an installer does with the settings it finds:

- A setting that is not valid is dropped with a note, and the daemon uses its default. An `AllowedCDHashes` that
  is not valid is never dropped: the installer stops with an error before it changes anything.
- A configuration file that the installer cannot read (not a valid property list, not owned by root, more than one
  hard link, or larger than 64 KiB) is replaced by a new one without any of these settings; that repairs a damaged
  installation. The exception is such a file that contains the text `AllowedCDHashes`: the installer cannot keep
  the list and does not replace the file. It stops with an error before it changes anything, until an
  administrator corrects the file or removes it.
- When the installed service is kept (the compatibility rules above), the installer does not read or change the
  settings at all.

What the users of a restricted Mac see: a program that is not on the list gets no answer, its connection is cut,
and its probe reports status 2 ("unreachable"), the same as for a service that is not answering. The application
therefore offers to install the service again, which does not help. Tell those users, or list the application.

## Security and privacy

What the service means for a Mac, in plain terms:

- **Any program running as the user logged in at the Mac's screen can use RealSense cameras through the service**,
  without a password and whatever program it is, including one the user never meant to give a camera to. That is
  the price of applications working without being known to the service. `AllowedCDHashes` narrows it to listed
  programs, but it pins versions rather than containing code: the user's programs can start a listed program, or
  copy an application and swap its libraries.
- **The macOS camera indicator does not light up and no camera permission prompt appears** for images that come
  through the service: macOS sees a USB device in use, not a camera. Privacy (PPPC) profiles cannot target it.
- **The MDM / Screen Time camera ban is honoured**: with `allowCamera` false the service refuses everyone, root
  included, and ends running sessions.
- Only the console user in a local graphical session and root are admitted. A background fast-user-switched
  session, an SSH login and system accounts are refused, and a session loses its cameras when its user stops being
  the one at the screen.
- The daemon runs as root and its message parser can be reached by every program of that user. It opens RealSense
  devices only (vendor `0x38E5`, and `0x8086` with a RealSense product ID), never a device with an audio, storage,
  hub, network, wireless or boot keyboard/mouse interface; it checks every field of every request; a program can
  only use the interfaces it opened, with class requests only.
- It always refuses the three hardware-monitor commands that cannot be undone (erase the whole flash, lock or
  unlock it for good). Everything else librealsense does is allowed, including firmware update and calibration
  writes, so a program of the logged-in user can leave a camera in need of recovery unless `ProtectedFlashRanges` or
  `DeniedHWMOpcodes` are set.
- The installer runs as root and installs the daemon that the application carries: the administrator who types the
  password trusts that application, as with any installer. It takes files only from folders that only root or
  administrators can change.
- The daemon writes who connected (pid, uid, path), captures, hand-backs and refused commands to the unified log,
  never image data: `log show --last 1h --predicate 'subsystem == "io.github.dolegadolegowski.realsense-usbd"'`.

Where this is not acceptable: unplug the camera when it is not in use, set `AllowedCDHashes`, switch the service
off in Login Items & Extensions, or uninstall it.

## Uninstall

```sh
sudo "/Applications/MyApp.app/Contents/Resources/realsense-usbd/uninstall-camera-access.sh"
```

It stops the daemon, gives every captured camera back to macOS and removes the installed files, the marker
included. This removes the service for every application; each of them offers to install it again at its next
start.

## Tests

```sh
"$KIT/tests/test-camera-access.sh" build/usbd/realsense-usbd
```

tests the installer, the uninstaller, the manifest script and `ensure-camera-access.sh` without root (dry runs into
a temporary folder, a stand-in for `osascript`). `rs-usbd-tests` from `build-daemon.sh` tests the daemon against a
mock USB backend.

## Where this folder lives

The canonical copy is `macos/broker/` in
[github.com/dolegadolegowski/realsense-viewer-macos](https://github.com/dolegadolegowski/realsense-viewer-macos).
Copies in other repositories are not edited there: a change is made in the canonical repository (with a larger
`RSUSBD_SERVICE_VERSION` when the daemon changes) and the folder is copied again.

---

> 🇵🇱 **Po polsku:** `realsense-usbd` to jedna wspólna usługa systemowa (LaunchDaemon działający jako root), dzięki
> której aplikacje korzystają z kamer Intel RealSense bez hasła administratora. macOS pozwala odebrać kamerę
> wbudowanemu sterownikowi USB wideo tylko procesowi roota, więc bez usługi każdy program musiałby działać jako
> root. Usługa jest jedna na cały komputer i nie należy do żadnej aplikacji ani kompilacji: wpuszcza każdy program
> użytkownika zalogowanego przy ekranie Maca (oraz roota), więc aplikacje napisane później działają z usługą
> zainstalowaną dziś. Każda aplikacja ma w sobie pliki z tego folderu, przy starcie sprawdza, czy usługa odpowiada
> (`RealSenseCameraAccess.state` w Swifcie albo `ensure-camera-access.sh --check`), a gdy nie — instaluje ją;
> nazwa i hasło administratora są potrzebne raz na komputer. Instalator nigdy nie zastępuje nowszej usługi
> starszą i nie rusza działającej usługi w tej samej wersji (inaczej dwie aplikacje restartowałyby sobie nawzajem
> demona i przerywały pracę kamery), a demon nadal obsługuje starsze wersje protokołu. Jako root (np. przez SSH
> albo z narzędzia do zarządzania) uruchamia się bezpośrednio `install-camera-access.sh --source <folder>`; pełna
> składnia jest opisana wyżej. **Bezpieczeństwo i prywatność:** przez usługę każdy
> program działający jako zalogowany użytkownik może używać kamer RealSense; wskaźnik kamery macOS się nie zapala
> i nie pojawia się pytanie o zgodę; respektowany jest zakaz kamery z MDM / Czasu przed ekranem (`allowCamera`).
> Usługa otwiera wyłącznie kamery RealSense i zawsze odrzuca trzy nieodwracalne polecenia (skasowanie całej
> pamięci flash, trwałe zablokowanie i odblokowanie). Administrator może ograniczyć usługę do wybranych programów
> kluczem `AllowedCDHashes`. Odinstalowanie: `sudo …/Contents/Resources/realsense-usbd/uninstall-camera-access.sh`
> — usuwa usługę dla wszystkich aplikacji. Oryginał tego folderu znajduje się w repozytorium
> `realsense-viewer-macos` (`macos/broker`); kopii w innych repozytoriach nie edytuje się na miejscu.
