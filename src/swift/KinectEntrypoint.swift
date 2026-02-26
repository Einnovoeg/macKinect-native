import Foundation
import Darwin

private enum BackendChoice {
    case auto
    case v1
    case v2
}

private struct CliOptions {
    var runGui = true
    var showHelp = false
    var listDevices = false
    var runPreview = false
    var previewSeconds = 5
    var backend: BackendChoice = .auto
}

private func eprintln(_ text: String) {
    FileHandle.standardError.write(Data((text + "\n").utf8))
}

private func printUsage(program: String) {
    print("""
Usage: \(program) [options]

Options:
  --gui               Run the graphical interface (default)
  --list              List connected devices
  --preview [sec]     Run a CLI preview for N seconds
  --backend [v1|v2]   Force a specific backend
  --help, -h          Show this help
""")
}

private func parsePositiveInt(_ text: String) -> Int? {
    guard let parsed = Int(text), parsed > 0 else { return nil }
    return parsed
}

private func parseBackendChoice(_ raw: String) -> BackendChoice? {
    switch raw {
    case "auto": return .auto
    case "v1": return .v1
    case "v2": return .v2
    default: return nil
    }
}

private func coerceInt(_ value: Any?) -> Int? {
    if let v = value as? Int { return v }
    if let v = value as? NSNumber { return v.intValue }
    if let v = value as? String { return Int(v) }
    return nil
}

private func parseOptions(arguments: [String]) -> CliOptions? {
    var options = CliOptions()
    var i = 0

    while i < arguments.count {
        let arg = arguments[i]

        if arg == "--help" || arg == "-h" {
            options.showHelp = true
            options.runGui = false
            i += 1
            continue
        }

        if arg == "--gui" {
            options.runGui = true
            i += 1
            continue
        }

        if arg == "--list" {
            options.listDevices = true
            options.runGui = false
            i += 1
            continue
        }

        if arg == "--preview" {
            options.runPreview = true
            options.runGui = false
            if i + 1 < arguments.count, !arguments[i + 1].hasPrefix("-") {
                guard let secs = parsePositiveInt(arguments[i + 1]) else {
                    eprintln("Invalid preview seconds: \(arguments[i + 1])")
                    return nil
                }
                options.previewSeconds = secs
                i += 2
                continue
            }
            i += 1
            continue
        }

        if arg.hasPrefix("--preview=") {
            options.runPreview = true
            options.runGui = false
            let value = String(arg.dropFirst("--preview=".count))
            guard let secs = parsePositiveInt(value) else {
                eprintln("Invalid preview seconds: \(arg)")
                return nil
            }
            options.previewSeconds = secs
            i += 1
            continue
        }

        if arg == "--backend" {
            guard i + 1 < arguments.count else {
                eprintln("--backend expects one value: auto, v1, or v2")
                return nil
            }
            guard let choice = parseBackendChoice(arguments[i + 1]) else {
                eprintln("Invalid backend value: \(arguments[i + 1])")
                return nil
            }
            options.backend = choice
            i += 2
            continue
        }

        // Unknown flags are ignored for GUI launches to preserve app behavior.
        i += 1
    }

    return options
}

private func generationMatches(choice: BackendChoice, generation: Int) -> Bool {
    switch choice {
    case .auto:
        return true
    case .v1:
        return generation == 1
    case .v2:
        return generation == 2
    }
}

private func generationLabel(_ generation: Int) -> String {
    generation == 2 ? "Kinect v2" : "Kinect v1"
}

private func runCli(options: CliOptions) -> Int32 {
    let bridge = KinectBridge.sharedInstance()
    let allDevices = bridge.discoverDevices()
    let devices = allDevices.filter { entry in
        guard let generation = coerceInt(entry["generation"]) else { return false }
        return generationMatches(choice: options.backend, generation: generation)
    }

    if options.listDevices || options.runPreview {
        if devices.isEmpty {
            print("No matching Kinect devices found.")
        } else {
            print("Devices:")
            for entry in devices {
                let generation = coerceInt(entry["generation"]) ?? 1
                let serial = (entry["serial"] as? String) ?? ""
                let name = (entry["name"] as? String) ?? generationLabel(generation)
                print("  - \(name) (\(generationLabel(generation))) serial: \(serial)")
            }
        }
    }

    if options.runPreview {
        guard let first = devices.first else {
            eprintln("No device available for preview.")
            return 1
        }
        guard let generation = coerceInt(first["generation"]),
              let serial = first["serial"] as? String else {
            eprintln("Selected device info is invalid.")
            return 1
        }

        guard bridge.openDevice(withGeneration: generation, serial: serial) else {
            let err = bridge.lastError()
            eprintln("Failed to open device for preview. \(err)")
            return 1
        }

        bridge.setStreamType(0)
        bridge.startStream()

        let end = Date().addingTimeInterval(TimeInterval(options.previewSeconds))
        var colorFrames = 0
        var depthFrames = 0
        var irFrames = 0

        while Date() < end {
            if let frame = bridge.pollFrame() {
                if frame.rgbData.count > 0 { colorFrames += 1 }
                if frame.depthData.count > 0 { depthFrames += 1 }
                if frame.irData.count > 0 { irFrames += 1 }
            }
            usleep(10_000)
        }

        bridge.stopStream()
        print("Preview finished:")
        print("  color frames: \(colorFrames)")
        print("  depth frames: \(depthFrames)")
        print("  infrared frames: \(irFrames)")
    }

    return 0
}

private func resolveLaunchMode() -> Int32? {
    let program = (CommandLine.arguments.first as NSString?)?.lastPathComponent ?? "macKinect"
    let args = Array(CommandLine.arguments.dropFirst())
    guard let options = parseOptions(arguments: args) else {
        printUsage(program: program)
        return 2
    }

    if options.showHelp {
        printUsage(program: program)
        return 0
    }

    if !options.runGui {
        return runCli(options: options)
    }

    return nil
}

@main
enum KinectEntrypoint {
    static func main() {
        if let exitCode = resolveLaunchMode() {
            Darwin.exit(exitCode)
        }
        KinectAppUI.main()
    }
}
