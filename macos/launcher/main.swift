// License: Apache 2.0. See LICENSE file in root directory.
//
// Launcher for the RealSense tools bundled as macOS apps.
//
// On macOS, RealSense cameras are accessed through libusb, which has to capture the USB device from the
// system UVC/HID drivers - and macOS only allows that for root. The RealSense camera service (the root
// LaunchDaemon realsense-usbd, macos/broker/README.md) does that for every RealSense application of the Mac. The
// launcher asks RealSenseCameraAccess (macos/broker/client, compiled into this program) whether the service
// answers, and then becomes the real tool (Contents/MacOS/<RSLaunchTarget>) as the current user, without any
// password.
//
// When the service is not installed, too old or not answering, the launcher installs it from this bundle: the
// macOS administrator prompt, once per Mac. Where that does not help or was cancelled it offers to run the tool
// once as root as before: the standard macOS authorization dialog, the tool runs as root in the user's session,
// the camera is handed back to macOS when it exits and the files it created in the user's home folder are given
// back to the user.

import AppKit

let bundle = Bundle.main
// Resolved once: every path below and the root commands use the same symlink-free path
let bundleURL: URL = {
    guard let resolved = realpath(bundle.bundleURL.path, nil) else { return bundle.bundleURL }
    defer { free(resolved) }
    return URL(fileURLWithPath: String(cString: resolved), isDirectory: true)
}()
let macosDir = bundleURL.appendingPathComponent("Contents/MacOS")
let appName = bundle.object(forInfoDictionaryKey: "CFBundleName") as? String ?? "RealSense Viewer"
let targetName = bundle.object(forInfoDictionaryKey: "RSLaunchTarget") as? String ?? "realsense-viewer"
let target = macosDir.appendingPathComponent(targetName).path
let releaseTool = macosDir.appendingPathComponent("rs-macos-release").path
let handbackTool = macosDir.appendingPathComponent("rs-macos-handback").path
// The camera service files of this bundle (Contents/MacOS/realsense-usbd, and service.plist with the installer in
// Contents/Resources/realsense-usbd); nil when one is missing, then only the administrator path is left.
let kit = RealSenseCameraAccess.Kit.inBundle(bundleURL)
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

// The do shell script command of "Run once as administrator" (DESIGN 7.5): a clean environment, every path quoted
// for the shell and then for AppleScript. `releaseCamera`: re-enumerate the camera afterwards so macOS gets it back
// (rs-macos-release). Not when the camera service is running and only turned this user away: the root tool then
// reaches the camera through the service as well, the service hands it back itself, and a re-enumeration would
// take the camera away from the service and from whoever is using it.
func runOnceCommand(uid: uid_t, gid: gid_t, home: String, user: String, stamp: String,
                    releaseCamera: Bool) -> String {
    let quote = RealSenseCameraAccess.shellQuote
    let args = passthroughArgs.map(quote).joined(separator: " ")
    let release = releaseCamera ? "\(quote(releaseTool)) >/dev/null 2>&1" : ":"
    let script = """
        export HOME=\(quote(home)) USER=\(quote(user)) LOGNAME=\(quote(user)) \
        SUDO_USER=\(quote(user)) SUDO_UID=\(uid) SUDO_GID=\(gid)
        cd "$HOME"
        \(quote(target)) \(args) >/dev/null 2>&1
        status=$?
        \(release)
        \(quote(handbackTool)) \(uid) \(gid) \(quote(stamp)) "$HOME/.realsense-config.json" "$HOME/Documents" 2>/dev/null
        exit $status
        """
    return "\(cleanEnv) /bin/sh -c \(quote(script))"
}

// Decide before AppKit starts: when the camera service admits this user, become the tool right away.
// state(kit:) blocks for the probe and probes a second time when an installed service does not answer at once
// (launchd may still be starting it).
let firstState = kit.map { RealSenseCameraAccess.state(kit: $0) }
if let firstState, firstState.isReady {
    execTarget()
}

final class Launcher: NSObject, NSApplicationDelegate {
    private var finished = false
    /// nil: this bundle carries no camera service files.
    private var state: RealSenseCameraAccess.State?

    init(state: RealSenseCameraAccess.State?) {
        self.state = state
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApp.activate()
        if let kit, let state, state.installationHelps {
            // Not installed, too old or not answering: straight to the administrator prompt (asked once per Mac).
            installCameraService(kit)
        } else {
            ask()
        }
    }

    /// What is wrong and what the user can do about it. `failure`: what an installation that was just tried ended
    /// with. `refusal`: why the service cannot be installed from this copy of the app (no install button then).
    private func ask(failure: String? = nil, refusal: String? = nil) {
        let alert = NSAlert()
        alert.messageText = appName
        var install: String?
        var runOnce = true
        var loginItems = false
        switch state {
        case nil:
            alert.informativeText = "\(appName) needs administrator access to control the RealSense camera over USB."
        case .ready:
            // Only after an installation that was reported as failed although the service answers now.
            relaunch()
            return
        case .notInstalled:
            alert.informativeText = "The RealSense camera service lets every user and every RealSense application "
                + "of this Mac use the camera without a password. Installing it needs an administrator's name and "
                + "password once."
            install = "Install camera service"
        case .needsUpdate:
            alert.informativeText = "The RealSense camera service installed on this Mac is too old for \(appName). "
                + "Updating it needs an administrator's name and password once."
            install = "Update camera service"
        case .notAnswering(let line):
            alert.informativeText = "The RealSense camera service is installed but does not answer"
                + (line.isEmpty ? "." : " (\(line)).") + " Installing it again needs an administrator's name and "
                + "password."
            install = "Reinstall camera service"
        case .switchedOff:
            alert.informativeText = "The RealSense camera service is switched off in System Settings > General > "
                + "Login Items & Extensions."
            loginItems = true
        case .notConsoleUser:
            alert.informativeText = "Only the user logged in at this Mac's screen can use the camera through the "
                + "RealSense camera service."
        case .cameraDisabled:
            alert.informativeText = "Camera use is disabled on this Mac by its administrator."
            runOnce = false   // it would get around the ban
        case .tooManyClients:
            alert.informativeText = "Too many programs are using the RealSense camera service."
            runOnce = false   // the service holds the cameras
        }
        if let failure {
            alert.informativeText = failure + "\n\n" + alert.informativeText
        }
        if let refusal {
            alert.informativeText += "\n\n" + refusal
            install = nil
        }
        var actions: [() -> Void] = []
        if let title = install, let kit {
            alert.addButton(withTitle: title)
            actions.append { self.installCameraService(kit) }
        }
        if loginItems {
            alert.addButton(withTitle: "Open Login Items Settings")
            actions.append {
                RealSenseCameraAccess.openLoginItemsSettings()
                NSApp.terminate(nil)
            }
        }
        if runOnce {
            alert.addButton(withTitle: "Run once as administrator")
            actions.append { self.runOnceAsAdministrator() }
        }
        alert.addButton(withTitle: actions.isEmpty ? "OK" : "Cancel").keyEquivalent = "\u{1b}"
        actions.append { NSApp.terminate(nil) }
        let index = alert.runModal().rawValue - NSApplication.ModalResponse.alertFirstButtonReturn.rawValue
        (index >= 0 && index < actions.count ? actions[index] : actions[actions.count - 1])()
    }

    // An administrator's name and password once; then a new instance probes and becomes the tool.
    private func installCameraService(_ kit: RealSenseCameraAccess.Kit) {
        let prompt = "\(appName) wants to install the RealSense camera service. It is installed once, for all users "
            + "and all RealSense applications of this Mac."
        DispatchQueue.global(qos: .userInitiated).async {
            var failure: RealSenseCameraAccess.InstallError?
            do {
                try RealSenseCameraAccess.install(kit: kit, prompt: prompt)
            } catch let error as RealSenseCameraAccess.InstallError {
                failure = error
            } catch {
                failure = .failed(error.localizedDescription)
            }
            // Nothing changed after a cancelled prompt or a refused bundle: keep the state, skip the probe.
            var after: RealSenseCameraAccess.State?
            switch failure {
            case .cancelled, .untrusted: break
            default: after = RealSenseCameraAccess.state(kit: kit)
            }
            DispatchQueue.main.async { self.installFinished(failure, after) }
        }
    }

    private func installFinished(_ failure: RealSenseCameraAccess.InstallError?,
                                 _ after: RealSenseCameraAccess.State?) {
        if let after {
            state = after
        }
        switch failure {
        case nil:
            // Relaunch only when the service answers now; otherwise the new instance would ask for the password
            // again.
            if let after, after.isReady {
                relaunch()
            } else {
                ask(failure: "The RealSense camera service is installed, but \(appName) still cannot use it.")
            }
        case .cancelled:
            ask()
        case .untrusted(let why):
            ask(refusal: "It can only be installed from a copy of \(appName) that only administrators can change "
                + "(\(why)). Move \(appName) to /Applications (an administrator must do this), then open it again.")
        case .failed(let message):
            ask(failure: "The RealSense camera service could not be installed: \(message)")
        }
    }

    private func relaunch() {
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
    }

    // Runs the tool as root. Settings and the files the tool saved to its default location (Documents) are handed
    // back to the user afterwards.
    private func runOnceAsAdministrator() {
        let uid = getuid(), gid = getgid()
        let home = NSHomeDirectory(), user = NSUserName()
        let stamp = NSTemporaryDirectory() + "rs-launch-\(getpid())"
        FileManager.default.createFile(atPath: stamp, contents: nil)

        let source = RealSenseCameraAccess.appleScriptSource(
            command: runOnceCommand(uid: uid, gid: gid, home: home, user: user, stamp: stamp,
                                    releaseCamera: state != .notConsoleUser),
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
let delegate = Launcher(state: firstState)
app.delegate = delegate
app.setActivationPolicy(.accessory)
app.run()
