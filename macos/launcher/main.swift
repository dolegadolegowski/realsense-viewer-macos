// License: Apache 2.0. See LICENSE file in root directory.
//
// Launcher for the RealSense tools bundled as macOS apps.
//
// On macOS, RealSense cameras are accessed through libusb, which has to capture the USB device from the
// system UVC/HID drivers - and macOS only allows that for root. The launcher asks for administrator
// rights with the standard macOS authorization dialog, runs the real tool (Contents/MacOS/<RSLaunchTarget>)
// as root in the user's session, hands the camera back to macOS when the tool exits and returns ownership
// of the files the tool created in the user's home folder.

import AppKit

let bundle = Bundle.main
let macosDir = bundle.bundleURL.appendingPathComponent("Contents/MacOS")
let appName = bundle.object(forInfoDictionaryKey: "CFBundleName") as? String ?? "RealSense Viewer"
let targetName = bundle.object(forInfoDictionaryKey: "RSLaunchTarget") as? String ?? "realsense-viewer"
let target = macosDir.appendingPathComponent(targetName).path
let releaseTool = macosDir.appendingPathComponent("rs-macos-release").path
let handbackTool = macosDir.appendingPathComponent("rs-macos-handback").path
let passthroughArgs = Array(CommandLine.arguments.dropFirst()).filter { !$0.hasPrefix("-psn_") }

// Already privileged (e.g. started with sudo from Terminal): just become the tool.
if geteuid() == 0 {
    let argv = ([target] + passthroughArgs).map { strdup($0) } + [nil]
    execv(target, argv)
    perror("execv")
    exit(1)
}

func shellQuote(_ s: String) -> String {
    return "'" + s.replacingOccurrences(of: "'", with: "'\\''") + "'"
}

func appleScriptQuote(_ s: String) -> String {
    return "\"" + s.replacingOccurrences(of: "\\", with: "\\\\").replacingOccurrences(of: "\"", with: "\\\"") + "\""
}

final class Launcher: NSObject, NSApplicationDelegate {
    private var finished = false

    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApp.activate()
        let uid = getuid(), gid = getgid()
        let home = NSHomeDirectory(), user = NSUserName()
        let stamp = NSTemporaryDirectory() + "rs-launch-\(getpid())"
        FileManager.default.createFile(atPath: stamp, contents: nil)

        // Runs as root. Settings and the files the tool saved to its default location (Documents) are handed
        // back to the user afterwards.
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
        let source = "do shell script \(appleScriptQuote(script)) with administrator privileges with prompt "
            + appleScriptQuote("\(appName) needs administrator access to control the RealSense camera over USB.")

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
let delegate = Launcher()
app.delegate = delegate
app.setActivationPolicy(.accessory)
app.run()
