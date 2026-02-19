# macKinect

Native macOS app for Kinect v1 and Kinect v2:

- live stream preview (`RGB`, `Infrared`, `Depth`)
- device selection and connection management
- camera and motor controls (tilt, LED, mirror, exposure, white balance)
- microphone controls and level meter
- 3D capture bundle export (`color.ppm`, `infrared.pgm`, `depth_mm.pgm`, `scan.ply`)
- optional system integration install for HAL/DAL plugins

Support the project: [buymeacoffee.com/einnovoeg](https://buymeacoffee.com/einnovoeg)

## Requirements

- macOS
- Xcode Command Line Tools
- CMake 3.20+
- libusb
- Kinect hardware and power adapter

## Build

```bash
cmake -S . -B build-control-center -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-control-center --target macKinect -j4
```

## Run

```bash
open build-control-center/macKinect.app
```

CLI mode:

```bash
build-control-center/macKinect.app/Contents/MacOS/macKinect --help
build-control-center/macKinect.app/Contents/MacOS/macKinect --list
build-control-center/macKinect.app/Contents/MacOS/macKinect --preview 5
```

## Package

```bash
./package-app.sh
```

Outputs:

- `dist/macKinect-<timestamp>.zip`
- `dist/macKinect-<timestamp>.dmg` (when DMG creation is available)

Installer package:

```bash
./package-installer.sh
```

Output:

- `dist/macKinect-Installer-<timestamp>.pkg`

## System Integration

In app: **System Integration -> Install System Integration**.

Or from Terminal:

```bash
./install-system-integration.sh
```

## Notes

- Kinect v1 microphone requires `audios.bin` firmware.
- Kinect v2 requires stable USB 3 and external power.
