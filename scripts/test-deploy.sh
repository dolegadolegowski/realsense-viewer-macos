#!/bin/bash
# Tests scripts/deploy-macs.sh and scripts/deploy/remote-install.sh without root, without SSH and without network:
# remote-install.sh runs in its --dry-run --root mode with a local DMG, and the controller runs against a fake
# "ssh" (runs the remote command locally through zsh -c, like sshd with a zsh login shell) and a fake "sudo"
# (checks the password it reads from stdin, then runs the command as the current user). Both record their argv.
# The README's one-line command runs under zsh with a fake "curl" that serves the bundle built from these scripts.
#
#   scripts/test-deploy.sh [dmg] [work dir]
#       dmg: default dist/RealSense-Viewer-2.58.4-macOS-arm64.dmg (must have the pinned sha256)
#       work dir: default a new temporary directory (removed at the end when every test passed)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd -P)"
CTL="$ROOT/scripts/deploy-macs.sh"
RI="$ROOT/scripts/deploy/remote-install.sh"
MB="$ROOT/scripts/deploy/make-bundle.sh"
README="$ROOT/README.md"
DMG="${1:-$ROOT/dist/RealSense-Viewer-2.58.4-macOS-arm64.dmg}"
SHA=2aed32042f8ded5c4a66a9a0c64b356f22579fe98db3af0e3493c680b8020471
LABEL=io.github.dolegadolegowski.realsense-usbd
[ "$(id -u)" != 0 ] || { echo "run as a normal user" >&2; exit 1; }
[ -f "$DMG" ] || { echo "no DMG: $DMG" >&2; exit 1; }

W="${2:-$(mktemp -d)}"
mkdir -p "$W"
W="$(cd "$W" && pwd -P)/deploy-test"
rm -rf "$W"
mkdir -p "$W"

PASS=0
FAIL=0
ok() { PASS=$((PASS + 1)); echo "ok   $*"; }
bad() { FAIL=$((FAIL + 1)); echo "FAIL $*"; }
check() { local name="$1"; shift; if "$@"; then ok "$name"; else bad "$name"; fi; }
# run <log> <cmd...>: status in RC, output in <log>
run() { local log="$1"; shift; RC=0; "$@" >"$log" 2>&1 || RC=$?; }
has() { grep -q -F -- "$2" "$1"; }
hasnt() { ! grep -q -F -- "$2" "$1"; }
empty_dir() { [ -z "$(ls -A "$1" 2>/dev/null)" ]; }
show_on_fail() { [ "$FAIL" = "$1" ] || { echo "---- $2"; tail -25 "$2"; echo "----"; }; }

# ---------------------------------------------------------------------------------------------------------------
echo "# static checks"
for s in "$CTL" "$RI" "$MB" "$0"; do
    check "bash 3.2 -n $(basename "$s")" /bin/bash -n "$s"
done
check "remote-install shebang" test "$(head -1 "$RI")" = "#!/bin/bash -p"
check "remote-install pinned sha256 matches the controller" \
    test "$(sed -n 's/^PINNED_SHA256=//p' "$RI")" = "$(sed -n 's/^PINNED_SHA256=//p' "$CTL")"
check "pinned tag matches" test "$(sed -n 's/^PINNED_TAG=//p' "$RI")" = "$(sed -n 's/^PINNED_TAG=//p' "$CTL")"
check "test DMG has the pinned sha256" test "$(shasum -a 256 "$DMG" | cut -d' ' -f1)" = "$SHA"
BASH4=0
for c in mapfile readarray "declare -A" "wait -n" ",,}" "^^}" "&>>" "|&"; do
    if grep -q -F -- "$c" "$CTL" "$RI"; then BASH4=1; echo "  bash 4 construct: $c"; fi
done
check "no bash 4 constructs (mapfile, declare -A, wait -n, case modification, &>>, |&)" test "$BASH4" = 0
if command -v shellcheck >/dev/null; then
    check "shellcheck" shellcheck -s bash "$CTL" "$RI" "$0"
else
    echo "skip shellcheck (not installed)"
fi

# ---------------------------------------------------------------------------------------------------------------
echo "# remote-install.sh --dry-run"
P="$W/r1"
run "$W/r1.log" /bin/bash "$RI" --dry-run --root "$P" --dmg "$DMG" --tools
check "install exit 0 (got $RC)" test "$RC" = 0
check "install RESULT OK" has "$W/r1.log" "rs-deploy: RESULT: OK"
check "sha256 checked" has "$W/r1.log" "sha256 ok"
check "signatures checked" has "$W/r1.log" "signatures ok"
check "camera access installer ran in its dry-run mode" has "$W/r1.log" "install-camera-access: dry run: installed layout"
for a in "RealSense Viewer.app" "RealSense Depth Quality Tool.app"; do
    check "$a installed" test -d "$P/Applications/$a"
    check "$a copy verifies" codesign --verify --deep --strict "$P/Applications/$a"
    check "$a has no quarantine" test -z "$(xattr -r "$P/Applications/$a" 2>/dev/null | grep com.apple.quarantine || true)"
    check "$a not group/world-writable" test -z "$(find "$P/Applications/$a" -perm +022 2>/dev/null | head -1)"
done
check "RealSense Tools installed" test -x "$P/Applications/RealSense Tools/bin/rs-enumerate-devices"
check "LaunchDaemon plist written under the prefix" test -f "$P/Library/LaunchDaemons/$LABEL.plist"
check "daemon written under the prefix" test -f "$P/Library/PrivilegedHelperTools/$LABEL"
check "no hidden staging copies left" test -z "$(ls -A "$P/Applications" | grep '^\.rs-deploy' || true)"
check "work dir removed" test -z "$(ls -A "$P/private/var/tmp" | grep '^rs-deploy' || true)"
check "lock released" test ! -e "$P/var/run/io.github.dolegadolegowski.rs-deploy.lock"
check "DMG detached" test -z "$(hdiutil info | grep -F "$P" || true)"
show_on_fail 0 "$W/r1.log"

# A closed connection (stdout is a pipe nobody reads any more, SIGPIPE at its default action): the install must still
# run to the end, including camera access, and clean up
F0=$FAIL
P="$W/rp"
set +e
perl -e '$SIG{PIPE}="DEFAULT"; exec @ARGV' /bin/bash "$RI" --dry-run --root "$P" --dmg "$DMG" --tools 2>&1 | head -n 1 >"$W/rp.log"
RC=${PIPESTATUS[0]}
set -e
check "closed output: still exit 0 (got $RC)" test "$RC" = 0
check "closed output: apps installed" test -d "$P/Applications/RealSense Depth Quality Tool.app"
check "closed output: camera access installed to the end" test -f "$P/Library/LaunchDaemons/$LABEL.plist"
check "closed output: lock released" test ! -e "$P/var/run/io.github.dolegadolegowski.rs-deploy.lock"
check "closed output: work dir removed" test -z "$(ls -A "$P/private/var/tmp" | grep '^rs-deploy' || true)"
check "closed output: no staging folder left" test -z "$(ls -A "$P/Applications" | grep '^\.rs-deploy' || true)"
check "closed output: DMG detached" test -z "$(hdiutil info | grep -F "$P" || true)"
show_on_fail "$F0" "$W/rp.log"
P="$W/r1"

F0=$FAIL
mkdir -p "$W/rl/var/run/io.github.dolegadolegowski.rs-deploy.lock"
run "$W/rl.log" /bin/bash "$RI" --dry-run --root "$W/rl" --dmg "$DMG"
check "stale lock: exit 1 (got $RC)" test "$RC" = 1
check "stale lock: names the command that removes it" has "$W/rl.log" "sudo rm -rf '"
check "stale lock: left in place, nothing installed" empty_dir "$W/rl/Applications"
show_on_fail "$F0" "$W/rl.log"

F0=$FAIL
before="$(stat -f %i "$P/Applications/RealSense Viewer.app")"
run "$W/r2.log" /bin/bash "$RI" --dry-run --root "$P" --dmg "$DMG"
check "re-run (update) exit 0 (got $RC)" test "$RC" = 0
check "re-run swapped in a new copy" test "$(stat -f %i "$P/Applications/RealSense Viewer.app")" != "$before"
check "re-run keeps the existing Tools folder updated" has "$W/r2.log" "exists: updating it too"
show_on_fail "$F0" "$W/r2.log"

F0=$FAIL
P="$W/r3"
run "$W/r3.log" /bin/bash "$RI" --dry-run --root "$P" --dmg "$DMG" \
    --sha256 0000000000000000000000000000000000000000000000000000000000000000
check "wrong sha256: exit 1 (got $RC)" test "$RC" = 1
check "wrong sha256: reported" has "$W/r3.log" "RESULT: FAILED on"
check "wrong sha256: mismatch named" has "$W/r3.log" "sha256 mismatch"
check "wrong sha256: nothing installed" empty_dir "$P/Applications"
check "wrong sha256: no camera access" test ! -e "$P/Library/LaunchDaemons/$LABEL.plist"
check "wrong sha256: work dir removed" test -z "$(ls -A "$P/private/var/tmp" | grep '^rs-deploy' || true)"
show_on_fail "$F0" "$W/r3.log"

F0=$FAIL
P="$W/r4"
run "$W/r4.log" /bin/bash "$RI" --dry-run --root "$P" --dmg "$W/does-not-exist.dmg"
check "missing DMG: exit 1 (got $RC)" test "$RC" = 1
check "missing DMG: reported" has "$W/r4.log" "no such file"
check "missing DMG: nothing installed" empty_dir "$P/Applications"
show_on_fail "$F0" "$W/r4.log"

F0=$FAIL
P="$W/r5"
run "$W/r5.log" /bin/bash "$RI" --dry-run --root "$P" --dmg "$DMG" --apps-only
check "--apps-only: exit 0 (got $RC)" test "$RC" = 0
check "--apps-only: apps installed" test -d "$P/Applications/RealSense Viewer.app"
check "--apps-only: no camera access" test ! -e "$P/Library/LaunchDaemons/$LABEL.plist"
check "--apps-only: no Tools unless asked" test ! -e "$P/Applications/RealSense Tools"
show_on_fail "$F0" "$W/r5.log"

F0=$FAIL
P="$W/r6"
mkdir -p "$P/Library/Application Support/RealSense Viewer"
plutil -create xml1 "$P/Library/Application Support/RealSense Viewer/usbd.plist"
plutil -insert BuildNumber -integer 999999999999 "$P/Library/Application Support/RealSense Viewer/usbd.plist"
run "$W/r6.log" /bin/bash "$RI" --dry-run --root "$P" --dmg "$DMG"
check "newer camera access installed: exit 1 (got $RC)" test "$RC" = 1
check "newer camera access installed: apps left alone" empty_dir "$P/Applications"
check "newer camera access installed: reported" has "$W/r6.log" "is newer than"
show_on_fail "$F0" "$W/r6.log"

F0=$FAIL
P="$W/r1"
run "$W/r7.log" /bin/bash "$RI" --dry-run --root "$P" --uninstall
check "uninstall: exit 0 (got $RC)" test "$RC" = 0
check "uninstall: LaunchDaemon plist removed" test ! -e "$P/Library/LaunchDaemons/$LABEL.plist"
check "uninstall: apps kept without --remove-apps" test -d "$P/Applications/RealSense Viewer.app"
run "$W/r8.log" /bin/bash "$RI" --dry-run --root "$P" --uninstall --remove-apps
check "uninstall --remove-apps: exit 0 (got $RC)" test "$RC" = 0
check "uninstall --remove-apps: uninstaller found and run again" has "$W/r8.log" "removing camera access"
check "uninstall --remove-apps: apps and tools removed" empty_dir "$P/Applications"
show_on_fail "$F0" "$W/r8.log"
mkdir -p "$W/r9"
run "$W/r9.log" /bin/bash "$RI" --dry-run --root "$W/r9" --uninstall --remove-apps
check "uninstall where nothing is installed: exit 0 (got $RC)" test "$RC" = 0
check "uninstall where nothing is installed: noted" has "$W/r9.log" "camera access is not installed"

F0=$FAIL
run "$W/u1.log" /bin/bash "$RI" --dmg "$DMG"
check "not root, no --dry-run: refused (exit $RC)" test "$RC" = 1
check "not root: says so" has "$W/u1.log" "run as root"
run "$W/u2.log" /bin/bash "$RI" --dry-run --root "$W/u" --tag v9.9.9
check "--tag without --sha256: refused (exit $RC)" test "$RC" = 1
run "$W/u3.log" /bin/bash "$RI" --dry-run --root "$W/u" --remove-apps
check "--remove-apps without --uninstall: refused (exit $RC)" test "$RC" = 1
run "$W/u4.log" /bin/bash "$RI" --bogus
check "unknown option: exit 2 (got $RC)" test "$RC" = 2
run "$W/u5.log" /bin/bash "$RI" --dry-run --root "$W/u" --asset '../x.dmg' --sha256 "$SHA"
check "bad asset name: refused (exit $RC)" test "$RC" = 1
show_on_fail "$F0" "$W/u5.log"

# ---------------------------------------------------------------------------------------------------------------
echo "# deploy-macs.sh against fake ssh/sudo"
FAKE="$W/fakebin"
REC="$W/rec"
mkdir -p "$FAKE" "$REC"
cat >"$FAKE/ssh" <<'EOF'
#!/bin/bash
# Fake ssh: records argv (NUL-separated), runs the remote command locally like sshd does with a zsh login shell.
printf '%s\0' "$@" >"$FAKE_REC/ssh.$$.argv"
while [ $# -gt 0 ]; do
    case "$1" in
        -[BbcDEeFIiJLlmOoPpQRSWw]) shift 2 ;;
        --) shift; break ;;
        -*) shift ;;
        *) break ;;
    esac
done
dest="$1"
shift
echo "$dest" >>"$FAKE_REC/dests"
case "$dest" in
    *down*) echo "ssh: connect to host ${dest#*@} port 22: Connection refused" >&2; exit 255 ;;
esac
case "$dest" in
    *slow*) sleep 4; : >"$FAKE_REC/late.$$" ;;
esac
cmd="$*"
cmd="${cmd//\/usr\/bin\/sudo/$FAKE_DIR/sudo}"
cmd="${cmd//@HOST@/${dest#*@}}"
exec /bin/zsh -c "$cmd"
EOF
cat >"$FAKE/sudo" <<'EOF'
#!/bin/bash
# Fake sudo: records argv; with -S reads one line of stdin and compares it with FAKE_SUDO_EXPECT (records only
# whether it matched), unless FAKE_SUDO_NOPASS=1 (like a cached or NOPASSWD sudo, reads nothing). Runs the command
# as the current user.
printf '%s\0' "$@" >"$FAKE_REC/sudo.$$.argv"
S=0
while [ $# -gt 0 ]; do
    case "$1" in
        -S) S=1; shift ;;
        -p | -u | -g) shift 2 ;;
        --) shift; break ;;
        -*) shift ;;
        *) break ;;
    esac
done
if [ "$S" = 1 ] && [ "${FAKE_SUDO_NOPASS:-0}" != 1 ]; then
    IFS= read -r pw || pw=
    if [ "$pw" != "$FAKE_SUDO_EXPECT" ]; then
        echo "Sorry, try again." >&2
        echo "sudo: 1 incorrect password attempt" >&2
        exit 1
    fi
    : >"$FAKE_REC/sudo.$$.pwok"
fi
exec "$@"
EOF
cat >"$FAKE/curl" <<'EOF'
#!/bin/bash
# Fake curl: no network; serves FAKE_CURL_FILE for FAKE_CURL_URL (-o <file> <url>), records the URL.
out=
url=
while [ $# -gt 0 ]; do
    case "$1" in
        -o) out="$2"; shift 2 ;;
        --proto | --proto-redir) shift 2 ;;
        -*) shift ;;
        *) url="$1"; shift ;;
    esac
done
echo "$url" >>"$FAKE_REC/curl.urls"
[ "$url" = "$FAKE_CURL_URL" ] || { echo "curl: (22) The requested URL returned error: 404" >&2; exit 22; }
cat "$FAKE_CURL_FILE" >"$out"
EOF
chmod 755 "$FAKE/ssh" "$FAKE/sudo" "$FAKE/curl"
# A test-only value (not a real password) with characters that break naive quoting
TESTPW="t3st-pw 'q' \"d\" \$HOME;x"
export FAKE_REC="$REC" FAKE_DIR="$FAKE" FAKE_SUDO_EXPECT="$TESTPW"
FPATH_="$FAKE:$PATH"
reset_rec() { rm -rf "$REC"; mkdir -p "$REC"; }
argv_has() { # argv_has <glob> <word>: some recorded argv (NUL-separated) has exactly <word>
    local f
    for f in "$REC"/$1; do
        [ -f "$f" ] || continue
        if tr '\0' '\n' <"$f" | grep -x -F -- "$2" >/dev/null; then return 0; fi
    done
    return 1
}
argv_hasnt() { ! argv_has "$@"; }
count() { ls "$REC" 2>/dev/null | grep -c -- "$1" || true; }
pw_nowhere() { # the password is in no recorded argv and in no log
    ! grep -r -q -F -- "$TESTPW" "$REC" "$@" 2>/dev/null
}
# DMG path with a space and a quote, root path with a space: exercises the quoting through ssh + zsh + sudo + bash -c
QDMG="$W/it's a.dmg"
ln "$DMG" "$QDMG" 2>/dev/null || cp "$DMG" "$QDMG"
CROOT="$W/c root"
printf '%s\n' '# lab Macs' '' '  hostC   # third' 'carol@hostD' >"$W/hosts.txt"

F0=$FAIL
reset_rec
RC=0
mkdir -p "$W/logs1"
chmod 755 "$W/logs1"
printf '%s\n' "$TESTPW" | PATH="$FPATH_" "$CTL" --ask-pass -l adminx --dmg "$QDMG" --dry-run --root "$CROOT/@HOST@" \
    --log-dir "$W/logs1" hostA bob@hostB -f "$W/hosts.txt" >"$W/c1.log" 2>&1 || RC=$?
check "controller --ask-pass, 4 hosts: exit 0 (got $RC)" test "$RC" = 0
check "hosts from args, -f file and -l login" \
    test "$(tr '\n' ' ' <"$REC/dests")" = "adminx@hostA bob@hostB adminx@hostC carol@hostD "
check "summary: 4 of 4 OK" has "$W/c1.log" "4 of 4 host(s) OK"
check "summary: per-host OK line" has "$W/c1.log" "OK      carol@hostD  "
check "sudo got the password on stdin on every host" test "$(count '\.pwok$')" = 4
check "sudo called with -S" argv_has 'sudo.*.argv' -S
check "ssh called with -T (no tty)" argv_has 'ssh.*.argv' -T
check "ssh not called with -t" argv_hasnt 'ssh.*.argv' -t
check "password in no argv and no log" pw_nowhere "$W/logs1" "$W/c1.log"
L1="$(ls -d "$W"/logs1/rs-deploy.* 2>/dev/null | head -1)"
check "per-host logs written to a new folder inside --log-dir" test "$(ls "$L1"/*.log 2>/dev/null | wc -l | tr -d ' ')" = 4
check "that log folder is private" test "$(stat -f %Lp "$L1")" = 700
check "--log-dir itself not chmod-ed" test "$(stat -f %Lp "$W/logs1")" = 755
for h in hostA hostB hostC hostD; do
    check "$h: apps installed under its root" test -d "$CROOT/$h/Applications/RealSense Viewer.app"
    check "$h: camera access installed (dry run)" test -f "$CROOT/$h/Library/LaunchDaemons/$LABEL.plist"
done
show_on_fail "$F0" "$W/c1.log"

F0=$FAIL
reset_rec
RC=0
printf '%s\n' "$TESTPW" | PATH="$FPATH_" "$CTL" --ask-pass --apps-only --dmg "$QDMG" --dry-run --root "$CROOT/@HOST@" \
    --log-dir "$W/logs2" admin@hostE admin@down.example >"$W/c2.log" 2>&1 || RC=$?
check "one unreachable host: exit 1 (got $RC)" test "$RC" = 1
check "unreachable host: FAILED with ssh's error" has "$W/c2.log" "FAILED  admin@down.example  (exit 255) ssh failed: ssh: connect to host"
check "other host still OK" has "$W/c2.log" "OK      admin@hostE"
check "summary: 1 of 2" has "$W/c2.log" "1 of 2 host(s) OK"
show_on_fail "$F0" "$W/c2.log"

F0=$FAIL
reset_rec
RC=0
printf '%s\n' "wrong-$TESTPW" | PATH="$FPATH_" "$CTL" --ask-pass -j 2 --apps-only --dmg "$QDMG" --dry-run \
    --root "$CROOT/@HOST@" --log-dir "$W/logs3" admin@hostF admin@hostF2 admin@hostF3 >"$W/c3.log" 2>&1 || RC=$?
check "wrong sudo password: exit 1 (got $RC)" test "$RC" = 1
check "wrong sudo password: FAILED with sudo's message" has "$W/c3.log" "FAILED  admin@hostF  (exit 1) sudo: 1 incorrect password attempt"
check "wrong sudo password: nothing installed" test ! -e "$CROOT/hostF/Applications/RealSense Viewer.app"
check "wrong sudo password: only the first host contacted (also with -j)" test "$(tr '\n' ' ' <"$REC/dests")" = "admin@hostF "
check "wrong sudo password: the others SKIPPED" has "$W/c3.log" "SKIPPED admin@hostF3  not attempted"
show_on_fail "$F0" "$W/c3.log"

F0=$FAIL
reset_rec
RC=0
printf '%s\n' "$TESTPW" | FAKE_SUDO_NOPASS=1 PATH="$FPATH_" "$CTL" --ask-pass --apps-only --dmg "$QDMG" --dry-run \
    --root "$CROOT/@HOST@" --log-dir "$W/logs4" admin@hostG >"$W/c4.log" 2>&1 || RC=$?
check "sudo without a password prompt (unread password on stdin): exit 0 (got $RC)" test "$RC" = 0
check "unread password not consumed or echoed by anything" pw_nowhere "$W/logs4" "$W/c4.log"
show_on_fail "$F0" "$W/c4.log"

F0=$FAIL
reset_rec
RC=0
start=$(date +%s)
printf '%s\n' "$TESTPW" | PATH="$FPATH_" "$CTL" --ask-pass -j 3 -l adminx --dmg "$QDMG" --dry-run \
    --root "$CROOT/@HOST@" --log-dir "$W/logs5" hostP1 hostP2 hostP3 >"$W/c5.log" 2>&1 || RC=$?
check "parallel -j 3: exit 0 (got $RC, $(($(date +%s) - start)) s)" test "$RC" = 0
check "parallel: 3 of 3 OK" has "$W/c5.log" "3 of 3 host(s) OK"
check "parallel: ssh BatchMode=yes" argv_has 'ssh.*.argv' BatchMode=yes
check "parallel: password fed to every sudo" test "$(count '\.pwok$')" = 3
check "parallel: password in no argv and no log" pw_nowhere "$W/logs5" "$W/c5.log"
show_on_fail "$F0" "$W/c5.log"

F0=$FAIL
reset_rec
RC=0
PATH="$FPATH_" "$CTL" --apps-only --dmg "$QDMG" --dry-run --root "$CROOT/@HOST@" --log-dir "$W/logs6" \
    --ssh-opt Port=2222 admin@hostT </dev/null >"$W/c6.log" 2>&1 || RC=$?
check "without --ask-pass: exit 0 (got $RC)" test "$RC" = 0
check "without --ask-pass: ssh -t" argv_has 'ssh.*.argv' -t
check "without --ask-pass: sudo without -S" argv_hasnt 'sudo.*.argv' -S
check "--ssh-opt passed as -o" argv_has 'ssh.*.argv' Port=2222
check "warns that sudo cannot prompt without a terminal" has "$W/c6.log" "stdin is not a terminal"
show_on_fail "$F0" "$W/c6.log"

F0=$FAIL
reset_rec
RC=0
printf '%s\n' "$TESTPW" | PATH="$FPATH_" "$CTL" --ask-pass --dmg "$QDMG" --dry-run --root "$CROOT/@HOST@" \
    --sha256 1111111111111111111111111111111111111111111111111111111111111111 \
    --log-dir "$W/logs7" admin@hostS >"$W/c7.log" 2>&1 || RC=$?
check "controller, wrong sha256: exit 1 (got $RC)" test "$RC" = 1
check "controller, wrong sha256: summary names it" has "$W/c7.log" "sha256 mismatch"
check "controller, wrong sha256: nothing installed" empty_dir "$CROOT/hostS/Applications"
show_on_fail "$F0" "$W/c7.log"

F0=$FAIL
reset_rec
RC=0
printf '%s\n' "$TESTPW" | PATH="$FPATH_" "$CTL" --ask-pass --uninstall --remove-apps --dry-run --root "$CROOT/@HOST@" \
    --log-dir "$W/logs8" adminx@hostA >"$W/c8.log" 2>&1 || RC=$?
check "controller --uninstall --remove-apps: exit 0 (got $RC)" test "$RC" = 0
check "uninstall: camera access removed" test ! -e "$CROOT/hostA/Library/LaunchDaemons/$LABEL.plist"
check "uninstall: apps removed" empty_dir "$CROOT/hostA/Applications"
show_on_fail "$F0" "$W/c8.log"

F0=$FAIL
reset_rec
expect2() { # expect2 <name> <args...>: usage error, exit 2, ssh never called
    local name="$1"
    shift
    RC=0
    PATH="$FPATH_" "$CTL" "$@" </dev/null >"$W/e.log" 2>&1 || RC=$?
    check "$name: exit 2 (got $RC)" test "$RC" = 2
    check "$name: no ssh" test ! -e "$REC/dests"
}
expect2 "no hosts"
expect2 "-j without --ask-pass" -j 2 hostA hostB
expect2 "--remove-apps without --uninstall" --remove-apps hostA
expect2 "--uninstall with --tools" --uninstall --tools hostA
expect2 "--tag without --sha256" --tag v9.9.9 hostA
expect2 "option-looking host" -oProxyCommand=x
expect2 "host with a shell metacharacter" 'host;touch x'
expect2 "host after -- that starts with -" -- -oProxyCommand=x
printf '%s\n' 'hostA hostB' >"$W/bad-hosts.txt"
expect2 "two hosts on one line" -f "$W/bad-hosts.txt"
expect2 "bad login" -l 'a b' hostA
expect2 "missing hosts file" -f "$W/nope.txt"
show_on_fail "$F0" "$W/e.log"

F0=$FAIL
reset_rec
# Interrupting a parallel run stops the ssh sessions too (the slow hosts' fake ssh would write late.* after 4 s)
printf '%s\n' "$TESTPW" | PATH="$FPATH_" "$CTL" --ask-pass -j 2 --uninstall --dry-run --root "$CROOT/@HOST@" \
    --log-dir "$W/logs9" adminx@fastA adminx@slowB adminx@slowC >"$W/c9.log" 2>&1 &
CPID=$!
n=0
while [ "$n" -lt 100 ] && [ "$(wc -l <"$REC/dests" 2>/dev/null | tr -d ' ' || echo 0)" != 3 ]; do
    sleep 0.2
    n=$((n + 1))
done
sleep 0.5
kill -TERM "$CPID" 2>/dev/null || true
RC=0
wait "$CPID" || RC=$?
sleep 5
check "interrupted -j run: exit 130 (got $RC)" test "$RC" = 130
check "interrupted -j run: says the targets finish on their own" has "$W/c9.log" "finishes (or rolls back) on its own"
check "interrupted -j run: no ssh session kept running" test "$(count '^late\.')" = 0
show_on_fail "$F0" "$W/c9.log"

# ---------------------------------------------------------------------------------------------------------------
echo "# deployment bundle and the README one-line command"
F0=$FAIL
"$MB" "$W/b1.tgz" >"$W/b1.out"
sleep 1
"$MB" "$W/b2.tgz" >"$W/b2.out"
BSHA="$(shasum -a 256 "$W/b1.tgz" | cut -d' ' -f1)"
check "bundle is reproducible" test "$BSHA" = "$(shasum -a 256 "$W/b2.tgz" | cut -d' ' -f1)"
check "make-bundle prints the hash" has "$W/b1.out" "$BSHA"
mkdir -p "$W/bx"
tar -xzf "$W/b1.tgz" -C "$W/bx"
check "bundle: deploy-macs.sh identical and executable" cmp -s "$CTL" "$W/bx/rs-deploy/deploy-macs.sh"
check "bundle: remote-install.sh identical" cmp -s "$RI" "$W/bx/rs-deploy/deploy/remote-install.sh"
check "bundle: scripts executable" test -x "$W/bx/rs-deploy/deploy-macs.sh" -a -x "$W/bx/rs-deploy/deploy/remote-install.sh"
check "bundle: nothing else in it" test "$(tar -tzf "$W/b1.tgz" | tr '\n' ' ')" = "rs-deploy/ rs-deploy/deploy-macs.sh rs-deploy/deploy/ rs-deploy/deploy/remote-install.sh "
check "README pins this bundle's sha256 (English and Polish command)" test "$(grep -o -F -- "$BSHA" "$README" | wc -l | tr -d ' ')" = 2
check "README has no placeholder left" hasnt "$README" "@BUNDLE_SHA@"
show_on_fail "$F0" "$W/b1.out"

F0=$FAIL
TAGV="$(sed -n 's/^PINNED_TAG=//p' "$CTL")"
LINE="$(grep -m1 '^d="$(mktemp -d)" && curl ' "$README" || true)"
check "README has the one-line command" test -n "$LINE"
check "one-liner downloads the bundle of the pinned tag" has "$README" "releases/download/$TAGV/rs-deploy-$TAGV.tar.gz"
mkdir -p "$W/tmp1"
reset_rec
RC=0
printf '%s\n' "$TESTPW" | TMPDIR="$W/tmp1" PATH="$FPATH_" FAKE_CURL_FILE="$W/b1.tgz" \
    FAKE_CURL_URL="https://github.com/dolegadolegowski/realsense-viewer-macos/releases/download/$TAGV/rs-deploy-$TAGV.tar.gz" \
    /bin/zsh -f -c "$LINE" >"$W/l1.log" 2>&1 || RC=$?
check "one-liner under zsh: hash checked" has "$W/l1.log" "rs-deploy.tar.gz: OK"
check "one-liner: deploy-macs.sh ran for both example hosts" test "$(tr '\n' ' ' <"$REC/dests" 2>/dev/null)" = "admin@mac-01.local admin@mac-02.local "
check "one-liner: remote-install.sh reached the targets (refuses: fake sudo is not root)" has "$W/l1.log" "run as root"
check "one-liner: exit 1 (got $RC)" test "$RC" = 1
check "one-liner: password in no argv and no log" pw_nowhere "$W/l1.log" "$W/tmp1"
show_on_fail "$F0" "$W/l1.log"
F0=$FAIL
reset_rec
printf 'not the bundle\n' >"$W/evil.tgz"
RC=0
printf '%s\n' "$TESTPW" | TMPDIR="$W/tmp1" PATH="$FPATH_" FAKE_CURL_FILE="$W/evil.tgz" \
    FAKE_CURL_URL="https://github.com/dolegadolegowski/realsense-viewer-macos/releases/download/$TAGV/rs-deploy-$TAGV.tar.gz" \
    /bin/zsh -f -c "$LINE" >"$W/l2.log" 2>&1 || RC=$?
check "one-liner, tampered bundle: fails (got $RC)" test "$RC" != 0
check "one-liner, tampered bundle: hash mismatch reported" has "$W/l2.log" "FAILED"
check "one-liner, tampered bundle: nothing run" test ! -e "$REC/dests"
show_on_fail "$F0" "$W/l2.log"

check "no disk image of the tests left attached" test -z "$(hdiutil info | grep -F "$W" || true)"
check "test password written nowhere under the work dir" pw_nowhere "$W"

echo
echo "$PASS passed, $FAIL failed"
if [ "$FAIL" = 0 ] && [ -z "${2:-}" ]; then rm -rf "$W"; fi
[ "$FAIL" = 0 ]
