#!/bin/bash
# Installs or updates RealSense Viewer, RealSense Depth Quality Tool and camera access (realsense-usbd) on many
# Macs over SSH. Runs on the administrator's own Mac as a normal user (never with sudo); for every target it logs
# in over SSH as an administrator account and runs scripts/deploy/remote-install.sh there with sudo. Each target
# downloads the release DMG from GitHub itself (pinned tag + sha256) and installs it; see README "Deploying to
# many Macs".
#
#   scripts/deploy-macs.sh [options] [user@]host ...
#   scripts/deploy-macs.sh [options] -f hosts.txt
#
# Options:
#   -l, --login <user>    administrator account for hosts given without user@ (default: ssh's default)
#   -f, --file <file>     read hosts from a file: one [user@]host per line, "#" starts a comment (repeatable)
#   --ask-pass            ask for the sudo password once and feed it to "sudo -S" on every target through the SSH
#                         channel's stdin (never on a command line, never in a file). If stdin is not a terminal,
#                         the first line of stdin is the password (e.g. piped from "security find-generic-password -w").
#                         Without it, ssh -t is used and sudo asks on every host. If the password is rejected
#                         before any host accepted it, the remaining hosts are skipped (a typo is not tried on
#                         every Mac).
#   -j, --jobs <n>        with --ask-pass: deploy to up to n hosts at once (needs SSH keys: BatchMode=yes)
#   --tools               also install "RealSense Tools" to "/Applications/RealSense Tools" (an existing one is
#                         always updated)
#   --apps-only           install/update only the apps, leave camera access alone
#   --allow-downgrade     allow installing camera access older than the installed one
#   --uninstall           remove camera access with the installed uninstaller
#   --remove-apps         with --uninstall: also delete the apps and "/Applications/RealSense Tools"
#   --tag <tag> --sha256 <hex> [--asset <name.dmg>]
#                         another release (default: the pinned one below; a new tag needs its sha256)
#   --dmg <path>          use a DMG already present on the targets at <path> (still checked against the sha256)
#   --ssh-opt <opt>       pass "-o <opt>" to ssh, e.g. --ssh-opt Port=2222 --ssh-opt IdentityFile=~/.ssh/admin
#   --log-dir <dir>       put the per-host logs in a new private folder inside <dir> (default: inside $TMPDIR)
#   --dry-run --root <dir>  for scripts/test-deploy.sh only (its fake ssh and sudo): remote-install.sh installs
#                         under <dir> without root. Against a real Mac every host fails, because the real sudo
#                         makes it root and remote-install.sh refuses to dry-run as root.
#
# Prints a per-host OK/FAILED summary. Exit status: 0 all hosts OK, 1 at least one failed, 2 usage error.

set -euo pipefail
unset CDPATH

ME=deploy-macs
PINNED_TAG=v2.58.4-macos.3
PINNED_ASSET=RealSense-Viewer-2.58.4-macOS-arm64.dmg
PINNED_SHA256=2aed32042f8ded5c4a66a9a0c64b356f22579fe98db3af0e3493c680b8020471

HERE="$(cd "$(dirname "$0")" && pwd -P)"
REMOTE_SCRIPT="$HERE/deploy/remote-install.sh"

die() { echo "$ME: error: $*" >&2; exit 1; }
usage_error() {
    echo "$ME: $*" >&2
    echo "usage: $0 [options] [user@]host ... | -f hosts.txt   (see the header of $0 for the options)" >&2
    exit 2
}
usage() { sed -n '2,/^$/s/^# \{0,1\}//p' "$0"; exit 0; }

[ "$(id -u)" != 0 ] || die "run this as your normal user, not with sudo (it uses your SSH keys and settings)"

HOSTS=()
LOGIN=
ASK=0
JOBS=1
SSH_OPTS=()
FLAGS=()
TAG=
SHA=
ASSET=
DMG=
DRY=0
ROOTP=
LOGDIR=
HAS_UNINSTALL=0
HAS_REMOVE=0
HAS_INSTALL_ONLY=0

RE_USER='[A-Za-z0-9_][A-Za-z0-9._-]{0,63}'
RE_HOST='[A-Za-z0-9][A-Za-z0-9._:%-]{0,252}'
add_host() {
    local h="$1"
    [[ "$h" =~ ^($RE_USER@)?$RE_HOST$ ]] || usage_error "not a valid [user@]host: '$h'"
    HOSTS+=("$h")
}
read_hosts_file() {
    local f="$1" line tok rest n=0
    [ -f "$f" ] && [ -r "$f" ] || usage_error "cannot read hosts file: $f"
    while IFS= read -r line || [ -n "$line" ]; do
        n=$((n + 1))
        line="${line%%#*}"
        line="${line%$'\r'}"
        tok=
        rest=
        read -r tok rest <<<"$line" || true
        [ -n "$tok" ] || continue
        [ -z "$rest" ] || usage_error "$f:$n: one host per line"
        add_host "$tok"
    done <"$f"
}

while [ $# -gt 0 ]; do
    case "$1" in
        -h | --help) usage ;;
        --ask-pass) ASK=1 ;;
        --tools | --apps-only | --allow-downgrade) FLAGS+=("$1"); HAS_INSTALL_ONLY=1 ;;
        --uninstall) FLAGS+=("$1"); HAS_UNINSTALL=1 ;;
        --remove-apps) FLAGS+=("$1"); HAS_REMOVE=1 ;;
        --dry-run) DRY=1 ;;
        -l | --login | -f | --file | -j | --jobs | --tag | --sha256 | --asset | --dmg | --ssh-opt | --log-dir | --root)
            [ $# -ge 2 ] && [ -n "$2" ] || usage_error "$1 needs a value"
            case "$1" in
                -l | --login) LOGIN="$2" ;;
                -f | --file) read_hosts_file "$2" ;;
                -j | --jobs) JOBS="$2" ;;
                --tag) TAG="$2" ;;
                --sha256) SHA="$2" ;;
                --asset) ASSET="$2" ;;
                --dmg) DMG="$2"; HAS_INSTALL_ONLY=1 ;;
                --ssh-opt) SSH_OPTS+=(-o "$2") ;;
                --log-dir) LOGDIR="$2" ;;
                --root) ROOTP="$2" ;;
            esac
            shift
            ;;
        --) shift; while [ $# -gt 0 ]; do add_host "$1"; shift; done; break ;;
        -*) usage_error "unknown option: $1" ;;
        *) add_host "$1" ;;
    esac
    shift
done

[ ${#HOSTS[@]} -gt 0 ] || usage_error "no hosts given"
if [ -n "$LOGIN" ]; then [[ "$LOGIN" =~ ^$RE_USER$ ]] || usage_error "not a valid login name: '$LOGIN'"; fi
[[ "$JOBS" =~ ^[0-9]{1,2}$ ]] && [ "$JOBS" -ge 1 ] || usage_error "--jobs must be 1..99"
[ "$JOBS" = 1 ] || [ "$ASK" = 1 ] || usage_error "--jobs needs --ask-pass (sudo cannot prompt on several hosts at once)"
[ "$HAS_REMOVE" = 0 ] || [ "$HAS_UNINSTALL" = 1 ] || usage_error "--remove-apps needs --uninstall"
[ "$HAS_UNINSTALL" = 0 ] || [ "$HAS_INSTALL_ONLY" = 0 ] \
    || usage_error "--uninstall cannot be combined with --tools, --apps-only, --allow-downgrade or --dmg"
if { [ -n "$TAG" ] || [ -n "$ASSET" ]; } && [ -z "$SHA" ]; then
    usage_error "--tag/--asset need --sha256 <the DMG's sha256>"
fi
TAG="${TAG:-$PINNED_TAG}"
ASSET="${ASSET:-$PINNED_ASSET}"
SHA="${SHA:-$PINNED_SHA256}"
[[ "$TAG" =~ ^[A-Za-z0-9][A-Za-z0-9._-]{0,99}$ ]] || usage_error "bad --tag"
[[ "$ASSET" =~ ^[A-Za-z0-9][A-Za-z0-9._-]{0,199}\.dmg$ ]] || usage_error "bad --asset"
SHA="$(printf '%s' "$SHA" | tr 'A-F' 'a-f')"
[[ "$SHA" =~ ^[0-9a-f]{64}$ ]] || usage_error "bad --sha256 (64 hex digits)"
if [ "$DRY" = 1 ]; then [ -n "$ROOTP" ] || usage_error "--dry-run needs --root <dir>"; fi
if [ -n "$ROOTP" ]; then [ "$DRY" = 1 ] || usage_error "--root needs --dry-run"; fi

[ -f "$REMOTE_SCRIPT" ] || die "missing $REMOTE_SCRIPT (keep deploy-macs.sh next to its deploy/ folder)"
/bin/bash -n "$REMOTE_SCRIPT" || die "$REMOTE_SCRIPT has a syntax error"

# Arguments for remote-install.sh
RARGS=(--tag "$TAG" --sha256 "$SHA" --asset "$ASSET")
RARGS+=(${FLAGS[@]+"${FLAGS[@]}"})
if [ -n "$DMG" ]; then RARGS+=(--dmg "$DMG"); fi
if [ "$DRY" = 1 ]; then RARGS+=(--dry-run --root "$ROOTP"); fi

# q <string>: one single-quoted word for the remote login shell (sh, bash or zsh)
q() {
    local s
    s="$(printf '%s' "$1" | LC_ALL=C sed "s/'/'\\\\''/g"; echo x)"
    printf "'%s'" "${s%x}"
}

# The remote command: the script travels as an argument of "bash -c" (no file on the target, nothing for another
# user to swap), so stdin stays free for sudo -S. remote-install.sh starts with exec </dev/null, so a password that
# sudo did not need is never read by anything else.
if [ "$ASK" = 1 ]; then
    REMOTE_CMD="/usr/bin/sudo -S -p '' --"
else
    REMOTE_CMD="/usr/bin/sudo -p $(q "[sudo] password for %u on %h: ") --"
fi
REMOTE_CMD="$REMOTE_CMD /bin/bash -p -c $(q "$(cat "$REMOTE_SCRIPT")") rs-remote-install"
for a in "${RARGS[@]}"; do REMOTE_CMD="$REMOTE_CMD $(q "$a")"; done

SSH_BASE=(-o ConnectTimeout=20 -o ServerAliveInterval=30 -o ServerAliveCountMax=6)
if [ "$ASK" = 1 ]; then
    SSH_BASE+=(-T)
    if [ "$JOBS" -gt 1 ]; then SSH_BASE+=(-o BatchMode=yes); fi
else
    SSH_BASE+=(-t)
    [ -t 0 ] || echo "$ME: warning: stdin is not a terminal; sudo on the targets cannot ask for a password (use --ask-pass)" >&2
fi
SSH_BASE+=(${SSH_OPTS[@]+"${SSH_OPTS[@]}"})

# Always a new private folder (mktemp -d: mode 0700); an existing --log-dir is never chmod-ed or overwritten
if [ -z "$LOGDIR" ]; then LOGDIR="${TMPDIR:-/tmp}"; fi
[ -d "$LOGDIR" ] || mkdir -p -- "$LOGDIR" || die "cannot create $LOGDIR"
LOGDIR="$(mktemp -d "${LOGDIR%/}/rs-deploy.XXXXXX")" || die "cannot create a log folder in $LOGDIR"

PW=
if [ "$ASK" = 1 ]; then
    if [ -t 0 ]; then
        IFS= read -r -s -p "sudo password of the administrator account on the targets: " PW || true
        echo >&2
    else
        IFS= read -r PW || true
    fi
    [ -n "$PW" ] || die "no password given"
fi
PIDS=()
on_exit() {
    PW=
    unset PW
}
on_signal() {
    local p
    # The ssh processes are children of the background subshells (and ignore SIGINT there): stop them too
    for p in ${PIDS[@]+"${PIDS[@]}"}; do
        pkill -TERM -P "$p" 2>/dev/null || true
        kill "$p" 2>/dev/null || true
    done
    echo "$ME: interrupted; a target that had already started finishes (or rolls back) on its own; run the" \
        "command again to see the state of every Mac (logs so far: $LOGDIR)" >&2
    exit 130
}
trap on_exit EXIT
trap on_signal HUP INT TERM

N=${#HOSTS[@]}
dest_of() {
    case "$1" in
        *@*) printf '%s' "$1" ;;
        *) if [ -n "$LOGIN" ]; then printf '%s@%s' "$LOGIN" "$1"; else printf '%s' "$1"; fi ;;
    esac
}
log_of() { printf '%s/%02d-%s.log' "$LOGDIR" "$1" "$(printf '%s' "$2" | tr -c 'A-Za-z0-9._@-' '_')"; }

# run_host <index> <dest> <log> <tee: 0|1>: writes the ssh exit status to <log>.rc
run_host() {
    local i="$1" dest="$2" log="$3" show="$4" rc
    set +e
    if [ "$ASK" = 1 ]; then
        if [ "$show" = 1 ]; then
            printf '%s\n' "$PW" | ssh "${SSH_BASE[@]}" -- "$dest" "$REMOTE_CMD" 2>&1 | tee "$log"
            rc=${PIPESTATUS[1]}
        else
            printf '%s\n' "$PW" | ssh "${SSH_BASE[@]}" -- "$dest" "$REMOTE_CMD" >"$log" 2>&1
            rc=${PIPESTATUS[1]}
        fi
    else
        ssh "${SSH_BASE[@]}" -- "$dest" "$REMOTE_CMD" 2>&1 | tee "$log"
        rc=${PIPESTATUS[0]}
    fi
    set -e
    echo "$rc" >"$log.rc"
}

# pw_verdict <log>: "ok" if sudo accepted the password (remote-install.sh ran), "bad" if sudo rejected it
pw_verdict() {
    if grep -q '^rs-deploy: ' "$1" 2>/dev/null; then
        echo ok
    elif grep -q -E 'incorrect password attempt|no password was provided|Sorry, try again' "$1" 2>/dev/null; then
        echo bad
    else
        echo unknown
    fi
}

echo "$ME: $N host(s), release $TAG ($ASSET, sha256 $SHA)"
echo "$ME: logs in $LOGDIR"
PW_ACCEPTED=0
STOPPED=
i=0
for h in "${HOSTS[@]}"; do
    i=$((i + 1))
    dest="$(dest_of "$h")"
    log="$(log_of "$i" "$dest")"
    if [ -n "$STOPPED" ]; then
        echo "skipped: not attempted, the sudo password was rejected on $STOPPED" >"$log"
        echo "SKIPPED" >"$log.rc"
        continue
    fi
    # The first host runs alone (also with -j), so a mistyped password is caught before it reaches every Mac
    if [ "$JOBS" -gt 1 ] && [ "$i" -gt 1 ]; then
        if [ ${#PIDS[@]} -ge "$JOBS" ]; then
            wait "${PIDS[0]}" || true
            PIDS=(${PIDS[@]+"${PIDS[@]:1}"})
        fi
        echo "==== [$i/$N] $dest: started (log: $log)"
        run_host "$i" "$dest" "$log" 0 &
        PIDS+=($!)
    else
        echo "==== [$i/$N] $dest"
        run_host "$i" "$dest" "$log" 1
        if [ "$ASK" = 1 ] && [ "$PW_ACCEPTED" = 0 ]; then
            case "$(pw_verdict "$log")" in
                ok) PW_ACCEPTED=1 ;;
                bad)
                    STOPPED="$dest"
                    if [ "$i" -lt "$N" ]; then
                        echo "$ME: the sudo password was rejected on $dest; skipping the other hosts (run again)" >&2
                    fi
                    ;;
            esac
        fi
    fi
done
for p in ${PIDS[@]+"${PIDS[@]}"}; do wait "$p" || true; done
PIDS=()
PW=
unset PW

# Summary
echo
echo "==== Summary ($TAG)"
OKS=0
i=0
for h in "${HOSTS[@]}"; do
    i=$((i + 1))
    dest="$(dest_of "$h")"
    log="$(log_of "$i" "$dest")"
    rc="$(cat "$log.rc" 2>/dev/null || echo 255)"
    if [ "$rc" = SKIPPED ]; then
        printf 'SKIPPED %s  not attempted (the sudo password was rejected on %s)\n' "$dest" "$STOPPED"
        continue
    fi
    msg="$(tr -d '\r' <"$log" 2>/dev/null | sed -n 's/^rs-deploy: RESULT: //p' | tail -1)"
    if [ "$rc" = 0 ] && [ "${msg#OK}" != "$msg" ]; then
        OKS=$((OKS + 1))
        printf 'OK      %s  %s\n' "$dest" "${msg#OK on }"
    else
        if [ -z "$msg" ]; then
            if [ "$rc" = 255 ]; then
                msg="ssh failed: $(tr -d '\r' <"$log" 2>/dev/null | grep -v '^[[:space:]]*$' | tail -1)"
            else
                msg="$(tr -d '\r' <"$log" 2>/dev/null | grep -v '^[[:space:]]*$' | tail -1)"
            fi
        fi
        printf 'FAILED  %s  (exit %s) %s\n' "$dest" "$rc" "${msg#FAILED on }"
    fi
done
echo "$OKS of $N host(s) OK; logs in $LOGDIR"
[ "$OKS" = "$N" ]
