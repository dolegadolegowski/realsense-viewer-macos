// License: Apache 2.0. See LICENSE file in root directory.
//
// Launcher for the RealSense tools bundled as macOS apps.
//
// On macOS, RealSense cameras are accessed through libusb, which has to capture the USB device from the
// system UVC/HID drivers - and macOS only allows that for root. With camera access installed (the root
// LaunchDaemon realsense-usbd, macos/broker/DESIGN.md) the tool talks to the camera through it: the launcher
// checks that the daemon admits this build (`realsense-usbd --probe`) and becomes the real tool
// (Contents/MacOS/<RSLaunchTarget>) as the current user, without any password.
//
// Otherwise it offers to install or update camera access (an administrator's name and password, once), or to
// run the tool once as root as before: the standard macOS authorization dialog, the tool runs as root in the
// user's session, the camera is handed back to macOS when it exits and the files it created in the user's
// home folder are given back to the user.

import AppKit
import ServiceManagement

let bundle = Bundle.main
// Resolved once: every path below, the trust check and the root commands use the same symlink-free path
let bundleURL: URL = {
    guard let resolved = realpath(bundle.bundleURL.path, nil) else { return bundle.bundleURL }
    defer { free(resolved) }
    return URL(fileURLWithPath: String(cString: resolved), isDirectory: true)
}()
let macosDir = bundleURL.appendingPathComponent("Contents/MacOS")
let brokerResources = bundleURL.appendingPathComponent("Contents/Resources/realsense-usbd")
let appName = bundle.object(forInfoDictionaryKey: "CFBundleName") as? String ?? "RealSense Viewer"
let targetName = bundle.object(forInfoDictionaryKey: "RSLaunchTarget") as? String ?? "realsense-viewer"
let target = macosDir.appendingPathComponent(targetName).path
let releaseTool = macosDir.appendingPathComponent("rs-macos-release").path
let handbackTool = macosDir.appendingPathComponent("rs-macos-handback").path
let daemonTool = macosDir.appendingPathComponent("realsense-usbd").path
let installScript = brokerResources.appendingPathComponent("install-camera-access.sh").path
let listToolScript = brokerResources.appendingPathComponent("camera-access-allowlist.sh").path   // the installer needs it
let bundledAllowlist = brokerResources.appendingPathComponent("allowlist.plist").path
let daemonLabel = "io.github.dolegadolegowski.realsense-usbd"
let installedDaemonPlist = "/Library/LaunchDaemons/\(daemonLabel).plist"
let installedConfig = "/Library/Application Support/RealSense Viewer/usbd.plist"
let cleanEnv = "/usr/bin/env -i PATH=/usr/bin:/bin:/usr/sbin:/sbin"
let passthroughArgs = Array(CommandLine.arguments.dropFirst()).filter { !$0.hasPrefix("-psn_") }

func execTarget() -> Never {
    let argv = ([target] + passthroughArgs).map { strdup($0) } + [nil]
    execv(target, argv)
    perror("execv")
    exit(1)
}

// Already privileged (e.g. started with sudo from Terminal): just become the tool.
if geteuid() == 0 {
    execTarget()
}

func shellQuote(_ s: String) -> String {
    return "'" + s.replacingOccurrences(of: "'", with: "'\\''") + "'"
}

func appleScriptQuote(_ s: String) -> String {
    return "\"" + s.replacingOccurrences(of: "\\", with: "\\\\").replacingOccurrences(of: "\"", with: "\\\"") + "\""
}

// The do shell script commands (DESIGN 7.4, 7.5): a clean environment, every path quoted for the shell and then
// for AppleScript.
func installCommand() -> String {
    return "\(cleanEnv) /bin/bash -p \(shellQuote(installScript)) --source \(shellQuote(macosDir.path))"
        + " --source \(shellQuote(brokerResources.path))"
}

func runOnceCommand(uid: uid_t, gid: gid_t, home: String, user: String, stamp: String) -> String {
    let args = passthroughArgs.map(shellQuote).joined(separator: " ")
    let script = """
        export HOME=\(shellQuote(home)) USER=\(shellQuote(user)) LOGNAME=\(shellQuote(user)) \
        SUDO_USER=\(shellQuote(user)) SUDO_UID=\(uid) SUDO_GID=\(gid)
        cd "$HOME"
        \(shellQuote(target)) \(args) >/dev/null 2>&1
        status=$?
        \(shellQuote(releaseTool)) >/dev/null 2>&1
        \(shellQuote(handbackTool)) \(uid) \(gid) \(shellQuote(stamp)) "$HOME/.realsense-config.json" "$HOME/Documents" 2>/dev/null
        exit $status
        """
    return "\(cleanEnv) /bin/sh -c \(shellQuote(script))"
}

func asAdministrator(_ command: String, prompt: String) -> String {
    return "do shell script \(appleScriptQuote(command)) with administrator privileges with prompt "
        + appleScriptQuote(prompt)
}

// `realsense-usbd --probe`: 0 OK, 2 unreachable/rejected, 3 not the console user, 4 camera disabled by policy,
// 5 protocol version, 6 too many clients (DESIGN 2.1.3). Not runnable or no answer within 8 s counts as 2.
struct Probe {
    let code: Int32
    let line: String
}

final class OutputBox: @unchecked Sendable {
    var data = Data()
}

func runProbe() -> Probe {
    var fds: [Int32] = [-1, -1]
    guard pipe(&fds) == 0 else { return Probe(code: 2, line: "pipe failed") }
    var actions: posix_spawn_file_actions_t?
    posix_spawn_file_actions_init(&actions)
    defer { posix_spawn_file_actions_destroy(&actions) }
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0)
    posix_spawn_file_actions_adddup2(&actions, fds[1], 1)
    posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0)
    posix_spawn_file_actions_addclose(&actions, fds[0])
    posix_spawn_file_actions_addclose(&actions, fds[1])
    let argv = [daemonTool, "--probe"].map { strdup($0) } + [nil]
    defer { argv.forEach { free($0) } }
    var pid: pid_t = 0
    let rc = posix_spawn(&pid, daemonTool, &actions, nil, argv, environ)
    close(fds[1])
    if rc != 0 {
        close(fds[0])
        return Probe(code: 2, line: "cannot run realsense-usbd: \(String(cString: strerror(rc)))")
    }

    let readFD = fds[0]
    let output = OutputBox()
    let readDone = DispatchSemaphore(value: 0)
    Thread.detachNewThread {
        var buffer = [UInt8](repeating: 0, count: 1024)
        while output.data.count < 4096 {
            let n = read(readFD, &buffer, buffer.count)
            if n > 0 {
                output.data.append(buffer, count: n)
            } else if n == 0 || errno != EINTR {
                break
            }
        }
        readDone.signal()
    }

    var status: Int32 = 0
    var exited = false
    let deadline = Date().addingTimeInterval(8)
    while Date() < deadline {
        let r = waitpid(pid, &status, WNOHANG)
        if r == pid { exited = true; break }
        if r < 0 && errno != EINTR { break }
        usleep(20_000)
    }
    if !exited {
        kill(pid, SIGKILL)
        while waitpid(pid, &status, 0) < 0 && errno == EINTR {}
    }
    let gotOutput = readDone.wait(timeout: .now() + 1) == .success
    if gotOutput { close(readFD) }   // otherwise the reader thread still owns it; leak the descriptor
    let line = String(decoding: output.data, as: UTF8.self)
        .split(separator: "\n").first.map(String.init) ?? ""
    guard exited else { return Probe(code: 2, line: "realsense-usbd --probe did not answer") }
    let signaled = (status & 0x7f) != 0
    return Probe(code: signaled ? 2 : (status >> 8) & 0xff, line: line)
}

// Trust rule of DESIGN 6.2: every component owned by root or an administrator, not world-writable, group-writable
// only for admin/wheel, no ACL entry that allows changing it; files regular with one link; not a translocated copy.
func isRootOrAdmin(_ uid: uid_t) -> Bool {
    if uid == 0 { return true }
    guard let pw = getpwuid(uid) else { return false }
    var groups = [Int32](repeating: 0, count: 256)
    var count = Int32(groups.count)
    guard getgrouplist(pw.pointee.pw_name, Int32(bitPattern: pw.pointee.pw_gid), &groups, &count) >= 0 else { return false }
    return groups.prefix(Int(count)).contains(80 /* admin */)
}

func aclAllowsWrite(_ path: String) -> Bool {
    guard let acl = acl_get_link_np(path, ACL_TYPE_EXTENDED) else { return errno != ENOENT && errno != EOPNOTSUPP }
    defer { acl_free(UnsafeMutableRawPointer(acl)) }
    let writes: [acl_perm_t] = [ACL_WRITE_DATA, ACL_APPEND_DATA, ACL_DELETE, ACL_DELETE_CHILD, ACL_WRITE_ATTRIBUTES,
                                ACL_WRITE_EXTATTRIBUTES, ACL_WRITE_SECURITY, ACL_CHANGE_OWNER]
    var entry: acl_entry_t?
    var id = ACL_FIRST_ENTRY.rawValue
    while acl_get_entry(acl, id, &entry) == 0, let e = entry {
        id = ACL_NEXT_ENTRY.rawValue
        var tag = ACL_UNDEFINED_TAG
        var perms: acl_permset_t?
        guard acl_get_tag_type(e, &tag) == 0, acl_get_permset(e, &perms) == 0, let p = perms else { return true }
        if tag == ACL_EXTENDED_ALLOW && writes.contains(where: { acl_get_perm_np(p, $0) == 1 }) { return true }
    }
    return false
}

func isTrusted(_ path: String, file: Bool) -> Bool {
    var st = stat()
    if file {
        guard lstat(path, &st) == 0, (st.st_mode & S_IFMT) == S_IFREG, st.st_nlink == 1 else { return false }
    }
    guard let resolved = realpath(path, nil) else { return false }
    var cur = String(cString: resolved)
    free(resolved)
    if cur.contains("/AppTranslocation/") { return false }
    while true {
        guard lstat(cur, &st) == 0, isRootOrAdmin(st.st_uid) else { return false }
        if st.st_mode & S_IWOTH != 0 { return false }
        if st.st_mode & S_IWGRP != 0 && st.st_gid != 0 && st.st_gid != 80 { return false }
        if aclAllowsWrite(cur) { return false }
        if cur == "/" { break }
        cur = (cur as NSString).deletingLastPathComponent
        if cur.isEmpty { cur = "/" }
    }
    return true
}

func bundleIsTrusted() -> Bool {
    return isTrusted(bundleURL.path, file: false) && isTrusted(daemonTool, file: true)
        && isTrusted(installScript, file: true) && isTrusted(listToolScript, file: true)
        && isTrusted(bundledAllowlist, file: true)
}

struct BuildInfo {
    let id: String?
    let number: Int?
    init?(path: String) {
        guard let dict = NSDictionary(contentsOfFile: path) else { return nil }
        id = dict["BuildID"] as? String
        number = (dict["BuildNumber"] as? NSNumber)?.intValue
    }
}

enum Situation {
    case cameraDisabled, tooManyClients, notConsoleUser, noBroker, notInstalled, switchedOff, daemonNewer,
         needsUpdate, needsReinstall
}

func situation(for probe: Probe) -> Situation {
    switch probe.code {
    case 4: return .cameraDisabled
    case 6: return .tooManyClients
    case 3: return .notConsoleUser
    default: break
    }
    guard FileManager.default.isExecutableFile(atPath: daemonTool),
          FileManager.default.fileExists(atPath: installScript),
          let bundled = BuildInfo(path: bundledAllowlist) else { return .noBroker }
    guard FileManager.default.fileExists(atPath: installedDaemonPlist) else { return .notInstalled }
    if SMAppService.statusForLegacyPlist(at: URL(fileURLWithPath: installedDaemonPlist)) == .requiresApproval {
        return .switchedOff
    }
    let installed = BuildInfo(path: installedConfig)
    if let i = installed?.number, let b = bundled.number, i > b { return .daemonNewer }
    if installed?.id == nil || installed?.id != bundled.id || probe.code == 5 { return .needsUpdate }
    return .needsReinstall
}

// The probe only shows that this bundle's realsense-usbd is admitted, which stays true when a newer build changed only
// the tools: the installed allowlist must also be this build's, or the tool itself would be turned away.
func installedIsThisBuild() -> Bool {
    guard let bundled = BuildInfo(path: bundledAllowlist)?.id else { return false }
    return BuildInfo(path: installedConfig)?.id == bundled
}

// Decide before AppKit starts: when the daemon admits this build, become the tool right away.
var firstProbe = runProbe()
if firstProbe.code == 0 && installedIsThisBuild() {
    execTarget()
}
if firstProbe.code == 2 && situation(for: firstProbe) == .needsReinstall {
    // Same build installed but not answering: launchd may still be starting it.
    sleep(2)
    firstProbe = runProbe()
    if firstProbe.code == 0 && installedIsThisBuild() {
        execTarget()
    }
}

final class Launcher: NSObject, NSApplicationDelegate {
    private var finished = false
    private var probe: Probe

    init(probe: Probe) {
        self.probe = probe
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApp.activate()
        ask()
    }

    private func ask() {
        let state = situation(for: probe)
        let trusted = bundleIsTrusted()
        let moveHint = "Move \(appName) to /Applications (an administrator must do this), then open it again."
        let alert = NSAlert()
        alert.messageText = appName
        var install: String?
        var runOnce = true
        var loginItems = false
        switch state {
        case .cameraDisabled:
            alert.informativeText = "Camera use is disabled on this Mac by its administrator."
            runOnce = false
        case .tooManyClients:
            alert.informativeText = "Too many RealSense programs are running."
            runOnce = false
        case .notConsoleUser:
            alert.informativeText = "Only the user logged in at this Mac's screen can use the camera."
        case .noBroker:
            alert.informativeText = "\(appName) needs administrator access to control the RealSense camera over USB."
        case .notInstalled:
            alert.informativeText = "Camera access lets every user of this Mac use RealSense cameras without a "
                + "password. Installing it once needs an administrator's name and password."
            install = "Install camera access for all users"
        case .switchedOff:
            alert.informativeText = "Camera access is switched off in System Settings > General > Login Items & "
                + "Extensions."
            loginItems = true
        case .daemonNewer:
            alert.informativeText = "Camera access is newer than this app; install the app from the same DMG."
        case .needsUpdate:
            alert.informativeText = "Camera access installed on this Mac belongs to another version of \(appName)."
            install = "Update camera access"
        case .needsReinstall:
            alert.informativeText = "Camera access is installed but does not answer"
                + (probe.line.isEmpty ? "." : " (\(probe.line)).")
            install = "Reinstall camera access"
        }
        if install != nil && !trusted {
            alert.informativeText += "\n\n" + moveHint
            install = nil
        }
        var actions: [() -> Void] = []
        if let title = install {
            alert.addButton(withTitle: title)
            actions.append { self.installCameraAccess() }
        }
        if loginItems {
            alert.addButton(withTitle: "Open Login Items settings")
            actions.append {
                SMAppService.openSystemSettingsLoginItems()
                NSApp.terminate(nil)
            }
        }
        if runOnce {
            alert.addButton(withTitle: "Run once as administrator")
            actions.append { self.runOnceAsAdministrator() }
        }
        alert.addButton(withTitle: "Cancel").keyEquivalent = "\u{1b}"
        actions.append { NSApp.terminate(nil) }
        let index = alert.runModal().rawValue - NSApplication.ModalResponse.alertFirstButtonReturn.rawValue
        (index >= 0 && index < actions.count ? actions[index] : actions[actions.count - 1])()
    }

    // Admin credentials once; then a new instance probes and becomes the tool.
    private func installCameraAccess() {
        let source = asAdministrator(installCommand(),
                                     prompt: "\(appName) wants to install camera access for all users of this Mac.")
        DispatchQueue.global(qos: .userInitiated).async {
            var error: NSDictionary?
            NSAppleScript(source: source)?.executeAndReturnError(&error)
            let after = error == nil ? Probe(code: 0, line: "") : runProbe()
            DispatchQueue.main.async { self.installFinished(error, after) }
        }
    }

    private func installFinished(_ error: NSDictionary?, _ after: Probe) {
        guard let error = error else {
            let configuration = NSWorkspace.OpenConfiguration()
            configuration.createsNewApplicationInstance = true
            configuration.arguments = passthroughArgs
            NSWorkspace.shared.openApplication(at: bundleURL, configuration: configuration) { _, openError in
                DispatchQueue.main.async {
                    if let openError = openError {
                        let alert = NSAlert()
                        alert.messageText = "\(appName) could not be started"
                        alert.informativeText = openError.localizedDescription
                        alert.runModal()
                    }
                    NSApp.terminate(nil)
                }
            }
            return
        }
        if (error[NSAppleScript.errorNumber] as? Int) != -128 /* user cancelled */ {
            let alert = NSAlert()
            alert.messageText = "Camera access could not be installed"
            alert.informativeText = error[NSAppleScript.errorMessage] as? String ?? "\(error)"
            alert.runModal()
        }
        probe = after
        ask()
    }

    // Runs the tool as root. Settings and the files the tool saved to its default location (Documents) are handed
    // back to the user afterwards.
    private func runOnceAsAdministrator() {
        let uid = getuid(), gid = getgid()
        let home = NSHomeDirectory(), user = NSUserName()
        let stamp = NSTemporaryDirectory() + "rs-launch-\(getpid())"
        FileManager.default.createFile(atPath: stamp, contents: nil)

        let source = asAdministrator(runOnceCommand(uid: uid, gid: gid, home: home, user: user, stamp: stamp),
            prompt: "\(appName) needs administrator access to control the RealSense camera over USB.")

        DispatchQueue.global(qos: .userInitiated).async {
            var error: NSDictionary?
            NSAppleScript(source: source)?.executeAndReturnError(&error)
            try? FileManager.default.removeItem(atPath: stamp)
            DispatchQueue.main.async { self.toolFinished(error) }
        }
        pollForTool()
    }

    // The tool is not started by LaunchServices, so hand it the activation once its window is up.
    private func pollForTool() {
        guard !finished else { return }
        let me = NSRunningApplication.current
        if let tool = runningTool() {
            NSApp.yieldActivation(to: tool)
            tool.activate(from: me, options: [])
            return
        }
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.25) { self.pollForTool() }
    }

    private func toolFinished(_ error: NSDictionary?) {
        finished = true
        if let error = error, (error[NSAppleScript.errorNumber] as? Int) != -128 /* user cancelled */ {
            let message = error[NSAppleScript.errorMessage] as? String ?? "\(error)"
            // A non-zero exit status of the tool is reported as error 1..255; only show real launch failures.
            if let code = error[NSAppleScript.errorNumber] as? Int, code > 0 && code < 256 {
                NSLog("\(targetName) exited with status \(code): \(message)")
            } else {
                let alert = NSAlert()
                alert.messageText = "\(appName) could not be started"
                alert.informativeText = message
                alert.runModal()
            }
        }
        NSApp.terminate(nil)
    }

    private func runningTool() -> NSRunningApplication? {
        return NSWorkspace.shared.runningApplications.first {
            $0.executableURL?.resolvingSymlinksInPath().path == URL(fileURLWithPath: target).resolvingSymlinksInPath().path
        }
    }

    func applicationShouldHandleReopen(_ sender: NSApplication, hasVisibleWindows flag: Bool) -> Bool {
        if let tool = runningTool() {
            NSApp.yieldActivation(to: tool)
            tool.activate(from: NSRunningApplication.current, options: [])
        }
        return false
    }
}

let app = NSApplication.shared
let delegate = Launcher(probe: firstProbe)
app.delegate = delegate
app.setActivationPolicy(.accessory)
app.run()
