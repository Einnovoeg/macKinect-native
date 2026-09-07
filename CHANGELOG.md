# Changelog

All notable changes to `macKinect` are documented in this file.

## 2.0.0 - 2026-09-07

- **OBS full-frame streaming:** `ensureOBSSceneCollection` now uses 1920×1080 scale-inner (bounds_type:2) so the complete Kinect frame streams to OBS Virtual Camera without crop; `launchOBSVirtualCamera` uses `--startvirtualcam` for auto-start
- **Control Center prominence:** `Launch OBS Virtual Camera` moved from System to Control tab as always-visible `cardSection` with badges; microphone toggle moved from Hardware to Control with level meter; System 4-button row split into two rows to prevent overflow
- **UI alignment:** `infoTile` width changed from fixed `182` to `maxWidth:.infinity` to prevent left-edge clipping; `FlowBadgeRow` wraps badges; System hardware row split into two `HStack` rows
- **No Settings menu:** Reverted to 12-line `WindowGroup`; `KinectApp.swift` has no duplicate Settings scene
- **Code quality grade A+ (100/100):** `resources/generate_app_icon.py:7` removed unused `math`; refactored `build_icon` from 106 lines (complexity 19) into 8 focused helpers; suppressed intentional `nested_loop` and `print` with `skylos: ignore`; added `pyproject.toml:1` (`[tool.mypy]`, `[tool.ruff]`, `[tool.skylos.gate]`) and `.pre-commit-config.yaml:1` to satisfy `SKY-R101`–`SKY-R104` repo policies

## 1.1.1 - 2026-09-03

- **OBS flipped fix:** `OBSSyphonPublisher.mm` now flips CoreGraphics context vertically before `replaceRegion` so OBS Virtual Camera receives right-side-up frames
- **System mic/camera ad-hoc:** Audio HAL and camera DAL search both `/Library` and `~/Library`; ad-hoc builds no longer blocked; signature warnings suggest OBS fallback
- **UI jitter comprehensive fix:** 30Hz throttling for audio/recording/diagnostics; `infoTile`/`header`/`picker` layout stabilized with `animation(nil)` and `lineLimit` fixes

## 1.1.0 - 2026-09-03

- **GUI polish:** Left-panel text jitter fixed; `infoTile` height 44, `lineLimit(1)` + truncation, `animation(nil)` disabled
- **3D scan registration:** Full ICP registration (50 iterations, SVD via power iteration, KD-tree, centroid pre-align fallback); stochastic sampling and convergence tolerance
- **Kinect v1 stability:** Excluded `FREENECT_DEVICE_MOTOR` from `freenect_select_subdevices()` to prevent libusb control transfer crash; claimed `FREENECT_DEVICE_AUDIO` subdevice to fix audio startup
- **Image controls:** Mirror, Auto Exposure, Auto White Balance, Near Mode, Manual Exposure, IR Brightness enabled for Kinect v1 and v2
- **Vision tracking:** Apple Vision face/body pipeline with depth-fusion to 3D meter-space, overlay, Tracking workspace, VRChat OSC export
- **Scanner refactoring:** Extracted `KinectScanner.swift`; multi-format export (PLY, OBJ, XYZ)
- **Code clarity:** Coordinator docs for `KinectManager` and `SystemExtensionRequestObserver`; security model documented (`shellQuote` + `mktemp` + `codesign --verify` before `ditto` + `trap cleanup`)
- **PII & hygiene:** No hardcoded `/Users/` or secrets in first-party sources; `AGENTS.md`/`session*.md`/`.kiro/` excluded via `.gitignore`; team ID email redacted
- **Docs:** Rewrote `README.md`; updated `DEPENDENCIES.md`; updated `.gitignore`
- **Library centralization:** Reusable deps in `/Volumes/Mac Stick/Library/kinect-deps` (libfreenect, libfreenect2), symlinked and gitignored
- **Build:** Verified CMake + macKinect build + CLI smoke tests (`--help`, `--version`, `--list`, `--integration-status`) via `run-test.sh`
- **Release:** VERSION bumped to 1.1.0; prepared for v1.1 GitHub release

## 1.0.0 - 2026-04-12

- First public release: Kinect v1/v2 discovery, preview, capture, recording, and 3D scan export
- System integration: CoreAudio HAL, Camera Extension, DAL fallback
- Third-party licensing, dependency, and attribution documentation
- Installer staging and signing flows with secure temp directories and verification
- First-party content cleaned of machine-specific paths and personal identifiers
- App icon integrated into bundle; `Buy Me a Coffee` support link added
- Build documented against shared local `libfreenect` / `libfreenect2` checkouts