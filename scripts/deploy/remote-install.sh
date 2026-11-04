#!/bin/bash -p
# Installs (or updates, or removes) RealSense Viewer, RealSense Depth Quality Tool and camera access on THIS Mac.
# Runs as root. scripts/deploy-macs.sh streams it to many Macs over SSH; it can also be run by hand on one Mac:
#
#   sudo bash remote-install.sh [--tools] [--apps-only] [--allow-downgrade] [--dmg <file>]
#                               [--tag <tag> --sha256 <hex> [--asset <name.dmg>]]
#   sudo bash remote-install.sh --uninstall [--remove-apps]
#   bash remote-install.sh --dry-run --root <dir> ...      (not as root: test mode, see below)
#
# Install/update: downloads the release DMG from GitHub over https (pinned tag + sha256; or --dmg <local file>,
# which is checked against the same sha256), attaches it read-only, checks the code signatures and bundle IDs,
# copies both apps into /Applications (root:wheel, not writable by others, no quarantine; each app is copied
# under a hidden name and then swapped in), with --tools (or when it is already there) also
# "/Applications/RealSense Tools", and then runs the INSTALLED app's install-camera-access.sh, which installs or
# updates the realsense-usbd LaunchDaemon so every user of this Mac can use the camera without a password.
# --apps-only skips camera access. Re-running is an update.
# --uninstall runs the installed uninstall-camera-access.sh; --remove-apps also deletes the apps and the tools.
# --dry-run --root <dir>: maps /Applications to <dir>/Applications, needs a non-root user, never runs chown or
# launchctl, and runs the camera access installer/uninstaller in their own --dry-run --root <dir> mode (download,
# sha256, read-only attach, signature checks and the copies still happen).
#
# The last line is "rs-deploy: RESULT: OK ..." or "rs-deploy: RESULT: FAILED ..."; exit status 0 = success,
# 1 = failure, 2 = usage error.

unset IFS CDPATH ENV BASH_ENV TMPDIR
export PATH=/usr/bin:/bin:/usr/sbin:/sbin
umask 022
set -euo pipefail
# Nothing below reads stdin. When deploy-macs.sh feeds sudo -S its password and sudo did not need it (cached or
# NOPASSWD), the line is still unread on stdin: never let any command see it.
exec </dev/null
# Once started, finish (or roll back) on the target even when the SSH connection goes away: a closed connection
# must not kill this script or the camera access installer halfway (e.g. between launchctl bootout and bootstrap).
# With SIGPIPE ignored a write to the dead connection just fails, and every write below ignores that failure.
trap '' HUP PIPE

ME=rs-deploy
REPO=dolegadolegowski/realsense-viewer-macos
PINNED_TAG=v2.58.4-macos.3
PINNED_ASSET=RealSense-Viewer-2.58.4-macOS-arm64.dmg
PINNED_SHA256=2aed32042f8ded5c4a66a9a0c64b356f22579fe98db3af0e3493c680b8020471
VIEWER="RealSense Viewer.app"
VIEWER_ID=io.github.dolegadolegowski.realsense-viewer
DQT="RealSense Depth Quality Tool.app"
DQT_ID=io.github.dolegadolegowski.realsense-depth-quality
TOOLS="RealSense Tools"
LABEL=io.github.dolegadolegowski.realsense-usbd

HOST_NAME="$(scutil --get LocalHostName 2>/dev/null || hostname -s 2>/dev/null || echo this-mac)"
# All output goes through the external printf, never bash's builtin echo/printf: after a failed write (closed
# connection) bash 3.2 keeps the builtin's unwritten text buffered and flushes it later into the next $(...), which
# would corrupt values such as the computed sha256.
say() { /usr/bin/printf '%s\n' "$*" 2>/dev/null || true; }
note() { say "$ME: $*"; }
warn() { say "$ME: warning: $*" >&2; }
die() {
    say "$ME: error: $*" >&2
    say "$ME: RESULT: FAILED on $HOST_NAME: $*"
    exit 1
}
# relay: copies stdin to stdout, ignoring write errors (a child never sees a closed connection)
relay() {
    local l
    while IFS= read -r l || [ -n "$l" ]; do say "$l"; done
}
usage() {
    cat >&2 <<EOF
usage: sudo bash $0 [--tools] [--apps-only] [--allow-downgrade] [--dmg <file>]
                    [--tag <tag> --sha256 <hex> [--asset <name.dmg>]]
       sudo bash $0 --uninstall [--remove-apps]
       bash $0 --dry-run --root <dir> [any of the above]   (test mode, not as root)
defaults: --tag $PINNED_TAG --asset $PINNED_ASSET --sha256 $PINNED_SHA256
EOF
    exit 2
}

UNINSTALL=0
REMOVE_APPS=0
WANT_TOOLS=0
APPS_ONLY=0
ALLOW_DOWNGRADE=0
DMG_SRC=
DRY=0
PREFIX=
TAG=
SHA=
ASSET=
while [ $# -gt 0 ]; do
    case "$1" in
        --tools) WANT_TOOLS=1 ;;
        --apps-only) APPS_ONLY=1 ;;
        --uninstall) UNINSTALL=1 ;;
        --remove-apps) REMOVE_APPS=1 ;;
        --allow-downgrade) ALLOW_DOWNGRADE=1 ;;
        --dry-run) DRY=1 ;;
        --dmg | --root | --tag | --sha256 | --asset)
            [ $# -ge 2 ] && [ -n "$2" ] || usage
            case "$1" in
                --dmg) DMG_SRC="$2" ;;
                --root) PREFIX="$2" ;;
                --tag) TAG="$2" ;;
                --sha256) SHA="$2" ;;
                --asset) ASSET="$2" ;;
            esac
            shift
            ;;
        -h | --help) usage ;;
        *) say "$ME: unknown argument: $1" >&2; usage ;;
    esac
    shift
done

# Option combinations
if [ "$REMOVE_APPS" = 1 ] && [ "$UNINSTALL" = 0 ]; then die "--remove-apps needs --uninstall"; fi
if [ "$UNINSTALL" = 1 ]; then
    if [ "$WANT_TOOLS" = 1 ] || [ "$APPS_ONLY" = 1 ] || [ "$ALLOW_DOWNGRADE" = 1 ] || [ -n "$DMG_SRC" ]; then
        die "--uninstall cannot be combined with --tools, --apps-only, --allow-downgrade or --dmg"
    fi
fi
if { [ -n "$TAG" ] || [ -n "$ASSET" ]; } && [ -z "$SHA" ]; then
    die "--tag/--asset need --sha256 <the new DMG's sha256> (the pinned hash belongs to $PINNED_TAG)"
fi
TAG="${TAG:-$PINNED_TAG}"
ASSET="${ASSET:-$PINNED_ASSET}"
SHA="${SHA:-$PINNED_SHA256}"
[[ "$TAG" =~ ^[A-Za-z0-9][A-Za-z0-9._-]{0,99}$ ]] || die "bad --tag"
[[ "$ASSET" =~ ^[A-Za-z0-9][A-Za-z0-9._-]{0,199}\.dmg$ ]] || die "bad --asset (a file name ending in .dmg)"
SHA="$(printf '%s' "$SHA" | tr 'A-F' 'a-f')"
[[ "$SHA" =~ ^[0-9a-f]{64}$ ]] || die "bad --sha256 (64 hex digits)"

# Who and where
if [ "$DRY" = 1 ]; then
    [ "$(id -u)" != 0 ] || die "--dry-run must not run as root"
    [ -n "$PREFIX" ] || die "--dry-run needs --root <dir>"
    mkdir -p -- "$PREFIX"
    PREFIX="$(/bin/realpath -- "$PREFIX")"
    [ "$PREFIX" != / ] || die "--root / is not a dry run"
else
    [ -z "$PREFIX" ] || die "--root is only allowed with --dry-run"
    [ "$(id -u)" = 0 ] || die "run as root: sudo bash $0 ..."
fi
[ "$(sysctl -n hw.optional.arm64 2>/dev/null || echo 0)" = 1 ] || die "this is not an Apple Silicon Mac"
OS_VERSION="$(sw_vers -productVersion 2>/dev/null || echo 0)"
OS_MAJOR="${OS_VERSION%%.*}"
[[ "$OS_MAJOR" =~ ^[0-9]+$ ]] && [ "$OS_MAJOR" -ge 27 ] || die "macOS $OS_VERSION is too old (27 or newer needed)"

APPS="$PREFIX/Applications"
TMPBASE="$PREFIX/private/var/tmp"
LOCK="$PREFIX/var/run/io.github.dolegadolegowski.rs-deploy.lock"
USBD_CONF="$PREFIX/Library/Application Support/RealSense Viewer/usbd.plist"
DAEMON_PLIST="$PREFIX/Library/LaunchDaemons/$LABEL.plist"
if [ "$DRY" = 1 ]; then
    mkdir -p -- "$APPS" "$TMPBASE" "$PREFIX/var/run"
fi
[ -d "$APPS" ] && [ ! -L "$APPS" ] || die "$APPS is not a directory"

LOCKED=0
WORK=
MNT=
MNT_ATTACHED=0
STAGE_DIR=
SWAP_OLD=
SWAP_DEST=
detach() {
    if [ "$MNT_ATTACHED" = 1 ]; then
        hdiutil detach "$MNT" -quiet 2>/dev/null || hdiutil detach "$MNT" -force -quiet 2>/dev/null \
            || warn "could not detach $MNT"
        MNT_ATTACHED=0
    fi
}
cleanup() {
    detach
    # Interrupted between the two renames of a swap: put the previous version back
    if [ -n "$SWAP_OLD" ] && [ -e "$SWAP_OLD" ]; then
        if [ ! -e "$SWAP_DEST" ]; then mv -- "$SWAP_OLD" "$SWAP_DEST" 2>/dev/null || true; else rm -rf -- "$SWAP_OLD"; fi
    fi
    if [ -n "$STAGE_DIR" ]; then rm -rf -- "$STAGE_DIR"; fi
    if [ -n "$WORK" ]; then rm -rf -- "$WORK"; fi
    if [ "$LOCKED" = 1 ]; then rm -f -- "$LOCK/pid"; rmdir -- "$LOCK" 2>/dev/null || true; fi
}
trap cleanup EXIT
trap 'exit 1' INT TERM
if ! mkdir -- "$LOCK" 2>/dev/null; then
    holder="$(cat -- "$LOCK/pid" 2>/dev/null || true)"
    if [[ "$holder" =~ ^[0-9]{1,10}$ ]] && kill -0 "$holder" 2>/dev/null; then
        die "another deployment (pid $holder) is running here"
    fi
    die "a stale lock from an interrupted deployment is in the way; remove it with: sudo rm -rf '$LOCK'"
fi
LOCKED=1
echo "$$" >"$LOCK/pid" || true

DRYTXT=
if [ "$DRY" = 1 ]; then DRYTXT=" (dry run under $PREFIX)"; fi
note "$HOST_NAME, macOS $OS_VERSION$DRYTXT"

# Programs of this project running for any user (a camera access update restarts the daemon: their camera drops)
running="$(ps -axo user=,comm= 2>/dev/null \
    | grep -E '/RealSense (Viewer|Depth Quality Tool)\.app/|/RealSense Tools/bin/' \
    | awk '{print $1}' | sort -u | tr '\n' ' ' || true)"
if [ -n "$running" ]; then
    warn "RealSense programs are running (users: ${running% }); updating camera access stops their camera, they need to restart the app"
fi

bundle_id() { plutil -extract CFBundleIdentifier raw -expect string "$1/Contents/Info.plist" 2>/dev/null || true; }
bundle_version() { plutil -extract CFBundleShortVersionString raw -expect string "$1/Contents/Info.plist" 2>/dev/null || echo "?"; }

# run_helper <script> <args...>: runs an installed camera access script with a clean environment, like the app does.
# It is not interruptible (SIGINT ignored, inherited) and its output goes through relay, so neither Ctrl-C nor a
# dropped connection can stop it between unloading and loading the daemon.
run_helper() {
    local script="$1" rc=0
    shift
    trap '' INT
    if [ "$DRY" = 1 ]; then
        /usr/bin/env -i PATH=/usr/bin:/bin:/usr/sbin:/sbin "$script" --dry-run --root "$PREFIX" "$@" 2>&1 | relay || rc=$?
    else
        /usr/bin/env -i PATH=/usr/bin:/bin:/usr/sbin:/sbin "$script" "$@" 2>&1 | relay || rc=$?
    fi
    trap 'exit 1' INT
    if [ "$rc" = 3 ] && [ "$DRY" = 1 ]; then
        warn "$(basename "$script"): trust checks failed in the dry run (report-only; --root is inside a folder others can change)"
        rc=0
    fi
    return "$rc"
}

# ---------------------------------------------------------------------------------------------------------------
if [ "$UNINSTALL" = 1 ]; then
    U=
    for c in "$APPS/$VIEWER/Contents/Resources/realsense-usbd/uninstall-camera-access.sh" \
        "$APPS/$DQT/Contents/Resources/realsense-usbd/uninstall-camera-access.sh" \
        "$APPS/$TOOLS/camera-access/uninstall-camera-access.sh"; do
        if [ -f "$c" ] && [ ! -L "$c" ]; then
            U="$c"
            break
        fi
    done
    if [ -n "$U" ]; then
        note "removing camera access: $U"
        rc=0
        run_helper "$U" || rc=$?
        [ "$rc" = 0 ] || die "uninstall-camera-access.sh failed (exit $rc)"
        WHAT="camera access removed"
    elif [ -e "$DAEMON_PLIST" ]; then
        die "camera access is installed but no uninstaller was found in $APPS; run uninstall-camera-access.sh from the DMG's RealSense Tools/camera-access with sudo"
    else
        note "camera access is not installed"
        WHAT="camera access was not installed"
    fi
    if [ "$REMOVE_APPS" = 1 ]; then
        removed=
        for name in "$VIEWER" "$DQT" "$TOOLS"; do
            dest="$APPS/$name"
            if [ -L "$dest" ]; then
                rm -f -- "$dest"
                continue
            fi
            [ -d "$dest" ] || continue
            case "$name" in
                "$VIEWER") [ "$(bundle_id "$dest")" = "$VIEWER_ID" ] || { warn "$dest is not RealSense Viewer, left alone"; continue; } ;;
                "$DQT") [ "$(bundle_id "$dest")" = "$DQT_ID" ] || { warn "$dest is not the Depth Quality Tool, left alone"; continue; } ;;
                "$TOOLS") [ -d "$dest/camera-access" ] || [ -d "$dest/bin" ] || { warn "$dest does not look like RealSense Tools, left alone"; continue; } ;;
            esac
            rm -rf -- "$dest" || die "cannot remove $dest (see README: App Management)"
            removed="$removed, ${name%.app}"
            note "removed $dest"
        done
        if [ -n "$removed" ]; then WHAT="$WHAT; removed ${removed#, }"; else WHAT="$WHAT; no apps to remove"; fi
    fi
    note "RESULT: OK on $HOST_NAME: $WHAT$DRYTXT"
    exit 0
fi

# ---------------------------------------------------------------------------------------------------------------
# Install / update
WORK="$(mktemp -d "$TMPBASE/rs-deploy.XXXXXX")"
chmod 700 "$WORK"
DMG="$WORK/$ASSET"
if [ -n "$DMG_SRC" ]; then
    [ -f "$DMG_SRC" ] || die "no such file: $DMG_SRC"
    note "using $DMG_SRC"
    # Hash and attach a private copy, so the checked file is the attached file
    cat -- "$DMG_SRC" >"$DMG" || die "cannot copy $DMG_SRC"
else
    URL="https://github.com/$REPO/releases/download/$TAG/$ASSET"
    note "downloading $URL"
    # -q first: never read a .curlrc (sudo may keep the administrator's HOME)
    curl -q --proto '=https' --proto-redir '=https' --tlsv1.2 -fsSL --retry 3 --retry-delay 3 \
        --connect-timeout 20 --max-time 1800 --max-filesize 1073741824 -o "$DMG" -- "$URL" \
        || die "download failed: $URL"
fi
GOT="$(shasum -a 256 "$DMG" | cut -d' ' -f1)"
[ "$GOT" = "$SHA" ] || die "sha256 mismatch for $ASSET: expected $SHA, got $GOT (nothing was installed)"
note "sha256 ok ($SHA)"

MNT="$WORK/mnt"
mkdir -- "$MNT"
if ! out="$(hdiutil attach -readonly -nobrowse -noautoopen -mountpoint "$MNT" "$DMG" 2>&1)"; then
    say "$out" >&2
    die "cannot attach $ASSET"
fi
MNT_ATTACHED=1

# check_app <bundle> <bundle id>
check_app() {
    [ -d "$1" ] && [ ! -L "$1" ] || die "$(basename "$1") is missing from the DMG"
    [ "$(bundle_id "$1")" = "$2" ] || die "$(basename "$1"): unexpected bundle identifier"
    codesign --verify --deep --strict "$1" 2>&1 | relay || die "$(basename "$1"): invalid code signature"
}
check_app "$MNT/$VIEWER" "$VIEWER_ID"
check_app "$MNT/$DQT" "$DQT_ID"
NEW_ALLOW="$MNT/$VIEWER/Contents/Resources/realsense-usbd/allowlist.plist"
NEW_BUILD="$(plutil -extract BuildNumber raw -expect integer "$NEW_ALLOW" 2>/dev/null || true)"
[[ "$NEW_BUILD" =~ ^[0-9]{1,18}$ ]] || die "$VIEWER: no camera access build number in the DMG"
note "signatures ok (RealSense Viewer $(bundle_version "$MNT/$VIEWER"), camera access build $NEW_BUILD)"

INSTALL_TOOLS=0
if [ "$WANT_TOOLS" = 1 ]; then
    INSTALL_TOOLS=1
elif [ -d "$APPS/$TOOLS" ]; then
    INSTALL_TOOLS=1
    note "$APPS/$TOOLS exists: updating it too (tools only work with camera access of the same build)"
fi
if [ "$INSTALL_TOOLS" = 1 ]; then
    [ -d "$MNT/$TOOLS/bin" ] && [ ! -L "$MNT/$TOOLS" ] && [ -d "$MNT/$TOOLS/camera-access" ] \
        || die "$TOOLS is missing from the DMG"
    for f in "$MNT/$TOOLS"/bin/* "$MNT/$TOOLS"/lib/* "$MNT/$TOOLS/camera-access/realsense-usbd"; do
        [ -f "$f" ] || continue
        codesign --verify --strict "$f" 2>&1 | relay || die "$TOOLS: invalid code signature: ${f#"$MNT/"}"
    done
fi

# Never swap in apps that the installed (newer) camera access would then refuse to update
if [ "$APPS_ONLY" = 0 ] && [ -f "$USBD_CONF" ] && [ ! -L "$USBD_CONF" ]; then
    OLD_BUILD="$(plutil -extract BuildNumber raw -expect integer "$USBD_CONF" 2>/dev/null || true)"
    if [[ "$OLD_BUILD" =~ ^[0-9]{1,18}$ ]] && [ "$OLD_BUILD" -gt "$NEW_BUILD" ] && [ "$ALLOW_DOWNGRADE" = 0 ]; then
        die "installed camera access (build $OLD_BUILD) is newer than $TAG (build $NEW_BUILD); nothing changed (use --allow-downgrade to go back)"
    fi
fi

# install_item <source in the DMG> <name in /Applications> <verify: app|tools>
# The copy is made, chowned, cleaned of ACLs, made read-only for others and verified inside a new root-only (0700)
# folder in /Applications, so no other user can open, change or add an ACL to any of it before it is final. The old
# version is moved into the same private folder, and the new one is then renamed into place (same file system).
install_item() {
    local src="$1" name="$2" kind="$3" dest="$APPS/$2" new f
    STAGE_DIR="$(mktemp -d "$APPS/.rs-deploy.XXXXXX")" || die "cannot create a staging folder in $APPS"
    chmod 700 "$STAGE_DIR" || die "chmod failed on $STAGE_DIR"
    new="$STAGE_DIR/new"
    ditto --noqtn --noacl "$src" "$new" || die "cannot copy $name to $APPS"
    if [ "$DRY" = 0 ]; then chown -R root:wheel "$new" || die "chown failed on $new"; fi
    chmod -R -N "$new" || die "cannot remove ACLs from $new"
    chmod -R a+rX,go-w "$new" || die "chmod failed on $new"
    xattr -dr com.apple.quarantine "$new" 2>/dev/null || true
    if [ "$kind" = app ]; then
        codesign --verify --deep --strict "$new" 2>&1 | relay || die "$name: the copy does not verify"
    else
        for f in "$new"/bin/* "$new"/lib/* "$new/camera-access/realsense-usbd"; do
            [ -f "$f" ] || continue
            codesign --verify --strict "$f" 2>&1 | relay || die "$name: the copy does not verify: ${f#"$new/"}"
        done
    fi
    if [ -L "$dest" ] || { [ -e "$dest" ] && [ ! -d "$dest" ]; }; then rm -f -- "$dest"; fi
    if [ -e "$dest" ]; then
        SWAP_OLD="$STAGE_DIR/old"
        SWAP_DEST="$dest"
        mv -- "$dest" "$SWAP_OLD" || die "cannot move the old $dest aside (see README: App Management)"
    fi
    mv -- "$new" "$dest" || die "cannot move the new $name into place"
    SWAP_OLD=
    SWAP_DEST=
    rm -rf -- "$STAGE_DIR" || warn "could not delete $STAGE_DIR (the previous version)"
    STAGE_DIR=
    note "installed $dest"
}
install_item "$MNT/$VIEWER" "$VIEWER" app
install_item "$MNT/$DQT" "$DQT" app
WHAT="RealSense Viewer $(bundle_version "$APPS/$VIEWER"), Depth Quality Tool $(bundle_version "$APPS/$DQT")"
if [ "$INSTALL_TOOLS" = 1 ]; then
    install_item "$MNT/$TOOLS" "$TOOLS" tools
    WHAT="$WHAT, RealSense Tools"
fi
detach

if [ "$APPS_ONLY" = 1 ]; then
    if [ -f "$USBD_CONF" ]; then
        warn "--apps-only: camera access of another build may be installed; the new apps then ask for an administrator until it is updated"
    fi
    WHAT="$WHAT; camera access not changed (--apps-only)"
else
    INST="$APPS/$VIEWER/Contents/Resources/realsense-usbd/install-camera-access.sh"
    note "installing camera access: $INST"
    args=(--source "$APPS/$VIEWER/Contents/MacOS" --source "$APPS/$VIEWER/Contents/Resources/realsense-usbd")
    if [ "$ALLOW_DOWNGRADE" = 1 ]; then args+=(--allow-downgrade); fi
    rc=0
    run_helper "$INST" "${args[@]}" || rc=$?
    [ "$rc" = 0 ] || die "install-camera-access.sh failed (exit $rc); the apps are installed, camera access is not"
    WHAT="$WHAT; camera access build $NEW_BUILD"
    console="$(stat -f %Su /dev/console 2>/dev/null || true)"
    case "$console" in
        "" | root | loginwindow | _mbsetupuser) ;;
        *) note "$console (logged in at the screen) may see \"Background Items Added\" for realsense-usbd" ;;
    esac
fi
note "RESULT: OK on $HOST_NAME: $WHAT ($TAG)$DRYTXT"
