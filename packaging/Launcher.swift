import AppKit
import Darwin

// The app bundle is immutable; Wine's writable state and logs are per-user.
final class Launcher: NSObject, NSApplicationDelegate {
    let fm = FileManager.default
    let resources = Bundle.main.resourceURL!
    let support = FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Library/Application Support/AP2700")
    let logs = FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Library/Logs/AP2700")
    var helper: Process?
    var wine: Process?
    var lockFD: Int32 = -1
    var env = ProcessInfo.processInfo.environment
    var logHandle: FileHandle?
    var stopping = false
    var prefix: URL { support.appendingPathComponent("prefix") }
    var wineBin: URL { resources.appendingPathComponent("Wine Stable.app/Contents/Resources/wine/bin") }

    func process(_ executable: URL, _ arguments: [String]) throws -> Process {
        let p = Process()
        p.executableURL = executable
        p.arguments = arguments
        p.environment = env
        p.currentDirectoryURL = prefix.appendingPathComponent("drive_c/AP2700")
        p.standardOutput = logHandle
        p.standardError = logHandle
        try p.run()
        return p
    }

    func prepare() throws {
        try fm.createDirectory(at: support, withIntermediateDirectories: true)
        try fm.createDirectory(at: logs, withIntermediateDirectories: true)
        lockFD = Darwin.open(support.appendingPathComponent("session.lock").path, O_CREAT | O_RDWR, 0o600)
        guard lockFD >= 0, flock(lockFD, LOCK_EX | LOCK_NB) == 0 else {
            throw NSError(domain: "AP2700", code: 1, userInfo: [NSLocalizedDescriptionKey: "AP2700 is already running. Close its current session first."])
        }
        // Children must not inherit the session lock.
        _ = fcntl(lockFD, F_SETFD, FD_CLOEXEC)
        let stamp = ISO8601DateFormatter().string(from: Date()).replacingOccurrences(of: ":", with: "-")
        let log = logs.appendingPathComponent("AP2700-\(stamp).log")
        fm.createFile(atPath: log.path, contents: nil)
        logHandle = try FileHandle(forWritingTo: log)
        let fresh = !fm.fileExists(atPath: prefix.path)
        if fresh {
            let staging = support.appendingPathComponent("prefix-\(UUID().uuidString).tmp")
            do {
                try fm.copyItem(at: resources.appendingPathComponent("prefix-template"), to: staging)
                try fm.createSymbolicLink(atPath: staging.appendingPathComponent("dosdevices/z:").path, withDestinationPath: "/")
                try fm.moveItem(at: staging, to: prefix)
            } catch {
                try? fm.removeItem(at: staging)
                throw error
            }
        }
        env["WINEPREFIX"] = prefix.path
        env["WINEDEBUG"] = "-all"
        env["MVK_CONFIG_LOG_LEVEL"] = "0"
        env["APUSB_TRACE"] = env["APUSB_TRACE"] ?? "0"
        // No Gecko/Mono download dialogs during initialization.
        env["WINEDLLOVERRIDES"] = "mscoree,mshtml="
        let initialized = prefix.appendingPathComponent(".apusb-initialized")
        if !fm.fileExists(atPath: initialized.path) {
            let boot = try process(wineBin.appendingPathComponent("wine"), ["wineboot", "-u"])
            boot.waitUntilExit()
            guard boot.terminationStatus == 0 else {
                throw NSError(domain: "AP2700", code: 2, userInfo: [NSLocalizedDescriptionKey: "Wine could not initialize. Apple Silicon requires Rosetta 2. See \(log.path)."])
            }
            try Data("1\n".utf8).write(to: initialized, options: .atomic)
        }
        // Reserve an ephemeral loopback port, then release it immediately before spawn.
        let fd = socket(AF_INET, SOCK_STREAM, 0)
        guard fd >= 0 else { throw POSIXError(.EIO) }
        var address = sockaddr_in()
        address.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
        address.sin_family = sa_family_t(AF_INET)
        address.sin_addr.s_addr = inet_addr("127.0.0.1")
        let bound = withUnsafePointer(to: &address) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { Darwin.bind(fd, $0, socklen_t(MemoryLayout<sockaddr_in>.size)) }
        }
        guard bound == 0 else { Darwin.close(fd); throw POSIXError(.EIO) }
        var length = socklen_t(MemoryLayout<sockaddr_in>.size)
        let found = withUnsafeMutablePointer(to: &address) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { getsockname(fd, $0, &length) }
        }
        Darwin.close(fd)
        guard found == 0 else { throw POSIXError(.EIO) }
        let port = UInt16(bigEndian: address.sin_port)
        env["APUSB_PORT"] = String(port)
        env["APUSB_TOKEN"] = UUID().uuidString.replacingOccurrences(of: "-", with: "").lowercased()
        helper = try process(resources.appendingPathComponent("apusb-bridge"), [String(port)])
        var ready = false
        for _ in 0..<100 {
            guard helper?.isRunning == true else { break }
            let probe = socket(AF_INET, SOCK_STREAM, 0)
            if probe >= 0 {
                let result = withUnsafePointer(to: &address) {
                    $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { connect(probe, $0, length) }
                }
                Darwin.close(probe)
                if result == 0 { ready = true; break }
            }
            usleep(50_000)
        }
        guard ready else {
            throw NSError(domain: "AP2700", code: 3, userInfo: [NSLocalizedDescriptionKey: "USB bridge failed to start. See \(log.path)."])
        }
    }

    func stop() {
        if stopping { return }
        stopping = true
        if wine?.isRunning == true {
            // Scoped to this app's private prefix; never affects another Wine application.
            if let server = try? process(wineBin.appendingPathComponent("wineserver"), ["-k"]) { server.waitUntilExit() }
        }
        if helper?.isRunning == true { helper?.terminate(); helper?.waitUntilExit() }
        try? logHandle?.close()
        if lockFD >= 0 { Darwin.close(lockFD); lockFD = -1 }
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        let menu = NSMenu()
        let item = NSMenuItem()
        let appMenu = NSMenu()
        appMenu.addItem(withTitle: "Quit AP2700", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        item.submenu = appMenu
        menu.addItem(item)
        NSApp.mainMenu = menu
        do {
            try prepare()
            wine = try process(wineBin.appendingPathComponent("wine"), ["C:\\AP2700\\Ap2700.exe"])
            wine?.terminationHandler = { [weak self] p in
                DispatchQueue.main.async {
                    if self?.stopping == false && p.terminationStatus != 0 {
                        let alert = NSAlert()
                        alert.messageText = "AP2700 stopped unexpectedly"
                        alert.informativeText = "See ~/Library/Logs/AP2700 for details."
                        alert.runModal()
                    }
                    NSApp.terminate(nil)
                }
            }
        } catch {
            let alert = NSAlert(error: error)
            alert.runModal()
            NSApp.terminate(nil)
        }
    }

    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        stop()
        return .terminateNow
    }
}

let launcher = Launcher()
if CommandLine.arguments.contains("--hardware-smoke-test") {
    do {
        try launcher.prepare()
        let test = try launcher.process(launcher.wineBin.appendingPathComponent("wine"), ["C:\\AP2700\\shim_smoke.exe"])
        test.waitUntilExit()
        let result = test.terminationStatus
        launcher.stop()
        print("AP2700 packaged hardware smoke test: exit \(result). Logs: \(launcher.logs.path)")
        exit(result)
    } catch {
        launcher.stop()
        fputs("\(error.localizedDescription)\n", stderr)
        exit(1)
    }
}
let app = NSApplication.shared
app.setActivationPolicy(.regular)
app.delegate = launcher
app.run()
