#!/bin/bash
# H7 (macos/broker/DESIGN.md 9.1): host-only tests of install-camera-access.sh and uninstall-camera-access.sh in their
# non-root --dry-run --root mode, with a fake ad-hoc signed daemon. Never needs root, never touches /Library.
#
#   scripts/test-camera-access.sh [work dir]     (default: a new temporary directory)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
INSTALL="$ROOT/macos/broker/install-camera-access.sh"
UNINSTALL="$ROOT/macos/broker/uninstall-camera-access.sh"
LABEL=io.github.dolegadolegowski.realsense-usbd
[ "$(id -u)" != 0 ] || { echo "run as a normal user" >&2; exit 1; }

W="${1:-$(mktemp -d)}"
mkdir -p "$W"
W="$(cd "$W" && pwd -P)"
rm -rf "$W/h7"
W="$W/h7"
mkdir -p "$W"

PASS=0
FAIL=0
ok() { PASS=$((PASS + 1)); echo "ok   $*"; }
bad() { FAIL=$((FAIL + 1)); echo "FAIL $*"; }
check() { local name="$1"; shift; if "$@"; then ok "$name"; else bad "$name"; fi; }

for s in "$INSTALL" "$UNINSTALL"; do
    check "bash -n $(basename "$s")" /bin/bash -n "$s"
    check "shebang $(basename "$s")" test "$(head -1 "$s")" = "#!/bin/bash -p"
done
if command -v shellcheck >/dev/null; then
    check "shellcheck" shellcheck -s bash "$INSTALL" "$UNINSTALL" "$0"
else
    echo "skip shellcheck (not installed)"
fi

# Fake daemon, signed like package.sh signs realsense-usbd
SRCDIR="$W/src/camera-access"
mkdir -p "$SRCDIR"
printf 'int main(void) { return 0; }\n' >"$W/fake.c"
clang -arch arm64 -o "$SRCDIR/realsense-usbd" "$W/fake.c"
codesign --force --sign - -o runtime -i "$LABEL" "$SRCDIR/realsense-usbd" 2>/dev/null
DCD="$(codesign -dvvv --arch arm64 "$SRCDIR/realsense-usbd" 2>&1 | sed -n 's/^CDHash=//p')"
OTHER=0123456789abcdef0123456789abcdef01234567

# make_allowlist <file> <BuildNumber> <DaemonCDHash> [BuildID]
make_allowlist() {
    local f="$1" bn="$2" dc="$3" bid="${4:-}"
    if [ -z "$bid" ]; then
        bid="$(printf '%s' "$(printf '%s\n' "$DCD" "$OTHER" | LC_ALL=C sort -u)" | shasum -a 256 | cut -d' ' -f1)"
    fi
    rm -f "$f"
    plutil -create xml1 "$f"
    plutil -insert Version -integer 1 "$f"
    plutil -insert BuildID -string "$bid" "$f"
    plutil -insert BuildNumber -integer "$bn" "$f"
    plutil -insert ProtocolVersion -integer 1 "$f"
    plutil -insert AllowedCDHashes -array "$f"
    plutil -insert AllowedCDHashes -string "$OTHER" -append "$f"
    plutil -insert AllowedCDHashes -string "$DCD" -append "$f"
    plutil -insert DaemonCDHash -string "$dc" "$f"
}
make_allowlist "$SRCDIR/allowlist.plist" 202611040000 "$DCD"
chmod 755 "$SRCDIR/realsense-usbd"
chmod 644 "$SRCDIR/allowlist.plist"

# run_install <log> <args...>: status in RC
run_install() { local log="$1"; shift; RC=0; "$INSTALL" "$@" >"$log" 2>&1 || RC=$?; }

# Baseline: the only trust failures allowed are ancestors of the work dir (e.g. /private/tmp is world-writable)
P1="$W/root1"
run_install "$W/base.log" --dry-run --root "$P1" --source "$SRCDIR"
grep 'untrusted' "$W/base.log" | sed 's/.*report-only): //' | sort -u >"$W/base.trust" || true
allowed_only=1
while read -r line; do
    p="${line%%:*}"
    case "$W" in "$p"/* | "$p") ;; *) allowed_only=0; echo "  unexpected: $line" ;; esac
done <"$W/base.trust"
check "baseline install exit 0 or 3 (got $RC)" test "$RC" = 0 -o "$RC" = 3
check "baseline trust failures only from work dir ancestors" test "$allowed_only" = 1

B="$P1/Library/PrivilegedHelperTools/$LABEL"
C="$P1/Library/Application Support/RealSense Viewer/usbd.plist"
D="$P1/Library/LaunchDaemons/$LABEL.plist"
mode() { stat -f %OLp "$1"; }
check "binary installed byte-identical" cmp -s "$SRCDIR/realsense-usbd" "$B"
check "binary mode 755" test "$(mode "$B")" = 755
# com.apple.provenance is added by macOS to every file a process writes and cannot be cleared
check "binary has no xattrs (except com.apple.provenance)" test -z "$(xattr "$B" | grep -v '^com.apple.provenance$')"
check "daemon plist has no xattrs (except com.apple.provenance)" test -z "$(xattr "$D" | grep -v '^com.apple.provenance$')"
check "no leftover tmp binary" test -z "$(find "$P1/Library/PrivilegedHelperTools" -name ".$LABEL.tmp.*")"
check "config dir mode 755" test "$(mode "$(dirname "$C")")" = 755
check "usbd.plist mode 644" test "$(mode "$C")" = 644
check "daemon plist mode 644" test "$(mode "$D")" = 644
check "daemon plist lint" plutil -lint -s "$D"
EXPECT_D='{"AssociatedBundleIdentifiers":["io.github.dolegadolegowski.realsense-viewer","io.github.dolegadolegowski.realsense-depth-quality"],"ExitTimeOut":30,"KeepAlive":{"SuccessfulExit":false},"Label":"io.github.dolegadolegowski.realsense-usbd","MachServices":{"io.github.dolegadolegowski.realsense-usbd":true},"ProcessType":"Interactive","ProgramArguments":["/Library/PrivilegedHelperTools/io.github.dolegadolegowski.realsense-usbd","--daemon"]}'
canon() { plutil -convert json -o - "$1" | python3 -I -c 'import json,sys; print(json.dumps(json.load(sys.stdin), sort_keys=True, separators=(",", ":")))'; }
check "daemon plist content (6.4)" test "$(canon "$D")" = "$EXPECT_D"
EXPECT_C="{\"AllowedCDHashes\":[\"$OTHER\",\"$DCD\"],\"BuildID\":\"$(plutil -extract BuildID raw "$SRCDIR/allowlist.plist")\",\"BuildNumber\":202611040000,\"DaemonCDHash\":\"$DCD\",\"ProtocolVersion\":1,\"Version\":1}"
check "usbd.plist = validated allowlist keys only" test "$(canon "$C")" = "$EXPECT_C"
check "no staging dir left" test -z "$(find "$P1/private/var/tmp" -mindepth 1)"
check "no lock left" test ! -e "$P1/var/run/$LABEL.install.lock"

# Admin keys survive a reinstall when valid, are dropped when invalid; unknown keys never survive
plutil -insert HandBackGraceSeconds -integer 30 "$C"
plutil -insert IdleExitSeconds -integer 5 "$C"
plutil -insert DeniedHWMOpcodes -json '[149,168]' "$C"
plutil -insert ProtectedFlashRanges -json '[{"Start":4096,"Length":8192}]' "$C"
plutil -insert Evil -string x "$C"
run_install "$W/keep.log" --dry-run --root "$P1" --source "$SRCDIR"
check "reinstall exit 0 or 3" test "$RC" = 0 -o "$RC" = 3
check "HandBackGraceSeconds kept" test "$(plutil -extract HandBackGraceSeconds raw "$C")" = 30
check "IdleExitSeconds 5 (out of range) dropped" test "$(plutil -type IdleExitSeconds "$C" 2>&1 | grep -c integer)" = 0
check "DeniedHWMOpcodes kept" test "$(plutil -extract DeniedHWMOpcodes json -o - "$C")" = "[149,168]"
check "ProtectedFlashRanges kept" test "$(canon "$C" | grep -o '"ProtectedFlashRanges":[^]]*]')" = '"ProtectedFlashRanges":[{"Length":8192,"Start":4096}]'
check "unknown key dropped" test "$(plutil -type Evil "$C" 2>&1 | grep -c string)" = 0
plutil -replace ProtectedFlashRanges -json '[{"Start":4294967295,"Length":2}]' "$C"
plutil -replace DeniedHWMOpcodes -json '[256]' "$C"
run_install "$W/drop.log" --dry-run --root "$P1" --source "$SRCDIR"
check "range past 2^32 dropped" grep -q "dropped invalid ProtectedFlashRanges" "$W/drop.log"
check "opcode 256 dropped" grep -q "dropped invalid DeniedHWMOpcodes" "$W/drop.log"

# Downgrade
plutil -replace BuildNumber -integer 209912312359 "$C"
run_install "$W/down.log" --dry-run --root "$P1" --source "$SRCDIR"
check "downgrade refused" test "$RC" = 1
check "downgrade message" grep -q "is newer than this one" "$W/down.log"
run_install "$W/down2.log" --dry-run --root "$P1" --source "$SRCDIR" --allow-downgrade
check "--allow-downgrade accepted (rc $RC)" test "$RC" = 0 -o "$RC" = 3

# Two sources (app layout: Contents/MacOS + Contents/Resources/realsense-usbd)
APP="$W/src/Test.app/Contents"
mkdir -p "$APP/MacOS" "$APP/Resources/realsense-usbd"
cp "$SRCDIR/realsense-usbd" "$APP/MacOS/"
cp "$SRCDIR/allowlist.plist" "$APP/Resources/realsense-usbd/"
run_install "$W/app.log" --dry-run --root "$W/root2" --source "$APP/MacOS" --source "$APP/Resources/realsense-usbd"
check "two --source dirs (rc $RC)" test "$RC" = 0 -o "$RC" = 3
run_install "$W/dup.log" --dry-run --root "$W/root2" --source "$SRCDIR" --source "$APP/MacOS"
check "ambiguous sources refused" grep -q "more than one --source" "$W/dup.log"

# Argument and mode rules
run_install "$W/noroot.log" --root "$W/root3" --source "$SRCDIR"
check "--root without --dry-run refused" grep -q "only allowed with --dry-run" "$W/noroot.log"
check "--root without --dry-run wrote nothing" test ! -e "$W/root3"
run_install "$W/nosudo.log" --source "$SRCDIR"
check "real install as non-root refused" grep -q "run as root" "$W/nosudo.log"
run_install "$W/noargs.log" --dry-run --root "$W/root3"
check "no --source refused (usage)" test "$RC" = 2

# Trust rule: every case must add its own report line to the baseline
trust_case() {   # trust_case <name> <expected substring> <install args...>
    local name="$1" want="$2"
    shift 2
    run_install "$W/t.log" --dry-run --root "$W/root4" "$@"
    if [ "$RC" = 3 ] && grep 'untrusted' "$W/t.log" | grep -qF -- "$want"; then ok "trust: $name"; else bad "trust: $name (rc $RC)"; sed 's/^/    /' "$W/t.log"; fi
}
WW="$W/src/ww"
mkdir -p "$WW"
cp "$SRCDIR/realsense-usbd" "$SRCDIR/allowlist.plist" "$WW/"
chmod 777 "$WW"
trust_case "world-writable source dir" "$WW: world-writable" --source "$WW"
chmod 755 "$WW"
chmod 666 "$WW/allowlist.plist"
trust_case "world-writable source file" "$WW/allowlist.plist: world-writable" --source "$WW"
chmod 644 "$WW/allowlist.plist"
GW="$W/src/gw"
mkdir -p "$GW"
cp "$SRCDIR/realsense-usbd" "$SRCDIR/allowlist.plist" "$GW/"
chmod 775 "$GW"
if [ "$(stat -f %g "$GW")" != 0 ] && [ "$(stat -f %g "$GW")" != 80 ]; then
    trust_case "group-writable dir of a non-admin group" "$GW: group-writable by group" --source "$GW"
fi
SL="$W/src/sl"
mkdir -p "$SL"
cp "$SRCDIR/realsense-usbd" "$SL/"
ln -s "$SRCDIR/allowlist.plist" "$SL/allowlist.plist"
trust_case "symlinked source file" "$SL/allowlist.plist: not a regular file" --source "$SL"
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
if [ "$RC" = 3 ] && grep -qF "goes through the symlink $SD" "$W/t.log"; then ok "uninstall: run through a symlinked dir"; else bad "uninstall: run through a symlinked dir (rc $RC)"; sed 's/^/    /' "$W/t.log"; fi
AD="$W/src/acl"
mkdir -p "$AD"
cp "$SRCDIR/realsense-usbd" "$SRCDIR/allowlist.plist" "$AD/"
chmod +a "everyone allow add_file,delete_child" "$AD"
trust_case "ACL entry allowing others to add files" "$AD: an ACL entry allows others to change it" --source "$AD"
chmod -a "everyone allow add_file,delete_child" "$AD"
chmod +a "everyone deny delete" "$AD"
run_install "$W/t.log" --dry-run --root "$W/root4" --source "$AD"
check "deny-only ACL accepted" bash -c '! grep -qF "$1: an ACL" "$2"' _ "$AD" "$W/t.log"
chmod -N "$AD"
HL="$W/src/hl"
mkdir -p "$HL"
cp "$SRCDIR/allowlist.plist" "$HL/"
ln "$SRCDIR/realsense-usbd" "$HL/realsense-usbd"
trust_case "hard-linked file" "has 2 hard links" --source "$HL"
TR="$W/src/AppTranslocation/X/d"
mkdir -p "$TR"
cp "$SRCDIR/realsense-usbd" "$SRCDIR/allowlist.plist" "$TR/"
trust_case "/AppTranslocation/ path" "translocated app" --source "$TR"

# Allowlist and signature validation (fatal in dry-run too)
bad_allowlist() {   # bad_allowlist <name> <expected substring>
    local name="$1" want="$2"
    run_install "$W/a.log" --dry-run --root "$W/root5" --source "$BA"
    if [ "$RC" = 1 ] && grep -qF -- "$want" "$W/a.log"; then ok "allowlist: $name"; else bad "allowlist: $name (rc $RC)"; sed 's/^/    /' "$W/a.log"; fi
}
BA="$W/src/ba"
mkdir -p "$BA"
cp "$SRCDIR/realsense-usbd" "$BA/"
make_allowlist "$BA/allowlist.plist" 1 "$OTHER"
bad_allowlist "DaemonCDHash is not the binary's" "is not the allowlist's DaemonCDHash"
make_allowlist "$BA/allowlist.plist" 1 "$DCD" "$(printf '%064d' 0)"
bad_allowlist "BuildID mismatch" "BuildID does not match"
make_allowlist "$BA/allowlist.plist" 1 "$DCD"
plutil -replace Version -integer 2 "$BA/allowlist.plist"
bad_allowlist "Version 2" "Version must be"
make_allowlist "$BA/allowlist.plist" 1 "$DCD"
plutil -replace AllowedCDHashes.0 -string "ABCDEF0123456789abcdef0123456789abcdef01" "$BA/allowlist.plist"
bad_allowlist "uppercase cdhash" "bad cdhash"
make_allowlist "$BA/allowlist.plist" 1 "$DCD"
plutil -replace BuildNumber -string 1 "$BA/allowlist.plist"
bad_allowlist "BuildNumber as string" "BuildNumber missing"
make_allowlist "$BA/allowlist.plist" 1 "$DCD"
printf 'not a plist' >"$BA/allowlist.plist"
bad_allowlist "not a plist" "not a valid property list"
make_allowlist "$BA/allowlist.plist" 1 "$DCD"
printf '#!/bin/sh\n' >"$BA/realsense-usbd"
chmod 755 "$BA/realsense-usbd"
bad_allowlist "daemon not Mach-O" "not an arm64 Mach-O executable"
cp "$SRCDIR/realsense-usbd" "$BA/realsense-usbd"
printf 'x' >>"$BA/realsense-usbd"
bad_allowlist "tampered daemon" "invalid code signature"

# Concurrent install/uninstall lock
mkdir -p "$W/root6/var/run/$LABEL.install.lock"
run_install "$W/lock.log" --dry-run --root "$W/root6" --source "$SRCDIR"
check "lock held -> refused" grep -q "another install/uninstall is running" "$W/lock.log"
check "foreign lock not removed" test -d "$W/root6/var/run/$LABEL.install.lock"

# Uninstall
mkdir -p "$P1/var/run/$LABEL" "$P1/private/var/tmp/rs-usbd.leftover"
touch "$P1/Library/PrivilegedHelperTools/.$LABEL.tmp.123" "$P1/Library/PrivilegedHelperTools/other-helper"
RC=0
"$UNINSTALL" --dry-run --root "$P1" >"$W/un.log" 2>&1 || RC=$?
check "uninstall exit 0 or 3 (rc $RC)" test "$RC" = 0 -o "$RC" = 3
check "uninstall removed binary" test ! -e "$B"
check "uninstall removed daemon plist" test ! -e "$D"
check "uninstall removed config and its dir" test ! -e "$(dirname "$C")"
check "uninstall removed tmp binary" test ! -e "$P1/Library/PrivilegedHelperTools/.$LABEL.tmp.123"
check "uninstall kept other helpers" test -e "$P1/Library/PrivilegedHelperTools/other-helper"
check "uninstall removed state dir" test ! -e "$P1/var/run/$LABEL"
check "uninstall removed staging dirs" test ! -e "$P1/private/var/tmp/rs-usbd.leftover"
RC=0
"$UNINSTALL" --root "$P1" >"$W/un2.log" 2>&1 || RC=$?
check "uninstall --root without --dry-run refused" grep -q "only allowed with --dry-run" "$W/un2.log"
RC=0
"$UNINSTALL" >"$W/un3.log" 2>&1 || RC=$?
check "uninstall as non-root refused" grep -q "run as root" "$W/un3.log"

echo
echo "H7: $PASS passed, $FAIL failed (logs in $W)"
[ "$FAIL" = 0 ]
