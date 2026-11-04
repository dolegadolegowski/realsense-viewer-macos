#!/bin/bash -p
unset IFS CDPATH ENV BASH_ENV TMPDIR
export PATH=/usr/bin:/bin:/usr/sbin:/sbin
umask 022
set -euo pipefail

# Removes camera access (the realsense-usbd LaunchDaemon) and hands every camera it captured back to macOS.
# Afterwards the apps ask for an administrator password at every launch again. Spec: macos/broker/DESIGN.md 6.5.
#
#   sudo uninstall-camera-access.sh
#   uninstall-camera-access.sh --dry-run --root <dir>
#       Not as root: removes the installed layout under <dir> only, runs no launchctl and no hand-back, and only
#       reports trust failures (exit status 3 at the end if there were any).

LABEL=io.github.dolegadolegowski.realsense-usbd
ME=uninstall-camera-access

die() { echo "$ME: error: $*" >&2; exit 1; }
note() { echo "$ME: $*"; }
usage() {
    echo "usage: sudo $0" >&2
    echo "       $0 --dry-run --root <dir>" >&2
    exit 2
}

DRY=0
PREFIX=
while [ $# -gt 0 ]; do
    case "$1" in
        --dry-run) DRY=1; shift ;;
        --root) [ $# -ge 2 ] || usage; PREFIX="$2"; shift 2 ;;
        *) usage ;;
    esac
done
if [ "$DRY" = 1 ]; then
    [ "$(id -u)" != 0 ] || die "--dry-run must not run as root"
    [ -n "$PREFIX" ] && [ -d "$PREFIX" ] || die "--dry-run needs --root <existing dir>"
    PREFIX="$(/bin/realpath -- "$PREFIX")"
    [ "$PREFIX" != / ] || die "--root / is not a dry run"
    mkdir -p -- "$PREFIX/var/run" "$PREFIX/private/var/tmp"
else
    [ -z "$PREFIX" ] || die "--root is only allowed with --dry-run"
    [ "$(id -u)" = 0 ] || die "run as root: sudo \"$0\""
fi

HELPER_DIR="$PREFIX/Library/PrivilegedHelperTools"
BIN="$HELPER_DIR/$LABEL"
CONF_DIR="$PREFIX/Library/Application Support/RealSense Viewer"
DAEMON_PLIST="$PREFIX/Library/LaunchDaemons/$LABEL.plist"
STATE_DIR="$PREFIX/var/run/$LABEL"
LOCK="$PREFIX/var/run/$LABEL.install.lock"
TMPBASE="$PREFIX/private/var/tmp"

LOCKED=0
cleanup() {
    if [ "$LOCKED" = 1 ]; then rmdir -- "$LOCK" 2>/dev/null || true; fi
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM
mkdir -- "$LOCK" 2>/dev/null || die "another install/uninstall is running (or a stale lock: $LOCK)"
LOCKED=1

# Trust rule (DESIGN 6.2) on this script
TRUST_FAILURES=0
untrusted() {
    if [ "$DRY" = 1 ]; then
        echo "$ME: untrusted (report-only): $*" >&2
        TRUST_FAILURES=$((TRUST_FAILURES + 1))
    else
        die "untrusted: $*"
    fi
}
admin_uid() {
    [ "$1" = 0 ] || [ "$(dsmemberutil checkmembership -u "$1" -G admin 2>/dev/null)" = "user is a member of the group" ]
}
self="$0"
if [ -L "$self" ] || [ ! -f "$self" ]; then
    untrusted "$self: not a regular file"
fi
rest="$(dirname -- "$self")"
case "$rest" in /*) ;; *) rest="$(pwd -P)/$rest" ;; esac
acc=
while [ -n "$rest" ]; do
    part="${rest%%/*}"
    if [ "$part" = "$rest" ]; then rest=; else rest="${rest#*/}"; fi
    [ -n "$part" ] || continue
    acc="$acc/$part"
    if [ -L "$acc" ]; then
        untrusted "$self: goes through the symlink $acc (use the real path)"
        break
    fi
done
real="$(/bin/realpath -- "$self")" || die "cannot resolve $self"
case "$real" in */AppTranslocation/*) untrusted "$real: translocated app (move it to /Applications first)" ;; esac
[ "$(stat -f %l -- "$real")" = 1 ] || untrusted "$real: has hard links"
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
cur="$real"
while :; do
    read -r uid gid perm <<<"$(stat -f '%u %g %OLp' -- "$cur")"
    [[ "$uid" =~ ^[0-9]+$ && "$gid" =~ ^[0-9]+$ && "$perm" =~ ^[0-7]+$ ]] || die "$cur: cannot stat"
    admin_uid "$uid" || untrusted "$cur: owner uid $uid is neither root nor an administrator"
    [ $((8#$perm & 8#002)) = 0 ] || untrusted "$cur: world-writable"
    if [ $((8#$perm & 8#020)) != 0 ] && [ "$gid" != 0 ] && [ "$gid" != 80 ]; then
        untrusted "$cur: group-writable by group $gid"
    fi
    ! acl_allows_write "$cur" || untrusted "$cur: an ACL entry allows others to change it"
    [ "$cur" != / ] || break
    cur="$(dirname -- "$cur")"
done

# Stop the daemon (it hands the cameras back on SIGTERM), then hand back what a crashed daemon left captured
if [ "$DRY" = 0 ]; then
    if launchctl print "system/$LABEL" >/dev/null 2>&1; then
        note "stopping the daemon"
        launchctl bootout "system/$LABEL" 2>/dev/null || true
        for ((i = 0; i < 70; i++)); do
            launchctl print "system/$LABEL" >/dev/null 2>&1 || break
            sleep 0.5
        done
        ! launchctl print "system/$LABEL" >/dev/null 2>&1 || die "the daemon did not stop within 35 s"
    fi
    if [ -f "$BIN" ] && [ ! -L "$BIN" ] && [ "$(stat -f %u "$BIN")" = 0 ]; then
        "$BIN" --handback-recorded || note "hand-back of recorded cameras failed; re-plug the camera to give it back to macOS"
    fi
fi

rm -f -- "$DAEMON_PLIST" "$BIN"
if [ -d "$HELPER_DIR" ] && [ ! -L "$HELPER_DIR" ]; then
    find "$HELPER_DIR" -maxdepth 1 -name ".$LABEL.tmp.*" -exec rm -f -- {} +
fi
if [ -d "$CONF_DIR" ] && [ ! -L "$CONF_DIR" ]; then
    rm -f -- "$CONF_DIR/usbd.plist"
    rmdir -- "$CONF_DIR" 2>/dev/null || true
fi
rm -rf -- "$STATE_DIR"
# Staging directories of interrupted installs: root's own (mktemp -d, 0700) only, never a user's lookalike
if [ "$DRY" = 1 ]; then OWNER_UID="$(id -u)"; else OWNER_UID=0; fi
find "$TMPBASE" -maxdepth 1 -name 'rs-usbd.*' -type d -user "$OWNER_UID" -exec rm -rf -- {} +

note "camera access removed"
if [ "$DRY" = 1 ] && [ "$TRUST_FAILURES" != 0 ]; then
    echo "$ME: dry run: $TRUST_FAILURES trust check(s) failed" >&2
    exit 3
fi
