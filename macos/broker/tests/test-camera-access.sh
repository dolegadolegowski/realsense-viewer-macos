#!/bin/bash
# Host-only tests of the camera service's scripts (DESIGN.md 9.1 H7 and 13.8): install-camera-access.sh and
# uninstall-camera-access.sh in their non-root --dry-run --root mode, make-service-manifest.sh, and
# ensure-camera-access.sh with stand-ins for the probe tool and for osascript. Never needs root, never touches
# /Library, never shows an administrator prompt.
#
#   tests/test-camera-access.sh <built realsense-usbd> [work dir]     (default: a new temporary directory)
#
# <built realsense-usbd> is what build-daemon.sh wrote; the test signs a copy of it and leaves the file alone.
# Exit status 0 when every check passed.
set -euo pipefail

KIT="$(cd "$(dirname "$0")/.." && pwd -P)"
INSTALL="$KIT/install-camera-access.sh"
UNINSTALL="$KIT/uninstall-camera-access.sh"
ENSURE="$KIT/ensure-camera-access.sh"
MAKE_MANIFEST="$KIT/make-service-manifest.sh"
LABEL=io.github.dolegadolegowski.realsense-usbd
[ $# -ge 1 ] && [ $# -le 2 ] || { echo "usage: $0 <built realsense-usbd> [work dir]" >&2; exit 2; }
[ "$(id -u)" != 0 ] || { echo "run as a normal user" >&2; exit 1; }
[ -f "$1" ] && [ -x "$1" ] || { echo "$1: not an executable file" >&2; exit 2; }
BUILT="$(cd "$(dirname "$1")" && pwd -P)/$(basename "$1")"

W="${2:-$(mktemp -d)}"
mkdir -p "$W"
W="$(cd "$W" && pwd -P)"
rm -rf "$W/h7"
W="$W/h7"
mkdir -p "$W"

PASS=0
FAIL=0
RC=0
ok() { PASS=$((PASS + 1)); echo "ok   $*"; }
bad() { FAIL=$((FAIL + 1)); echo "FAIL $*"; }
check() { local name="$1"; shift; if "$@"; then ok "$name"; else bad "$name"; fi; }
has() { grep -qF -- "$2" "$1"; }
has_not() { ! grep -qF -- "$2" "$1"; }
mode() { stat -f %OLp "$1"; }
inode() { stat -f %i "$1"; }
# A dry run ends with 3 when a trust check failed, which is so wherever the work dir is below /private/tmp
rc_ok() { [ "$RC" = 0 ] || [ "$RC" = 3 ]; }
# Property lists are compared in plutil's XML form, which sorts the keys
canon() { plutil -convert xml1 -o - "$1"; }
# is_plist <file> <json>: the file holds exactly these keys and values
is_plist() { [ "$(canon "$1")" = "$(printf '%s' "$2" | plutil -convert xml1 -o - -)" ]; }
# json_plist <file> <json>: writes the property list
json_plist() { printf '%s' "$2" | plutil -convert xml1 -o "$1" -; }

for s in "$INSTALL" "$UNINSTALL" "$ENSURE"; do
    check "bash -n $(basename "$s")" /bin/bash -n "$s"
    check "shebang $(basename "$s")" test "$(head -1 "$s")" = "#!/bin/bash -p"
done
if command -v shellcheck >/dev/null; then
    check "shellcheck" shellcheck -s bash "$INSTALL" "$UNINSTALL" "$ENSURE" "$0"
else
    echo "skip shellcheck (not installed)"
fi

echo "# the daemon: --version, signature, manifest"
VERSION_LINE="$("$BUILT" --version 2>/dev/null || true)"
RE_VERSION='^realsense-usbd ([1-9][0-9]*) protocol ([1-9][0-9]*)-([1-9][0-9]*)$'
if ! [[ "$VERSION_LINE" =~ $RE_VERSION ]]; then
    bad "$BUILT --version prints \"realsense-usbd <service version> protocol <min>-<max>\" (got: $VERSION_LINE)"
    exit 1
fi
SV="${BASH_REMATCH[1]}"
PMIN="${BASH_REMATCH[2]}"
PMAX="${BASH_REMATCH[3]}"
ok "--version: service version $SV, protocol $PMIN-$PMAX"
check "service version is 2 or more" test "$SV" -ge 2

# The daemon as an application ships it: signed, then described by its manifest
cdhash_of() { codesign -dvvv --arch arm64 "$1" 2>&1 | sed -n 's/^CDHash=//p'; }
SRCDIR="$W/src/camera-access"
mkdir -p "$SRCDIR"
cp "$BUILT" "$SRCDIR/realsense-usbd"
chmod 755 "$SRCDIR/realsense-usbd"
codesign --force --sign - -o runtime -i "$LABEL" "$SRCDIR/realsense-usbd" 2>/dev/null
DCD="$(cdhash_of "$SRCDIR/realsense-usbd")"
check "signed copy has an arm64 cdhash" test "${#DCD}" = 40
# The same program under another signature: valid, but not the one a manifest for $DCD names
mkdir -p "$W/src/other"
OTHERBIN="$W/src/other/realsense-usbd"
cp "$BUILT" "$OTHERBIN"
chmod 755 "$OTHERBIN"
codesign --force --sign - -o runtime -i "$LABEL.other" "$OTHERBIN" 2>/dev/null
ODCD="$(cdhash_of "$OTHERBIN")"
check "another signature gives another cdhash" test "${#ODCD}" = 40 -a "$ODCD" != "$DCD"
OTHER=0123456789abcdef0123456789abcdef01234567

# make_manifest <file> <ServiceVersion> <DaemonCDHash>: what make-service-manifest.sh writes, by hand
make_manifest() {
    rm -f "$1"
    plutil -create xml1 "$1"
    plutil -insert Version -integer 2 "$1"
    plutil -insert ServiceVersion -integer "$2" "$1"
    plutil -insert ProtocolMin -integer "$PMIN" "$1"
    plutil -insert ProtocolMax -integer "$PMAX" "$1"
    plutil -insert DaemonCDHash -string "$3" "$1"
    chmod 644 "$1"
}
MANIFEST_JSON="{\"DaemonCDHash\":\"$DCD\",\"ProtocolMax\":$PMAX,\"ProtocolMin\":$PMIN,\"ServiceVersion\":$SV,\"Version\":2}"
if [ -x "$MAKE_MANIFEST" ]; then
    RC=0
    "$MAKE_MANIFEST" "$SRCDIR/realsense-usbd" "$SRCDIR/service.plist" >"$W/mm.log" 2>&1 || RC=$?
    check "make-service-manifest.sh (rc $RC)" test "$RC" = 0
    check "manifest = Version 2, the --version numbers and the cdhash" is_plist "$SRCDIR/service.plist" "$MANIFEST_JSON"
    check "manifest mode 644" test "$(mode "$SRCDIR/service.plist")" = 644
    mm_refuses() {   # mm_refuses <name> <binary>: non-zero status, a message, nothing written
        local name="$1" out="$W/mm-refused.plist"
        rm -f "$out"
        RC=0
        "$MAKE_MANIFEST" "$2" "$out" >"$W/mm.log" 2>&1 || RC=$?
        if [ "$RC" != 0 ] && [ -s "$W/mm.log" ] && [ ! -e "$out" ]; then
            ok "manifest: $name refused"
        else
            bad "manifest: $name refused (rc $RC)"
        fi
    }
    cp "$BUILT" "$W/src/unsigned"
    codesign --remove-signature "$W/src/unsigned" 2>/dev/null
    mm_refuses "unsigned daemon" "$W/src/unsigned"
    printf '#!/bin/sh\necho "realsense-usbd 2 protocol 1-1"\n' >"$W/src/script"
    chmod 755 "$W/src/script"
    mm_refuses "a script" "$W/src/script"
    if command -v clang >/dev/null; then
        printf '#include <stdio.h>\nint main(void) { puts("realsense-usbd two"); return 0; }\n' >"$W/src/odd.c"
        clang -arch arm64 -o "$W/src/odd" "$W/src/odd.c"
        codesign --force --sign - -o runtime -i "$LABEL" "$W/src/odd" 2>/dev/null
        mm_refuses "another --version line" "$W/src/odd"
    else
        echo "skip manifest: another --version line (no clang)"
    fi
else
    echo "skip make-service-manifest.sh (not in this kit): writing the manifest by hand"
    make_manifest "$SRCDIR/service.plist" "$SV" "$DCD"
fi
chmod 644 "$SRCDIR/service.plist"

# run_install <log> <args...>: status in RC
run_install() { local log="$1"; shift; RC=0; "$INSTALL" "$@" >"$log" 2>&1 || RC=$?; }

echo "# fresh installation"
# Baseline: the only trust failures allowed are ancestors of the work dir and of the kit (e.g. /private/tmp is
# world-writable)
P1="$W/root1"
run_install "$W/base.log" --dry-run --root "$P1" --source "$SRCDIR"
grep 'untrusted' "$W/base.log" | sed 's/.*report-only): //' | sort -u >"$W/base.trust" || true
allowed_only=1
while read -r line; do
    p="${line%%:*}"
    case "$W" in "$p"/* | "$p") continue ;; esac
    case "$KIT" in "$p"/* | "$p") continue ;; esac
    allowed_only=0
    echo "  unexpected: $line"
done <"$W/base.trust"
check "baseline install exit 0 or 3 (got $RC)" rc_ok
check "baseline trust failures only from ancestors of the work dir and the kit" test "$allowed_only" = 1
check "baseline says it was a dry run" has "$W/base.log" "install-camera-access: dry run: installed layout written under $P1"

B="$P1/Library/PrivilegedHelperTools/$LABEL"
C="$P1/Library/Application Support/$LABEL/usbd.plist"
M="$P1/Library/Application Support/RealSense Viewer/usbd.plist"
D="$P1/Library/LaunchDaemons/$LABEL.plist"
MARKER_JSON='{"BuildNumber":999999999999}'
check "binary installed byte-identical" cmp -s "$SRCDIR/realsense-usbd" "$B"
check "binary mode 755" test "$(mode "$B")" = 755
# com.apple.provenance is added by macOS to every file a process writes and cannot be cleared
check "binary has no xattrs (except com.apple.provenance)" test -z "$(xattr "$B" | grep -v '^com.apple.provenance$')"
check "daemon plist has no xattrs (except com.apple.provenance)" test -z "$(xattr "$D" | grep -v '^com.apple.provenance$')"
check "no leftover tmp binary" test -z "$(find "$P1/Library/PrivilegedHelperTools" -name ".$LABEL.tmp.*")"
check "config dir mode 755" test "$(mode "$(dirname "$C")")" = 755
check "usbd.plist mode 644" test "$(mode "$C")" = 644
check "usbd.plist = the validated manifest values only" is_plist "$C" "$MANIFEST_JSON"
check "legacy marker dir mode 755" test "$(mode "$(dirname "$M")")" = 755
check "legacy marker mode 644" test "$(mode "$M")" = 644
check "legacy marker = BuildNumber 999999999999 and nothing else" is_plist "$M" "$MARKER_JSON"
check "daemon plist mode 644" test "$(mode "$D")" = 644
check "daemon plist lint" plutil -lint -s "$D"
DAEMON_JSON='{"AssociatedBundleIdentifiers":["io.github.dolegadolegowski.realsense-viewer","io.github.dolegadolegowski.realsense-depth-quality","io.github.dolegadolegowski.IntelFaceScan"],"ExitTimeOut":30,"KeepAlive":{"SuccessfulExit":false},"Label":"io.github.dolegadolegowski.realsense-usbd","MachServices":{"io.github.dolegadolegowski.realsense-usbd":true},"ProcessType":"Interactive","ProgramArguments":["/Library/PrivilegedHelperTools/io.github.dolegadolegowski.realsense-usbd","--daemon"]}'
check "daemon plist content (6.4, 13.5)" is_plist "$D" "$DAEMON_JSON"
check "no staging dir left" test -z "$(find "$P1/private/var/tmp" -mindepth 1)"
check "no lock left" test ! -e "$P1/var/run/$LABEL.install.lock"

echo "# the administrator's settings"
# They survive a reinstall when valid and are dropped when invalid; unknown keys never survive
plutil -insert HandBackGraceSeconds -integer 30 "$C"
plutil -insert IdleExitSeconds -integer 5 "$C"
plutil -insert DeniedHWMOpcodes -json '[149,168]' "$C"
plutil -insert ProtectedFlashRanges -json '[{"Start":4096,"Length":8192}]' "$C"
plutil -insert AllowedCDHashes -json "[\"$OTHER\",\"$DCD\"]" "$C"
plutil -insert Evil -string x "$C"
run_install "$W/keep.log" --dry-run --root "$P1" --source "$SRCDIR"
check "reinstall exit 0 or 3 (got $RC)" rc_ok
check "HandBackGraceSeconds kept" test "$(plutil -extract HandBackGraceSeconds raw "$C")" = 30
check "IdleExitSeconds 5 (out of range) dropped" test "$(plutil -type IdleExitSeconds "$C" 2>&1 | grep -c integer)" = 0
check "valid settings kept (also AllowedCDHashes), unknown key dropped" is_plist "$C" "{\"AllowedCDHashes\":[\"$OTHER\",\"$DCD\"],\"DaemonCDHash\":\"$DCD\",\"DeniedHWMOpcodes\":[149,168],\"HandBackGraceSeconds\":30,\"ProtectedFlashRanges\":[{\"Length\":8192,\"Start\":4096}],\"ProtocolMax\":$PMAX,\"ProtocolMin\":$PMIN,\"ServiceVersion\":$SV,\"Version\":2}"
check "AllowedCDHashes kept: said so" has "$W/keep.log" "kept AllowedCDHashes (2)"
plutil -replace ProtectedFlashRanges -json '[{"Start":4294967295,"Length":2}]' "$C"
plutil -replace DeniedHWMOpcodes -json '[256]' "$C"
plutil -replace AllowedCDHashes -json '[]' "$C"
run_install "$W/drop.log" --dry-run --root "$P1" --source "$SRCDIR"
check "range past 2^32 dropped" has "$W/drop.log" "dropped invalid ProtectedFlashRanges"
check "opcode 256 dropped" has "$W/drop.log" "dropped invalid DeniedHWMOpcodes"
check "an empty AllowedCDHashes stays empty" test "$(plutil -extract AllowedCDHashes raw -expect array "$C")" = 0
# A list of programs that is not valid stops the installation before anything is replaced
plutil -replace AllowedCDHashes -json '["ABCDEF0123456789abcdef0123456789abcdef01"]' "$C"
plutil -insert Sentinel -string here "$C"
before="$(inode "$B")"
run_install "$W/badlist.log" --dry-run --root "$P1" --source "$SRCDIR"
check "invalid AllowedCDHashes: refused (rc $RC)" test "$RC" = 1
check "invalid AllowedCDHashes: message names the key" has "$W/badlist.log" "AllowedCDHashes is not valid"
check "invalid AllowedCDHashes: configuration not replaced" test "$(plutil -extract Sentinel raw "$C")" = here
check "invalid AllowedCDHashes: binary not replaced" test "$(inode "$B")" = "$before"
plutil -replace AllowedCDHashes -string all "$C"
run_install "$W/badlist2.log" --dry-run --root "$P1" --source "$SRCDIR"
check "AllowedCDHashes of another type: refused (rc $RC)" test "$RC" = 1
plutil -remove AllowedCDHashes "$C"
run_install "$W/relist.log" --dry-run --root "$P1" --source "$SRCDIR"
check "after the key is removed: installs (rc $RC)" rc_ok
# What is left of the settings above: the manifest values and the one setting that stayed valid
KEPT_JSON="{\"DaemonCDHash\":\"$DCD\",\"HandBackGraceSeconds\":30,\"ProtocolMax\":$PMAX,\"ProtocolMin\":$PMIN,\"ServiceVersion\":$SV,\"Version\":2}"
check "after the key is removed: configuration rewritten" is_plist "$C" "$KEPT_JSON"
# A configuration that cannot be read: replaced by a new one (the repair), unless it names AllowedCDHashes. Then
# the list cannot be kept, and the installation stops before anything is replaced.
cp "$C" "$W/conf.save"
no_stage_or_lock() { [ -z "$(find "$P1/private/var/tmp" -mindepth 1)" ] && [ ! -e "$P1/var/run/$LABEL.install.lock" ]; }
plutil -insert AllowedCDHashes -json "[\"$OTHER\"]" "$C"
sed 's|</array>|</aray>|' "$C" >"$W/conf.broken"
cp "$W/conf.broken" "$C"
before="$(inode "$B")"
run_install "$W/unreadable.log" --dry-run --root "$P1" --source "$SRCDIR"
check "unreadable configuration that names AllowedCDHashes: refused (rc $RC)" test "$RC" = 1
check "unreadable configuration that names AllowedCDHashes: says so" has "$W/unreadable.log" "names AllowedCDHashes"
check "unreadable configuration that names AllowedCDHashes: file left as it is" cmp -s "$W/conf.broken" "$C"
check "unreadable configuration that names AllowedCDHashes: binary not replaced" test "$(inode "$B")" = "$before"
check "unreadable configuration that names AllowedCDHashes: no staging dir or lock left" no_stage_or_lock
# The same for a valid list in a file with a second hard link, here in the binary format
cp "$W/conf.save" "$C"
plutil -insert AllowedCDHashes -json "[\"$OTHER\"]" "$C"
plutil -convert binary1 "$C"
ln "$C" "$W/conf.link"
run_install "$W/unreadable2.log" --dry-run --root "$P1" --source "$SRCDIR"
check "hard-linked binary configuration that names AllowedCDHashes: refused (rc $RC)" test "$RC" = 1 -a "$(inode "$B")" = "$before"
check "hard-linked binary configuration that names AllowedCDHashes: says so" has "$W/unreadable2.log" "names AllowedCDHashes"
rm -f "$W/conf.link"
run_install "$W/unreadable3.log" --dry-run --root "$P1" --source "$SRCDIR"
check "the same file with one link: installs, list kept (rc $RC)" has "$W/unreadable3.log" "kept AllowedCDHashes (1)"
# Without the key there is no restriction to lose: the damaged file is replaced, its other settings with it
printf '<plist version="1.0"><dict><key>HandBackGraceSeconds</key><integer>30</integer>' >"$C"
run_install "$W/damaged.log" --dry-run --root "$P1" --source "$SRCDIR"
check "unreadable configuration without AllowedCDHashes: installs (rc $RC)" rc_ok
check "unreadable configuration without AllowedCDHashes: said so" has "$W/damaged.log" "ignoring the existing $C"
check "unreadable configuration without AllowedCDHashes: taken for a damaged installation" has "$W/damaged.log" "replacing a camera service without a valid configuration"
check "unreadable configuration without AllowedCDHashes: a new configuration, no settings" is_plist "$C" "$MANIFEST_JSON"
cp "$W/conf.save" "$C"

echo "# a newer installed service is kept"
plutil -replace ServiceVersion -integer $((SV + 1)) "$C"
plutil -insert Sentinel -string here "$C"
rm -f "$M"
# Leftovers of the withdrawn lists of other applications (DESIGN 13.5.8): only an installation removes them
XD="$(dirname "$M")/usbd.d"
XT="$(dirname "$B")/$LABEL-allowlist.sh"
leftovers() {   # leftovers <usbd.d path> <list tool path>: what that design left behind
    mkdir -p "$1"
    json_plist "$1/other-app.plist" "{\"AllowedCDHashes\":[\"$OTHER\"]}"
    touch "$1/.other-app.plist.tmp.1" "$2.keep" "$(dirname "$2")/.$(basename "$2").tmp.77"
    ln -s "$W/outside-file" "$1/link.plist"
    printf '#!/bin/bash -p\n' >"$2"
}
no_leftovers() { [ ! -e "$1" ] && [ ! -L "$1" ] && [ ! -e "$2" ] && [ ! -e "$(dirname "$2")/.$(basename "$2").tmp.77" ]; }
mkdir -p "$W/outside-lists"
touch "$W/outside-file" "$W/outside-lists/keep-me"
leftovers "$XD" "$XT"
before="$(inode "$B") $(inode "$D")"
run_install "$W/newer.log" --dry-run --root "$P1" --source "$SRCDIR"
check "newer installed: exit 0 or 3 (got $RC)" rc_ok
check "newer installed: said so" has "$W/newer.log" "the installed camera service is version $((SV + 1)), newer than this application's $SV: keeping it"
check "newer installed: nothing written, said so" has "$W/newer.log" "dry run: nothing under $P1 was changed"
check "newer installed: configuration untouched" test "$(plutil -extract ServiceVersion raw "$C") $(plutil -extract Sentinel raw "$C")" = "$((SV + 1)) here"
check "newer installed: binary and daemon plist untouched" test "$(inode "$B") $(inode "$D")" = "$before"
check "newer installed: legacy marker not written" test ! -e "$M"
check "newer installed: usbd.d and the list tool left alone" test -f "$XD/other-app.plist" -a -f "$XT"
check "newer installed: no staging dir or lock left" test -z "$(find "$P1/private/var/tmp" -mindepth 1)" -a ! -e "$P1/var/run/$LABEL.install.lock"
# A later service may write a later configuration schema (Version). It is still seen as newer and kept.
plutil -replace Version -integer 3 "$C"
run_install "$W/newer-schema.log" --dry-run --root "$P1" --source "$SRCDIR"
check "newer installed, configuration schema 3: exit 0 or 3 (got $RC)" rc_ok
check "newer installed, configuration schema 3: kept, said so" has "$W/newer-schema.log" "the installed camera service is version $((SV + 1)), newer than this application's $SV: keeping it"
check "newer installed, configuration schema 3: configuration untouched" test "$(plutil -extract Version raw "$C") $(plutil -extract ServiceVersion raw "$C") $(plutil -extract Sentinel raw "$C")" = "3 $((SV + 1)) here"
check "newer installed, configuration schema 3: binary and daemon plist untouched" test "$(inode "$B") $(inode "$D")" = "$before"
plutil -replace Version -integer 2 "$C"
run_install "$W/down.log" --dry-run --root "$P1" --source "$SRCDIR" --allow-downgrade
check "--allow-downgrade: installs (rc $RC)" rc_ok
check "--allow-downgrade: said so" has "$W/down.log" "with the older $SV (--allow-downgrade)"
check "--allow-downgrade: configuration is this application's, settings kept" is_plist "$C" "$KEPT_JSON"
check "--allow-downgrade: legacy marker written" is_plist "$M" "$MARKER_JSON"
check "installation removes usbd.d, the list tool and its temporary file" no_leftovers "$XD" "$XT"
check "installation removes usbd.d: said so" has "$W/down.log" "removed $XD"
check "installation removes usbd.d: a link in it is not followed, other files stay" test -e "$W/outside-file" -a -e "$XT.keep"
# Kept only when it is whole: the daemon the configuration names, and its job
plutil -replace ServiceVersion -integer $((SV + 1)) "$C"
cp "$OTHERBIN" "$B"
run_install "$W/notwhole.log" --dry-run --root "$P1" --source "$SRCDIR"
check "newer configuration, another daemon: installs (rc $RC)" rc_ok
check "newer configuration, another daemon: said so" has "$W/notwhole.log" "is missing or is another one: installing version $SV"
check "newer configuration, another daemon: daemon replaced" cmp -s "$SRCDIR/realsense-usbd" "$B"
check "newer configuration, another daemon: configuration replaced" is_plist "$C" "$KEPT_JSON"
plutil -replace ServiceVersion -integer $((SV + 1)) "$C"
rm -f "$D"
run_install "$W/nojob.log" --dry-run --root "$P1" --source "$SRCDIR"
check "newer configuration, no daemon plist: installs (rc $RC)" rc_ok
check "newer configuration, no daemon plist: plist written" is_plist "$D" "$DAEMON_JSON"
check "newer configuration, no daemon plist: configuration replaced" is_plist "$C" "$KEPT_JSON"
# The same version: a real installation keeps it when its daemon answers. A dry run cannot ask the daemon, so it
# writes the staged version again, with and without --allow-downgrade.
before="$(inode "$B")"
run_install "$W/same.log" --dry-run --root "$P1" --source "$SRCDIR"
check "same version, dry run: installed again (rc $RC)" test "$(inode "$B")" != "$before"
check "same version, dry run: not reported as kept" has_not "$W/same.log" "already installed"
check "same version, dry run: says the probe was skipped" has "$W/same.log" "dry run: installed layout written under $P1 (launchctl and the probe skipped)"
before="$(inode "$B")"
run_install "$W/same-force.log" --dry-run --root "$P1" --source "$SRCDIR" --allow-downgrade
check "same version, --allow-downgrade: installed again (rc $RC)" test "$(inode "$B")" != "$before"
check "same version, --allow-downgrade: not called a downgrade" has_not "$W/same-force.log" "with the older"

echo "# upgrade from generation 1"
G="$W/root-gen1"
GB="$G/Library/PrivilegedHelperTools/$LABEL"
GC="$G/Library/Application Support/$LABEL/usbd.plist"
GM="$G/Library/Application Support/RealSense Viewer/usbd.plist"
GD="$G/Library/LaunchDaemons/$LABEL.plist"
GEN1_JSON="{\"Version\":1,\"BuildID\":\"$(printf '%064d' 7)\",\"BuildNumber\":202611040000,\"ProtocolVersion\":1,\"AllowedCDHashes\":[\"$OTHER\",\"$ODCD\"],\"DaemonCDHash\":\"$ODCD\",\"HandBackGraceSeconds\":12,\"IdleExitSeconds\":120,\"DeniedHWMOpcodes\":[149],\"ProtectedFlashRanges\":[{\"Start\":0,\"Length\":4096}]}"
GEN1_JOB='{"Label":"io.github.dolegadolegowski.realsense-usbd","AssociatedBundleIdentifiers":["io.github.dolegadolegowski.realsense-viewer","io.github.dolegadolegowski.realsense-depth-quality"]}'
mkdir -p "$(dirname "$GB")" "$(dirname "$GM")" "$(dirname "$GD")"
json_plist "$GM" "$GEN1_JSON"
cp "$OTHERBIN" "$GB"
json_plist "$GD" "$GEN1_JOB"
GX="$(dirname "$GM")/usbd.d"
GT="$(dirname "$GB")/$LABEL-allowlist.sh"
leftovers "$GX" "$GT"
run_install "$W/gen1.log" --dry-run --root "$G" --source "$SRCDIR"
check "generation 1: upgraded (rc $RC)" rc_ok
check "generation 1 (also with lists of other applications): said so" has "$W/gen1.log" "replacing generation 1 of camera access"
check "generation 1: usbd.d, the list tool and its temporary file removed" no_leftovers "$GX" "$GT"
check "generation 1: daemon replaced" cmp -s "$SRCDIR/realsense-usbd" "$GB"
check "generation 1: daemon plist replaced" is_plist "$GD" "$DAEMON_JSON"
check "generation 1: settings migrated, but not its AllowedCDHashes or build keys" is_plist "$GC" "{\"DaemonCDHash\":\"$DCD\",\"DeniedHWMOpcodes\":[149],\"HandBackGraceSeconds\":12,\"IdleExitSeconds\":120,\"ProtectedFlashRanges\":[{\"Length\":4096,\"Start\":0}],\"ProtocolMax\":$PMAX,\"ProtocolMin\":$PMIN,\"ServiceVersion\":$SV,\"Version\":2}"
check "generation 1: its configuration replaced by the marker" is_plist "$GM" "$MARKER_JSON"
# A generation-1 configuration next to this service's own: the settings come from this service's
json_plist "$GM" "$GEN1_JSON"
plutil -replace HandBackGraceSeconds -integer 44 "$GC"
ln -s "$W/outside-lists" "$GX"
run_install "$W/gen1b.log" --dry-run --root "$G" --source "$SRCDIR"
check "usbd.d as a symlink: the link removed, its target left alone" test ! -L "$GX" -a -e "$W/outside-lists/keep-me"
check "both configurations: settings from the new path" test "$(plutil -extract HandBackGraceSeconds raw "$GC")" = 44
check "both configurations: marker restored" is_plist "$GM" "$MARKER_JSON"
# The marker alone is not an installation of generation 1
mkdir -p "$W/root-marker/Library/Application Support/RealSense Viewer"
json_plist "$W/root-marker/Library/Application Support/RealSense Viewer/usbd.plist" "$MARKER_JSON"
run_install "$W/marker.log" --dry-run --root "$W/root-marker" --source "$SRCDIR"
check "marker alone: a fresh installation (rc $RC)" rc_ok
check "marker alone: not taken for generation 1" has_not "$W/marker.log" "generation 1"
# Daemon files without any configuration count as generation 1 (older than every shared service)
mkdir -p "$W/root-stray/Library/LaunchDaemons"
json_plist "$W/root-stray/Library/LaunchDaemons/$LABEL.plist" "$GEN1_JOB"
run_install "$W/stray.log" --dry-run --root "$W/root-stray" --source "$SRCDIR"
check "daemon plist without configuration: replaced, said so (rc $RC)" has "$W/stray.log" "replacing a camera service without a valid configuration"
check "daemon plist without configuration: new plist" is_plist "$W/root-stray/Library/LaunchDaemons/$LABEL.plist" "$DAEMON_JSON"

echo "# sources"
# Application layouts: Contents/MacOS or Contents/Helpers + Contents/Resources/realsense-usbd
APP="$W/src/Test.app/Contents"
mkdir -p "$APP/MacOS" "$APP/Helpers" "$APP/Resources/realsense-usbd"
cp "$SRCDIR/realsense-usbd" "$APP/MacOS/"
cp "$SRCDIR/service.plist" "$APP/Resources/realsense-usbd/"
run_install "$W/app.log" --dry-run --root "$W/root2" --source "$APP/MacOS" --source "$APP/Resources/realsense-usbd"
check "two --source dirs (rc $RC)" rc_ok
mv "$APP/MacOS/realsense-usbd" "$APP/Helpers/"
run_install "$W/app2.log" --dry-run --root "$W/root2b" --source "$APP/Helpers" --source "$APP/Resources/realsense-usbd"
check "daemon in Contents/Helpers (rc $RC)" rc_ok
run_install "$W/dup.log" --dry-run --root "$W/root2" --source "$SRCDIR" --source "$APP/Helpers"
check "ambiguous sources refused" has "$W/dup.log" "more than one --source"
# A folder name with a space, both kinds of quotes, a backslash and a dollar sign
ODD="$W/src/Cam's \"Test\" \\ \$HOME App"
mkdir -p "$ODD"
cp "$SRCDIR/realsense-usbd" "$SRCDIR/service.plist" "$INSTALL" "$ODD/"
RC=0
"$ODD/install-camera-access.sh" --dry-run --root "$W/root odd" --source "$ODD" >"$W/odd.log" 2>&1 || RC=$?
check "installs from and into folders with spaces and quotes (rc $RC)" rc_ok
check "installs from and into folders with spaces and quotes: daemon there" cmp -s "$SRCDIR/realsense-usbd" "$W/root odd/Library/PrivilegedHelperTools/$LABEL"

echo "# arguments"
run_install "$W/noroot.log" --root "$W/root3" --source "$SRCDIR"
check "--root without --dry-run refused" has "$W/noroot.log" "only allowed with --dry-run"
check "--root without --dry-run wrote nothing" test ! -e "$W/root3"
run_install "$W/nosudo.log" --source "$SRCDIR"
check "real install as non-root refused" has "$W/nosudo.log" "run as root"
run_install "$W/noargs.log" --dry-run --root "$W/root3"
check "no --source refused (usage)" test "$RC" = 2
run_install "$W/three.log" --dry-run --root "$W/root3" --source "$SRCDIR" --source "$APP/Helpers" --source "$APP/MacOS"
check "three --source refused (usage)" test "$RC" = 2

echo "# trust rule"
# Every case must add its own report line to the baseline
trust_case() {   # trust_case <name> <expected substring> <install args...>
    local name="$1" want="$2"
    shift 2
    run_install "$W/t.log" --dry-run --root "$W/root4" "$@"
    if [ "$RC" = 3 ] && grep 'untrusted' "$W/t.log" | grep -qF -- "$want"; then
        ok "trust: $name"
    else
        bad "trust: $name (rc $RC)"
        sed 's/^/    /' "$W/t.log"
    fi
}
WW="$W/src/ww"
mkdir -p "$WW"
cp "$SRCDIR/realsense-usbd" "$SRCDIR/service.plist" "$WW/"
chmod 777 "$WW"
trust_case "world-writable source dir" "$WW: world-writable" --source "$WW"
chmod 755 "$WW"
chmod 666 "$WW/service.plist"
trust_case "world-writable source file" "$WW/service.plist: world-writable" --source "$WW"
chmod 644 "$WW/service.plist"
GW="$W/src/gw"
mkdir -p "$GW"
cp "$SRCDIR/realsense-usbd" "$SRCDIR/service.plist" "$GW/"
chmod 775 "$GW"
if [ "$(stat -f %g "$GW")" != 0 ] && [ "$(stat -f %g "$GW")" != 80 ]; then
    trust_case "group-writable dir of a non-admin group" "$GW: group-writable by group" --source "$GW"
fi
SL="$W/src/sl"
mkdir -p "$SL"
cp "$SRCDIR/realsense-usbd" "$SL/"
ln -s "$SRCDIR/service.plist" "$SL/service.plist"
trust_case "symlinked source file" "$SL/service.plist: not a regular file" --source "$SL"
LD="$W/src/linkdir"
ln -s "$SRCDIR" "$LD"
trust_case "source through a symlinked dir" "$LD: goes through the symlink $LD" --source "$LD"
ln -s "$W/src" "$W/src/linkparent"
trust_case "source below a symlinked dir" "goes through the symlink $W/src/linkparent" --source "$W/src/linkparent/camera-access"
SD="$W/src/scriptlink"
ln -s "$(dirname "$INSTALL")" "$SD"
INSTALL_REAL="$INSTALL"
INSTALL="$SD/$(basename "$INSTALL_REAL")"
trust_case "script run through a symlinked dir" "goes through the symlink $SD" --source "$SRCDIR"
INSTALL="$INSTALL_REAL"
RC=0
mkdir -p "$W/root7"
"$SD/$(basename "$UNINSTALL")" --dry-run --root "$W/root7" >"$W/t.log" 2>&1 || RC=$?
if [ "$RC" = 3 ] && has "$W/t.log" "goes through the symlink $SD"; then
    ok "uninstall: run through a symlinked dir"
else
    bad "uninstall: run through a symlinked dir (rc $RC)"
    sed 's/^/    /' "$W/t.log"
fi
AD="$W/src/acl"
mkdir -p "$AD"
cp "$SRCDIR/realsense-usbd" "$SRCDIR/service.plist" "$AD/"
chmod +a "everyone allow add_file,delete_child" "$AD"
trust_case "ACL entry allowing others to add files" "$AD: an ACL entry allows others to change it" --source "$AD"
chmod -a "everyone allow add_file,delete_child" "$AD"
chmod +a "everyone deny delete" "$AD"
run_install "$W/t.log" --dry-run --root "$W/root4" --source "$AD"
check "deny-only ACL accepted" has_not "$W/t.log" "$AD: an ACL"
chmod -N "$AD"
HL="$W/src/hl"
mkdir -p "$HL"
cp "$SRCDIR/service.plist" "$HL/"
ln "$SRCDIR/realsense-usbd" "$HL/realsense-usbd"
trust_case "hard-linked file" "has 2 hard links" --source "$HL"
rm -f "$HL/realsense-usbd"
TR="$W/src/AppTranslocation/X/d"
mkdir -p "$TR"
cp "$SRCDIR/realsense-usbd" "$SRCDIR/service.plist" "$TR/"
trust_case "/AppTranslocation/ path" "translocated app" --source "$TR"

echo "# manifest and signature"
# Fatal in a dry run too, and before anything is written
refused() {   # refused <name> <expected substring>: installing from $BA fails with that message
    local name="$1" want="$2"
    run_install "$W/a.log" --dry-run --root "$W/root5" --source "$BA"
    if [ "$RC" = 1 ] && has "$W/a.log" "$want"; then
        ok "refused: $name"
    else
        bad "refused: $name (rc $RC)"
        sed 's/^/    /' "$W/a.log"
    fi
}
BA="$W/src/ba"
mkdir -p "$BA"
cp "$SRCDIR/realsense-usbd" "$BA/"
make_manifest "$BA/service.plist" "$SV" "$OTHER"
refused "DaemonCDHash is not the daemon's" "is not the manifest's DaemonCDHash"
make_manifest "$BA/service.plist" "$SV" "$DCD"
plutil -replace Version -integer 1 "$BA/service.plist"
refused "Version 1" "Version must be the integer 2"
plutil -replace Version -string 2 "$BA/service.plist"
refused "Version as a string" "Version must be the integer 2"
make_manifest "$BA/service.plist" 1 "$DCD"
refused "ServiceVersion 1" "ServiceVersion must be"
plutil -replace ServiceVersion -string "$SV" "$BA/service.plist"
refused "ServiceVersion as a string" "ServiceVersion missing"
plutil -remove ServiceVersion "$BA/service.plist"
refused "no ServiceVersion" "ServiceVersion missing"
make_manifest "$BA/service.plist" "$SV" "$DCD"
plutil -replace ProtocolMin -integer 0 "$BA/service.plist"
refused "ProtocolMin 0" "ProtocolMin <= ProtocolMax"
plutil -replace ProtocolMin -integer $((PMAX + 1)) "$BA/service.plist"
refused "ProtocolMin above ProtocolMax" "ProtocolMin <= ProtocolMax"
plutil -replace ProtocolMin -integer 1 "$BA/service.plist"
plutil -replace ProtocolMax -integer 1000 "$BA/service.plist"
refused "ProtocolMax 1000" "ProtocolMin <= ProtocolMax"
plutil -remove ProtocolMax "$BA/service.plist"
refused "no ProtocolMax" "ProtocolMax missing"
make_manifest "$BA/service.plist" "$SV" "$DCD"
plutil -remove ProtocolMin "$BA/service.plist"
refused "no ProtocolMin" "ProtocolMin missing"
make_manifest "$BA/service.plist" "$SV" "ABCDEF0123456789abcdef0123456789abcdef01"
refused "uppercase DaemonCDHash" "bad DaemonCDHash"
make_manifest "$BA/service.plist" "$SV" "$DCD"
plutil -remove DaemonCDHash "$BA/service.plist"
refused "no DaemonCDHash" "DaemonCDHash missing"
printf 'not a plist' >"$BA/service.plist"
refused "not a property list" "not a valid property list"
rm -f "$BA/service.plist"
json_plist "$BA/allowlist.plist" "{\"Version\":1,\"DaemonCDHash\":\"$DCD\"}"
refused "generation 1's allowlist.plist instead of service.plist" "no service.plist"
rm -f "$BA/allowlist.plist"
make_manifest "$BA/service.plist" "$SV" "$DCD"
printf '#!/bin/sh\n' >"$BA/realsense-usbd"
chmod 755 "$BA/realsense-usbd"
refused "daemon not Mach-O" "not an arm64 Mach-O executable"
cp "$SRCDIR/realsense-usbd" "$BA/realsense-usbd"
printf 'x' >>"$BA/realsense-usbd"
refused "tampered daemon" "invalid code signature"
cp "$OTHERBIN" "$BA/realsense-usbd"
refused "daemon under another signature" "cdhash $ODCD is not the manifest's DaemonCDHash"
rm -f "$BA/realsense-usbd"
refused "no daemon" "no realsense-usbd"
check "refused installations wrote no daemon, configuration or job" test ! -e "$W/root5/Library/PrivilegedHelperTools" -a ! -e "$W/root5/Library/Application Support" -a -z "$(find "$W/root5/Library/LaunchDaemons" -mindepth 1)"

echo "# lock"
mkdir -p "$W/root6/var/run/$LABEL.install.lock"
run_install "$W/lock.log" --dry-run --root "$W/root6" --source "$SRCDIR"
check "lock held -> refused" has "$W/lock.log" "another install/uninstall is running"
check "foreign lock not removed" test -d "$W/root6/var/run/$LABEL.install.lock"

echo "# uninstall"
mkdir -p "$P1/var/run/$LABEL" "$P1/private/var/tmp/rs-usbd.leftover"
touch "$P1/Library/PrivilegedHelperTools/.$LABEL.tmp.123" "$P1/Library/PrivilegedHelperTools/other-helper"
leftovers "$XD" "$XT"
RC=0
"$UNINSTALL" --dry-run --root "$P1" >"$W/un.log" 2>&1 || RC=$?
check "uninstall exit 0 or 3 (rc $RC)" rc_ok
check "uninstall removed binary" test ! -e "$B"
check "uninstall removed daemon plist" test ! -e "$D"
check "uninstall removed config and its dir" test ! -e "$(dirname "$C")"
check "uninstall removed the legacy marker and its dir" test ! -e "$(dirname "$M")"
check "uninstall removed usbd.d, the list tool and its temporary file" no_leftovers "$XD" "$XT"
check "uninstall: a link in usbd.d is not followed" test -e "$W/outside-file"
check "uninstall removed tmp binary" test ! -e "$P1/Library/PrivilegedHelperTools/.$LABEL.tmp.123"
check "uninstall kept other helpers" test -e "$P1/Library/PrivilegedHelperTools/other-helper"
check "uninstall removed state dir" test ! -e "$P1/var/run/$LABEL"
check "uninstall removed staging dirs" test ! -e "$P1/private/var/tmp/rs-usbd.leftover"
# The marker goes; its "RealSense Viewer" folder stays when something else is in it
touch "$(dirname "$GM")/something-else"
ln -s "$W/outside-lists" "$GX"
RC=0
"$UNINSTALL" --dry-run --root "$G" >"$W/un-gen1.log" 2>&1 || RC=$?
check "uninstall: usbd.d as a symlink removed, its target left alone" test ! -L "$GX" -a -e "$W/outside-lists/keep-me"
check "uninstall: marker removed, its folder kept when not empty (rc $RC)" test ! -e "$GM" -a -e "$(dirname "$GM")/something-else"
check "uninstall: nothing else left" test ! -e "$GB" -a ! -e "$GD" -a ! -e "$(dirname "$GC")"
RC=0
"$UNINSTALL" --root "$P1" >"$W/un2.log" 2>&1 || RC=$?
check "uninstall --root without --dry-run refused" has "$W/un2.log" "only allowed with --dry-run"
RC=0
"$UNINSTALL" >"$W/un3.log" 2>&1 || RC=$?
check "uninstall as non-root refused" has "$W/un3.log" "run as root"

echo "# ensure-camera-access.sh"
# Stand-ins: a probe tool that answers what the file "answer" next to it says, and an osascript that records its
# arguments and does what the file "mode" next to it says. The stand-in osascript is the only one on PATH when the
# script runs, so no administrator prompt can appear.
FAKEBIN="$W/fakebin"
mkdir -p "$FAKEBIN"
cat >"$W/probe-tool" <<'TOOL'
#!/bin/sh
[ "$1" = --probe ] || exit 1
case "$(cat "$(dirname "$0")/answer")" in
    0) echo "ok 7"; exit 0 ;;
    3) echo "not the console user"; exit 3 ;;
    4) echo "camera disabled by policy"; exit 4 ;;
    5) echo "protocol version 9"; exit 5 ;;
    6) echo "too many clients"; exit 6 ;;
    silent3) exit 3 ;;
    killed) kill -TERM $$ ;;
    usage) echo "usage: realsense-usbd"; exit 1 ;;
    *) echo "unreachable: the camera service is not installed, switched off, or does not admit this program"; exit 2 ;;
esac
TOOL
cat >"$FAKEBIN/osascript" <<'TOOL'
#!/bin/sh
here="$(dirname "$0")"
n=0
for a in "$@"; do
    n=$((n + 1))
    printf '%s' "$a" >"$here/arg$n"
done
echo "$n" >"$here/argc"
answer="$(cat "$here/answer-path")"
case "$(cat "$here/mode")" in
    installs)
        echo 0 >"$answer"
        printf 'install-camera-access: installed the RealSense camera service, version 7 (ok 7)\r'
        printf 'install-camera-access:   /Library/somewhere\r' ;;
    installs-late) (sleep 2; echo 0 >"$answer") >/dev/null 2>&1 & ;;
    keeps-5) echo 5 >"$answer" ;;
    cancelled) echo "0:160: execution error: User canceled. (-128)" >&2; exit 1 ;;
    fails)
        printf '0:160: execution error: install-camera-access: error: untrusted: /somewhere: world-writable\r' >&2
        printf 'second line (1)\n' >&2
        exit 1 ;;
    garbled) echo "something else went wrong" >&2; exit 1 ;;
esac
exit 0
TOOL
chmod 755 "$W/probe-tool" "$FAKEBIN/osascript"

# kit_app <Contents dir> <MacOS|Helpers>: an application bundle with the stand-in probe tool as its realsense-usbd
kit_app() {
    mkdir -p "$1/$2" "$1/Resources/realsense-usbd"
    cp "$W/probe-tool" "$1/$2/realsense-usbd"
    cp "$INSTALL" "$UNINSTALL" "$ENSURE" "$SRCDIR/service.plist" "$1/Resources/realsense-usbd/"
}
# run_ensure <stderr log> <script> <args...>: stdout in OUT, status in RC, CALLED = 1 when osascript was run
OUT=
CALLED=0
run_ensure() {
    local log="$1" script="$2"
    shift 2
    [ -x "$FAKEBIN/osascript" ] || { echo "the stand-in osascript is missing: not running $script" >&2; exit 1; }
    rm -f "$FAKEBIN"/arg[0-9]* "$FAKEBIN/argc"
    RC=0
    OUT="$(env PATH="$FAKEBIN" "$script" "$@" 2>"$log")" || RC=$?
    if [ -e "$FAKEBIN/argc" ]; then CALLED=1; else CALLED=0; fi
}
# What --check says when nothing answers depends on whether this Mac has the service's job
if [ -e "/Library/LaunchDaemons/$LABEL.plist" ]; then
    NO_ANSWER=2
    NO_ANSWER_TEXT="unreachable: the camera service is not installed, switched off, or does not admit this program"
    OLD_PROTOCOL=5
    OLD_PROTOCOL_TEXT="protocol version 9"
else
    NO_ANSWER=10
    NO_ANSWER_TEXT="not installed"
    OLD_PROTOCOL=10
    OLD_PROTOCOL_TEXT="not installed"
fi

EA="$W/ens/Cam's \"Test\" \\ \$HOME App/Demo.app/Contents"
kit_app "$EA" MacOS
ER="$EA/Resources/realsense-usbd"
EE="$ER/ensure-camera-access.sh"
echo "$EA/MacOS/answer" >"$FAKEBIN/answer-path"
echo installs >"$FAKEBIN/mode"
check_case() {   # check_case <answer> <expected status> <expected line>
    echo "$1" >"$EA/MacOS/answer"
    run_ensure "$W/e.log" "$EE" --check
    if [ "$RC" = "$2" ] && [ "$OUT" = "$3" ] && [ "$CALLED" = 0 ]; then
        ok "--check, probe answers $1: $2 \"$3\""
    else
        bad "--check, probe answers $1: want $2 \"$3\", got $RC \"$OUT\" (osascript called: $CALLED)"
    fi
}
check_case 0 0 "ok 7"
check_case 3 3 "not the console user"
check_case 4 4 "camera disabled by policy"
check_case 6 6 "too many clients"
check_case silent3 3 "not the user logged in at this Mac's screen"
check_case 2 "$NO_ANSWER" "$NO_ANSWER_TEXT"
check_case 5 "$OLD_PROTOCOL" "$OLD_PROTOCOL_TEXT"
echo killed >"$EA/MacOS/answer"
run_ensure "$W/e.log" "$EE" --check
check "--check, probe tool killed: not reachable (rc $RC)" test "$RC" = "$NO_ANSWER" -a "$CALLED" = 0
echo usage >"$EA/MacOS/answer"
run_ensure "$W/e.log" "$EE" --check
check "--check, probe tool exits 1: not reachable (rc $RC)" test "$RC" = "$NO_ANSWER" -a "$CALLED" = 0
# The built daemon as the probe tool: --check agrees with its own --probe
rc=0
env -i "$SRCDIR/realsense-usbd" --probe >"$W/real-probe.out" 2>/dev/null || rc=$?
case "$rc" in
    0 | 3 | 4 | 6) want="$rc" ;;
    5) want="$OLD_PROTOCOL" ;;
    *) want="$NO_ANSWER" ;;
esac
run_ensure "$W/e.log" "$ENSURE" --check --source "$SRCDIR"
check "--check with the built daemon: its --probe exits $rc, the script $want (got $RC \"$OUT\")" test "$RC" = "$want" -a "$CALLED" = 0
if [ "$rc" = 0 ]; then
    check "--check with the built daemon: prints the probe's line" test "$OUT" = "$(head -1 "$W/real-probe.out")"
fi
run_ensure "$W/e.log" "$EE" --check --bogus
check "unknown option: usage (rc $RC)" test "$RC" = 2 -a "$CALLED" = 0
run_ensure "$W/e.log" "$EE" --check --source "$FAKEBIN"
check "no realsense-usbd in --source: exit 2" test "$RC" = 2 -a "$CALLED" = 0
check "no realsense-usbd in --source: says so" has "$W/e.log" "no realsense-usbd in"

# Installation: the AppleScript is that of RealSenseCameraAccess.install, with every path quoted for the shell and
# then for AppleScript. The reference quoting here is done with sed.
ref_shell_quote() { printf "'%s'" "$(printf '%s' "$1" | sed "s/'/'\\\\''/g")"; }
ref_applescript_quote() { printf '"%s"' "$(printf '%s' "$1" | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g')"; }
# install_command <installer> <source dir>...: the command that must run as root
install_command() {
    local c dir
    c="/usr/bin/env -i PATH=/usr/bin:/bin:/usr/sbin:/sbin /bin/bash -p $(ref_shell_quote "$1")"
    shift
    for dir in "$@"; do c="$c --source $(ref_shell_quote "$dir")"; done
    printf '%s' "$c"
}
# script_runs <command>: the recorded AppleScript runs exactly this command with administrator privileges
script_runs() {
    case "$(cat "$FAKEBIN/arg2" 2>/dev/null)" in
        "do shell script $(ref_applescript_quote "$1") with administrator privileges with prompt \""*) return 0 ;;
    esac
    return 1
}
PROMPT="Demo's \"camera\" service \\ once"
WANT_COMMAND="$(install_command "$ER/install-camera-access.sh" "$EA/MacOS" "$ER")"
WANT_TAIL=" with administrator privileges with prompt $(ref_applescript_quote "$PROMPT")"
echo 2 >"$EA/MacOS/answer"
run_ensure "$W/e-install.log" "$EE" --prompt "$PROMPT"
check "install: exit 0 once the service answers (rc $RC)" test "$RC" = 0
check "install: prints the probe's line" test "$OUT" = "ok 7"
check "install: osascript -e <script>" test "$CALLED" = 1 -a "$(cat "$FAKEBIN/argc" 2>/dev/null)" = 2 -a "$(cat "$FAKEBIN/arg1" 2>/dev/null)" = "-e"
check "install: the script, with a path that has a space, quotes, a backslash and a dollar sign" test "$(cat "$FAKEBIN/arg2" 2>/dev/null)" = "do shell script $(ref_applescript_quote "$WANT_COMMAND")$WANT_TAIL"
check "install: the installer's lines go to stderr" has "$W/e-install.log" "install-camera-access: installed the RealSense camera service"
# The same string read back the way macOS reads it: AppleScript's string literal, then the shell's words. Only the
# literal is given to the real osascript (no "do shell script", no privileges).
SRC="$(cat "$FAKEBIN/arg2" 2>/dev/null || true)"
LITERAL="${SRC#do shell script }"
LITERAL="${LITERAL%"$WANT_TAIL"}"
if [ "$LITERAL" != "$SRC" ] && [ "$(/usr/bin/osascript -e 'return "a \"b\" \\ c"' 2>/dev/null)" = 'a "b" \ c' ]; then
    COMMAND="$(/usr/bin/osascript -e "return $LITERAL" 2>/dev/null || true)"
    check "install: AppleScript reads the literal back as the command" test "$COMMAND" = "$WANT_COMMAND"
    eval "set -- $COMMAND"
    check "install: the shell reads the command as 10 words" test "$#" = 10
    check "install: installer and --source paths arrive whole" test "${6:-}" = "$ER/install-camera-access.sh" -a "${7:-}" = "--source" -a "${8:-}" = "$EA/MacOS" -a "${9:-}" = "--source" -a "${10:-}" = "$ER"
else
    echo "skip reading the AppleScript literal back (osascript cannot run here, or the script is not as expected)"
fi
echo 2 >"$EA/MacOS/answer"
run_ensure "$W/e.log" "$EE"
check "install: a default prompt when none is given" test "$CALLED" = 1 -a "$(grep -c 'with administrator privileges with prompt "The RealSense camera service' "$FAKEBIN/arg2")" = 1
echo 5 >"$EA/MacOS/answer"
run_ensure "$W/e.log" "$EE"
check "install: also when the service speaks another protocol version (rc $RC)" test "$RC" = 0 -a "$CALLED" = 1
echo installs-late >"$FAKEBIN/mode"
echo 2 >"$EA/MacOS/answer"
run_ensure "$W/e.log" "$EE"
check "install: waits for the daemon to start (rc $RC)" test "$RC" = 0 -a "$OUT" = "ok 7"
echo keeps-5 >"$FAKEBIN/mode"
echo 2 >"$EA/MacOS/answer"
run_ensure "$W/e.log" "$EE"
check "install: the exit status is the probe's afterwards (rc $RC)" test "$RC" = 5 -a "$OUT" = "protocol version 9"
echo cancelled >"$FAKEBIN/mode"
echo 2 >"$EA/MacOS/answer"
run_ensure "$W/e.log" "$EE"
check "cancelled prompt: exit 20 (rc $RC)" test "$RC" = 20 -a -z "$OUT"
echo fails >"$FAKEBIN/mode"
run_ensure "$W/e.log" "$EE"
check "installer failure: exit 21 (rc $RC)" test "$RC" = 21 -a -z "$OUT"
check "installer failure: its message on stderr, without osascript's wrapping" test "$(cat "$W/e.log")" = "install-camera-access: error: untrusted: /somewhere: world-writable
second line"
echo garbled >"$FAKEBIN/mode"
run_ensure "$W/e.log" "$EE"
check "osascript failure of another kind: exit 21 with its text (rc $RC)" test "$RC" = 21 -a "$(cat "$W/e.log")" = "something else went wrong"
echo installs >"$FAKEBIN/mode"
for a in 3 4 6; do
    echo "$a" >"$EA/MacOS/answer"
    run_ensure "$W/e.log" "$EE"
    check "probe answers $a: no installation, exit $a (rc $RC)" test "$RC" = "$a" -a "$CALLED" = 0
done
echo 0 >"$EA/MacOS/answer"
run_ensure "$W/e.log" "$EE"
check "service ready: no installation, exit 0" test "$RC" = 0 -a "$CALLED" = 0 -a "$OUT" = "ok 7"

# The other layouts, and files that are missing
EH="$W/ens/Helper.app/Contents"
kit_app "$EH" Helpers
EHR="$EH/Resources/realsense-usbd"
echo "$EH/Helpers/answer" >"$FAKEBIN/answer-path"
echo 2 >"$EH/Helpers/answer"
run_ensure "$W/e.log" "$EHR/ensure-camera-access.sh"
check "daemon in Contents/Helpers: found (rc $RC)" test "$RC" = 0 -a "$OUT" = "ok 7"
check "daemon in Contents/Helpers: installed from there" script_runs "$(install_command "$EHR/install-camera-access.sh" "$EH/Helpers" "$EHR")"
EF="$W/ens/one-folder"
mkdir -p "$EF"
cp "$W/probe-tool" "$EF/realsense-usbd"
cp "$INSTALL" "$ENSURE" "$SRCDIR/service.plist" "$EF/"
echo "$EF/answer" >"$FAKEBIN/answer-path"
echo 2 >"$EF/answer"
run_ensure "$W/e.log" "$EF/ensure-camera-access.sh"
check "everything in one folder: one --source (rc $RC)" script_runs "$(install_command "$EF/install-camera-access.sh" "$EF")"
ln -s "$EF" "$W/ens/link"
echo 2 >"$EF/answer"
run_ensure "$W/e.log" "$W/ens/link/ensure-camera-access.sh"
check "run through a symlink: the installer gets real paths" script_runs "$(install_command "$EF/install-camera-access.sh" "$EF")"
echo 2 >"$EF/answer"
run_ensure "$W/e.log" "$ENSURE" --source "$EHR" --source "$W/ens/link"
check "--source: the folder with both files, by its real path" script_runs "$(install_command "$KIT/install-camera-access.sh" "$EF")"
rm -f "$EF/install-camera-access.sh"
echo 2 >"$EF/answer"
run_ensure "$W/e.log" "$EF/ensure-camera-access.sh"
check "no installer next to the script: exit 2, no prompt (rc $RC)" test "$RC" = 2 -a "$CALLED" = 0
check "no installer next to the script: says so" has "$W/e.log" "install-camera-access.sh is missing"
run_ensure "$W/e.log" "$EF/ensure-camera-access.sh" --check
check "no installer next to the script: --check still answers (rc $RC)" test "$RC" = "$NO_ANSWER"
cp "$INSTALL" "$EF/"
rm -f "$EF/service.plist"
run_ensure "$W/e.log" "$EF/ensure-camera-access.sh"
check "no service.plist: exit 2, no prompt (rc $RC)" test "$RC" = 2 -a "$CALLED" = 0

echo
echo "camera access scripts: $PASS passed, $FAIL failed (logs in $W)"
[ "$FAIL" = 0 ]
