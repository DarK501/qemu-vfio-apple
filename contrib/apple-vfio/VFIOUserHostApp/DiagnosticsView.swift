import AppKit
import Foundation
import SwiftUI
import UniformTypeIdentifiers

struct DiagnosticsView: View {
    @Environment(\.dismiss) private var dismiss
    @AppStorage("diagnosticsSSHHost") private var host = "ubuntu@127.0.0.1"
    @AppStorage("diagnosticsSSHPort") private var port = "2222"
    @State private var command = ""
    @State private var busy = false
    @State private var output = ""

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("Host and guest diagnostics").font(.title2)
            Text("Uses your existing SSH keys and known_hosts. Set up and trust the guest in Terminal first. DMA debugfs collection uses non-interactive sudo if available. Logs and command output may contain sensitive data.")
                .font(.callout).foregroundStyle(.secondary)
            HStack {
                TextField("SSH host or user@host", text: $host)
                TextField("Port", text: $port).frame(width: 70)
            }
            HStack {
                Button("Export diagnostics…") { exportDiagnostics() }
                Text("Host, QEMU, guest PCI, DMA and interrupt diagnostics")
                    .font(.caption).foregroundStyle(.secondary)
            }
            Divider()
            TextField("Guest command (non-interactive)", text: $command)
            HStack {
                Button("Run on guest") { runCommand() }
                    .disabled(command.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
                Text("Non-interactive; SSH is stopped after 30 seconds. Guest processes may continue.")
                    .font(.caption).foregroundStyle(.secondary)
            }
            ScrollView {
                Text(output.isEmpty ? "Output and collection errors appear here." : output)
                    .font(.system(.caption, design: .monospaced))
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .textSelection(.enabled)
            }
            .frame(minHeight: 160)
            .overlay(RoundedRectangle(cornerRadius: 4).stroke(.quaternary))
            HStack {
                if busy { ProgressView().controlSize(.small) }
                Spacer()
                Button("Close") { dismiss() }
            }
        }
        .padding(20)
        .frame(minWidth: 650, minHeight: 420)
        .disabled(busy)
    }

    private func connection() throws -> DiagnosticsConnection {
        let target = host.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !target.isEmpty, !target.hasPrefix("-"),
              !target.contains(where: { $0.isWhitespace || $0.isNewline }),
              let number = Int(port), (1...65535).contains(number) else {
            throw DiagnosticsError.message("Enter an SSH host and a port from 1 to 65535.")
        }
        return DiagnosticsConnection(host: target, port: number)
    }

    private func exportDiagnostics() {
        do {
            let ssh = try connection()
            let panel = NSSavePanel()
            panel.nameFieldStringValue = "qemu-vfio-diagnostics.zip"
            panel.allowedContentTypes = [.zip]
            guard panel.runModal() == .OK, let destination = panel.url else { return }
            busy = true
            output = "Collecting diagnostics…"
            Task {
                do {
                    let result = try await Task.detached(priority: .utility) {
                        try DiagnosticsRunner.collect(ssh: ssh, destination: destination)
                    }.value
                    output = result
                } catch {
                    output = "Export failed: \(error.localizedDescription)"
                }
                busy = false
            }
        } catch {
            output = error.localizedDescription
        }
    }

    private func runCommand() {
        do {
            let ssh = try connection()
            let remoteCommand = command
            let confirm = NSAlert()
            confirm.messageText = "Run command on \(ssh.host)?"
            confirm.informativeText = "The command runs with the guest user's permissions. Its output is saved in the diagnostics log directory."
            confirm.addButton(withTitle: "Run")
            confirm.addButton(withTitle: "Cancel")
            guard confirm.runModal() == .alertFirstButtonReturn else { return }
            busy = true
            output = "Running command…"
            Task {
                do {
                    let result = try await Task.detached(priority: .utility) {
                        try DiagnosticsRunner.runRemote(ssh: ssh, command: remoteCommand)
                    }.value
                    output = result
                } catch {
                    output = "Command failed: \(error.localizedDescription)"
                }
                busy = false
            }
        } catch {
            output = error.localizedDescription
        }
    }
}

private struct DiagnosticsConnection: Sendable {
    let host: String
    let port: Int

    nonisolated var arguments: [String] {
        ["-T", "-p", String(port), "-o", "BatchMode=yes",
         "-o", "StrictHostKeyChecking=yes", "-o", "ConnectTimeout=8",
         "-o", "ServerAliveInterval=5", "-o", "ServerAliveCountMax=2", host]
    }
}

private enum DiagnosticsError: LocalizedError {
    case message(String)
    nonisolated var errorDescription: String? {
        if case .message(let text) = self { return text }
        return nil
    }
}

private enum DiagnosticsRunner {
    private nonisolated static var logRoot: URL {
        FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent("Library/Logs/qemu-vfio-apple", isDirectory: true)
    }

    private nonisolated static func newDirectory(_ prefix: String) throws -> URL {
        let dir = logRoot.appendingPathComponent("\(prefix)-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true,
                                                attributes: [.posixPermissions: 0o700])
        return dir
    }

    private nonisolated static func run(_ executable: String, _ arguments: [String],
                            directory: URL, name: String) throws -> String {
        let out = directory.appendingPathComponent("\(name).txt")
        let err = directory.appendingPathComponent("\(name).stderr.txt")
        guard FileManager.default.createFile(atPath: out.path, contents: nil),
              FileManager.default.createFile(atPath: err.path, contents: nil) else {
            throw DiagnosticsError.message("Could not create \(name) output files.")
        }
        let stdout = try FileHandle(forWritingTo: out)
        let stderr = try FileHandle(forWritingTo: err)
        defer { try? stdout.close(); try? stderr.close() }
        let process = Process()
        process.executableURL = URL(fileURLWithPath: executable)
        process.arguments = arguments
        process.standardInput = FileHandle.nullDevice
        process.standardOutput = stdout
        process.standardError = stderr
        try process.run()
        let timeout = DispatchWorkItem {
            if process.isRunning { process.terminate() }
        }
        DispatchQueue.global().asyncAfter(deadline: .now() + 30, execute: timeout)
        process.waitUntilExit()
        timeout.cancel()
        let status = process.terminationStatus
        return "\(name): \(status == 0 ? "OK" : "failed (status \(status)); see \(name).stderr.txt")"
    }

    private nonisolated static func copyQemuLogs(to directory: URL) throws -> [String] {
        let fm = FileManager.default
        let limit = 20 * 1024 * 1024
        let items = try fm.contentsOfDirectory(at: logRoot,
                                               includingPropertiesForKeys: [.contentModificationDateKey, .fileSizeKey])
            .filter { $0.lastPathComponent.hasPrefix("qemu-") && $0.pathExtension == "log" }
        let sorted = try items.map { ($0, try $0.resourceValues(forKeys: [.contentModificationDateKey, .fileSizeKey])) }
            .sorted { ($0.1.contentModificationDate ?? .distantPast) > ($1.1.contentModificationDate ?? .distantPast) }
        var notes: [String] = []
        for (url, values) in sorted.prefix(5) {
            guard let size = values.fileSize else {
                notes.append("Skipped \(url.lastPathComponent): size unavailable.")
                continue
            }
            do {
                if size <= limit {
                    try fm.copyItem(at: url, to: directory.appendingPathComponent(url.lastPathComponent))
                    notes.append("Copied \(url.lastPathComponent) (may be incomplete if QEMU is running).")
                } else {
                    let tail = directory.appendingPathComponent(url.lastPathComponent + ".tail")
                    guard fm.createFile(atPath: tail.path, contents: nil) else {
                        throw DiagnosticsError.message("Could not create \(tail.lastPathComponent).")
                    }
                    let source = try FileHandle(forReadingFrom: url)
                    defer { try? source.close() }
                    let target = try FileHandle(forWritingTo: tail)
                    defer { try? target.close() }
                    try source.seek(toOffset: UInt64(size - limit))
                    var remaining = limit
                    while remaining > 0 {
                        guard let chunk = try source.read(upToCount: min(remaining, 65536)),
                              !chunk.isEmpty else {
                            throw DiagnosticsError.message("Log changed while reading; tail is incomplete.")
                        }
                        try target.write(contentsOf: chunk)
                        remaining -= chunk.count
                    }
                    notes.append("Copied last 20 MiB of \(url.lastPathComponent) to \(tail.lastPathComponent); it may start mid-line or omit later writes.")
                }
            } catch {
                notes.append("Could not fully copy \(url.lastPathComponent): \(error.localizedDescription)")
            }
        }
        if sorted.isEmpty { notes.append("No QEMU guest-errors logs found.") }
        return notes
    }

    nonisolated static func collect(ssh: DiagnosticsConnection, destination: URL) throws -> String {
        let dir = try newDirectory("report")
        var notes = ["Files may contain sensitive host and guest information.",
                     "Host unified logs are collected without sudo and may be incomplete.",
                     "Guest DMA debugfs collection uses sudo -n and requires debugfs and passwordless permission."]
        let jobs: [(String, [String], String)] = [
            ("/usr/bin/sw_vers", [], "host-version"),
            ("/usr/bin/uname", ["-a"], "host-uname"),
            ("/usr/bin/log", ["show", "--last", "15m", "--info", "--style", "compact",
                              "--predicate", "subsystem == 'uk.co.hitwithwrench.vfio' OR process == 'kernelmanagerd' OR process == 'sysextd' OR eventMessage CONTAINS[c] 'vfio-user-dext'"], "host-unified"),
            ("/usr/bin/ssh", ssh.arguments + ["uname -a"], "guest-uname"),
            ("/usr/bin/ssh", ssh.arguments + ["journalctl -k -b --no-pager -n 5000"], "guest-kernel"),
            ("/usr/bin/ssh", ssh.arguments + ["lspci -nnvv"], "guest-pci"),
            ("/usr/bin/ssh", ssh.arguments + ["cat /proc/interrupts"], "guest-interrupts"),
            ("/usr/bin/ssh", ssh.arguments + ["cat /proc/cmdline"], "guest-cmdline"),
            ("/usr/bin/ssh", ssh.arguments + ["cat /proc/modules"], "guest-modules"),
            ("/usr/bin/ssh", ssh.arguments + ["sudo -n cat /sys/kernel/debug/apple_dma/snapshot"], "guest-dma-snapshot"),
            ("/usr/bin/ssh", ssh.arguments + ["sudo -n tail -n 5000 /sys/kernel/debug/apple_dma/trace"], "guest-dma-trace"),
            ("/usr/bin/ssh", ssh.arguments + ["for f in /sys/module/apple_dma/parameters/*; do printf '%s: ' \"$f\"; cat \"$f\" || exit; done"], "guest-dma-parameters")
        ]
        for (binary, args, name) in jobs {
            do {
                notes.append(try run(binary, args, directory: dir, name: name))
            } catch {
                notes.append("\(name): failed: \(error.localizedDescription)")
            }
        }
        do { notes.append(contentsOf: try copyQemuLogs(to: dir)) }
        catch { notes.append("QEMU log collection failed: \(error.localizedDescription)") }
        try notes.joined(separator: "\n").write(to: dir.appendingPathComponent("manifest.txt"),
                                                atomically: true, encoding: .utf8)
        let archiveStatus = try run("/usr/bin/ditto",
                                    ["-c", "-k", "--sequesterRsrc", "--keepParent",
                                     dir.path, destination.path],
                                    directory: logRoot, name: "export-\(UUID().uuidString)")
        guard archiveStatus.contains(": OK") else {
            throw DiagnosticsError.message("\(archiveStatus). Report retained at \(dir.path).")
        }
        return "Exported to \(destination.path)\nReport retained at \(dir.path)\n\n" +
               notes.joined(separator: "\n")
    }

    private nonisolated static func readPreview(_ file: URL) throws -> String {
        let handle = try FileHandle(forReadingFrom: file)
        defer { try? handle.close() }
        return String(decoding: try handle.read(upToCount: 65536) ?? Data(), as: UTF8.self)
    }

    nonisolated static func runRemote(ssh: DiagnosticsConnection, command: String) throws -> String {
        let dir = try newDirectory("command")
        let status = try run("/usr/bin/ssh", ssh.arguments + [command], directory: dir, name: "guest-command")
        let stdout = try readPreview(dir.appendingPathComponent("guest-command.txt"))
        let stderr = try readPreview(dir.appendingPathComponent("guest-command.stderr.txt"))
        return "\(status)\nSaved at \(dir.path) (preview limited to 64 KiB per stream)\n\n\(stdout)\n\(stderr)"
    }
}
