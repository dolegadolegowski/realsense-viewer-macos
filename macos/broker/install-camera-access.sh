#!/bin/bash -p
unset IFS CDPATH ENV BASH_ENV TMPDIR
export PATH=/usr/bin:/bin:/usr/sbin:/sbin
umask 022
set -euo pipefail

# Installs camera access for all users of this Mac: the RealSense USB broker realsense-usbd as a root
# LaunchDaemon. Afterwards RealSense Viewer, the Depth Quality Tool and the allowlisted command line tools of the
# same DMG use the camera without an administrator password, also on standard accounts.
# Spec: macos/broker/DESIGN.md section 6.
#
#   sudo install-camera-access.sh --source <dir> [--source <dir>] [--allow-downgrade]
#       <dir> holds realsense-usbd and allowlist.plist: "RealSense Tools/camera-access", or an app's
#       Contents/MacOS together with Contents/Resources/realsense-usbd.
#   install-camera-access.sh --dry-run --root <dir> --source <dir> [--source <dir>] [--allow-downgrade]
#       Not as root: writes the installed layout under <dir>, runs no launchctl and no probe, and only reports
#       trust failures (exit status 3 at the end if there were any).

LABEL=io.github.dolegadolegowski.realsense-usbd
ME=install-camera-access

die() { echo "$ME: error: $*" >&2; exit 1; }
note() { echo "$ME: $*"; }
usage() {
    echo "usage: sudo $0 --source <dir> [--source <dir>] [--allow-downgrade]" >&2
    echo "       $0 --dry-run --root <dir> --source <dir> [--source <dir>] [--allow-downgrade]" >&2
    exit 2
}

SOURCES=()
ALLOW_DOWNGRADE=0
DRY=0
PREFIX=
while [ $# -gt 0 ]; do
    case "$1" in
        --source) [ $# -ge 2 ] || usage; SOURCES+=("$2"); shift 2 ;;
        --allow-downgrade) ALLOW_DOWNGRADE=1; shift ;;
        --dry-run) DRY=1; shift ;;
        --root) [ $# -ge 2 ] || usage; PREFIX="$2"; shift 2 ;;
        *) usage ;;
    esac
done
[ ${#SOURCES[@]} -ge 1 ] && [ ${#SOURCES[@]} -le 2 ] || usage

if [ "$DRY" = 1 ]; then
    [ "$(id -u)" != 0 ] || die "--dry-run must not run as root"
    [ -n "$PREFIX" ] || die "--dry-run needs --root <dir>"
    mkdir -p -- "$PREFIX"
    PREFIX="$(/bin/realpath -- "$PREFIX")"
    [ "$PREFIX" != / ] || die "--root / is not a dry run"
else
    [ -z "$PREFIX" ] || die "--root is only allowed with --dry-run"
    [ "$(id -u)" = 0 ] || die "run as root (sudo), or install from RealSense Viewer"
fi

HELPER_DIR="$PREFIX/Library/PrivilegedHelperTools"
BIN="$HELPER_DIR/$LABEL"
CONF_DIR="$PREFIX/Library/Application Support/RealSense Viewer"
CONF="$CONF_DIR/usbd.plist"
DAEMON_PLIST="$PREFIX/Library/LaunchDaemons/$LABEL.plist"
LOCK="$PREFIX/var/run/$LABEL.install.lock"
TMPBASE="$PREFIX/private/var/tmp"
if [ "$DRY" = 1 ]; then
    mkdir -p -- "$PREFIX/var/run" "$TMPBASE" "$PREFIX/Library/LaunchDaemons"
fi

LOCKED=0
STAGE=
TMPBIN=
# shellcheck disable=SC2329  # run by the EXIT trap
cleanup() {
    if [ -n "$TMPBIN" ]; then rm -f -- "$TMPBIN"; fi
    if [ -n "$STAGE" ]; then rm -rf -- "$STAGE"; fi
    if [ "$LOCKED" = 1 ]; then rmdir -- "$LOCK" 2>/dev/null || true; fi
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM
mkdir -- "$LOCK" 2>/dev/null || die "another install/uninstall is running (or a stale lock: $LOCK)"
LOCKED=1

# Trust rule (DESIGN 6.2): only install what an administrator controls.
TRUST_FAILURES=0
ADMIN_UIDS=" 0 "
NON_ADMIN_UIDS=" "
untrusted() {
    if [ "$DRY" = 1 ]; then
        echo "$ME: untrusted (report-only): $*" >&2
        TRUST_FAILURES=$((TRUST_FAILURES + 1))
    else
        die "untrusted: $*"
    fi
}
admin_uid() {
    case "$ADMIN_UIDS" in *" $1 "*) return 0 ;; esac
    case "$NON_ADMIN_UIDS" in *" $1 "*) return 1 ;; esac
    if [ "$(dsmemberutil checkmembership -u "$1" -G admin 2>/dev/null)" = "user is a member of the group" ]; then
        ADMIN_UIDS="$ADMIN_UIDS$1 "
        return 0
    fi
    NON_ADMIN_UIDS="$NON_ADMIN_UIDS$1 "
    return 1
}
# A symlink in the given path would be re-resolved by every later use; refuse it instead of checking only its target
no_symlink_in() {
    local rest="$1" acc='' part
    case "$rest" in /*) ;; *) rest="$(pwd -P)/$rest" ;; esac
    while [ -n "$rest" ]; do
        part="${rest%%/*}"
        if [ "$part" = "$rest" ]; then rest=; else rest="${rest#*/}"; fi
        [ -n "$part" ] || continue
        acc="$acc/$part"
        if [ -L "$acc" ]; then
            untrusted "$1: goes through the symlink $acc (use the real path)"
            return 1
        fi
    done
}
# Any ACL entry that allows changing the item (ls -led prints entries as "<n>: <who> allow|deny <perms>")
acl_allows_write() {
    local out line first=1 re='^ *[0-9]+: .* allow .*(write|append|add_file|add_subdirectory|delete|chown)'
    out="$(ls -led -- "$1" 2>/dev/null)" || return 0
    while IFS= read -r line; do
        [ "$first" = 0 ] && [[ "$line" =~ $re ]] && return 0
        first=0
    done <<<"$out"
    return 1
}
# trust_path <path> file|dir
trust_path() {
    local path="$1" kind="$2" real cur st uid gid perm links
    if [ "$kind" = file ]; then no_symlink_in "$(dirname -- "$path")" || return 0; else no_symlink_in "$path" || return 0; fi
    if [ "$kind" = file ] && { [ -L "$path" ] || [ ! -f "$path" ]; }; then
        untrusted "$path: not a regular file"
        return 0
    fi
    if [ "$kind" = dir ] && [ ! -d "$path" ]; then
        untrusted "$path: not a directory"
        return 0
    fi
    real="$(/bin/realpath -- "$path" 2>/dev/null)" || { untrusted "$path: cannot resolve"; return 0; }
    case "$real" in
        */AppTranslocation/*) untrusted "$real: translocated app (move it to /Applications first)"; return 0 ;;
    esac
    if [ "$kind" = file ]; then
        links="$(stat -f %l -- "$real")"
        [ "$links" = 1 ] || untrusted "$real: has $links hard links"
    fi
    cur="$real"
    while :; do
        st="$(stat -f '%u %g %OLp' -- "$cur")" || { untrusted "$cur: cannot stat"; return 0; }
        read -r uid gid perm <<<"$st"
        [[ "$uid" =~ ^[0-9]+$ && "$gid" =~ ^[0-9]+$ && "$perm" =~ ^[0-7]+$ ]] || { untrusted "$cur: bad stat"; return 0; }
        admin_uid "$uid" || untrusted "$cur: owner uid $uid is neither root nor an administrator"
        [ $((8#$perm & 8#002)) = 0 ] || untrusted "$cur: world-writable"
        if [ $((8#$perm & 8#020)) != 0 ] && [ "$gid" != 0 ] && [ "$gid" != 80 ]; then
            untrusted "$cur: group-writable by group $gid"
        fi
        ! acl_allows_write "$cur" || untrusted "$cur: an ACL entry allows others to change it"
        [ "$cur" != / ] || break
        cur="$(dirname -- "$cur")"
    done
}

# 1. The script itself and every source
trust_path "$0" file
SRC_BIN=
SRC_ALLOW=
for dir in "${SOURCES[@]}"; do
    trust_path "$dir" dir
    dir="$(/bin/realpath -- "$dir" 2>/dev/null)" || die "cannot resolve a --source directory"
    for name in realsense-usbd allowlist.plist; do
        [ -e "$dir/$name" ] || [ -L "$dir/$name" ] || continue
        trust_path "$dir/$name" file
        if [ "$name" = realsense-usbd ]; then
            [ -z "$SRC_BIN" ] || die "realsense-usbd found in more than one --source"
            SRC_BIN="$dir/$name"
        else
            [ -z "$SRC_ALLOW" ] || die "allowlist.plist found in more than one --source"
            SRC_ALLOW="$dir/$name"
        fi
    done
done
[ -n "$SRC_BIN" ] || die "no realsense-usbd in the --source directories"
[ -n "$SRC_ALLOW" ] || die "no allowlist.plist in the --source directories"

# 2. Stage; everything below reads only the staged copies
STAGE="$(mktemp -d "$TMPBASE/rs-usbd.XXXXXX")"
ditto --noextattr --noqtn "$SRC_BIN" "$STAGE/realsense-usbd"
ditto --noextattr --noqtn "$SRC_ALLOW" "$STAGE/allowlist.plist"
S_BIN="$STAGE/realsense-usbd"
S_ALLOW="$STAGE/allowlist.plist"
[ -f "$S_BIN" ] && [ ! -L "$S_BIN" ] && [ -f "$S_ALLOW" ] && [ ! -L "$S_ALLOW" ] || die "staged copies are not regular files"
[ "$(stat -f %z "$S_BIN")" -le 16777216 ] || die "realsense-usbd is larger than 16 MiB"
[ "$(stat -f %z "$S_ALLOW")" -le 65536 ] || die "allowlist.plist is larger than 64 KiB"
case "$(file -b "$S_BIN")" in
    "Mach-O 64-bit executable arm64"*) ;;
    *) die "realsense-usbd is not an arm64 Mach-O executable" ;;
esac

# 3. Allowlist (DESIGN 5.1) and the daemon's signature
RE_HEX40='^[0-9a-f]{40}$'
RE_HEX64='^[0-9a-f]{64}$'
RE_UINT='^[0-9]{1,18}$'
pl_get() { plutil -extract "$1" raw -expect "$2" "$3" 2>/dev/null; }
plutil -lint -s "$S_ALLOW" >/dev/null || die "allowlist.plist is not a valid property list"
[ "$(plutil -type Version "$S_ALLOW" 2>/dev/null)" = integer ] && [ "$(pl_get Version integer "$S_ALLOW")" = 1 ] \
    || die "allowlist.plist: Version must be the integer 1"
BUILD_ID="$(pl_get BuildID string "$S_ALLOW")" || die "allowlist.plist: BuildID missing"
[[ "$BUILD_ID" =~ $RE_HEX64 ]] || die "allowlist.plist: bad BuildID"
BUILD_NUMBER="$(pl_get BuildNumber integer "$S_ALLOW")" || die "allowlist.plist: BuildNumber missing"
[[ "$BUILD_NUMBER" =~ $RE_UINT ]] || die "allowlist.plist: bad BuildNumber"
PROTOCOL="$(pl_get ProtocolVersion integer "$S_ALLOW")" || die "allowlist.plist: ProtocolVersion missing"
[[ "$PROTOCOL" =~ ^[0-9]{1,9}$ ]] || die "allowlist.plist: bad ProtocolVersion"
COUNT="$(pl_get AllowedCDHashes array "$S_ALLOW")" || die "allowlist.plist: AllowedCDHashes missing"
[[ "$COUNT" =~ ^[0-9]{1,3}$ ]] && [ "$COUNT" -ge 1 ] && [ "$COUNT" -le 256 ] \
    || die "allowlist.plist: AllowedCDHashes must hold 1..256 entries"
HASHES=()
for ((i = 0; i < COUNT; i++)); do
    h="$(pl_get "AllowedCDHashes.$i" string "$S_ALLOW")" || die "allowlist.plist: AllowedCDHashes.$i is not a string"
    [[ "$h" =~ $RE_HEX40 ]] || die "allowlist.plist: bad cdhash at AllowedCDHashes.$i"
    HASHES+=("$h")
done
DAEMON_CDHASH="$(pl_get DaemonCDHash string "$S_ALLOW")" || die "allowlist.plist: DaemonCDHash missing"
[[ "$DAEMON_CDHASH" =~ $RE_HEX40 ]] || die "allowlist.plist: bad DaemonCDHash"
case " ${HASHES[*]} " in *" $DAEMON_CDHASH "*) ;; *) die "allowlist.plist: DaemonCDHash is not in AllowedCDHashes" ;; esac
SORTED="$(printf '%s\n' "${HASHES[@]}" | LC_ALL=C sort -u)"
[ "$(printf '%s' "$SORTED" | shasum -a 256 | cut -d' ' -f1)" = "$BUILD_ID" ] \
    || die "allowlist.plist: BuildID does not match AllowedCDHashes"
codesign --verify --strict "$S_BIN" || die "realsense-usbd: invalid code signature"
CDHASH="$(codesign -dvvv --arch arm64 "$S_BIN" 2>&1 | sed -n 's/^CDHash=//p')"
[ "$CDHASH" = "$DAEMON_CDHASH" ] || die "realsense-usbd: cdhash $CDHASH is not the allowlist's DaemonCDHash"

# Existing configuration: admin keys of DESIGN 5.2 are kept when valid; BuildNumber guards against downgrades
OLD_CONF=
if [ -f "$CONF" ] && [ ! -L "$CONF" ]; then
    read -r c_uid c_links c_size <<<"$(stat -f '%u %l %z' -- "$CONF")"
    if { [ "$c_uid" = 0 ] || [ "$DRY" = 1 ]; } && [ "$c_links" = 1 ] && [ "$c_size" -le 65536 ] \
        && plutil -lint -s "$CONF" >/dev/null 2>&1; then
        OLD_CONF="$STAGE/old-usbd.plist"
        ditto --noextattr --noqtn "$CONF" "$OLD_CONF"
    else
        note "ignoring the existing $CONF (not a valid root-owned property list)"
    fi
fi
if [ -n "$OLD_CONF" ]; then
    old_build="$(pl_get BuildNumber integer "$OLD_CONF" || true)"
    if [[ "$old_build" =~ $RE_UINT ]] && [ "$old_build" -gt "$BUILD_NUMBER" ]; then
        [ "$ALLOW_DOWNGRADE" = 1 ] || die "installed camera access (build $old_build) is newer than this one ($BUILD_NUMBER); install the apps from the newer DMG, or pass --allow-downgrade"
        note "downgrading from build $old_build to $BUILD_NUMBER"
    fi
fi

# 5. Stop the running daemon (it hands the cameras back on SIGTERM)
if [ "$DRY" = 0 ] && launchctl print "system/$LABEL" >/dev/null 2>&1; then
    note "stopping the running daemon"
    launchctl bootout "system/$LABEL" 2>/dev/null || true
    for ((i = 0; i < 70; i++)); do
        launchctl print "system/$LABEL" >/dev/null 2>&1 || break
        sleep 0.5
    done
    ! launchctl print "system/$LABEL" >/dev/null 2>&1 || die "the running daemon did not stop within 35 s"
fi

OWNER=(-o root -g wheel)
if [ "$DRY" = 1 ]; then OWNER=(); fi

# 6. Binary
install -d ${OWNER[@]+"${OWNER[@]}"} -m 755 "$HELPER_DIR"
TMPBIN="$HELPER_DIR/.$LABEL.tmp.$$"
install ${OWNER[@]+"${OWNER[@]}"} -m 755 "$S_BIN" "$TMPBIN"
xattr -c "$TMPBIN"
mv -f -- "$TMPBIN" "$BIN"
TMPBIN=

# 7. Configuration from the validated values only
if [ -L "$CONF_DIR" ] || { [ -e "$CONF_DIR" ] && [ ! -d "$CONF_DIR" ]; }; then
    rm -f -- "$CONF_DIR"
fi
mkdir -p -- "$CONF_DIR"
if [ "$DRY" = 0 ]; then chown root:wheel "$CONF_DIR"; fi
chmod 755 "$CONF_DIR"
NEW_CONF="$STAGE/usbd.plist"
plutil -create xml1 "$NEW_CONF"
plutil -insert Version -integer 1 "$NEW_CONF"
plutil -insert BuildID -string "$BUILD_ID" "$NEW_CONF"
plutil -insert BuildNumber -integer "$BUILD_NUMBER" "$NEW_CONF"
plutil -insert ProtocolVersion -integer "$PROTOCOL" "$NEW_CONF"
plutil -insert AllowedCDHashes -array "$NEW_CONF"
for h in "${HASHES[@]}"; do plutil -insert AllowedCDHashes -string "$h" -append "$NEW_CONF"; done
plutil -insert DaemonCDHash -string "$DAEMON_CDHASH" "$NEW_CONF"

# copy_int_key <key> <min> <max>
copy_int_key() {
    local v
    [ "$(plutil -type "$1" "$OLD_CONF" 2>/dev/null)" = integer ] || return 0
    v="$(pl_get "$1" integer "$OLD_CONF")"
    if [[ "$v" =~ $RE_UINT ]] && [ "$v" -ge "$2" ] && [ "$v" -le "$3" ]; then
        plutil -insert "$1" -integer "$v" "$NEW_CONF"
        note "kept $1 = $v"
    else
        note "dropped invalid $1 from the existing configuration"
    fi
}
copy_opcodes() {
    local n v i vals=()
    [ "$(plutil -type DeniedHWMOpcodes "$OLD_CONF" 2>/dev/null)" = array ] || return 0
    n="$(pl_get DeniedHWMOpcodes array "$OLD_CONF")"
    [[ "$n" =~ ^[0-9]{1,3}$ ]] && [ "$n" -le 256 ] || { note "dropped invalid DeniedHWMOpcodes"; return 0; }
    for ((i = 0; i < n; i++)); do
        v="$(pl_get "DeniedHWMOpcodes.$i" integer "$OLD_CONF" || true)"
        [[ "$v" =~ ^[0-9]{1,3}$ ]] && [ "$v" -le 255 ] || { note "dropped invalid DeniedHWMOpcodes"; return 0; }
        vals+=("$v")
    done
    plutil -insert DeniedHWMOpcodes -array "$NEW_CONF"
    for v in ${vals[@]+"${vals[@]}"}; do plutil -insert DeniedHWMOpcodes -integer "$v" -append "$NEW_CONF"; done
    note "kept DeniedHWMOpcodes (${vals[*]:-})"
}
copy_ranges() {
    local n s l i starts=() lengths=()
    [ "$(plutil -type ProtectedFlashRanges "$OLD_CONF" 2>/dev/null)" = array ] || return 0
    n="$(pl_get ProtectedFlashRanges array "$OLD_CONF")"
    [[ "$n" =~ ^[0-9]{1,3}$ ]] && [ "$n" -le 256 ] || { note "dropped invalid ProtectedFlashRanges"; return 0; }
    for ((i = 0; i < n; i++)); do
        s="$(pl_get "ProtectedFlashRanges.$i.Start" integer "$OLD_CONF" || true)"
        l="$(pl_get "ProtectedFlashRanges.$i.Length" integer "$OLD_CONF" || true)"
        if ! [[ "$s" =~ ^[0-9]{1,10}$ && "$l" =~ ^[0-9]{1,10}$ ]] || [ "$l" -le 0 ] \
            || [ $((s + l)) -gt 4294967296 ]; then
            note "dropped invalid ProtectedFlashRanges"
            return 0
        fi
        starts+=("$s")
        lengths+=("$l")
    done
    plutil -insert ProtectedFlashRanges -array "$NEW_CONF"
    for ((i = 0; i < n; i++)); do
        plutil -insert ProtectedFlashRanges -xml \
            "<dict><key>Start</key><integer>${starts[$i]}</integer><key>Length</key><integer>${lengths[$i]}</integer></dict>" \
            -append "$NEW_CONF"
    done
    note "kept ProtectedFlashRanges ($n)"
}
if [ -n "$OLD_CONF" ]; then
    copy_int_key HandBackGraceSeconds 0 600
    copy_int_key IdleExitSeconds 10 3600
    copy_opcodes
    copy_ranges
fi
plutil -lint -s "$NEW_CONF" >/dev/null
install -S ${OWNER[@]+"${OWNER[@]}"} -m 644 "$NEW_CONF" "$CONF"

# 8. LaunchDaemon (constant content, DESIGN 6.4)
cat >"$STAGE/daemon.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>                 <string>io.github.dolegadolegowski.realsense-usbd</string>
    <key>ProgramArguments</key>      <array><string>/Library/PrivilegedHelperTools/io.github.dolegadolegowski.realsense-usbd</string><string>--daemon</string></array>
    <key>MachServices</key>          <dict><key>io.github.dolegadolegowski.realsense-usbd</key><true/></dict>
    <key>KeepAlive</key>             <dict><key>SuccessfulExit</key><false/></dict>
    <key>ProcessType</key>           <string>Interactive</string>
    <key>ExitTimeOut</key>           <integer>30</integer>
    <key>AssociatedBundleIdentifiers</key>
    <array><string>io.github.dolegadolegowski.realsense-viewer</string><string>io.github.dolegadolegowski.realsense-depth-quality</string></array>
</dict>
</plist>
PLIST
plutil -lint -s "$STAGE/daemon.plist" >/dev/null
install -S ${OWNER[@]+"${OWNER[@]}"} -m 644 "$STAGE/daemon.plist" "$DAEMON_PLIST"
xattr -c "$DAEMON_PLIST"

if [ "$DRY" = 1 ]; then
    note "dry run: installed layout written under $PREFIX (launchctl and the probe skipped)"
    if [ "$TRUST_FAILURES" != 0 ]; then
        echo "$ME: dry run: $TRUST_FAILURES trust check(s) failed" >&2
        exit 3
    fi
    exit 0
fi

# 9. Load
launchctl enable "system/$LABEL"
if ! out="$(launchctl bootstrap system "$DAEMON_PLIST" 2>&1)"; then
    echo "$out" >&2
    case "$out" in
        *" 5:"*) echo "$ME: if \"realsense-usbd\" is switched off in System Settings > General > Login Items & Extensions, switch it on there and run this again" >&2 ;;
    esac
    die "launchctl bootstrap failed"
fi

# 10. Verify: loaded, and a root client passes HELLO
launchctl print "system/$LABEL" >/dev/null || die "the daemon is not loaded"
probe=
for ((i = 0; i < 20; i++)); do
    if probe="$("$BIN" --probe 2>&1)"; then
        note "installed camera access build $BUILD_NUMBER ($probe)"
        note "  $BIN"
        note "  $CONF"
        note "  $DAEMON_PLIST"
        exit 0
    fi
    sleep 0.5
done
die "the daemon does not answer: $probe"
