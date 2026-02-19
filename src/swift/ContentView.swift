import SwiftUI
import CoreGraphics
import AppKit

struct ContentView: View {
    @StateObject private var manager = KinectManager()

    @State private var rgbImage: CGImage?
    @State private var irImage: CGImage?
    @State private var depthImage: CGImage?

    private let frameTimer = Timer.publish(every: 0.033, on: .main, in: .common).autoconnect()
    private let supportURL = URL(string: "https://buymeacoffee.com/einnovoeg")!

    var body: some View {
        ZStack {
            LinearGradient(
                colors: [Color(red: 0.05, green: 0.09, blue: 0.14), Color(red: 0.02, green: 0.03, blue: 0.05)],
                startPoint: .topLeading,
                endPoint: .bottomTrailing
            )
            .ignoresSafeArea()

            HStack(spacing: 16) {
                controlsPanel
                    .frame(width: 360)

                VStack(spacing: 12) {
                    Text("Live Preview")
                        .font(.title2.weight(.semibold))
                        .foregroundStyle(.white.opacity(0.95))
                        .frame(maxWidth: .infinity, alignment: .leading)

                    previewImage
                        .frame(maxWidth: .infinity, maxHeight: .infinity)
                        .background(Color.black)
                        .clipShape(RoundedRectangle(cornerRadius: 14, style: .continuous))
                        .overlay(
                            RoundedRectangle(cornerRadius: 14, style: .continuous)
                                .stroke(Color.white.opacity(0.18), lineWidth: 1)
                        )

                    streamSelector
                }
            }
        }
        .padding(16)
        .tint(Color(red: 0.17, green: 0.76, blue: 0.90))
        .frame(minWidth: 1200, minHeight: 760)
        .onReceive(frameTimer) { _ in
            guard let frame = manager.pollFrame() else { return }
            rgbImage = frame.rgbData.rgbCGImage(width: frame.width, height: frame.height)
            irImage = frame.irData.grayCGImage(width: frame.width, height: frame.height)
            depthImage = frame.depthData.depthCGImage(width: frame.width, height: frame.height)
        }
    }

    private var streamSelector: some View {
        Picker("Stream", selection: Binding(
            get: { manager.streamType },
            set: { manager.streamType = $0 }
        )) {
            ForEach(KinectStreamType.allCases) { stream in
                Text(stream.title).tag(stream)
            }
        }
        .pickerStyle(.segmented)
        .padding(8)
        .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 12, style: .continuous))
    }

    @ViewBuilder
    private var previewImage: some View {
        switch manager.streamType {
        case .rgb:
            if let rgbImage {
                Image(decorative: rgbImage, scale: 1.0)
                    .resizable()
                    .aspectRatio(contentMode: .fit)
            } else {
                PlaceholderView(title: "RGB Stream")
            }
        case .ir:
            if let irImage {
                Image(decorative: irImage, scale: 1.0)
                    .resizable()
                    .aspectRatio(contentMode: .fit)
            } else {
                PlaceholderView(title: "Infrared Stream")
            }
        case .depth:
            if let depthImage {
                Image(decorative: depthImage, scale: 1.0)
                    .resizable()
                    .aspectRatio(contentMode: .fit)
            } else {
                PlaceholderView(title: "Depth Stream")
            }
        }
    }

    private var controlsPanel: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 12) {
                GroupBox {
                    VStack(alignment: .leading, spacing: 8) {
                        Text("macKinect")
                            .font(.system(size: 28, weight: .bold, design: .rounded))
                        Text("Control Kinect camera, audio, and depth streams on macOS.")
                            .font(.callout)
                            .foregroundStyle(.secondary)
                        Link("Support development", destination: supportURL)
                            .font(.caption.weight(.semibold))
                    }
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(.vertical, 2)
                }
                .groupBoxStyle(.automatic)

                GroupBox("Devices") {
                    VStack(alignment: .leading, spacing: 8) {
                        Picker("Device", selection: $manager.selectedDeviceID) {
                            ForEach(manager.devices) { device in
                                Text("\(device.generationLabel) - \(device.serial)").tag(device.id)
                            }
                        }
                        .labelsHidden()
                        .frame(maxWidth: .infinity)

                        HStack {
                            Button("Refresh") { manager.refreshDevices() }
                                .buttonStyle(.bordered)
                            Button("Connect") { manager.connectSelectedDevice() }
                                .buttonStyle(.borderedProminent)
                                .disabled(manager.selectedDeviceID.isEmpty)
                            Button(manager.streaming ? "Stop Stream" : "Start Stream") {
                                manager.streaming ? manager.stopStreaming() : manager.startStreaming()
                            }
                            .buttonStyle(.borderedProminent)
                            .disabled(!manager.connected)
                        }
                    }
                }

                GroupBox("Camera + Motor") {
                    VStack(alignment: .leading, spacing: 10) {
                        Toggle("Mirror", isOn: Binding(get: { manager.mirror }, set: manager.setMirror))
                        Toggle("Auto Exposure", isOn: Binding(get: { manager.autoExposure }, set: manager.setAutoExposure))
                        Toggle("Auto White Balance", isOn: Binding(get: { manager.autoWhiteBalance }, set: manager.setAutoWhiteBalance))
                        Toggle("Near Mode", isOn: Binding(get: { manager.nearMode }, set: manager.setNearMode))
                            .disabled(!manager.supportsDepth)

                        HStack {
                            Text("Tilt")
                            Slider(value: Binding(
                                get: { Double(manager.tiltAngle) },
                                set: { manager.setTilt(Int($0)) }
                            ), in: -30...30, step: 1)
                            Text("\(manager.tiltAngle)°")
                                .frame(width: 45, alignment: .trailing)
                        }
                        .disabled(!manager.supportsMotor)

                        HStack {
                            Text("LED")
                            Stepper(value: Binding(
                                get: { manager.ledMode },
                                set: { manager.setLed($0) }
                            ), in: 0...6) {
                                Text("\(manager.ledMode)")
                            }
                        }
                        .disabled(!manager.supportsLed)

                        HStack {
                            Text("Exposure (\(manager.manualExposureUs) us)")
                            Slider(value: Binding(
                                get: { Double(manager.manualExposureUs) },
                                set: { manager.setManualExposure(Int($0)) }
                            ), in: 1_000...200_000, step: 1_000)
                        }

                        HStack {
                            Text("IR Brightness (\(manager.irBrightness))")
                            Slider(value: Binding(
                                get: { Double(manager.irBrightness) },
                                set: { manager.setIrBrightness(Int($0)) }
                            ), in: 1...50, step: 1)
                        }
                    }
                }

                GroupBox("Microphone") {
                    VStack(alignment: .leading, spacing: 10) {
                        Toggle("Enable Kinect microphone stream", isOn: Binding(
                            get: { manager.audioEnabled },
                            set: { manager.setAudioEnabled($0) }
                        ))
                        .disabled(!manager.supportsAudioInput)

                        HStack {
                            Text("Input level")
                            ProgressView(value: min(max(Double(manager.audioLevel), 0), 1))
                                .frame(maxWidth: .infinity)
                            Text(String(format: "%.2f", manager.audioLevel))
                                .frame(width: 50, alignment: .trailing)
                        }
                    }
                }

                GroupBox("3D Scanner") {
                    VStack(alignment: .leading, spacing: 8) {
                        Button("Capture RGB/IR/Depth + PLY") {
                            manager.captureScanBundle()
                        }
                        .disabled(!manager.connected || !manager.streaming)

                        if manager.lastCapturePointCount > 0 {
                            Text("Last point cloud: \(manager.lastCapturePointCount) points")
                                .font(.caption)
                                .foregroundColor(.secondary)
                        }

                        if !manager.lastCapturePath.isEmpty {
                            Text(manager.lastCapturePath)
                                .font(.caption2)
                                .foregroundColor(.secondary)
                                .lineLimit(3)

                            Button("Reveal in Finder") {
                                NSWorkspace.shared.selectFile(nil, inFileViewerRootedAtPath: manager.lastCapturePath)
                            }
                            .buttonStyle(.link)
                        }
                    }
                }

                GroupBox("System Integration") {
                    VStack(alignment: .leading, spacing: 8) {
                        Toggle("Use Kinect as system camera/microphone", isOn: Binding(
                            get: { manager.publishToSystem },
                            set: { manager.setSystemPublish($0) }
                        ))
                        Button(manager.systemIntegrationInstallInProgress ? "Installing..." : "Install System Integration") {
                            manager.installSystemIntegration()
                        }
                        .disabled(manager.systemIntegrationInstallInProgress)
                        Button("Re-check integration status") {
                            manager.refreshSystemIntegrationStatus()
                        }
                        .buttonStyle(.link)
                        Text(manager.systemPublishNote)
                            .font(.caption)
                            .foregroundColor(.secondary)
                        if !manager.systemIntegrationInstallResult.isEmpty {
                            Text(manager.systemIntegrationInstallResult)
                                .font(.caption)
                                .foregroundColor(.secondary)
                        }
                    }
                }

                GroupBox("Status") {
                    VStack(alignment: .leading, spacing: 6) {
                        Text(manager.connected ? "Connected" : "Disconnected")
                            .foregroundColor(manager.connected ? .green : .red)
                        Text(manager.status)
                            .font(.callout)
                            .foregroundColor(.secondary)
                    }
                    .frame(maxWidth: .infinity, alignment: .leading)
                }
            }
        }
        .padding(2)
    }
}

private struct PlaceholderView: View {
    let title: String

    var body: some View {
        Rectangle()
            .fill(Color.black)
            .overlay(
                Text(title)
                    .foregroundColor(.white.opacity(0.8))
                    .font(.title3)
            )
    }
}

private extension Data {
    func rgbCGImage(width: Int, height: Int) -> CGImage? {
        guard width > 0, height > 0 else { return nil }
        guard count >= width * height * 3 else { return nil }

        guard let provider = CGDataProvider(data: self as CFData) else { return nil }
        return CGImage(
            width: width,
            height: height,
            bitsPerComponent: 8,
            bitsPerPixel: 24,
            bytesPerRow: width * 3,
            space: CGColorSpaceCreateDeviceRGB(),
            bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.none.rawValue),
            provider: provider,
            decode: nil,
            shouldInterpolate: false,
            intent: .defaultIntent
        )
    }

    func grayCGImage(width: Int, height: Int) -> CGImage? {
        guard width > 0, height > 0 else { return nil }
        guard count >= width * height else { return nil }

        guard let provider = CGDataProvider(data: self as CFData) else { return nil }
        return CGImage(
            width: width,
            height: height,
            bitsPerComponent: 8,
            bitsPerPixel: 8,
            bytesPerRow: width,
            space: CGColorSpaceCreateDeviceGray(),
            bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.none.rawValue),
            provider: provider,
            decode: nil,
            shouldInterpolate: false,
            intent: .defaultIntent
        )
    }

    func depthCGImage(width: Int, height: Int) -> CGImage? {
        guard width > 0, height > 0 else { return nil }
        guard count >= width * height * 2 else { return nil }

        let sampleCount = width * height
        var gray = [UInt8](repeating: 0, count: sampleCount)
        withUnsafeBytes { rawBuffer in
            let depth = rawBuffer.bindMemory(to: UInt16.self)
            for i in 0..<sampleCount {
                let d = depth[i]
                if d == 0 {
                    gray[i] = 0
                } else {
                    let t = Swift.max(0.0, Swift.min(1.0, (Double(d) - 400.0) / 5600.0))
                    gray[i] = UInt8((1.0 - t) * 255.0)
                }
            }
        }

        let grayData = Data(gray)
        return grayData.grayCGImage(width: width, height: height)
    }
}
