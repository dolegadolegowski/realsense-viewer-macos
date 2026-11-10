#!/bin/bash -p
unset IFS CDPATH ENV BASH_ENV TMPDIR
export PATH=/usr/bin:/bin:/usr/sbin:/sbin
umask 022
set -euo pipefail

# Installs the shared RealSense camera service for all users of this Mac: realsense-usbd as a root LaunchDaemon.
# macOS lets only root take a RealSense camera from its USB video driver; with the service installed, every
# application that carries these files (README.md) uses the cameras as the logged-in user, without an administrator
# password, also on standard accounts. One installed service serves all of them. It belongs to no application and to
# no build, and this installer never replaces a newer service with an older one, nor a working service with another
# build of the same version, so every application may run it. Spec: DESIGN.md sections 6 and 13.
#
#   sudo install-camera-access.sh --source <dir> [--source <dir>] [--allow-downgrade]
#       The source directories together hold realsense-usbd and service.plist: an application's Contents/MacOS (or
#       Contents/Helpers) and Contents/Resources/realsense-usbd, or one folder with both files.
#       The installed service is kept, and only loaded if it was not, when it is whole and either newer than this
#       application's or the same version and answering. Everything else is replaced by this application's service:
#       an older version, generation 1, a damaged installation, the same version when it does not answer.
#       --allow-downgrade skips both keep rules: it always installs this application's service, over a newer one
#       and over the same version alike.
#   install-camera-access.sh --dry-run --root <dir> --source <dir> [--source <dir>] [--allow-downgrade]
#       Not as root: writes the installed layout under <dir>, runs no launchctl and no probe (so the same version
#       is always written again), and only reports trust failures (exit status 3 at the end if there were any).
#
# Exit status: 0 installed, or the installed service kept; 1 error; 2 usage; 3 dry run with trust failures.

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
    [ "$(id -u)" = 0 ] || die "run as root (sudo), or let an application install it (ensure-camera-access.sh)"
fi

HELPER_DIR="$PREFIX/Library/PrivilegedHelperTools"
BIN="$HELPER_DIR/$LABEL"
CONF_DIR="$PREFIX/Library/Application Support/$LABEL"
CONF="$CONF_DIR/usbd.plist"
# Generation 1 (the service that admitted the programs of one RealSense Viewer build) kept its configuration here.
# Its installers and launchers still read BuildNumber from this file, so it stays as a marker (step 9).
LEGACY_DIR="$PREFIX/Library/Application Support/RealSense Viewer"
LEGACY_CONF="$LEGACY_DIR/usbd.plist"
LEGACY_MARKER_BUILD=999999999999
# Leftovers of a withdrawn design (lists of other applications, DESIGN 13.5.8); an installation removes them
EXTRA_DIR="$LEGACY_DIR/usbd.d"
LIST_TOOL="$HELPER_DIR/$LABEL-allowlist.sh"
DAEMON_PLIST="$PREFIX/Library/LaunchDaemons/$LABEL.plist"
LOCK="$PREFIX/var/run/$LABEL.install.lock"
TMPBASE="$PREFIX/private/var/tmp"
if [ "$DRY" = 1 ]; then
    mkdir -p -- "$PREFIX/var/run" "$TMPBASE" "$PREFIX/Library/LaunchDaemons"
fi

LOCKED=0
STAGE=
TMPBIN=
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
    local rest="$1" acc="" part
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
SRC_MANIFEST=
for dir in "${SOURCES[@]}"; do
    trust_path "$dir" dir
    dir="$(/bin/realpath -- "$dir" 2>/dev/null)" || die "cannot resolve a --source directory"
    for name in realsense-usbd service.plist; do
        [ -e "$dir/$name" ] || [ -L "$dir/$name" ] || continue
        trust_path "$dir/$name" file
        if [ "$name" = realsense-usbd ]; then
            [ -z "$SRC_BIN" ] || die "realsense-usbd found in more than one --source"
            SRC_BIN="$dir/$name"
        else
            [ -z "$SRC_MANIFEST" ] || die "service.plist found in more than one --source"
            SRC_MANIFEST="$dir/$name"
        fi
    done
done
[ -n "$SRC_BIN" ] || die "no realsense-usbd in the --source directories"
[ -n "$SRC_MANIFEST" ] || die "no service.plist in the --source directories"

# 2. Stage; everything below reads only the staged copies. The staged daemon is never run.
STAGE="$(mktemp -d "$TMPBASE/rs-usbd.XXXXXX")"
ditto --noextattr --noqtn "$SRC_BIN" "$STAGE/realsense-usbd"
ditto --noextattr --noqtn "$SRC_MANIFEST" "$STAGE/service.plist"
S_BIN="$STAGE/realsense-usbd"
S_MANIFEST="$STAGE/service.plist"
[ -f "$S_BIN" ] && [ ! -L "$S_BIN" ] && [ -f "$S_MANIFEST" ] && [ ! -L "$S_MANIFEST" ] \
    || die "staged copies are not regular files"
[ "$(stat -f %z "$S_BIN")" -le 16777216 ] || die "realsense-usbd is larger than 16 MiB"
[ "$(stat -f %z "$S_MANIFEST")" -le 65536 ] || die "service.plist is larger than 64 KiB"
case "$(file -b "$S_BIN")" in
    "Mach-O 64-bit executable arm64"*) ;;
    *) die "realsense-usbd is not an arm64 Mach-O executable" ;;
esac

# 3. Manifest (DESIGN 13.4) and the daemon's signature
RE_HEX40='^[0-9a-f]{40}$'
RE_UINT='^[0-9]{1,18}$'
pl_get() { plutil -extract "$1" raw -expect "$2" "$3" 2>/dev/null; }
cdhash_of() { codesign -dvvv --arch arm64 "$1" 2>&1 | sed -n 's/^CDHash=//p'; }
plutil -lint -s "$S_MANIFEST" >/dev/null || die "service.plist is not a valid property list"
[ "$(pl_get Version integer "$S_MANIFEST")" = 2 ] || die "service.plist: Version must be the integer 2"
SERVICE_VERSION="$(pl_get ServiceVersion integer "$S_MANIFEST")" || die "service.plist: ServiceVersion missing"
[[ "$SERVICE_VERSION" =~ $RE_UINT ]] && [ "$SERVICE_VERSION" -ge 2 ] \
    || die "service.plist: ServiceVersion must be an integer of 2 or more"
PROTO_MIN="$(pl_get ProtocolMin integer "$S_MANIFEST")" || die "service.plist: ProtocolMin missing"
PROTO_MAX="$(pl_get ProtocolMax integer "$S_MANIFEST")" || die "service.plist: ProtocolMax missing"
[[ "$PROTO_MIN" =~ ^[0-9]{1,3}$ && "$PROTO_MAX" =~ ^[0-9]{1,3}$ ]] && [ "$PROTO_MIN" -ge 1 ] \
    && [ "$PROTO_MIN" -le "$PROTO_MAX" ] || die "service.plist: need 1 <= ProtocolMin <= ProtocolMax <= 999"
DAEMON_CDHASH="$(pl_get DaemonCDHash string "$S_MANIFEST")" || die "service.plist: DaemonCDHash missing"
[[ "$DAEMON_CDHASH" =~ $RE_HEX40 ]] || die "service.plist: bad DaemonCDHash"
codesign --verify --strict "$S_BIN" || die "realsense-usbd: invalid code signature"
CDHASH="$(cdhash_of "$S_BIN")"
[ "$CDHASH" = "$DAEMON_CDHASH" ] || die "realsense-usbd: cdhash $CDHASH is not the manifest's DaemonCDHash"

# 4. What is installed (DESIGN 13.5): the service version, and where the administrator's settings come from
# stage_plist <path> <staged name>: copies a property list that only root could have written; prints the copy's path
stage_plist() {
    local uid links size
    [ -f "$1" ] && [ ! -L "$1" ] || return 1
    read -r uid links size <<<"$(stat -f '%u %l %z' -- "$1")"
    { [ "$uid" = 0 ] || [ "$DRY" = 1 ]; } && [ "$links" = 1 ] && [ "$size" -le 65536 ] \
        && plutil -lint -s "$1" >/dev/null 2>&1 || return 1
    ditto --noextattr --noqtn "$1" "$STAGE/$2" || return 1
    echo "$STAGE/$2"
}
INSTALLED=0          # 0 nothing, 1 generation 1, else the ServiceVersion of the installed configuration
INSTALLED_WHAT="generation 1 of camera access (it admitted the programs of one RealSense Viewer build only)"
INSTALLED_CDHASH=
OLD_CONF=            # staged copy of the configuration that holds the administrator's settings
OLD_GEN=0            # whose it is: 2 this service's, 1 generation 1's
if [ -e "$CONF" ] || [ -L "$CONF" ]; then
    if OLD_CONF="$(stage_plist "$CONF" old-usbd.plist)"; then
        OLD_GEN=2
        # Version is the schema of the file. A later service may raise it, and this installer still has to see
        # that service as newer: every schema from 2 on keeps ServiceVersion and DaemonCDHash (DESIGN 13.5.3).
        v="$(pl_get ServiceVersion integer "$OLD_CONF" || true)"
        cv="$(pl_get Version integer "$OLD_CONF" || true)"
        if [[ "$cv" =~ $RE_UINT ]] && [ "$cv" -ge 2 ] && [[ "$v" =~ $RE_UINT ]] && [ "$v" -ge 2 ]; then
            INSTALLED="$v"
            INSTALLED_CDHASH="$(pl_get DaemonCDHash string "$OLD_CONF" || true)"
        fi
    else
        OLD_CONF=
        # No setting is taken from a file like this, and a new configuration is the repair. Not when the file
        # names AllowedCDHashes: a new configuration would drop the administrator's restriction (see
        # copy_cdhashes). The key is looked for as bytes; both plist formats store it as this ASCII text.
        if [ -f "$CONF" ] && [ ! -L "$CONF" ] && LC_ALL=C grep -q -a -e AllowedCDHashes -- "$CONF"; then
            die "$CONF is not a valid property list owned by root (with one link, of at most 64 KiB) and names" \
                "AllowedCDHashes, which restricts who may use the cameras, so it is not replaced:" \
                "correct the file or remove it, then install again"
        fi
        note "ignoring the existing $CONF (not a valid root-owned property list)"
    fi
fi
if [ "$INSTALLED" = 0 ]; then
    if GEN1_CONF="$(stage_plist "$LEGACY_CONF" gen1-usbd.plist)" && pl_get BuildID string "$GEN1_CONF" >/dev/null; then
        INSTALLED=1
        if [ -z "$OLD_CONF" ]; then
            OLD_CONF="$GEN1_CONF"
            OLD_GEN=1
        fi
    elif [ -e "$BIN" ] || [ -L "$BIN" ] || [ -e "$DAEMON_PLIST" ] || [ -L "$DAEMON_PLIST" ]; then
        INSTALLED=1
        INSTALLED_WHAT="a camera service without a valid configuration (generation 1, or a damaged installation)"
    fi
fi

# installed_is_whole: the daemon, the job and the configuration are there, and the daemon is the one the
# configuration names (so "version $INSTALLED" is true of what would be kept and, as root, run)
installed_is_whole() {
    local uid perm got
    [ -f "$BIN" ] && [ ! -L "$BIN" ] && [ -f "$DAEMON_PLIST" ] && [ ! -L "$DAEMON_PLIST" ] || return 1
    if [ "$DRY" = 0 ]; then
        read -r uid perm <<<"$(stat -f '%u %OLp' -- "$BIN")"
        [ "$uid" = 0 ] && [ $((8#$perm & 8#022)) = 0 ] || return 1
    fi
    codesign --verify --strict "$BIN" 2>/dev/null || return 1
    got="$(cdhash_of "$BIN")" || return 1
    [[ "$INSTALLED_CDHASH" =~ $RE_HEX40 ]] && [ "$got" = "$INSTALLED_CDHASH" ]
}
finish_dry_run() {
    if [ "$TRUST_FAILURES" != 0 ]; then
        echo "$ME: dry run: $TRUST_FAILURES trust check(s) failed" >&2
        exit 3
    fi
    exit 0
}
load_job() {
    local out
    launchctl enable "system/$LABEL"
    if ! out="$(launchctl bootstrap system "$DAEMON_PLIST" 2>&1)"; then
        echo "$out" >&2
        case "$out" in
            *" 5:"*) echo "$ME: if \"realsense-usbd\" is switched off in System Settings > General > Login Items & Extensions, switch it on there and run this again" >&2 ;;
        esac
        return 1
    fi
}
# The installed daemon answers a root client (retried for 10 s); its line is left in PROBE. Exit 4 (camera use
# disabled by MDM / Screen Time) and 6 (too many programs connected) are answers of the running daemon too: it is
# installed and works, and no installation changes them.
PROBE=
daemon_answers() {
    local i rc
    for ((i = 0; i < 20; i++)); do
        rc=0
        PROBE="$("$BIN" --probe 2>&1)" || rc=$?
        case "$rc" in 0 | 4 | 6) return 0 ;; esac
        sleep 0.5
    done
    return 1
}
# installed_answers: all that is done to a service that is kept (step 5). Its job is loaded when it was not, and
# its daemon is asked. When either fails, NOT_KEPT says which.
NOT_KEPT=
installed_answers() {
    if ! launchctl print "system/$LABEL" >/dev/null 2>&1; then
        note "the installed camera service is not loaded: loading it"
        if ! load_job; then
            NOT_KEPT="cannot load the installed camera service (version $INSTALLED)"
            return 1
        fi
    fi
    if ! daemon_answers; then
        NOT_KEPT="the installed camera service (version $INSTALLED) does not answer: $PROBE"
        return 1
    fi
}

# 5. What is installed is kept when it is whole and newer than this application's service, or the same version and
# answering. The first rule is what lets every application run this installer. The second is for applications that
# carry different builds of one version: they must not replace, and so restart, each other's daemon (a restart ends
# every running camera session), and with AllowedCDHashes they would take turns at whose bundled probe is admitted.
# --allow-downgrade skips both rules.
if [ "$INSTALLED" -gt "$SERVICE_VERSION" ]; then
    if [ "$ALLOW_DOWNGRADE" = 1 ]; then
        note "replacing the installed camera service, version $INSTALLED," \
            "with the older $SERVICE_VERSION (--allow-downgrade)"
    elif installed_is_whole; then
        note "the installed camera service is version $INSTALLED," \
            "newer than this application's $SERVICE_VERSION: keeping it"
        if [ "$DRY" = 1 ]; then
            note "dry run: nothing under $PREFIX was changed (launchctl and the probe skipped)"
            finish_dry_run
        fi
        HOW="update this application, or run this installer with --allow-downgrade to replace the service"
        HOW="$HOW with version $SERVICE_VERSION"
        installed_answers || die "$NOT_KEPT; $HOW"
        note "kept the installed camera service, version $INSTALLED ($PROBE)"
        exit 0
    else
        note "the installed configuration names version $INSTALLED, but the daemon or its job" \
            "is missing or is another one: installing version $SERVICE_VERSION"
    fi
elif [ "$INSTALLED" = "$SERVICE_VERSION" ] && [ "$ALLOW_DOWNGRADE" = 0 ] && [ "$DRY" = 0 ] && installed_is_whole; then
    # A dry run cannot ask the daemon, so it never comes here and writes the staged version. launchctl's words about
    # a job that cannot be loaded are left out: the installation below loads it again and reports that.
    if installed_answers 2>/dev/null; then
        note "the RealSense camera service, version $INSTALLED, is already installed and answering ($PROBE):" \
            "nothing was replaced"
        exit 0
    fi
    note "$NOT_KEPT: installing version $SERVICE_VERSION again"
elif [ "$INSTALLED" = 1 ]; then
    note "replacing $INSTALLED_WHAT"
fi

# 6. Everything that will be written, built in the stage before anything installed is touched.
# Configuration (DESIGN 13.3): the validated manifest values, plus the administrator's settings when valid.
NEW_CONF="$STAGE/usbd.plist"
plutil -create xml1 "$NEW_CONF"
plutil -insert Version -integer 2 "$NEW_CONF"
plutil -insert ServiceVersion -integer "$SERVICE_VERSION" "$NEW_CONF"
plutil -insert ProtocolMin -integer "$PROTO_MIN" "$NEW_CONF"
plutil -insert ProtocolMax -integer "$PROTO_MAX" "$NEW_CONF"
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
# The administrator's list of programs that may use the service (the daemon also admits its own --probe, which
# step 12 uses). An invalid list stops the installation: the list is a restriction, and dropping it would open the
# service to every program of the logged-in user.
copy_cdhashes() {
    local kind n h i vals=() why
    why="is not valid (it must be an array of at most 256 strings of 40 lowercase hex digits);"
    why="$why it restricts who may use the cameras, so it is not dropped: correct it or remove the key, then install again"
    kind="$(plutil -type AllowedCDHashes "$OLD_CONF" 2>/dev/null)" || return 0
    [ "$kind" = array ] || die "$CONF: AllowedCDHashes $why"
    n="$(pl_get AllowedCDHashes array "$OLD_CONF")"
    [[ "$n" =~ ^[0-9]{1,3}$ ]] && [ "$n" -le 256 ] || die "$CONF: AllowedCDHashes $why"
    for ((i = 0; i < n; i++)); do
        h="$(pl_get "AllowedCDHashes.$i" string "$OLD_CONF" || true)"
        [[ "$h" =~ $RE_HEX40 ]] || die "$CONF: AllowedCDHashes $why"
        vals+=("$h")
    done
    plutil -insert AllowedCDHashes -array "$NEW_CONF"
    for h in ${vals[@]+"${vals[@]}"}; do plutil -insert AllowedCDHashes -string "$h" -append "$NEW_CONF"; done
    if [ "$n" -gt 0 ]; then note "kept AllowedCDHashes ($n): only these programs may use the service"; fi
}
if [ -n "$OLD_CONF" ]; then
    copy_int_key HandBackGraceSeconds 0 600
    copy_int_key IdleExitSeconds 10 3600
    copy_opcodes
    copy_ranges
    # Never from generation 1: there the list was the build's pin, not an administrator's choice
    if [ "$OLD_GEN" = 2 ]; then copy_cdhashes; fi
fi
plutil -lint -s "$NEW_CONF" >/dev/null

# Marker for generation 1 (DESIGN 13.5): its installers then refuse to put their old daemon over this one
# ("installed camera access is newer") and its launchers say "Camera access is newer than this app".
plutil -create xml1 "$STAGE/legacy-usbd.plist"
plutil -insert BuildNumber -integer "$LEGACY_MARKER_BUILD" "$STAGE/legacy-usbd.plist"
plutil -lint -s "$STAGE/legacy-usbd.plist" >/dev/null

# LaunchDaemon (constant content, DESIGN 6.4 and 13.5)
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
    <array>
        <string>io.github.dolegadolegowski.realsense-viewer</string>
        <string>io.github.dolegadolegowski.realsense-depth-quality</string>
        <string>io.github.dolegadolegowski.IntelFaceScan</string>
    </array>
</dict>
</plist>
PLIST
plutil -lint -s "$STAGE/daemon.plist" >/dev/null

# 7. Stop the running daemon (it hands the cameras back on SIGTERM)
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
# root_dir <dir>: a directory of root's, whatever was there under that name
root_dir() {
    if [ -L "$1" ] || { [ -e "$1" ] && [ ! -d "$1" ]; }; then
        rm -f -- "$1"
    fi
    mkdir -p -- "$1"
    if [ "$DRY" = 0 ]; then chown root:wheel "$1"; fi
    chmod 755 "$1"
}
# remove_item <path>: a file, a link (never followed) or a whole directory
remove_item() {
    if [ -L "$1" ] || { [ -e "$1" ] && [ ! -d "$1" ]; }; then
        rm -f -- "$1"
    elif [ -d "$1" ]; then
        rm -rf -- "$1"
    fi
}

# 8. Binary
install -d ${OWNER[@]+"${OWNER[@]}"} -m 755 "$HELPER_DIR"
TMPBIN="$HELPER_DIR/.$LABEL.tmp.$$"
install ${OWNER[@]+"${OWNER[@]}"} -m 755 "$S_BIN" "$TMPBIN"
xattr -c "$TMPBIN"
mv -f -- "$TMPBIN" "$BIN"
TMPBIN=

# 9. Configuration, then the marker in place of generation 1's configuration
root_dir "$CONF_DIR"
install -S ${OWNER[@]+"${OWNER[@]}"} -m 644 "$NEW_CONF" "$CONF"
root_dir "$LEGACY_DIR"
install -S ${OWNER[@]+"${OWNER[@]}"} -m 644 "$STAGE/legacy-usbd.plist" "$LEGACY_CONF"
# The lists of other applications and the tool that wrote them: this service admits every application without them
if [ -e "$EXTRA_DIR" ] || [ -L "$EXTRA_DIR" ]; then
    remove_item "$EXTRA_DIR"
    note "removed $EXTRA_DIR (lists of other applications: not needed any more)"
fi
remove_item "$LIST_TOOL"
find "$HELPER_DIR" -maxdepth 1 -name ".$LABEL-allowlist.sh.tmp.*" -exec rm -f -- {} +

# 10. LaunchDaemon
install -S ${OWNER[@]+"${OWNER[@]}"} -m 644 "$STAGE/daemon.plist" "$DAEMON_PLIST"
xattr -c "$DAEMON_PLIST"

if [ "$DRY" = 1 ]; then
    note "dry run: installed layout written under $PREFIX (launchctl and the probe skipped)"
    finish_dry_run
fi

# 11. Load
load_job || die "launchctl bootstrap failed"

# 12. Verify: loaded, and the daemon answers a root client
launchctl print "system/$LABEL" >/dev/null || die "the daemon is not loaded"
daemon_answers || die "the daemon does not answer: $PROBE"
note "installed the RealSense camera service, version $SERVICE_VERSION ($PROBE)"
note "  $BIN"
note "  $CONF"
note "  $DAEMON_PLIST"
