#!/bin/bash -p
# The program that shows the administrator prompt is taken from the caller's PATH (normally /usr/bin/osascript),
# before PATH is set below: the tests put a stand-in there and so never show a prompt. This script runs as the user
# with the user's own environment, so that choice gives the caller nothing they do not have already.
OSASCRIPT="$(command -v osascript 2>/dev/null || true)"
unset IFS CDPATH ENV BASH_ENV TMPDIR
export PATH=/usr/bin:/bin:/usr/sbin:/sbin
umask 022
set -euo pipefail

# Makes sure the shared RealSense camera service (realsense-usbd, README.md) can be used by the user who runs this:
# asks it with "realsense-usbd --probe" and, when it is not installed, too old or not answering, installs it from
# the files next to this script behind the standard macOS administrator prompt (an administrator's name and
# password, once per Mac). The shell counterpart of client/RealSenseCameraAccess.swift, for applications in any
# language and for people. Runs as the user, never as root. Spec: DESIGN.md 13.6.
#
#   ensure-camera-access.sh [--check] [--prompt <text>] [--source <dir>]...
#       --check          only report: never installs, never asks for a password
#       --prompt <text>  text of the administrator prompt (say what is installed and that it is asked once)
#       --source <dir>   where realsense-usbd and service.plist are. Default: this script's folder, and ../../MacOS
#                        and ../../Helpers when they hold realsense-usbd (an application bundle, where this script
#                        is in Contents/Resources/realsense-usbd). install-camera-access.sh is always the one next
#                        to this script.
#
# Standard output: one line, the answer of the probe ("ok <service version>" when the service can be used).
# Exit status:
#    0  the service answers and admits this user
#    2  not reachable: switched off, not answering, or this program is not on the administrator's list
#       (also: wrong arguments, run as root, files missing next to this script; the reason is on stderr)
#    3  only the user logged in at the Mac's screen (and root) can use the service
#    4  camera use is disabled on this Mac by its administrator (MDM / Screen Time)
#    5  the installed service does not speak this application's protocol version
#    6  too many programs are connected to the service
#   10  with --check: the service is not installed
#   20  the administrator prompt was cancelled
#   21  the installer failed (its message is on stderr)
# Without --check, 3, 4 and 6 end the script at once (installing changes nothing); in every other case but 0 the
# installer runs, and the status is that of the probe afterwards.

LABEL=io.github.dolegadolegowski.realsense-usbd
ME=ensure-camera-access
INSTALLED_JOB="/Library/LaunchDaemons/$LABEL.plist"

fail() { echo "$ME: error: $*" >&2; exit 2; }
usage() {
    echo "usage: $0 [--check] [--prompt <text>] [--source <dir>]..." >&2
    exit 2
}

case "$OSASCRIPT" in /*) ;; *) OSASCRIPT=/usr/bin/osascript ;; esac
CHECK=0
PROMPT="The RealSense camera service lets applications on this Mac use Intel RealSense cameras without an"
PROMPT="$PROMPT administrator password. It is installed once, for all users."
SOURCES=()
while [ $# -gt 0 ]; do
    case "$1" in
        --check) CHECK=1; shift ;;
        --prompt) [ $# -ge 2 ] || usage; PROMPT="$2"; shift 2 ;;
        --source) [ $# -ge 2 ] || usage; SOURCES+=("$2"); shift 2 ;;
        *) usage ;;
    esac
done
[ "$(id -u)" != 0 ] || fail "run this as the user who uses the camera; root runs install-camera-access.sh itself"

# Every path below comes from resolved ones (no symlinks): the installer refuses a path that goes through one.
SELF="$(/bin/realpath -- "$0" 2>/dev/null)" || fail "cannot resolve $0"
SELF_DIR="$(dirname -- "$SELF")"
has_daemon() { [ -f "$1/realsense-usbd" ] && [ -x "$1/realsense-usbd" ]; }
CANDIDATES=()
if [ ${#SOURCES[@]} -gt 0 ]; then
    for given in "${SOURCES[@]}"; do
        dir="$(/bin/realpath -- "$given" 2>/dev/null)" && [ -d "$dir" ] || fail "--source $given: not a directory"
        CANDIDATES+=("$dir")
    done
else
    CANDIDATES=("$SELF_DIR")
    for dir in "$SELF_DIR/../../MacOS" "$SELF_DIR/../../Helpers"; do
        if has_daemon "$dir"; then CANDIDATES+=("$(/bin/realpath -- "$dir")"); fi
    done
fi
# One folder with both files, else the first folder with each
DAEMON_DIR=
MANIFEST_DIR=
for dir in "${CANDIDATES[@]}"; do
    if has_daemon "$dir" && [ -f "$dir/service.plist" ]; then
        DAEMON_DIR="$dir"
        MANIFEST_DIR="$dir"
        break
    fi
    if [ -z "$DAEMON_DIR" ] && has_daemon "$dir"; then DAEMON_DIR="$dir"; fi
    if [ -z "$MANIFEST_DIR" ] && [ -f "$dir/service.plist" ]; then MANIFEST_DIR="$dir"; fi
done
[ -n "$DAEMON_DIR" ] || fail "no realsense-usbd in ${CANDIDATES[*]}"
DAEMON="$DAEMON_DIR/realsense-usbd"

# probe: runs the application's own "realsense-usbd --probe" with an empty environment and leaves its exit status in
# CODE and its line in LINE. A tool that cannot be run, crashes or does not answer within 8 s counts as 2 (not
# reachable), as in RealSenseCameraAccess.probe.
CODE=2
LINE=
probe() {
    local out rc=0
    out="$({
        # Job control gives the tool its own process group, so the watchdog also ends whatever it started
        set -m
        /usr/bin/env -i "$DAEMON" --probe </dev/null &
        pid=$!
        (
            trap 'kill "$timer"; exit 0' TERM
            sleep 8 &
            timer=$!
            wait "$timer" && kill -9 -- "-$pid"
        ) >/dev/null &
        watchdog=$!
        rc=0
        wait "$pid" || rc=$?
        kill "$watchdog" || true
        exit "$rc"
    } 2>/dev/null)" || rc=$?
    LINE="${out%%$'\n'*}"
    LINE="${LINE:0:300}"
    case "$rc" in
        0 | 3 | 4 | 5 | 6) CODE="$rc" ;;
        126 | 127) CODE=2; LINE="cannot run $DAEMON" ;;
        137) CODE=2; LINE="realsense-usbd --probe did not answer" ;;
        *) CODE=2 ;;
    esac
    if [ -z "$LINE" ]; then
        case "$CODE" in
            0) LINE="ok" ;;
            3) LINE="not the user logged in at this Mac's screen" ;;
            4) LINE="camera use is disabled on this Mac by its administrator" ;;
            5) LINE="the installed camera service does not speak this application's protocol version" ;;
            6) LINE="too many programs are connected to the camera service" ;;
            *) LINE="unreachable: the camera service is not installed, switched off, or does not admit this program" ;;
        esac
    fi
}
answer() { echo "$LINE"; exit "$CODE"; }

probe
if [ "$CODE" = 0 ]; then answer; fi
case "$CODE" in 3 | 4 | 6) answer ;; esac
if [ "$CHECK" = 1 ]; then
    if [ ! -e "$INSTALLED_JOB" ]; then
        echo "not installed"
        exit 10
    fi
    answer
fi

# Install (or update, or repair): the command and its quoting are those of RealSenseCameraAccess.installCommand
INSTALLER="$SELF_DIR/install-camera-access.sh"
[ -f "$INSTALLER" ] || fail "install-camera-access.sh is missing next to this script"
[ -n "$MANIFEST_DIR" ] || fail "no service.plist in ${CANDIDATES[*]}"
# POSIX shell single quoting
shell_quote() {
    local s="$1" q="'" r="'\\''"
    s=${s//"$q"/$r}
    printf '%s' "$q$s$q"
}
# AppleScript string literal
applescript_quote() {
    local s="$1" b="\\" q='"'
    s=${s//"$b"/$b$b}
    s=${s//"$q"/$b$q}
    printf '%s' "$q$s$q"
}
COMMAND="/usr/bin/env -i PATH=/usr/bin:/bin:/usr/sbin:/sbin /bin/bash -p $(shell_quote "$INSTALLER")"
COMMAND="$COMMAND --source $(shell_quote "$DAEMON_DIR")"
if [ "$MANIFEST_DIR" != "$DAEMON_DIR" ]; then COMMAND="$COMMAND --source $(shell_quote "$MANIFEST_DIR")"; fi
SCRIPT="do shell script $(applescript_quote "$COMMAND") with administrator privileges with prompt"
SCRIPT="$SCRIPT $(applescript_quote "$PROMPT")"

rc=0
out="$("$OSASCRIPT" -e "$SCRIPT" 2>&1)" || rc=$?
out="${out//$'\r\n'/$'\n'}"
out="${out//$'\r'/$'\n'}"
while [[ "$out" == *$'\n' ]]; do out="${out%$'\n'}"; done
if [ "$rc" != 0 ]; then
    # "<position>: execution error: <message> (<number>)"; -128 is a cancelled prompt, 1..255 the installer's status
    message="$out"
    number=
    re='execution error: (.*) \((-?[0-9]+)\)$'
    if [[ "$out" =~ $re ]]; then
        message="${BASH_REMATCH[1]}"
        number="${BASH_REMATCH[2]}"
    fi
    if [ "$number" = -128 ]; then
        echo "$ME: the administrator prompt was cancelled" >&2
        exit 20
    fi
    echo "${message:-$ME: the installer failed (${number:-$rc})}" >&2
    exit 21
fi
# What the installer said (what it installed or kept)
if [ -n "$out" ]; then echo "$out" >&2; fi

# launchd may still be starting the daemon
deadline=$((SECONDS + 10))
while :; do
    probe
    [ "$CODE" = 2 ] && [ "$SECONDS" -lt "$deadline" ] || break
    sleep 0.5
done
answer
