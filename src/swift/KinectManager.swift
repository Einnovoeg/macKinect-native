import Foundation
import SwiftUI
import AppKit

enum KinectStreamType: Int, CaseIterable, Identifiable {
    case rgb = 0
    case ir = 1
    case depth = 2

    var id: Int { rawValue }

    var title: String {
        switch self {
        case .rgb: return "RGB"
        case .ir: return "Infrared"
        case .depth: return "Depth"
        }
    }
}

struct KinectDeviceRecord: Identifiable {
    let generation: Int
    let serial: String
    let name: String

    var id: String { "\(generation):\(serial)" }
    var generationLabel: String { generation == 2 ? "Kinect v2" : "Kinect v1" }
}

final class KinectManager: ObservableObject {
    @Published var devices: [KinectDeviceRecord] = []
    @Published var selectedDeviceID: String = ""

    @Published var connected = false
    @Published var streaming = false
    @Published var status = "Idle"

    @Published var streamType: KinectStreamType = .rgb {
        didSet {
            bridge?.setStreamType(streamType.rawValue)
        }
    }

    @Published var tiltAngle = 0
    @Published var ledMode = 1
    @Published var mirror = true
    @Published var autoExposure = true
    @Published var autoWhiteBalance = true
    @Published var nearMode = false
    @Published var manualExposureUs = 33333
    @Published var irBrightness = 20

    @Published var audioEnabled = false
    @Published var audioLevel: Float = 0

    @Published var supportsMotor = false
    @Published var supportsLed = false
    @Published var supportsAudioInput = false
    @Published var supportsDepth = false
    @Published var supportsIr = false

    @Published var publishToSystem = false
    @Published var systemPublishNote = "Not active"
    @Published var systemIntegrationInstallInProgress = false
    @Published var systemIntegrationInstallResult = ""
    @Published var lastCapturePath = ""
    @Published var lastCapturePointCount = 0

    private var bridge: KinectBridge?
    private var currentDevice: KinectDeviceRecord?
    private var lastFrame: KinectFrame?
    private let systemAudioHalDisplayName = "KinectAudioHAL.driver"
    private let systemCameraDalDisplayName = "KinectCameraDAL.plugin"
    private let systemAudioHalPath = "/Library/Audio/Plug-Ins/HAL/KinectAudioHAL.driver"
    private let systemCameraPluginPath = "/Library/CoreMediaIO/Plug-Ins/DAL/KinectCameraDAL.plugin"

    init() {
        bridge = KinectBridge.sharedInstance()
        refreshDevices()
        refreshSystemIntegrationStatus()
    }

    private func coerceInt(_ value: Any?) -> Int? {
        if let v = value as? Int { return v }
        if let v = value as? NSNumber { return v.intValue }
        if let v = value as? String { return Int(v) }
        return nil
    }

    func refreshDevices() {
        guard let bridge else { return }
        let records = (bridge.discoverDevices() as? [[String: Any]] ?? []).compactMap { dict -> KinectDeviceRecord? in
            guard
                let generation = coerceInt(dict["generation"]),
                let serial = dict["serial"] as? String,
                let name = dict["name"] as? String
            else { return nil }
            return KinectDeviceRecord(generation: generation, serial: serial, name: name)
        }
        devices = records

        if let selected = devices.first(where: { $0.id == selectedDeviceID }) {
            selectedDeviceID = selected.id
        } else {
            selectedDeviceID = devices.first?.id ?? ""
        }
    }

    func connectSelectedDevice() {
        guard let bridge else { return }
        guard let selected = devices.first(where: { $0.id == selectedDeviceID }) else {
            status = "No device selected."
            return
        }

        if !bridge.openDevice(withGeneration: selected.generation, serial: selected.serial) {
            connected = false
            streaming = false
            let hint = bridge.lastError().trimmingCharacters(in: .whitespacesAndNewlines)
            if hint.isEmpty {
                status = "Failed to open \(selected.generationLabel) \(selected.serial)."
            } else {
                status = "Failed to open \(selected.generationLabel) \(selected.serial): \(hint)"
            }
            return
        }

        connected = true
        currentDevice = selected
        status = "Connected to \(selected.generationLabel) \(selected.serial)"
        applyCurrentSettings()
        updateCapabilities()
    }

    func disconnect() {
        bridge?.stopStream()
        connected = false
        streaming = false
        audioEnabled = false
        currentDevice = nil
        lastFrame = nil
        status = "Disconnected."
    }

    func startStreaming() {
        guard connected else {
            status = "Connect a device first."
            return
        }
        bridge?.startStream()
        streaming = bridge?.isStreaming() ?? false
        status = streaming ? "Streaming started." : "Could not start stream."
    }

    func stopStreaming() {
        bridge?.stopStream()
        streaming = false
        status = "Streaming stopped."
    }

    func pollFrame() -> KinectFrame? {
        guard streaming else { return nil }
        let frame = bridge?.pollFrame()
        if let frame {
            lastFrame = frame
        }
        audioLevel = bridge?.audioLevel() ?? 0
        return frame
    }

    func applyCurrentSettings() {
        guard bridge != nil else { return }
        bridge?.setStreamType(streamType.rawValue)
        bridge?.setTilt(tiltAngle)
        bridge?.setLed(ledMode)
        bridge?.setMirror(mirror)
        bridge?.setAutoExposure(autoExposure)
        bridge?.setAutoWhiteBalance(autoWhiteBalance)
        bridge?.setNearMode(nearMode)
        bridge?.setManualExposureUs(manualExposureUs)
        bridge?.setIrBrightness(irBrightness)
    }

    func setTilt(_ value: Int) {
        tiltAngle = max(-30, min(30, value))
        bridge?.setTilt(tiltAngle)
    }

    func setLed(_ value: Int) {
        ledMode = max(0, min(6, value))
        bridge?.setLed(ledMode)
    }

    func setMirror(_ value: Bool) {
        mirror = value
        bridge?.setMirror(value)
    }

    func setAutoExposure(_ value: Bool) {
        autoExposure = value
        bridge?.setAutoExposure(value)
    }

    func setAutoWhiteBalance(_ value: Bool) {
        autoWhiteBalance = value
        bridge?.setAutoWhiteBalance(value)
    }

    func setNearMode(_ value: Bool) {
        nearMode = value
        bridge?.setNearMode(value)
    }

    func setManualExposure(_ value: Int) {
        manualExposureUs = max(1000, min(200000, value))
        bridge?.setManualExposureUs(manualExposureUs)
    }

    func setIrBrightness(_ value: Int) {
        irBrightness = max(1, min(50, value))
        bridge?.setIrBrightness(irBrightness)
    }

    func setAudioEnabled(_ value: Bool) {
        let applied = bridge?.setAudioEnabled(value) ?? false
        audioEnabled = applied
        status = applied ? "Audio enabled." : "Audio unavailable on this device/session."
    }

    func setSystemPublish(_ value: Bool) {
        refreshSystemIntegrationStatus(requestEnable: value)
    }

    func installSystemIntegration() {
        guard !systemIntegrationInstallInProgress else { return }

        let fileManager = FileManager.default
        guard let pluginRoot = Bundle.main.builtInPlugInsPath else {
            systemIntegrationInstallResult = "App bundle plugins path is unavailable."
            return
        }

        let bundledAudioHalPath = (pluginRoot as NSString).appendingPathComponent("HAL/\(systemAudioHalDisplayName)")
        if !fileManager.fileExists(atPath: bundledAudioHalPath) {
            systemIntegrationInstallResult = "Bundled audio HAL plugin not found in app package."
            refreshSystemIntegrationStatus()
            return
        }

        let bundledCameraDalPath = (pluginRoot as NSString).appendingPathComponent("DAL/\(systemCameraDalDisplayName)")
        let includeCameraDal = fileManager.fileExists(atPath: bundledCameraDalPath)
        let appFrameworksPath = Bundle.main.privateFrameworksPath

        systemIntegrationInstallInProgress = true
        systemIntegrationInstallResult = "Installing system integration components..."

        DispatchQueue.global(qos: .userInitiated).async {
            let result = Self.runSystemIntegrationInstall(
                audioHalSourcePath: bundledAudioHalPath,
                cameraDalSourcePath: includeCameraDal ? bundledCameraDalPath : nil,
                appFrameworksPath: appFrameworksPath
            )

            DispatchQueue.main.async {
                self.systemIntegrationInstallInProgress = false
                self.systemIntegrationInstallResult = result.message
                self.refreshSystemIntegrationStatus(requestEnable: self.publishToSystem)
            }
        }
    }

    func updateCapabilities() {
        guard let caps = bridge?.deviceCapabilities() as? [String: Any] else { return }
        supportsMotor = caps["supportsMotor"] as? Bool ?? false
        supportsLed = caps["supportsLed"] as? Bool ?? false
        supportsAudioInput = caps["supportsAudioInput"] as? Bool ?? false
        supportsDepth = caps["supportsDepth"] as? Bool ?? false
        supportsIr = caps["supportsIr"] as? Bool ?? false
    }

    func refreshSystemIntegrationStatus() {
        refreshSystemIntegrationStatus(requestEnable: publishToSystem)
    }

    private func refreshSystemIntegrationStatus(requestEnable: Bool) {
        let fileManager = FileManager.default
        let bundledAudioHalPath = Bundle.main.bundlePath + "/Contents/PlugIns/HAL/\(systemAudioHalDisplayName)"

        let bundledAudioHalAvailable = fileManager.fileExists(atPath: bundledAudioHalPath)
        let installedAudioHalAvailable = fileManager.fileExists(atPath: systemAudioHalPath)
        let installedCameraPluginAvailable = fileManager.fileExists(atPath: systemCameraPluginPath)

        let hasAnySystemIntegration = installedAudioHalAvailable || installedCameraPluginAvailable
        publishToSystem = requestEnable && hasAnySystemIntegration

        if !bundledAudioHalAvailable {
            systemPublishNote = "Bundled audio HAL plugin is missing from this app package."
            return
        }

        if !installedAudioHalAvailable && !installedCameraPluginAvailable {
            systemPublishNote = "System integration is not installed yet. Click 'Install System Integration' to install bundled components."
            return
        }

        if installedAudioHalAvailable && !installedCameraPluginAvailable {
            systemPublishNote = publishToSystem
                ? "System microphone integration enabled. Camera integration requires Kinect camera DAL plugin."
                : "System microphone integration is ready. Camera integration requires Kinect camera DAL plugin."
            return
        }

        if !installedAudioHalAvailable && installedCameraPluginAvailable {
            systemPublishNote = publishToSystem
                ? "System camera integration enabled. Microphone integration requires audio HAL installation."
                : "System camera integration is ready. Microphone integration requires audio HAL installation."
            return
        }

        systemPublishNote = publishToSystem ? "System camera/microphone publishing enabled." : "System camera/microphone integration is ready."
    }

    private struct PrivilegedInstallResult {
        let success: Bool
        let message: String
    }

    private static func shellQuote(_ value: String) -> String {
        "'" + value.replacingOccurrences(of: "'", with: "'\\''") + "'"
    }

    private static func runSystemIntegrationInstall(
        audioHalSourcePath: String,
        cameraDalSourcePath: String?,
        appFrameworksPath: String?
    ) -> PrivilegedInstallResult {
        var commands: [String] = []
        commands.append("/usr/bin/ditto \(shellQuote(audioHalSourcePath)) \(shellQuote("/Library/Audio/Plug-Ins/HAL/KinectAudioHAL.driver"))")

        if let cameraDalSourcePath {
            let stagedRoot = "/tmp/KinectCameraDAL-install.$$"
            let stagedPlugin = "\(stagedRoot)/KinectCameraDAL.plugin"
            let stagedFrameworks = "\(stagedPlugin)/Frameworks"
            let stagedPluginBinary = "\(stagedPlugin)/KinectCameraDAL"
            let stagedLibfreenect = "\(stagedFrameworks)/libfreenect.0.7.5.dylib"
            let stagedLibfreenect2 = "\(stagedFrameworks)/libfreenect2.0.2.0.dylib"
            let stagedLibUsb = "\(stagedFrameworks)/libusb-1.0.0.dylib"
            let stagedLibTurboJpeg = "\(stagedFrameworks)/libturbojpeg.0.4.0.dylib"
            let systemDalPath = "/Library/CoreMediaIO/Plug-Ins/DAL/KinectCameraDAL.plugin"

            commands.append("/bin/rm -rf \(shellQuote(stagedRoot))")
            commands.append("/bin/mkdir -p \(shellQuote(stagedRoot))")
            commands.append("/usr/bin/ditto \(shellQuote(cameraDalSourcePath)) \(shellQuote(stagedPlugin))")
            if let appFrameworksPath {
                commands.append("/bin/mkdir -p \(shellQuote(stagedFrameworks))")
                let frameworkCopyScript = """
if [ -d \(shellQuote(appFrameworksPath)) ]; then \
for pattern in libfreenect.0*.dylib libfreenect2*.dylib libusb-1.0*.dylib libturbojpeg*.dylib; do \
for lib in \(shellQuote(appFrameworksPath))/$pattern; do \
[ -e "$lib" ] || continue; \
/usr/bin/ditto "$lib" \(shellQuote(stagedFrameworks))/$(basename "$lib"); \
done; \
done; \
fi
"""
                commands.append(frameworkCopyScript)
            }
            commands.append("if [ -f \(shellQuote(stagedPluginBinary)) ]; then /usr/bin/install_name_tool -add_rpath @loader_path/Frameworks \(shellQuote(stagedPluginBinary)) >/dev/null 2>&1 || true; fi")
            commands.append("if [ -f \(shellQuote(stagedPluginBinary)) ]; then /usr/bin/install_name_tool -change /opt/homebrew/opt/libusb/lib/libusb-1.0.0.dylib @rpath/libusb-1.0.0.dylib \(shellQuote(stagedPluginBinary)) || true; fi")
            commands.append("if [ -f \(shellQuote(stagedPluginBinary)) ]; then /usr/bin/install_name_tool -change /opt/homebrew/opt/jpeg-turbo/lib/libturbojpeg.0.dylib @rpath/libturbojpeg.0.dylib \(shellQuote(stagedPluginBinary)) || true; fi")
            commands.append("if [ -f \(shellQuote(stagedLibfreenect)) ]; then /usr/bin/install_name_tool -id @loader_path/libfreenect.0.7.5.dylib \(shellQuote(stagedLibfreenect)) || true; fi")
            commands.append("if [ -f \(shellQuote(stagedLibfreenect)) ]; then /usr/bin/install_name_tool -change @executable_path/../Frameworks/libusb-1.0.0.dylib @loader_path/libusb-1.0.0.dylib \(shellQuote(stagedLibfreenect)) || true; fi")
            commands.append("if [ -f \(shellQuote(stagedLibfreenect2)) ]; then /usr/bin/install_name_tool -id @loader_path/libfreenect2.0.2.0.dylib \(shellQuote(stagedLibfreenect2)) || true; fi")
            commands.append("if [ -f \(shellQuote(stagedLibfreenect2)) ]; then /usr/bin/install_name_tool -change @executable_path/../Frameworks/libusb-1.0.0.dylib @loader_path/libusb-1.0.0.dylib \(shellQuote(stagedLibfreenect2)) || true; fi")
            commands.append("if [ -f \(shellQuote(stagedLibfreenect2)) ]; then /usr/bin/install_name_tool -change @executable_path/../Frameworks/libturbojpeg.0.dylib @loader_path/libturbojpeg.0.dylib \(shellQuote(stagedLibfreenect2)) || true; fi")
            commands.append("if [ -f \(shellQuote(stagedLibUsb)) ]; then /usr/bin/install_name_tool -id @loader_path/libusb-1.0.0.dylib \(shellQuote(stagedLibUsb)) || true; fi")
            commands.append("if [ -f \(shellQuote(stagedLibTurboJpeg)) ]; then /usr/bin/install_name_tool -id @loader_path/libturbojpeg.0.4.0.dylib \(shellQuote(stagedLibTurboJpeg)) || true; fi")
            commands.append("/usr/bin/codesign --force --deep --sign - --timestamp=none \(shellQuote(stagedPlugin))")
            commands.append("/usr/bin/ditto \(shellQuote(stagedPlugin)) \(shellQuote(systemDalPath))")
            commands.append("/usr/bin/codesign --force --deep --sign - --timestamp=none \(shellQuote(systemDalPath))")
            commands.append("/bin/rm -rf \(shellQuote(stagedRoot))")
        }

        commands.append("/usr/bin/killall coreaudiod >/dev/null 2>&1 || true")
        commands.append("/usr/bin/killall VDCAssistant AppleCameraAssistant >/dev/null 2>&1 || true")
        let shellCommand = commands.joined(separator: "; ")

        let escapedCommand = shellCommand
            .replacingOccurrences(of: "\\", with: "\\\\")
            .replacingOccurrences(of: "\"", with: "\\\"")
        let appleScriptSource = "do shell script \"\(escapedCommand)\" with administrator privileges"

        guard let script = NSAppleScript(source: appleScriptSource) else {
            return PrivilegedInstallResult(success: false, message: "Could not create privileged installer request.")
        }

        var errorInfo: NSDictionary?
        _ = script.executeAndReturnError(&errorInfo)
        if let errorInfo {
            let code = (errorInfo[NSAppleScript.errorNumber] as? Int) ?? 0
            let message = (errorInfo[NSAppleScript.errorMessage] as? String) ?? "Unknown install error."
            if code == -128 {
                return PrivilegedInstallResult(success: false, message: "Installation was canceled.")
            }
            return PrivilegedInstallResult(success: false, message: "Install failed: \(message)")
        }

        return PrivilegedInstallResult(success: true, message: "System integration installed. You can now enable system publish.")
    }

    func captureScanBundle() {
        guard connected else {
            status = "Connect a device first."
            return
        }

        guard let frame = lastFrame ?? bridge?.pollFrame() else {
            status = "No frame available yet."
            return
        }
        lastFrame = frame

        let width = frame.width
        let height = frame.height
        guard width > 0, height > 0 else {
            status = "Invalid frame dimensions."
            return
        }

        let pixelCount = width * height
        let expectedRgbBytes = pixelCount * 3
        let expectedDepthBytes = pixelCount * MemoryLayout<UInt16>.size
        let expectedIrBytes = pixelCount

        let captureDir = captureRootDirectory().appendingPathComponent(Self.captureTimestamp(), isDirectory: true)

        do {
            try FileManager.default.createDirectory(at: captureDir, withIntermediateDirectories: true)

            var wroteColor = false
            var wroteDepth = false
            var wroteIr = false
            var points = 0

            if frame.rgbData.count >= expectedRgbBytes {
                try writeColorPPM(frame.rgbData, width: width, height: height, to: captureDir.appendingPathComponent("color.ppm"))
                wroteColor = true
            }

            if frame.depthData.count >= expectedDepthBytes {
                try writeDepthPGM(frame.depthData, width: width, height: height, to: captureDir.appendingPathComponent("depth_mm.pgm"))
                wroteDepth = true
                points = try writePointCloudPLY(
                    depthData: frame.depthData,
                    rgbData: frame.rgbData,
                    irData: frame.irData,
                    width: width,
                    height: height,
                    generation: currentDevice?.generation ?? 1,
                    to: captureDir.appendingPathComponent("scan.ply")
                )
            }

            if frame.irData.count >= expectedIrBytes {
                try writeIrPGM(frame.irData, width: width, height: height, to: captureDir.appendingPathComponent("infrared.pgm"))
                wroteIr = true
            }

            lastCapturePath = captureDir.path
            lastCapturePointCount = points
            status = "Capture saved (\(wroteColor ? "RGB" : "-")/\(wroteDepth ? "Depth" : "-")/\(wroteIr ? "IR" : "-"), points: \(points)"
        } catch {
            status = "Capture failed: \(error.localizedDescription)"
        }
    }

    private func captureRootDirectory() -> URL {
        FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent("Pictures", isDirectory: true)
            .appendingPathComponent("KinectCaptures", isDirectory: true)
    }

    private func writeColorPPM(_ data: Data, width: Int, height: Int, to url: URL) throws {
        let pixelCount = width * height
        let expected = pixelCount * 3
        var output = Data("P6\n\(width) \(height)\n255\n".utf8)
        output.append(data.prefix(expected))
        try output.write(to: url, options: .atomic)
    }

    private func writeIrPGM(_ data: Data, width: Int, height: Int, to url: URL) throws {
        let pixelCount = width * height
        let expected = pixelCount
        var output = Data("P5\n\(width) \(height)\n255\n".utf8)
        output.append(data.prefix(expected))
        try output.write(to: url, options: .atomic)
    }

    private func writeDepthPGM(_ data: Data, width: Int, height: Int, to url: URL) throws {
        let pixelCount = width * height
        let expected = pixelCount * MemoryLayout<UInt16>.size
        guard data.count >= expected else {
            throw NSError(domain: "KinectManager", code: 1001, userInfo: [NSLocalizedDescriptionKey: "Depth frame is incomplete"])
        }

        var bytes = [UInt8](repeating: 0, count: expected)
        data.withUnsafeBytes { rawBuffer in
            let depth = rawBuffer.bindMemory(to: UInt16.self)
            for i in 0..<pixelCount {
                let value = UInt16(littleEndian: depth[i])
                bytes[i * 2] = UInt8((value >> 8) & 0xFF)
                bytes[i * 2 + 1] = UInt8(value & 0xFF)
            }
        }

        var output = Data("P5\n\(width) \(height)\n65535\n".utf8)
        output.append(contentsOf: bytes)
        try output.write(to: url, options: .atomic)
    }

    private func writePointCloudPLY(
        depthData: Data,
        rgbData: Data,
        irData: Data,
        width: Int,
        height: Int,
        generation: Int,
        to url: URL
    ) throws -> Int {
        let pixelCount = width * height
        let expectedDepthBytes = pixelCount * MemoryLayout<UInt16>.size
        guard depthData.count >= expectedDepthBytes else {
            throw NSError(domain: "KinectManager", code: 1002, userInfo: [NSLocalizedDescriptionKey: "Depth frame is incomplete"])
        }

        let intrinsics = pointCloudIntrinsics(width: width, height: height, generation: generation)
        let hasRgb = rgbData.count >= pixelCount * 3
        let hasIr = irData.count >= pixelCount

        let rgbBytes: [UInt8] = hasRgb ? Array(rgbData.prefix(pixelCount * 3)) : []
        let irBytes: [UInt8] = hasIr ? Array(irData.prefix(pixelCount)) : []

        var valid = 0
        var body = String()
        body.reserveCapacity(pixelCount * 26)

        depthData.withUnsafeBytes { rawBuffer in
            let depth = rawBuffer.bindMemory(to: UInt16.self)
            for y in 0..<height {
                for x in 0..<width {
                    let index = y * width + x
                    let d = UInt16(littleEndian: depth[index])
                    if d < 350 || d > 6000 {
                        continue
                    }

                    let z = Double(d) / 1000.0
                    let worldX = (Double(x) - intrinsics.cx) / intrinsics.fx * z
                    let worldY = (Double(y) - intrinsics.cy) / intrinsics.fy * z

                    let r: UInt8
                    let g: UInt8
                    let b: UInt8
                    if hasRgb {
                        let rgbIndex = index * 3
                        r = rgbBytes[rgbIndex]
                        g = rgbBytes[rgbIndex + 1]
                        b = rgbBytes[rgbIndex + 2]
                    } else if hasIr {
                        let v = irBytes[index]
                        r = v
                        g = v
                        b = v
                    } else {
                        let t = min(max((Double(d) - 350.0) / 5650.0, 0.0), 1.0)
                        let v = UInt8((1.0 - t) * 255.0)
                        r = v
                        g = v
                        b = v
                    }

                    body += "\(worldX) \(worldY) \(z) \(r) \(g) \(b)\n"
                    valid += 1
                }
            }
        }

        let header = """
        ply
        format ascii 1.0
        element vertex \(valid)
        property float x
        property float y
        property float z
        property uchar red
        property uchar green
        property uchar blue
        end_header

        """
        try (header + body).write(to: url, atomically: true, encoding: .ascii)
        return valid
    }

    private func pointCloudIntrinsics(width: Int, height: Int, generation: Int) -> (fx: Double, fy: Double, cx: Double, cy: Double) {
        if generation == 2, width == 512, height == 424 {
            return (365.456, 365.456, 254.878, 205.395)
        }
        return (594.214, 591.040, 339.307, 242.739)
    }

    private static func captureTimestamp() -> String {
        let formatter = DateFormatter()
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.dateFormat = "yyyyMMdd-HHmmss"
        return formatter.string(from: Date())
    }
}
