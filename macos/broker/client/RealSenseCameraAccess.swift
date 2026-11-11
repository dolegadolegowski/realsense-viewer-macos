// License: Apache 2.0. See LICENSE file in root directory.
//
// RealSenseCameraAccess - what a macOS application needs to use the shared RealSense camera service
// (realsense-usbd, README.md next to the client folder): is the service installed and answering, and if not,
// install it once with an administrator's name and password. One file, Foundation and ServiceManagement only:
// add it to the application's sources as it is (do not edit it per application).
//
//     let kit = RealSenseCameraAccess.Kit.inBundle(Bundle.main.bundleURL)!
//     switch RealSenseCameraAccess.state(kit: kit) {                    // blocking, a few seconds at most
//     case .ready: break                                                // librealsense reaches the camera as the user
//     case let state where state.installationHelps:
//         try RealSenseCameraAccess.install(kit: kit, prompt: "MyApp wants to install the RealSense camera service.")
//     default: ...                                                      // explain; see State
//     }
//
// Nothing here needs the main thread; state(kit:) and install(kit:prompt:) block, so call them off the main thread
// in an application with a user interface.

import Darwin
import Foundation
import ServiceManagement

public enum RealSenseCameraAccess {
    /// launchd label and Mach service name of the camera service.
    public static let serviceLabel = "io.github.dolegadolegowski.realsense-usbd"
    public static let installedDaemonPlist = "/Library/LaunchDaemons/\(serviceLabel).plist"
    public static let installedDaemon = "/Library/PrivilegedHelperTools/\(serviceLabel)"
    /// Written by the installer, world-readable: ServiceVersion, ProtocolMin, ProtocolMax and the administrator's
    /// optional settings.
    public static let installedConfig = "/Library/Application Support/\(serviceLabel)/usbd.plist"

    // MARK: The files an application carries

    /// The camera service files inside an application: the daemon (also the probe tool), its manifest and the
    /// installer.
    public struct Kit: Sendable, Equatable {
        public var daemon: URL
        public var manifest: URL
        public var installScript: URL

        /// The folders are resolved here (no symbolic links), so the probe, the trust check and the installer, which
        /// refuses a path that goes through a symbolic link, all see one path.
        public init(daemon: URL, manifest: URL, installScript: URL) {
            self.daemon = Kit.inResolvedFolder(daemon)
            self.manifest = Kit.inResolvedFolder(manifest)
            self.installScript = Kit.inResolvedFolder(installScript)
        }

        private static func inResolvedFolder(_ file: URL) -> URL {
            guard let folder = resolvedPath(file.deletingLastPathComponent().path) else { return file }
            return URL(fileURLWithPath: folder, isDirectory: true).appendingPathComponent(file.lastPathComponent)
        }

        /// The standard layout: `Contents/MacOS/realsense-usbd` (or `Contents/Helpers/realsense-usbd`) and
        /// `Contents/Resources/realsense-usbd/{service.plist, install-camera-access.sh}`. The bundle path is
        /// resolved once (no symbolic links), so the trust check, the probe and the installer all see one path.
        /// nil when a file is missing.
        public static func inBundle(_ bundleURL: URL) -> Kit? {
            let root = URL(fileURLWithPath: resolvedPath(bundleURL.path) ?? bundleURL.path, isDirectory: true)
            let contents = root.appendingPathComponent("Contents")
            let resources = contents.appendingPathComponent("Resources/realsense-usbd")
            let fm = FileManager.default
            guard let daemon = ["MacOS", "Helpers"]
                .map({ contents.appendingPathComponent($0).appendingPathComponent("realsense-usbd") })
                .first(where: { fm.isExecutableFile(atPath: $0.path) }) else { return nil }
            let kit = Kit(daemon: daemon, manifest: resources.appendingPathComponent("service.plist"),
                          installScript: resources.appendingPathComponent("install-camera-access.sh"))
            return kit.isComplete ? kit : nil
        }

        public var isComplete: Bool {
            let fm = FileManager.default
            return fm.isExecutableFile(atPath: daemon.path) && fm.fileExists(atPath: manifest.path)
                && fm.fileExists(atPath: installScript.path)
        }

        /// ServiceVersion of the daemon this application carries (from its manifest).
        public var serviceVersion: Int? { RealSenseCameraAccess.integer("ServiceVersion", inPlist: manifest.path) }
    }

    // MARK: Probe

    /// The answer of `realsense-usbd --probe`: 0 the service admits this user, 2 not reachable (not installed,
    /// switched off, not answering, or this program is not on the administrator's list), 3 not the user logged in at
    /// the Mac's screen, 4 camera use disabled by the Mac's administrator (MDM / Screen Time), 5 the installed
    /// service does not speak this application's protocol version, 6 too many programs use it.
    public struct Probe: Sendable, Equatable {
        public let code: Int32
        public let line: String

        public init(code: Int32, line: String) {
            self.code = code
            self.line = line
        }

        /// From "ok <service version>".
        public var serviceVersion: Int? {
            let words = line.split(separator: " ")
            guard code == 0, words.count >= 2, words[0] == "ok" else { return nil }
            return Int(words[1])
        }
    }

    /// Runs the application's own `realsense-usbd --probe` as the current user. Never touches USB. A tool that cannot
    /// be run or does not answer within `timeout` counts as code 2.
    public static func probe(daemon: URL, timeout: TimeInterval = 8) -> Probe {
        let process = Process()
        process.executableURL = daemon
        process.arguments = ["--probe"]
        process.environment = [:]
        process.standardInput = FileHandle.nullDevice
        process.standardError = FileHandle.nullDevice
        let stdout = Pipe()
        process.standardOutput = stdout
        let exited = DispatchSemaphore(value: 0)
        process.terminationHandler = { _ in exited.signal() }
        do {
            try process.run()
        } catch {
            return Probe(code: 2, line: "cannot run \(daemon.lastPathComponent): \(error.localizedDescription)")
        }
        let output = OutputBox()
        let readDone = DispatchSemaphore(value: 0)
        let reader = stdout.fileHandleForReading
        // A thread of its own, not a block on a global dispatch queue: macOS starts no such block while as many
        // threads of the Swift cooperative pool as the Mac has cores are in use, and the probe's line would be lost.
        let outputReader = Thread {
            while output.count < 4096, let data = try? reader.read(upToCount: 1024), !data.isEmpty {
                output.append(data)
            }
            readDone.signal()
        }
        outputReader.name = "RealSenseCameraAccess probe output"
        outputReader.start()
        if exited.wait(timeout: .now() + timeout) == .timedOut {
            kill(process.processIdentifier, SIGKILL)
            exited.wait()
            _ = readDone.wait(timeout: .now() + 1)
            return Probe(code: 2, line: "realsense-usbd --probe did not answer")
        }
        _ = readDone.wait(timeout: .now() + 1)
        let line = String(decoding: output.data, as: UTF8.self).split(separator: "\n").first.map(String.init) ?? ""
        guard process.terminationReason == .exit else { return Probe(code: 2, line: "realsense-usbd --probe crashed") }
        return Probe(code: process.terminationStatus, line: line)
    }

    private final class OutputBox: @unchecked Sendable {
        private let lock = NSLock()
        private var bytes = Data()
        var count: Int { lock.withLock { bytes.count } }
        var data: Data { lock.withLock { bytes } }
        func append(_ more: Data) { lock.withLock { bytes.append(more) } }
    }

    // MARK: State

    public enum State: Sendable, Equatable {
        /// The service answers and admits this user: start librealsense as the user, no password.
        case ready(serviceVersion: Int?)
        /// No camera service on this Mac.
        case notInstalled
        /// Installed, but an administrator switched it off in System Settings > General > Login Items & Extensions.
        case switchedOff
        /// Installed, but too old for this application (another protocol version, or the first generation that only
        /// admitted the programs of one build).
        case needsUpdate
        /// Installed and not answering (the text says what the probe saw).
        case notAnswering(String)
        /// Only the user logged in at the Mac's screen (and root) can use the camera through the service.
        case notConsoleUser
        /// Camera use is disabled on this Mac by its administrator (MDM / Screen Time). No way around it.
        case cameraDisabled
        /// Too many programs are connected to the service.
        case tooManyClients

        /// True when running the installer is the fix (an administrator's name and password, once).
        public var installationHelps: Bool {
            switch self {
            case .notInstalled, .needsUpdate, .notAnswering: return true
            default: return false
            }
        }

        public var isReady: Bool {
            if case .ready = self { return true }
            return false
        }
    }

    /// Whether the camera service can be used right now, and if not, why. Blocks for the probe (normally well under
    /// a second; a few seconds when launchd is just starting the daemon or nothing answers).
    public static func state(kit: Kit, probeTimeout: TimeInterval = 8) -> State {
        func facts() -> (installed: Bool, switchedOff: Bool, currentGeneration: Bool) {
            let installed = FileManager.default.fileExists(atPath: installedDaemonPlist)
            let off = installed && SMAppService.statusForLegacyPlist(at: URL(fileURLWithPath: installedDaemonPlist))
                == .requiresApproval
            return (installed, off, installedServiceVersion() != nil)
        }
        var result = probe(daemon: kit.daemon, timeout: probeTimeout)
        var f = facts()
        var decided = state(probe: result, installed: f.installed, switchedOff: f.switchedOff,
                            currentGeneration: f.currentGeneration)
        if case .notAnswering = decided {
            // launchd may still be starting it (right after an installation or a restart of the daemon).
            Thread.sleep(forTimeInterval: 2)
            result = probe(daemon: kit.daemon, timeout: probeTimeout)
            f = facts()
            decided = state(probe: result, installed: f.installed, switchedOff: f.switchedOff,
                            currentGeneration: f.currentGeneration)
        }
        return decided
    }

    /// The decision of state(kit:) from its inputs. `currentGeneration`: the installed configuration is the shared
    /// service's (it names a ServiceVersion); false for the first generation, which admitted one build only.
    public static func state(probe: Probe, installed: Bool, switchedOff: Bool, currentGeneration: Bool) -> State {
        switch probe.code {
        case 0: return .ready(serviceVersion: probe.serviceVersion)
        case 3: return .notConsoleUser
        case 4: return .cameraDisabled
        case 6: return .tooManyClients
        default: break
        }
        if !installed { return .notInstalled }
        if switchedOff { return .switchedOff }
        if probe.code == 5 || !currentGeneration { return .needsUpdate }
        return .notAnswering(probe.line)
    }

    /// ServiceVersion of the installed service, from its configuration. nil when not installed, or installed by the
    /// first generation.
    public static func installedServiceVersion() -> Int? { integer("ServiceVersion", inPlist: installedConfig) }

    /// Opens System Settings > General > Login Items & Extensions, where an administrator switches the service on.
    public static func openLoginItemsSettings() { SMAppService.openSystemSettingsLoginItems() }

    // MARK: Installation

    public enum InstallError: Error, Equatable, CustomStringConvertible {
        /// The administrator prompt was cancelled.
        case cancelled
        /// The installer would refuse these files (see untrustedReason(kit:)).
        case untrusted(String)
        /// The installer failed; its message.
        case failed(String)

        public var description: String {
            switch self {
            case .cancelled: return "The administrator prompt was cancelled."
            case .untrusted(let why): return why
            case .failed(let message): return message
            }
        }
    }

    /// Installs (or updates, or repairs) the camera service from the application's own files: shows the standard
    /// macOS administrator prompt with `prompt` (an administrator's name and password, also on a standard account)
    /// and runs install-camera-access.sh as root. The installer never replaces a newer service, nor one of the same
    /// version that is installed and answering (it then changes nothing and succeeds). Blocks until it ended;
    /// afterwards state(kit:) is .ready unless something else is wrong.
    public static func install(kit: Kit, prompt: String) throws {
        if let why = untrustedReason(kit: kit) { throw InstallError.untrusted(why) }
        let source = appleScriptSource(command: installCommand(kit: kit), prompt: prompt)
        let result = runAppleScript(source)
        switch result {
        case .success: return
        case .failure(let number, let message):
            if number == -128 { throw InstallError.cancelled }
            throw InstallError.failed(message.isEmpty ? "the installer failed (\(number))" : message)
        }
    }

    /// The command install(kit:prompt:) runs as root: a clean environment and every path quoted for the shell.
    public static func installCommand(kit: Kit, allowDowngrade: Bool = false) -> String {
        var sources = [kit.daemon.deletingLastPathComponent().path]
        let manifestFolder = kit.manifest.deletingLastPathComponent().path
        if !sources.contains(manifestFolder) { sources.append(manifestFolder) }
        var words = ["/usr/bin/env", "-i", "PATH=/usr/bin:/bin:/usr/sbin:/sbin", "/bin/bash", "-p",
                     shellQuote(kit.installScript.path)]
        for source in sources { words += ["--source", shellQuote(source)] }
        if allowDowngrade { words.append("--allow-downgrade") }
        return words.joined(separator: " ")
    }

    public static func appleScriptSource(command: String, prompt: String, privileged: Bool = true) -> String {
        guard privileged else { return "do shell script \(appleScriptQuote(command))" }
        return "do shell script \(appleScriptQuote(command)) with administrator privileges with prompt "
            + appleScriptQuote(prompt)
    }

    /// POSIX shell single quoting.
    public static func shellQuote(_ s: String) -> String {
        "'" + s.replacingOccurrences(of: "'", with: "'\\''") + "'"
    }

    /// AppleScript string literal.
    public static func appleScriptQuote(_ s: String) -> String {
        "\"" + s.replacingOccurrences(of: "\\", with: "\\\\").replacingOccurrences(of: "\"", with: "\\\"") + "\""
    }

    // MARK: Trust rule of the installer (DESIGN 6.2)

    /// Why the installer would refuse to install from this application: nil when the daemon, the manifest, the
    /// installer and every folder above them can only be changed by root or an administrator (so not a copy in a
    /// standard user's folder, and not a translocated or quarantined copy run from a download). The text is meant
    /// for the user: the fix is to move the application to /Applications as an administrator.
    public static func untrustedReason(kit: Kit) -> String? {
        for file in [kit.daemon.path, kit.manifest.path, kit.installScript.path] {
            if let why = untrustedReason(path: file, file: true) { return why }
        }
        return nil
    }

    public static func untrustedReason(path: String, file: Bool) -> String? {
        var st = stat()
        if file {
            guard lstat(path, &st) == 0, (st.st_mode & S_IFMT) == S_IFREG else { return "\(path) is not a regular file" }
            guard st.st_nlink == 1 else { return "\(path) has more than one hard link" }
        }
        guard var current = resolvedPath(path) else { return "\(path) cannot be resolved" }
        if current.contains("/AppTranslocation/") {
            return "\(path) runs from a temporary, read-only copy made by macOS (move the application to /Applications)"
        }
        while true {
            guard lstat(current, &st) == 0 else { return "\(current) cannot be examined" }
            guard isRootOrAdministrator(st.st_uid) else {
                return "\(current) belongs to a user who is not an administrator"
            }
            if st.st_mode & S_IWOTH != 0 { return "\(current) can be changed by everyone" }
            if st.st_mode & S_IWGRP != 0 && st.st_gid != 0 && st.st_gid != 80 {
                return "\(current) can be changed by group \(st.st_gid)"
            }
            if aclAllowsWrite(current) { return "\(current) has an access control entry that lets others change it" }
            if current == "/" { return nil }
            current = (current as NSString).deletingLastPathComponent
            if current.isEmpty { current = "/" }
        }
    }

    static func isRootOrAdministrator(_ uid: uid_t) -> Bool {
        if uid == 0 { return true }
        guard let pw = getpwuid(uid) else { return false }
        var groups = [Int32](repeating: 0, count: 256)
        var count = Int32(groups.count)
        guard getgrouplist(pw.pointee.pw_name, Int32(bitPattern: pw.pointee.pw_gid), &groups, &count) >= 0 else {
            return false
        }
        return groups.prefix(Int(count)).contains(80 /* admin */)
    }

    static func aclAllowsWrite(_ path: String) -> Bool {
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

    // MARK: Running the administrator script

    public enum ScriptResult: Sendable, Equatable {
        case success(String)
        /// AppleScript's error number (-128: cancelled; 1...255: the command's exit status) and message.
        case failure(number: Int, message: String)
    }

    /// Runs an AppleScript and waits for it: in-process (NSAppleScript, the prompt is then the application's) when an
    /// AppKit application is running, through /usr/bin/osascript otherwise, where an in-process "do shell script"
    /// can wait forever for an Apple event nobody dispatches. `inProcess` overrides the choice (tests).
    public static func runAppleScript(_ source: String, inProcess: Bool? = nil) -> ScriptResult {
        if inProcess ?? hasAppKitApplication {
            var info: NSDictionary?
            guard let script = NSAppleScript(source: source) else {
                return .failure(number: -1, message: "cannot create the authorization script")
            }
            let result = script.executeAndReturnError(&info)
            if let info {
                return .failure(number: (info[NSAppleScript.errorNumber] as? NSNumber)?.intValue ?? -1,
                                message: normalizedLines(info[NSAppleScript.errorMessage] as? String ?? "unknown error"))
            }
            return .success(normalizedLines(result.stringValue ?? ""))
        }
        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/usr/bin/osascript")
        process.arguments = ["-e", source]
        process.environment = [:]
        process.standardInput = FileHandle.nullDevice
        let stdout = Pipe(), stderr = Pipe()
        process.standardOutput = stdout
        process.standardError = stderr
        let exited = DispatchSemaphore(value: 0)
        process.terminationHandler = { _ in exited.signal() }
        do {
            try process.run()
        } catch {
            return .failure(number: -1, message: "/usr/bin/osascript: \(error.localizedDescription)")
        }
        // Both pipes at once: a full one would block osascript before it exits. The second reader is a thread of
        // its own, not a block on a global dispatch queue: macOS starts no such block while as many threads of the
        // Swift cooperative pool as the Mac has cores are in use (running or blocked), and the wait below would not
        // end although osascript had exited.
        let errors = OutputBox()
        let errorsDone = DispatchSemaphore(value: 0)
        let errorReader = stderr.fileHandleForReading
        let errorThread = Thread {
            errors.append(errorReader.readDataToEndOfFile())
            errorsDone.signal()
        }
        errorThread.name = "RealSenseCameraAccess osascript stderr"
        errorThread.start()
        let outputData = stdout.fileHandleForReading.readDataToEndOfFile()
        errorsDone.wait()
        exited.wait()
        if process.terminationStatus == 0 {
            return .success(normalizedLines(String(decoding: outputData, as: UTF8.self)))
        }
        let text = String(decoding: errors.data, as: UTF8.self).trimmingCharacters(in: .whitespacesAndNewlines)
        guard let parsed = parseOSAScriptError(text) else { return .failure(number: -1, message: text) }
        return .failure(number: parsed.number, message: normalizedLines(parsed.message))
    }

    /// True in an AppKit application (NSApp exists), looked up at run time so this file does not link AppKit.
    static var hasAppKitApplication: Bool {
        // RTLD_DEFAULT: every image loaded in the process. Without AppKit there is no NSApp symbol at all.
        guard let symbol = dlsym(UnsafeMutableRawPointer(bitPattern: -2), "NSApp") else { return false }
        return symbol.load(as: UnsafeRawPointer?.self) != nil
    }

    /// "0:42: execution error: <message> (<number>)" -> (number, message).
    static func parseOSAScriptError(_ text: String) -> (number: Int, message: String)? {
        guard let marker = text.range(of: "execution error: "), text.hasSuffix(")"),
              let open = text.range(of: " (", options: .backwards), open.lowerBound >= marker.upperBound,
              let number = Int(text[open.upperBound..<text.index(before: text.endIndex)]) else { return nil }
        return (number, String(text[marker.upperBound..<open.lowerBound]))
    }

    /// AppleScript returns the shell's lines separated by carriage returns.
    static func normalizedLines(_ text: String) -> String {
        text.replacingOccurrences(of: "\r\n", with: "\n").replacingOccurrences(of: "\r", with: "\n")
            .trimmingCharacters(in: .whitespacesAndNewlines)
    }

    // MARK: Helpers

    static func resolvedPath(_ path: String) -> String? {
        guard let resolved = realpath(path, nil) else { return nil }
        defer { free(resolved) }
        return String(cString: resolved)
    }

    static func integer(_ key: String, inPlist path: String) -> Int? {
        guard let data = FileManager.default.contents(atPath: path), data.count <= 65536,
              let plist = try? PropertyListSerialization.propertyList(from: data, format: nil) as? [String: Any],
              let number = plist[key] as? NSNumber, CFGetTypeID(number) == CFNumberGetTypeID(),
              !CFNumberIsFloatType(number) else { return nil }
        return number.intValue
    }
}
