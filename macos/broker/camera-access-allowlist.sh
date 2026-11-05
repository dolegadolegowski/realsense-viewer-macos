#!/bin/bash -p
unset IFS CDPATH ENV BASH_ENV TMPDIR
export PATH=/usr/bin:/bin:/usr/sbin:/sbin
umask 022
set -euo pipefail

# Adds or removes the camera access allowlist of another app, e.g. IntelFaceScan, so that its programs use RealSense
# cameras through realsense-usbd without an administrator password. The lists live in
# "/Library/Application Support/RealSense Viewer/usbd.d/<name>.plist"; the viewer's own list (usbd.plist) is never
# touched. Adding or removing a list restarts the daemon: running RealSense programs lose the camera for a moment
# and reconnect. Spec: macos/broker/DESIGN.md 5.4, 5.5 and 6.7.
#
#   sudo camera-access-allowlist.sh --add <name> --source <list.plist> [--allow-downgrade]
#       <name> is the list's reverse-DNS name in lower case (e.g. io.github.dolegadolegowski.intelfacescan);
#       <list.plist> is the list the app ships, in a folder only root or an administrator can change.
#   sudo camera-access-allowlist.sh --remove <name>
#   camera-access-allowlist.sh --dry-run --root <dir> (--add <name> --source <list.plist> [--allow-downgrade] | --remove <name>)
#       Not as root: works on the installed layout under <dir>, runs no launchctl and no probe, and only reports
#       trust failures (exit status 3 at the end if there were any).
# Exit status: 0 done (also: removing a list that is not there, or camera access not loaded), 1 refused or failed,
# 2 usage, 3 dry run with trust failures.

LABEL=io.github.dolegadolegowski.realsense-usbd
ME=camera-access-allowlist

die() { echo "$ME: error: $*" >&2; exit 1; }
note() { echo "$ME: $*"; }
usage() {
    echo "usage: sudo $0 --add <name> --source <list.plist> [--allow-downgrade]" >&2
    echo "       sudo $0 --remove <name>" >&2
    echo "       $0 --dry-run --root <dir> (--add <name> --source <list.plist> [--allow-downgrade] | --remove <name>)" >&2
    exit 2
}

MODE=
NAME=
SOURCE=
ALLOW_DOWNGRADE=0
DRY=0
PREFIX=
while [ $# -gt 0 ]; do
    case "$1" in
        --add | --remove) [ $# -ge 2 ] && [ -z "$MODE" ] || usage; MODE="${1#--}"; NAME="$2"; shift 2 ;;
        --source) [ $# -ge 2 ] && [ -z "$SOURCE" ] || usage; SOURCE="$2"; shift 2 ;;
        --allow-downgrade) ALLOW_DOWNGRADE=1; shift ;;
        --dry-run) DRY=1; shift ;;
        --root) [ $# -ge 2 ] || usage; PREFIX="$2"; shift 2 ;;
        *) usage ;;
    esac
done
[ -n "$MODE" ] || usage
if [ "$MODE" = add ]; then
    [ -n "$SOURCE" ] || usage
else
    [ -z "$SOURCE" ] && [ "$ALLOW_DOWNGRADE" = 0 ] || usage
fi

# List names (DESIGN 5.4, the same rule as the daemon): reverse DNS in lower case with at least one dot, 3..100
# characters; no "/", no "..", no leading dot. Explicit characters, so the locale cannot widen a range.
LN='abcdefghijklmnopqrstuvwxyz0123456789'
RE_NAME="^[$LN]([$LN-]*[$LN])?([.][$LN]([$LN-]*[$LN])?)+\$"
valid_name() {
    [ "${#1}" -ge 3 ] && [ "${#1}" -le 100 ] && [[ "$1" != *[!$LN.-]* ]] && [[ "$1" =~ $RE_NAME ]]
}
valid_name "$NAME" || die "bad list name (expected reverse DNS in lower case, e.g. io.github.example.app)"

if [ "$DRY" = 1 ]; then
    [ "$(id -u)" != 0 ] || die "--dry-run must not run as root"
    [ -n "$PREFIX" ] || die "--dry-run needs --root <dir>"
    mkdir -p -- "$PREFIX"
    PREFIX="$(/bin/realpath -- "$PREFIX")"
    [ "$PREFIX" != / ] || die "--root / is not a dry run"
else
    [ -z "$PREFIX" ] || die "--root is only allowed with --dry-run"
    [ "$(id -u)" = 0 ] || die "run as root (sudo)"
fi

HELPER_DIR="$PREFIX/Library/PrivilegedHelperTools"
BIN="$HELPER_DIR/$LABEL"
CONF_DIR="$PREFIX/Library/Application Support/RealSense Viewer"
CONF="$CONF_DIR/usbd.plist"
EXTRA_DIR="$CONF_DIR/usbd.d"
LIST="$EXTRA_DIR/$NAME.plist"
LOCK="$PREFIX/var/run/$LABEL.install.lock"
TMPBASE="$PREFIX/private/var/tmp"
if [ "$DRY" = 1 ]; then
    mkdir -p -- "$PREFIX/var/run" "$TMPBASE"
fi

LOCKED=0
STAGE=
TMPLIST=
# shellcheck disable=SC2329  # run by the EXIT trap
cleanup() {
    if [ -n "$TMPLIST" ]; then rm -f -- "$TMPLIST"; fi
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

trust_path "$0" file

OWNER=(-o root -g wheel)
if [ "$DRY" = 1 ]; then OWNER=(); fi
RE_HEX40='^[0-9a-f]{40}$'
RE_HEX64='^[0-9a-f]{64}$'
RE_UINT='^[0-9]{1,18}$'
pl_get() { plutil -extract "$1" raw -expect "$2" "$3" 2>/dev/null; }
# a regular file that is not a symlink, has one link and (except in a dry run) belongs to root
root_file() {
    local uid links
    [ -f "$1" ] && [ ! -L "$1" ] || return 1
    read -r uid links <<<"$(stat -f '%u %l' -- "$1")"
    [ "$links" = 1 ] && { [ "$uid" = 0 ] || [ "$DRY" = 1 ]; }
}
# hashes_of <plist> <max>: its AllowedCDHashes, one per line, if they are 1..<max> lowercase 40-hex strings
hashes_of() {
    local n i h
    n="$(pl_get AllowedCDHashes array "$1")" || return 1
    [[ "$n" =~ ^[0-9]{1,3}$ ]] && [ "$n" -ge 1 ] && [ "$n" -le "$2" ] || return 1
    for ((i = 0; i < n; i++)); do
        h="$(pl_get "AllowedCDHashes.$i" string "$1")" || return 1
        [[ "$h" =~ $RE_HEX40 ]] || return 1
        echo "$h"
    done
}

# restart the daemon so it reads usbd.d again (DESIGN 5.5: read once per process)
reload() {
    local probe='' i
    if [ "$DRY" = 1 ]; then
        note "dry run: launchctl and the probe skipped"
        return 0
    fi
    if ! launchctl print "system/$LABEL" >/dev/null 2>&1; then
        note "camera access is not loaded (switched off in Login Items?); the list applies when it runs"
        return 0
    fi
    note "restarting camera access; running RealSense programs reconnect"
    launchctl kickstart -k "system/$LABEL" >/dev/null 2>&1 || die "launchctl kickstart failed"
    root_file "$BIN" || die "the installed $BIN is missing or not owned by root"
    for ((i = 0; i < 90; i++)); do
        if probe="$("$BIN" --probe 2>&1)"; then
            return 0
        fi
        sleep 0.5
    done
    die "camera access did not come back: $probe"
}

finish() {
    if [ "$DRY" = 1 ] && [ "$TRUST_FAILURES" != 0 ]; then
        echo "$ME: dry run: $TRUST_FAILURES trust check(s) failed" >&2
        exit 3
    fi
    exit 0
}

if [ "$MODE" = remove ]; then
    if [ -d "$CONF_DIR" ] && [ ! -L "$CONF_DIR" ] && [ -d "$EXTRA_DIR" ] && [ ! -L "$EXTRA_DIR" ] \
        && { [ -e "$LIST" ] || [ -L "$LIST" ]; }; then
        if [ -d "$LIST" ] && [ ! -L "$LIST" ]; then rm -rf -- "$LIST"; else rm -f -- "$LIST"; fi
        reload
        note "removed $NAME"
    else
        note "$NAME is not enabled"
    fi
    finish
fi

# --add
# 1. The source: another app's bundled list, in a folder only root or an administrator can change
trust_path "$SOURCE" file
[ -f "$SOURCE" ] || die "$SOURCE: not a file"
SRC_REAL="$(/bin/realpath -- "$SOURCE" 2>/dev/null)" || die "cannot resolve $SOURCE"

# 2. Camera access (the viewer's usbd.plist) must be installed; its protocol version and own hashes
STAGE="$(mktemp -d "$TMPBASE/rs-usbd.XXXXXX")"
NOT_INSTALLED="camera access (RealSense Viewer) is not installed on this Mac"
if ! { [ -d "$CONF_DIR" ] && [ ! -L "$CONF_DIR" ] && root_file "$CONF"; }; then die "$NOT_INSTALLED"; fi
ditto --noextattr --noqtn "$CONF" "$STAGE/usbd.plist"
plutil -lint -s "$STAGE/usbd.plist" >/dev/null 2>&1 || die "$NOT_INSTALLED"
INSTALLED_PROTOCOL="$(pl_get ProtocolVersion integer "$STAGE/usbd.plist")" || die "$NOT_INSTALLED"
[[ "$INSTALLED_PROTOCOL" =~ ^[0-9]{1,9}$ ]] || die "$NOT_INSTALLED"
OWN=()
while IFS= read -r h; do OWN+=("$h"); done < <(hashes_of "$STAGE/usbd.plist" 256 || echo invalid)
[ "${#OWN[@]}" -ge 1 ] && [ "${OWN[${#OWN[@]} - 1]}" != invalid ] || die "$NOT_INSTALLED"

# 3. Stage the source; everything below reads only the staged copy
S_LIST="$STAGE/list.plist"
ditto --noextattr --noqtn "$SRC_REAL" "$S_LIST"
[ -f "$S_LIST" ] && [ ! -L "$S_LIST" ] || die "the staged list is not a regular file"
[ "$(stat -f %z "$S_LIST")" -le 65536 ] || die "$NAME: the list is larger than 64 KiB"

# 4. Validate (DESIGN 5.4)
plutil -lint -s "$S_LIST" >/dev/null 2>&1 || die "$NAME: the list is not a valid property list"
[ "$(plutil -type Version "$S_LIST" 2>/dev/null)" = integer ] && [ "$(pl_get Version integer "$S_LIST")" = 1 ] \
    || die "$NAME: Version must be the integer 1"
[ "$(pl_get Name string "$S_LIST" || true)" = "$NAME" ] || die "$NAME: the list's Name is not $NAME"
BUILD_ID="$(pl_get BuildID string "$S_LIST")" && [[ "$BUILD_ID" =~ $RE_HEX64 ]] \
    || die "$NAME: BuildID must be 64 lowercase hex digits"
BUILD_NUMBER="$(pl_get BuildNumber integer "$S_LIST")" && [[ "$BUILD_NUMBER" =~ $RE_UINT ]] \
    || die "$NAME: BuildNumber must be an integer 0..999999999999999999"
PROTOCOL="$(pl_get ProtocolVersion integer "$S_LIST")" && [[ "$PROTOCOL" =~ ^[0-9]{1,10}$ ]] \
    && [ "$PROTOCOL" -ge 1 ] && [ "$PROTOCOL" -le 2147483647 ] \
    || die "$NAME: ProtocolVersion must be an integer 1..2147483647"
HASHES=()
while IFS= read -r h; do HASHES+=("$h"); done < <(hashes_of "$S_LIST" 64 || echo invalid)
[ "${#HASHES[@]}" -ge 1 ] && [ "${HASHES[${#HASHES[@]} - 1]}" != invalid ] \
    || die "$NAME: AllowedCDHashes must hold 1..64 cdhashes of 40 lowercase hex digits"
SORTED="$(printf '%s\n' "${HASHES[@]}" | LC_ALL=C sort -u)"
[ "$(grep -c . <<<"$SORTED")" = "${#HASHES[@]}" ] || die "$NAME: AllowedCDHashes holds a cdhash twice"
[ "$(printf '%s' "$SORTED" | shasum -a 256 | cut -d' ' -f1)" = "$BUILD_ID" ] \
    || die "$NAME: BuildID does not match AllowedCDHashes"
[ "$PROTOCOL" -eq "$INSTALLED_PROTOCOL" ] \
    || die "$NAME was made for camera access protocol $PROTOCOL, this Mac's camera access speaks $INSTALLED_PROTOCOL; update one of them"

# 5. Capacity (DESIGN 5.5: the daemon reads the first 16 list names and admits at most 256 hashes in total). Every
# other usbd.d/<valid name>.plist takes one of the 16 places, as in the daemon; the hashes of the valid ones count.
OTHERS=0
OTHER_HASHES=()
if [ -d "$EXTRA_DIR" ] && [ ! -L "$EXTRA_DIR" ]; then
    for f in "$EXTRA_DIR"/*.plist; do
        [ -e "$f" ] || [ -L "$f" ] || continue
        base="${f##*/}"
        [ "$base" != "$NAME.plist" ] || continue
        valid_name "${base%.plist}" || continue
        OTHERS=$((OTHERS + 1))
        root_file "$f" && [ "$(stat -f %z -- "$f")" -le 65536 ] || continue
        if list_hashes="$(hashes_of "$f" 64)"; then
            while IFS= read -r h; do OTHER_HASHES+=("$h"); done <<<"$list_hashes"
        fi
    done
fi
NEW_HASHES="$( { printf '%s\n' ${OTHER_HASHES[@]+"${OTHER_HASHES[@]}"} "${HASHES[@]}"; } | LC_ALL=C sort -u \
    | LC_ALL=C comm -23 - <(printf '%s\n' "${OWN[@]}" | LC_ALL=C sort -u) | grep -c . || true)"
TOTAL=$((${#OWN[@]} + NEW_HASHES))
if [ $((OTHERS + 1)) -gt 16 ] || [ "$TOTAL" -gt 256 ]; then
    die "camera access cannot admit more programs ($((OTHERS + 1)) lists of at most 16, $TOTAL code hashes of at most 256)"
fi

# 6. Downgrade
if root_file "$LIST"; then
    old_build="$(pl_get BuildNumber integer "$LIST" || true)"
    if [[ "$old_build" =~ $RE_UINT ]] && [ "$old_build" -gt "$BUILD_NUMBER" ]; then
        [ "$ALLOW_DOWNGRADE" = 1 ] || die "$NAME build $old_build is enabled, newer than this one ($BUILD_NUMBER); pass --allow-downgrade to replace it"
        note "downgrading $NAME from build $old_build to $BUILD_NUMBER"
    fi
fi

# 7. The directory of the lists: root:wheel 0755; its other files are not touched
if [ -L "$EXTRA_DIR" ] || { [ -e "$EXTRA_DIR" ] && [ ! -d "$EXTRA_DIR" ]; }; then
    rm -f -- "$EXTRA_DIR"
fi
if [ -d "$EXTRA_DIR" ]; then
    if [ "$DRY" = 0 ]; then chown root:wheel "$EXTRA_DIR"; fi
    chmod 755 "$EXTRA_DIR"
else
    install -d ${OWNER[@]+"${OWNER[@]}"} -m 755 "$EXTRA_DIR"
fi

# 8. The list, from the validated values only, renamed into place
NEW_LIST="$STAGE/new.plist"
plutil -create xml1 "$NEW_LIST"
plutil -insert Version -integer 1 "$NEW_LIST"
plutil -insert Name -string "$NAME" "$NEW_LIST"
plutil -insert BuildID -string "$BUILD_ID" "$NEW_LIST"
plutil -insert BuildNumber -integer "$BUILD_NUMBER" "$NEW_LIST"
plutil -insert ProtocolVersion -integer "$PROTOCOL" "$NEW_LIST"
plutil -insert AllowedCDHashes -array "$NEW_LIST"
while IFS= read -r h; do plutil -insert AllowedCDHashes -string "$h" -append "$NEW_LIST"; done <<<"$SORTED"
plutil -lint -s "$NEW_LIST" >/dev/null
TMPLIST="$EXTRA_DIR/.$NAME.plist.tmp.$$"
install ${OWNER[@]+"${OWNER[@]}"} -m 644 "$NEW_LIST" "$TMPLIST"
xattr -c "$TMPLIST"
if [ -d "$LIST" ] && [ ! -L "$LIST" ]; then rm -rf -- "$LIST"; fi
mv -f -- "$TMPLIST" "$LIST"
TMPLIST=

# 9. Restart the daemon so it reads the list
reload
note "enabled $NAME (build $BUILD_NUMBER, ${#HASHES[@]} code hashes)"
finish
