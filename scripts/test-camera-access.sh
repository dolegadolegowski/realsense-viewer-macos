#!/bin/bash
# H7 (macos/broker/DESIGN.md 9.1): host-only tests of install-camera-access.sh, uninstall-camera-access.sh and
# camera-access-allowlist.sh in their non-root --dry-run --root mode, with a fake ad-hoc signed daemon. Never needs
# root, never touches /Library.
#
#   scripts/test-camera-access.sh [work dir]     (default: a new temporary directory)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
INSTALL="$ROOT/macos/broker/install-camera-access.sh"
UNINSTALL="$ROOT/macos/broker/uninstall-camera-access.sh"
LISTTOOL="$ROOT/macos/broker/camera-access-allowlist.sh"
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

for s in "$INSTALL" "$UNINSTALL" "$LISTTOOL"; do
    check "bash -n $(basename "$s")" /bin/bash -n "$s"
    check "shebang $(basename "$s")" test "$(head -1 "$s")" = "#!/bin/bash -p"
done
if command -v shellcheck >/dev/null; then
    check "shellcheck" shellcheck -s bash "$INSTALL" "$UNINSTALL" "$LISTTOOL" "$0"
else
    echo "skip shellcheck (not installed)"
fi
# The trust rule (DESIGN 6.2) is the same code in the installer and the list tool
fn_body() { sed -n "/^$1() {/,/^}/p" "$2"; }
same_text() { [ -n "$1" ] && [ "$1" = "$2" ]; }
for f in admin_uid no_symlink_in acl_allows_write trust_path; do
    check "6.2 function $f identical in the list tool" same_text "$(fn_body "$f" "$INSTALL")" "$(fn_body "$f" "$LISTTOOL")"
done

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
T1="$P1/Library/PrivilegedHelperTools/$LABEL-allowlist.sh"
check "list tool installed byte-identical" cmp -s "$LISTTOOL" "$T1"
check "list tool mode 755" test "$(mode "$T1")" = 755
check "list tool has no xattrs (except com.apple.provenance)" test -z "$(xattr "$T1" | grep -v '^com.apple.provenance$')"
check "no temporary file left in PrivilegedHelperTools" test -z "$(find "$P1/Library/PrivilegedHelperTools" -name '.*')"
check "no usbd.d created by the installer" test ! -e "$(dirname "$C")/usbd.d"
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
# shellcheck disable=SC2016  # $1 and $2 belong to the inner shell
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

# List tool (DESIGN 6.7): lists of other apps in usbd.d, on a prefix with camera access installed (dry run)
LT="$W/lt"
run_install "$W/lt-install.log" --dry-run --root "$LT" --source "$SRCDIR"
check "list tool: camera access installed in its prefix (rc $RC)" test "$RC" = 0 -o "$RC" = 3
XD="$LT/Library/Application Support/RealSense Viewer/usbd.d"
NAME=io.github.example.app
H1=1111111111111111111111111111111111111111
H2=2222222222222222222222222222222222222222
# run_tool <log> <args...>: status in RC
run_tool() { local log="$1"; shift; RC=0; "$LISTTOOL" "$@" >"$log" 2>&1 || RC=$?; }
build_id() { printf '%s' "$(printf '%s\n' "$@" | LC_ALL=C sort -u)" | shasum -a 256 | cut -d' ' -f1; }
# make_list <file> <name> <BuildNumber> <hash>...: a valid list (DESIGN 5.4)
make_list() {
    local f="$1" n="$2" bn="$3" h
    shift 3
    rm -f "$f"
    plutil -create xml1 "$f"
    plutil -insert Version -integer 1 "$f"
    plutil -insert Name -string "$n" "$f"
    plutil -insert BuildID -string "$(build_id "$@")" "$f"
    plutil -insert BuildNumber -integer "$bn" "$f"
    plutil -insert ProtocolVersion -integer 1 "$f"
    plutil -insert AllowedCDHashes -array "$f"
    for h in "$@"; do plutil -insert AllowedCDHashes -string "$h" -append "$f"; done
    chmod 644 "$f"
}
hashes() {   # hashes <count> <prefix digit>: distinct 40-hex hashes
    local i
    for ((i = 0; i < $1; i++)); do printf '%s%039x\n' "$2" "$i"; done
}
LS="$W/src/lists"
mkdir -p "$LS"
make_list "$LS/list.plist" "$NAME" 202611050000 "$H2" "$H1"
plutil -insert Evil -string x "$LS/list.plist"
run_tool "$W/lt-add.log" --dry-run --root "$LT" --add "$NAME" --source "$LS/list.plist"
check "list tool: --add (rc $RC)" test "$RC" = 0 -o "$RC" = 3
check "list tool: --add reports what it enabled" grep -q "enabled $NAME (build 202611050000, 2 code hashes)" "$W/lt-add.log"
check "list tool: usbd.d mode 755" test "$(mode "$XD")" = 755
check "list tool: list mode 644" test "$(mode "$XD/$NAME.plist")" = 644
EXPECT_L="{\"AllowedCDHashes\":[\"$H1\",\"$H2\"],\"BuildID\":\"$(build_id "$H1" "$H2")\",\"BuildNumber\":202611050000,\"Name\":\"$NAME\",\"ProtocolVersion\":1,\"Version\":1}"
check "list tool: list = validated keys only, sorted hashes" test "$(canon "$XD/$NAME.plist")" = "$EXPECT_L"
check "list tool: list has no xattrs (except com.apple.provenance)" test -z "$(xattr "$XD/$NAME.plist" | grep -v '^com.apple.provenance$')"
check "list tool: no temporary file left" test -z "$(find "$XD" -name '.*')"
check "list tool: no staging dir left" test -z "$(find "$LT/private/var/tmp" -mindepth 1)"
check "list tool: no lock left" test ! -e "$LT/var/run/$LABEL.install.lock"
check "list tool: usbd.plist untouched" test "$(canon "$LT/Library/Application Support/RealSense Viewer/usbd.plist")" = "$EXPECT_C"

# refused contents (fatal in a dry run too)
tool_refuses() {   # tool_refuses <name> <expected substring> <tool args...>
    local name="$1" want="$2"
    shift 2
    run_tool "$W/r.log" --dry-run --root "$LT" "$@"
    if [ "$RC" = 1 ] && grep -qF -- "$want" "$W/r.log"; then ok "list tool refuses: $name"; else bad "list tool refuses: $name (rc $RC)"; sed 's/^/    /' "$W/r.log"; fi
}
BL="$W/src/badlists"
mkdir -p "$BL"
tool_refuses "Name mismatch" "Name is not io.github.other.app" --add io.github.other.app --source "$LS/list.plist"
for n in ../x A.b x io.github..x .a.b a.b- 'a b.c'; do
    tool_refuses "bad name '$n'" "bad list name" --add "$n" --source "$LS/list.plist"
done
make_list "$BL/v2.plist" "$NAME" 1 "$H1"
plutil -replace Version -integer 2 "$BL/v2.plist"
tool_refuses "Version 2" "Version must be" --add "$NAME" --source "$BL/v2.plist"
make_list "$BL/upper.plist" "$NAME" 1 "$H1" ABCDEF0123456789abcdef0123456789abcdef01
tool_refuses "upper-case cdhash" "AllowedCDHashes must hold" --add "$NAME" --source "$BL/upper.plist"
make_list "$BL/short.plist" "$NAME" 1 "$H1" 0123
tool_refuses "short cdhash" "AllowedCDHashes must hold" --add "$NAME" --source "$BL/short.plist"
# shellcheck disable=SC2046  # one word per hash
make_list "$BL/65.plist" "$NAME" 1 $(hashes 65 3)
tool_refuses "65 cdhashes" "AllowedCDHashes must hold" --add "$NAME" --source "$BL/65.plist"
make_list "$BL/none.plist" "$NAME" 1 "$H1"
plutil -replace AllowedCDHashes -json '[]' "$BL/none.plist"
tool_refuses "no cdhash" "AllowedCDHashes must hold" --add "$NAME" --source "$BL/none.plist"
make_list "$BL/dup.plist" "$NAME" 1 "$H1" "$H1"
tool_refuses "duplicate cdhash" "twice" --add "$NAME" --source "$BL/dup.plist"
make_list "$BL/bid.plist" "$NAME" 1 "$H1"
plutil -replace BuildID -string "$(printf '%064d' 0)" "$BL/bid.plist"
tool_refuses "BuildID mismatch" "BuildID does not match" --add "$NAME" --source "$BL/bid.plist"
make_list "$BL/bn.plist" "$NAME" 1 "$H1"
plutil -replace BuildNumber -string 1 "$BL/bn.plist"
tool_refuses "BuildNumber as string" "BuildNumber must be" --add "$NAME" --source "$BL/bn.plist"
make_list "$BL/pv.plist" "$NAME" 1 "$H1"
plutil -replace ProtocolVersion -integer 2 "$BL/pv.plist"
tool_refuses "other ProtocolVersion" "was made for camera access protocol 2, this Mac's camera access speaks 1" --add "$NAME" --source "$BL/pv.plist"
make_list "$BL/big.plist" "$NAME" 1 "$H1"
plutil -insert Pad -string "$(printf '%070000d' 0)" "$BL/big.plist"
tool_refuses "list over 64 KiB" "larger than 64 KiB" --add "$NAME" --source "$BL/big.plist"
printf 'not a plist' >"$BL/text.plist"
tool_refuses "not a plist" "not a valid property list" --add "$NAME" --source "$BL/text.plist"
check "list tool: refused lists left the enabled one alone" test "$(canon "$XD/$NAME.plist")" = "$EXPECT_L"

# source trust (report-only in a dry run: exit 3 with the reason)
tool_trust() {   # tool_trust <name> <expected substring> <tool args...>
    local name="$1" want="$2"
    shift 2
    run_tool "$W/t.log" --dry-run --root "$LT" "$@"
    if [ "$RC" = 3 ] && grep 'untrusted' "$W/t.log" | grep -qF -- "$want"; then ok "list tool trust: $name"; else bad "list tool trust: $name (rc $RC)"; sed 's/^/    /' "$W/t.log"; fi
}
ln -s "$LS/list.plist" "$BL/link.plist"
tool_trust "symlinked source" "$BL/link.plist: not a regular file" --add "$NAME" --source "$BL/link.plist"
ln "$LS/list.plist" "$BL/hard.plist"
tool_trust "hard-linked source" "has 2 hard links" --add "$NAME" --source "$BL/hard.plist"
rm -f "$BL/hard.plist"
LWW="$W/src/lww"
mkdir -p "$LWW"
cp "$LS/list.plist" "$LWW/list.plist"
chmod 777 "$LWW"
tool_trust "world-writable source dir" "$LWW: world-writable" --add "$NAME" --source "$LWW/list.plist"
chmod 755 "$LWW"
LTR="$W/src/AppTranslocation/Y/d"
mkdir -p "$LTR"
cp "$LS/list.plist" "$LTR/list.plist"
tool_trust "/AppTranslocation/ source" "translocated app" --add "$NAME" --source "$LTR/list.plist"

# state
mkdir -p "$W/lt-empty"
run_tool "$W/r.log" --dry-run --root "$W/lt-empty" --add "$NAME" --source "$LS/list.plist"
check "list tool: camera access not installed -> refused (rc $RC)" grep -q "is not installed on this Mac" "$W/r.log"
check "list tool: nothing written without camera access" test ! -e "$W/lt-empty/Library/Application Support/RealSense Viewer/usbd.d"
for ((i = 1; i <= 16; i++)); do
    n="$(printf 'l%02d.example' "$i")"
    make_list "$XD/$n.plist" "$n" 1 "$(printf '%s%039x' 4 "$i")"
done
tool_refuses "a 17th list" "camera access cannot admit more programs (17 lists" --add "$NAME" --source "$LS/list.plist"
rm -f "$XD"/l*.example.plist
# shellcheck disable=SC2046
make_list "$XD/a.many.plist" a.many 1 $(hashes 64 5)
# shellcheck disable=SC2046
make_list "$XD/b.many.plist" b.many 1 $(hashes 64 6)
# shellcheck disable=SC2046
make_list "$XD/c.many.plist" c.many 1 $(hashes 64 7)
# shellcheck disable=SC2046
make_list "$XD/d.many.plist" d.many 1 $(hashes 60 8)
run_tool "$W/r.log" --dry-run --root "$LT" --add "$NAME" --source "$LS/list.plist"
check "list tool: exactly 256 code hashes accepted (rc $RC)" test "$RC" = 0 -o "$RC" = 3
make_list "$BL/three.plist" "$NAME" 202611050000 "$H1" "$H2" 3333333333333333333333333333333333333333
tool_refuses "more than 256 code hashes" "camera access cannot admit more programs (5 lists of at most 16, 257 code hashes" --add "$NAME" --source "$BL/three.plist"
rm -f "$XD"/?.many.plist
make_list "$BL/newer.plist" "$NAME" 209912312359 "$H1"
run_tool "$W/r.log" --dry-run --root "$LT" --add "$NAME" --source "$BL/newer.plist"
check "list tool: a newer build replaces the list (rc $RC)" test "$(plutil -extract BuildNumber raw "$XD/$NAME.plist")" = 209912312359
tool_refuses "downgrade" "newer than this one (202611050000)" --add "$NAME" --source "$LS/list.plist"
run_tool "$W/r.log" --dry-run --root "$LT" --add "$NAME" --source "$LS/list.plist" --allow-downgrade
check "list tool: --allow-downgrade accepted (rc $RC)" test "$(canon "$XD/$NAME.plist")" = "$EXPECT_L"
make_list "$XD/z.other.plist" z.other 1 "$H1"
run_tool "$W/r.log" --dry-run --root "$LT" --remove "$NAME"
check "list tool: --remove (rc $RC)" test "$RC" = 0 -o "$RC" = 3
check "list tool: --remove deleted the list" test ! -e "$XD/$NAME.plist"
check "list tool: --remove kept the other list" test -f "$XD/z.other.plist"
run_tool "$W/r.log" --dry-run --root "$LT" --remove "$NAME"
check "list tool: --remove of an absent list (rc $RC)" test "$RC" = 0 -o "$RC" = 3
check "list tool: --remove of an absent list says so" grep -q "$NAME is not enabled" "$W/r.log"
rm -f "$XD/z.other.plist"

# arguments and modes
run_tool "$W/r.log" --root "$LT" --remove "$NAME"
check "list tool: --root without --dry-run refused" test "$RC" = 1 -a "$(grep -c 'only allowed with --dry-run' "$W/r.log")" = 1
run_tool "$W/r.log" --remove "$NAME"
check "list tool: real run as non-root refused" grep -q "run as root" "$W/r.log"
for args in "" "--add $NAME" "--remove" "--remove $NAME --source $LS/list.plist" "--remove $NAME --allow-downgrade" \
    "--add $NAME --remove $NAME --source $LS/list.plist" "--bogus"; do
    # shellcheck disable=SC2086  # word splitting wanted
    run_tool "$W/r.log" --dry-run --root "$LT" $args
    check "list tool: usage error '$args' -> 2 (rc $RC)" test "$RC" = 2
done
mkdir -p "$LT/var/run/$LABEL.install.lock"
run_tool "$W/r.log" --dry-run --root "$LT" --remove "$NAME"
check "list tool: install lock held -> refused" grep -q "another install/uninstall is running" "$W/r.log"
rmdir "$LT/var/run/$LABEL.install.lock"

# Installer and uninstaller with lists of other apps (DESIGN 6.3 step 7, 6.5)
run_tool "$W/r.log" --dry-run --root "$LT" --add "$NAME" --source "$LS/list.plist"
check "installer: a list to keep (rc $RC)" test -f "$XD/$NAME.plist"
cp "$XD/$NAME.plist" "$W/keep.plist"
chmod 775 "$XD"
run_install "$W/k.log" --dry-run --root "$LT" --source "$SRCDIR"
check "installer: reinstall with usbd.d (rc $RC)" test "$RC" = 0 -o "$RC" = 3
check "installer: the list of another app survives byte-identical" cmp -s "$W/keep.plist" "$XD/$NAME.plist"
check "installer: usbd.d 0775 -> 0755" test "$(mode "$XD")" = 755
check "installer: reports the kept lists" grep -q "kept 1 list(s) of other apps (usbd.d)" "$W/k.log"
no_line() { ! grep -q -- "$1" "$2"; }
check "installer: no capacity warning" no_line "will skip" "$W/k.log"
plutil -replace BuildNumber -integer 209912312359 "$LT/Library/Application Support/RealSense Viewer/usbd.plist"
run_install "$W/k.log" --dry-run --root "$LT" --source "$SRCDIR" --allow-downgrade
check "installer: a downgrade keeps the list too" cmp -s "$W/keep.plist" "$XD/$NAME.plist"
# shellcheck disable=SC2046
make_list "$XD/a.many.plist" a.many 1 $(hashes 64 5)
# shellcheck disable=SC2046
make_list "$XD/b.many.plist" b.many 1 $(hashes 64 6)
# shellcheck disable=SC2046
make_list "$XD/c.many.plist" c.many 1 $(hashes 64 7)
# shellcheck disable=SC2046
make_list "$XD/d.many.plist" d.many 1 $(hashes 64 8)
run_install "$W/k.log" --dry-run --root "$LT" --source "$SRCDIR"
check "installer: warns when the lists exceed 256 code hashes (rc $RC)" grep -q "the daemon will skip some lists of other apps" "$W/k.log"
check "installer: the warning is not a failure" test "$RC" = 0 -o "$RC" = 3
rm -f "$XD"/?.many.plist
mv "$XD" "$W/xd-real"
ln -s "$W/xd-real" "$XD"
run_install "$W/k.log" --dry-run --root "$LT" --source "$SRCDIR"
check "installer: a symlinked usbd.d is removed (rc $RC)" test ! -e "$XD" -a ! -L "$XD"
check "installer: ... with a note" grep -q "usbd.d: not a directory" "$W/k.log"
check "installer: ... and its target is not touched" cmp -s "$W/keep.plist" "$W/xd-real/$NAME.plist"
LONE="$W/src/lone"
mkdir -p "$LONE"
cp "$INSTALL" "$LONE/install-camera-access.sh"
RC=0
"$LONE/install-camera-access.sh" --dry-run --root "$W/root8" --source "$SRCDIR" >"$W/k.log" 2>&1 || RC=$?
check "installer: without the list tool next to it -> exit 1 (rc $RC)" test "$RC" = 1
check "installer: ... says why" grep -q "camera-access-allowlist.sh is missing next to the installer" "$W/k.log"
run_tool "$W/r.log" --dry-run --root "$LT" --add "$NAME" --source "$LS/list.plist"
touch "$XD/.$NAME.plist.tmp.5" "$LT/Library/PrivilegedHelperTools/.$LABEL-allowlist.sh.tmp.7"
RC=0
"$UNINSTALL" --dry-run --root "$LT" >"$W/un-lt.log" 2>&1 || RC=$?
check "uninstall with lists (rc $RC)" test "$RC" = 0 -o "$RC" = 3
check "uninstall: removed usbd.d with its dot files" test ! -e "$XD"
check "uninstall: reports the removed lists" grep -q "removed 1 list(s) of other apps" "$W/un-lt.log"
check "uninstall: removed the list tool" test ! -e "$LT/Library/PrivilegedHelperTools/$LABEL-allowlist.sh"
check "uninstall: removed the temporary list tool" test ! -e "$LT/Library/PrivilegedHelperTools/.$LABEL-allowlist.sh.tmp.7"
check "uninstall: removed the config dir" test ! -e "$LT/Library/Application Support/RealSense Viewer"
run_tool "$W/r.log" --dry-run --root "$LT" --add "$NAME" --source "$LS/list.plist"
check "list tool after uninstall: camera access not installed" grep -q "is not installed on this Mac" "$W/r.log"

# Uninstall
mkdir -p "$P1/var/run/$LABEL" "$P1/private/var/tmp/rs-usbd.leftover"
touch "$P1/Library/PrivilegedHelperTools/.$LABEL.tmp.123" "$P1/Library/PrivilegedHelperTools/other-helper"
RC=0
"$UNINSTALL" --dry-run --root "$P1" >"$W/un.log" 2>&1 || RC=$?
check "uninstall exit 0 or 3 (rc $RC)" test "$RC" = 0 -o "$RC" = 3
check "uninstall removed binary" test ! -e "$B"
check "uninstall removed the list tool" test ! -e "$T1"
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
