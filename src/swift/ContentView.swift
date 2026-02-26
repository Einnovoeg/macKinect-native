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
            backgroundView

            HStack(alignment: .top, spacing: 16) {
                controlsPanel
                    .frame(width: 392)

                previewPanel
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
            }
            .padding(16)
        }
        .tint(Color(red: 0.12, green: 0.79, blue: 0.93))
        .frame(minWidth: 1280, minHeight: 820)
        .onReceive(frameTimer) { _ in
            guard let frame = manager.pollFrame() else { return }
            rgbImage = frame.rgbData.rgbCGImage(width: frame.width, height: frame.height)
            irImage = frame.irData.grayCGImage(width: frame.width, height: frame.height)
            depthImage = frame.depthData.depthCGImage(width: frame.width, height: frame.height)

            if let image = selectedPreviewImage {
                manager.appendPreviewFrameForRecording(image, streamType: manager.streamType)
            }
        }
    }

    private var backgroundView: some View {
        ZStack {
            LinearGradient(
                colors: [
                    Color(red: 0.04, green: 0.08, blue: 0.12),
                    Color(red: 0.02, green: 0.03, blue: 0.06)
                ],
                startPoint: .topLeading,
                endPoint: .bottomTrailing
            )
            .ignoresSafeArea()

            Circle()
                .fill(Color(red: 0.12, green: 0.79, blue: 0.93).opacity(0.12))
                .frame(width: 480, height: 480)
                .blur(radius: 30)
                .offset(x: 420, y: -250)

            Circle()
                .fill(Color.white.opacity(0.05))
                .frame(width: 360, height: 360)
                .blur(radius: 24)
                .offset(x: -460, y: 280)
        }
    }

    private var previewPanel: some View {
        VStack(spacing: 12) {
            HStack(alignment: .center, spacing: 12) {
                VStack(alignment: .leading, spacing: 8) {
                    Text("Live Preview")
                        .font(.title2.weight(.semibold))
                        .foregroundStyle(.white)

                    streamSelector
                        .frame(maxWidth: 360, alignment: .leading)
                }

                Spacer(minLength: 8)

                HStack(spacing: 8) {
                    Button {
                        captureStillImageFromPreview()
                    } label: {
                        Label("Capture Image", systemImage: "camera")
                    }
                    .buttonStyle(.bordered)
                    .disabled(selectedPreviewImage == nil)

                    Button {
                        toggleVideoRecordingFromPreview()
                    } label: {
                        Label(manager.isRecordingVideo ? "Stop Recording" : "Record Video",
                              systemImage: manager.isRecordingVideo ? "stop.circle.fill" : "record.circle")
                    }
                    .buttonStyle(.borderedProminent)
                    .disabled((!manager.isRecordingVideo && selectedPreviewImage == nil) || !manager.streaming)
                }
            }
            .padding(14)
            .background(panelCardBackground)

            ZStack(alignment: .topLeading) {
                previewImage
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                    .background(Color.black)
                    .clipShape(RoundedRectangle(cornerRadius: 16, style: .continuous))
                    .overlay(
                        RoundedRectangle(cornerRadius: 16, style: .continuous)
                            .stroke(Color.white.opacity(0.16), lineWidth: 1)
                    )

                VStack(alignment: .leading, spacing: 6) {
                    statusBadge(manager.connected ? "Connected" : "Disconnected",
                               color: manager.connected ? .green : .red)
                    if manager.streaming {
                        statusBadge("Streaming \(manager.streamType.title)", color: Color(red: 0.12, green: 0.79, blue: 0.93))
                    }
                    if manager.isRecordingVideo {
                        statusBadge("REC \(String(format: "%.1fs", manager.recordingVideoSeconds))", color: .red)
                    }
                }
                .padding(12)
            }

            HStack(spacing: 12) {
                infoTile(title: "Mic", value: manager.audioEnabled ? (manager.audioStreamActive ? "Active" : "Armed") : "Off")
                infoTile(title: "Scanner", value: manager.scannerBusy ? "Capturing" : "Ready")
                infoTile(title: "DAL", value: manager.systemCameraDalInstalled ? "Installed" : "Missing")
                infoTile(title: "HAL", value: manager.systemAudioHalInstalled ? "Installed" : "Missing")
            }
        }
    }

    private var controlsPanel: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 12) {
                cardSection {
                    VStack(alignment: .leading, spacing: 6) {
                        Text("macKinect")
                            .font(.system(size: 30, weight: .bold, design: .rounded))
                            .foregroundStyle(.white)
                        Text("Kinect v1/v2 camera, depth, infrared, audio, and scanner control for macOS.")
                            .font(.callout)
                            .foregroundStyle(.white.opacity(0.75))
                        Link("Support development", destination: supportURL)
                            .font(.caption.weight(.semibold))
                    }
                    .frame(maxWidth: .infinity, alignment: .leading)
                }

                cardSection(title: "Devices") {
                    VStack(alignment: .leading, spacing: 10) {
                        Picker("Device", selection: $manager.selectedDeviceID) {
                            if manager.devices.isEmpty {
                                Text("No Kinect detected").tag("")
                            }
                            ForEach(manager.devices) { device in
                                Text("\(device.generationLabel) • \(device.serial)").tag(device.id)
                            }
                        }
                        .labelsHidden()
                        .frame(maxWidth: .infinity)

                        HStack(spacing: 8) {
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

                cardSection(title: "Media Capture") {
                    VStack(alignment: .leading, spacing: 12) {
                        VStack(alignment: .leading, spacing: 8) {
                            Text("Picture")
                                .font(.subheadline.weight(.semibold))
                                .foregroundStyle(.white.opacity(0.9))

                            Picker("Picture Format", selection: $manager.stillImageFormat) {
                                ForEach(StillImageFormat.allCases) { format in
                                    Text(format.title).tag(format)
                                }
                            }
                            .pickerStyle(.segmented)

                            if manager.stillImageFormat.supportsQuality {
                                HStack {
                                    Text("Quality")
                                        .foregroundStyle(.white.opacity(0.75))
                                    Slider(value: $manager.stillImageQuality, in: 0.1...1.0)
                                    Text("\(Int(manager.stillImageQuality * 100))")
                                        .monospacedDigit()
                                        .frame(width: 36, alignment: .trailing)
                                        .foregroundStyle(.white.opacity(0.9))
                                }
                            }
                        }

                        Divider().overlay(Color.white.opacity(0.08))

                        VStack(alignment: .leading, spacing: 8) {
                            Text("Video")
                                .font(.subheadline.weight(.semibold))
                                .foregroundStyle(.white.opacity(0.9))
                            Text("Format: QuickTime (.mov), H.264")
                                .font(.caption)
                                .foregroundStyle(.white.opacity(0.65))

                            Picker("Video Quality", selection: $manager.videoQualityPreset) {
                                ForEach(VideoQualityPreset.allCases) { preset in
                                    Text(preset.title).tag(preset)
                                }
                            }
                            .pickerStyle(.segmented)

                            HStack(spacing: 8) {
                                Button {
                                    captureStillImageFromPreview()
                                } label: {
                                    Label("Capture Image", systemImage: "camera")
                                }
                                .buttonStyle(.bordered)
                                .disabled(selectedPreviewImage == nil)

                                Button {
                                    toggleVideoRecordingFromPreview()
                                } label: {
                                    Label(manager.isRecordingVideo ? "Stop" : "Record",
                                          systemImage: manager.isRecordingVideo ? "stop.fill" : "record.circle")
                                }
                                .buttonStyle(.borderedProminent)
                                .disabled((!manager.isRecordingVideo && selectedPreviewImage == nil) || !manager.streaming)
                            }

                            if !manager.lastVideoPath.isEmpty {
                                Text((manager.lastVideoPath as NSString).lastPathComponent)
                                    .font(.caption2)
                                    .foregroundStyle(.white.opacity(0.65))
                                    .lineLimit(2)
                            }
                        }
                    }
                }

                cardSection(title: "Camera + Motor") {
                    VStack(alignment: .leading, spacing: 10) {
                        Toggle("Mirror", isOn: Binding(get: { manager.mirror }, set: manager.setMirror))
                        Toggle("Auto Exposure", isOn: Binding(get: { manager.autoExposure }, set: manager.setAutoExposure))
                        Toggle("Auto White Balance", isOn: Binding(get: { manager.autoWhiteBalance }, set: manager.setAutoWhiteBalance))
                        Toggle("Near Mode", isOn: Binding(get: { manager.nearMode }, set: manager.setNearMode))
                            .disabled(!manager.supportsDepth)

                        settingSlider(label: "Tilt", valueText: "\(manager.tiltAngle)°") {
                            Slider(value: Binding(get: { Double(manager.tiltAngle) }, set: { manager.setTilt(Int($0)) }), in: -30...30, step: 1)
                        }
                        .disabled(!manager.supportsMotor)

                        HStack {
                            Text("LED")
                            Spacer()
                            Stepper(value: Binding(get: { manager.ledMode }, set: { manager.setLed($0) }), in: 0...6) {
                                Text("\(manager.ledMode)")
                                    .monospacedDigit()
                            }
                            .labelsHidden()
                        }
                        .foregroundStyle(.white.opacity(0.9))
                        .disabled(!manager.supportsLed)

                        settingSlider(label: "Manual Exposure", valueText: "\(manager.manualExposureUs) us") {
                            Slider(value: Binding(get: { Double(manager.manualExposureUs) }, set: { manager.setManualExposure(Int($0)) }), in: 1_000...200_000, step: 1_000)
                        }

                        settingSlider(label: "IR Brightness", valueText: "\(manager.irBrightness)") {
                            Slider(value: Binding(get: { Double(manager.irBrightness) }, set: { manager.setIrBrightness(Int($0)) }), in: 1...50, step: 1)
                        }
                    }
                }

                cardSection(title: "Microphone") {
                    VStack(alignment: .leading, spacing: 10) {
                        Toggle("Enable Kinect microphone stream", isOn: Binding(
                            get: { manager.audioEnabled },
                            set: { manager.setAudioEnabled($0) }
                        ))
                        .disabled(!manager.supportsAudioInput)

                        HStack(spacing: 8) {
                            statusBadge(manager.audioEnabled ? (manager.audioStreamActive ? "Active" : "Armed") : "Off",
                                       color: manager.audioStreamActive ? .green : (manager.audioEnabled ? .orange : .gray))
                            if !manager.supportsAudioInput {
                                Text("Not supported on current device/backend")
                                    .font(.caption)
                                    .foregroundStyle(.white.opacity(0.6))
                            }
                        }

                        HStack {
                            Text("Input level")
                                .foregroundStyle(.white.opacity(0.8))
                            ProgressView(value: min(max(Double(manager.audioLevel), 0), 1))
                                .frame(maxWidth: .infinity)
                            Text(String(format: "%.2f", manager.audioLevel))
                                .monospacedDigit()
                                .frame(width: 50, alignment: .trailing)
                                .foregroundStyle(.white.opacity(0.8))
                        }
                    }
                }

                cardSection(title: "3D Scanner") {
                    VStack(alignment: .leading, spacing: 10) {
                        Text("Capture RGB / IR / depth images and generate a point cloud (PLY).")
                            .font(.caption)
                            .foregroundStyle(.white.opacity(0.65))

                        Button {
                            manager.captureScanBundle()
                        } label: {
                            Label(manager.scannerBusy ? "Capturing..." : "Capture Scan Bundle", systemImage: "cube.transparent")
                        }
                        .buttonStyle(.borderedProminent)
                        .disabled(!manager.connected || !manager.streaming || manager.scannerBusy)

                        if manager.lastCapturePointCount > 0 {
                            Text("Last point cloud: \(manager.lastCapturePointCount) points")
                                .font(.caption)
                                .foregroundStyle(.white.opacity(0.75))
                        }

                        if !manager.lastCapturePath.isEmpty {
                            Text(manager.lastCapturePath)
                                .font(.caption2)
                                .foregroundStyle(.white.opacity(0.55))
                                .lineLimit(3)
                            Button("Reveal in Finder") {
                                NSWorkspace.shared.selectFile(nil, inFileViewerRootedAtPath: manager.lastCapturePath)
                            }
                            .buttonStyle(.link)
                        }
                    }
                }

                cardSection(title: "System Camera / Mic Integration") {
                    VStack(alignment: .leading, spacing: 10) {
                        HStack(spacing: 8) {
                            statusBadge("DAL \(manager.systemCameraDalInstalled ? "Installed" : "Missing")",
                                       color: manager.systemCameraDalInstalled ? .green : .gray)
                            statusBadge("HAL \(manager.systemAudioHalInstalled ? "Installed" : "Missing")",
                                       color: manager.systemAudioHalInstalled ? .green : .gray)
                        }

                        Toggle("Use Kinect as system camera/microphone", isOn: Binding(
                            get: { manager.publishToSystem },
                            set: { manager.setSystemPublish($0) }
                        ))

                        HStack(spacing: 8) {
                            Button(manager.systemIntegrationInstallInProgress ? "Installing..." : "Install Integration") {
                                manager.installSystemIntegration()
                            }
                            .buttonStyle(.borderedProminent)
                            .disabled(manager.systemIntegrationInstallInProgress)

                            Button("Re-check") {
                                manager.refreshSystemIntegrationStatus()
                            }
                            .buttonStyle(.bordered)
                        }

                        Text(manager.systemPublishNote)
                            .font(.caption)
                            .foregroundStyle(.white.opacity(0.65))
                        if !manager.systemIntegrationInstallResult.isEmpty {
                            Text(manager.systemIntegrationInstallResult)
                                .font(.caption)
                                .foregroundStyle(.white.opacity(0.65))
                        }
                    }
                }

                cardSection(title: "Status") {
                    VStack(alignment: .leading, spacing: 6) {
                        Text(manager.status)
                            .font(.callout)
                            .foregroundStyle(.white.opacity(0.9))
                        Text(manager.connected ? "Device connected" : "No active device")
                            .font(.caption)
                            .foregroundStyle(.white.opacity(0.6))
                    }
                }
            }
            .padding(.vertical, 2)
        }
        .scrollIndicators(.hidden)
    }

    private var panelCardBackground: some View {
        RoundedRectangle(cornerRadius: 16, style: .continuous)
            .fill(Color.white.opacity(0.05))
            .overlay(
                RoundedRectangle(cornerRadius: 16, style: .continuous)
                    .stroke(Color.white.opacity(0.10), lineWidth: 1)
            )
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
    }

    private var selectedPreviewImage: CGImage? {
        switch manager.streamType {
        case .rgb:
            return rgbImage
        case .ir:
            return irImage
        case .depth:
            return depthImage
        }
    }

    private func captureStillImageFromPreview() {
        guard let image = selectedPreviewImage else { return }
        manager.captureStillImage(image, streamType: manager.streamType)
    }

    private func toggleVideoRecordingFromPreview() {
        if manager.isRecordingVideo {
            manager.stopVideoRecording()
            return
        }
        guard let image = selectedPreviewImage else { return }
        manager.startVideoRecording(image, streamType: manager.streamType)
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

    @ViewBuilder
    private func cardSection<Content: View>(title: String? = nil, @ViewBuilder content: () -> Content) -> some View {
        VStack(alignment: .leading, spacing: 10) {
            if let title {
                Text(title)
                    .font(.headline)
                    .foregroundStyle(.white.opacity(0.95))
            }
            content()
        }
        .padding(14)
        .background(panelCardBackground)
    }

    @ViewBuilder
    private func settingSlider<SliderView: View>(label: String, valueText: String, @ViewBuilder slider: () -> SliderView) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack {
                Text(label)
                    .foregroundStyle(.white.opacity(0.8))
                Spacer()
                Text(valueText)
                    .foregroundStyle(.white.opacity(0.7))
                    .font(.caption)
                    .monospacedDigit()
            }
            slider()
        }
    }

    private func statusBadge(_ text: String, color: Color) -> some View {
        Text(text)
            .font(.caption.weight(.semibold))
            .foregroundStyle(.white)
            .padding(.horizontal, 10)
            .padding(.vertical, 5)
            .background(color.opacity(0.24), in: Capsule())
            .overlay(
                Capsule().stroke(color.opacity(0.45), lineWidth: 1)
            )
    }

    private func infoTile(title: String, value: String) -> some View {
        VStack(alignment: .leading, spacing: 2) {
            Text(title)
                .font(.caption)
                .foregroundStyle(.white.opacity(0.55))
            Text(value)
                .font(.subheadline.weight(.semibold))
                .foregroundStyle(.white.opacity(0.92))
                .lineLimit(1)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(10)
        .background(panelCardBackground)
    }
}

private struct PlaceholderView: View {
    let title: String

    var body: some View {
        Rectangle()
            .fill(Color.black)
            .overlay(
                VStack(spacing: 8) {
                    Image(systemName: "video.slash")
                        .font(.system(size: 28))
                        .foregroundStyle(.white.opacity(0.45))
                    Text(title)
                        .foregroundColor(.white.opacity(0.75))
                        .font(.title3)
                }
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
