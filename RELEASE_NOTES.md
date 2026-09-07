# Release Notes

## macKinect 2.0

Release date: 2026-09-07

### Highlights

- **OBS full-frame streaming:** Virtual Camera now streams the complete Kinect frame at 1920×1080 with scale-inner rendering; no cropping or black bars; `launchOBSVirtualCamera` uses `--startvirtualcam` for auto-start
- **Control Center prominence:** `Launch OBS Virtual Camera` and microphone toggle moved to Control tab as always-visible prominent cards; System 4-button row split to prevent overflow; no Settings menu (12-line `WindowGroup`)
- **UI alignment:** `infoTile` flexible width prevents left-edge clipping; badge wrapping prevents overflow; System row split into two `HStack` rows
- **Code quality grade A+ (100/100):** Fixed Skylos quality gate — `build_icon` refactored, unused import removed, repo policies added (`mypy`, `ruff`, `skylos.gate`, `pre-commit`)

### What changed in 2.0

- `KinectManager.swift`: `ensureOBSSceneCollection` scaled to 1920×1080 with `bounds_type:2` (scale-inner, complete frame); `launchOBSVirtualCamera` uses `--startvirtualcam`, removed AppleScript fallback
- `ContentView.swift`: Restored polished 724f671 layout; `obsProminentCard` + `micProminentCard` fixed above `ScrollView` in Control; `infoTile` flexible width; System 4-button row split; no Settings scene
- `KinectApp.swift`: Kept at 12 lines, no Settings duplicate
- `OBSSyphonPublisher.mm`: Vertical flip preserved for right-side-up frames
- `resources/generate_app_icon.py`: Removed `math` import; split 106-line `build_icon` into 8 helpers (`_create_background`, `_create_scan_grid`, `_create_sensor_shadow`, `_create_sensor`, `_create_beam`, `_create_point_cloud`, `_create_lens_glow`, `_create_lens_layer`); suppressed intentional `SKY-P403` nested-loop and `SKY-L009` prints
- `pyproject.toml` / `.pre-commit-config.yaml`: Added `mypy`, `ruff`, `skylos.gate`, `pre-commit` policies for `SKY-R101`–`SKY-R104`
- `VERSION`: `1.1.1` → `2.0.0`; `pyproject.toml` version `1.1.0` → `2.0.0`

### Verification summary

- `swiftc -parse` and `clang -fsyntax-only` clean for modified files
- `CMake` configure `MacOSX26.5.sdk` + `arm64` + `Ninja` → build + `fixup_bundle` verified
- Window captures confirm no clipping; OBS scene file has 1920×1080 centered source

---

## macKinect 1.1

Release date: 2026-09-03

### Highlights

- Native macOS Kinect control center for Kinect v1 and v2
- Live RGB, infrared, and depth preview with still and video capture + Vision overlay
- Simple scan bundle export with point-cloud output + full ICP registration
- Apple Vision 3D skeletal tracking with VRChat OSC export (RGB+IR, depth-fused)
- System microphone and camera integration paths, with OBS Virtual Camera fallback
- Stable left-panel layout: no more text moving/shrinking when app is open
- Public repository cleanup for licensing, privacy, packaging, and release documentation
- First-party app icon integrated into the app bundle and packaged release assets

### What changed in 1.1

- Fixed left-panel jitter: `infoTile` height, `lineLimit(1)` + truncation, `animation(nil)` disabled; picker stabilized
- Replaced flat point-cloud concatenation with ICP (50 iter, KD-tree, centroid pre-align) in `PointCloudMerger.swift`
- Resolved Kinect v1 `libusb` crashes (motor subdevice excluded, audio subdevice claimed)
- Enabled image controls (Mirror, Exposure, White Balance, Near Mode, IR Brightness) for both v1 and v2
- Added Vision tracking pipeline + `KinectScanner`/`TrackingService`/`OSCTrackerSender` modules
- Added coordinator/security comments in `KinectManager.swift` and `ContentView.swift`
- Centralized reusable libs to `/Volumes/Mac Stick/Library/kinect-deps` (symlinked, gitignored)
- Rewrote `README.md` / `DEPENDENCIES.md` for centralized-deps and ICP; updated `.gitignore`

### Known limitations

- Camera Extension activation depends on valid Apple signing, entitlements, provisioning, and user approval; use OBS Virtual Camera until `systemextensionsctl list` shows `*[activated enabled]`
- DAL publishing is a compatibility fallback and may be blocked by macOS 12.1+ security policies
- Kinect v1 audio requires user-supplied `audios.bin` firmware blob
- ICP registration implemented but benefits from further tuning on real scans
- `.pkg` wrapper remains unsigned unless `MACOS_PKG_SIGN_IDENTITY` is configured during packaging
- Hardware-dependent features need broader real-device validation

---

## macKinect 1.0

Release date: 2026-04-12

### Highlights

- First public release: Kinect v1 and v2 discovery, preview, capture, and recording
- 3D scan bundle export with point-cloud output
- System microphone and camera integration paths, with OBS Virtual Camera fallback
- Third-party licensing, dependency, and attribution documentation
- Installer staging and signing flows with secure temp directories and verification
- First-party content cleaned of machine-specific paths and personal identifiers
- App icon integrated into bundle; `Buy Me a Coffee` support link added

### Verification summary

- successful CMake configure
- successful `macKinect` app build
- CLI smoke tests: `--help`, `--version`, `--list`, `--integration-status`
- PII scan, license audit, packaging, icon verification

### Known limitations

- Camera Extension activation depends on valid Apple signing, entitlements, provisioning, and user approval
- DAL publishing is not reliable on all modern macOS versions
- Kinect v1 image-control writes disabled because the underlying `libfreenect` control-transfer path can crash in `libusb`
- Kinect v1 audio still requires a user-supplied `audios.bin` firmware blob
- The `.pkg` wrapper remains unsigned unless `MACOS_PKG_SIGN_IDENTITY` is configured during packaging
- Hardware-dependent features need broader real-device validation