#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build-control-center"
APP_BUNDLE="${BUILD_DIR}/macKinect.app"
DIST_DIR="${SCRIPT_DIR}/dist"
STAMP="$(date +%Y%m%d-%H%M%S)"
ZIP_PATH="${DIST_DIR}/macKinect-${STAMP}.zip"
DMG_PATH="${DIST_DIR}/macKinect-${STAMP}.dmg"
STAGING_DIR="${DIST_DIR}/macKinect-${STAMP}"
MODULE_CACHE_DIR="${BUILD_DIR}/clang-module-cache"
SIGN_IDENTITY="${MACOS_SIGN_IDENTITY:--}"

cmake -S "${SCRIPT_DIR}" -B "${BUILD_DIR}" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_Swift_FLAGS="-module-cache-path ${MODULE_CACHE_DIR}"
mkdir -p "${MODULE_CACHE_DIR}"
CLANG_MODULE_CACHE_PATH="${MODULE_CACHE_DIR}" cmake --build "${BUILD_DIR}" --target macKinect -j4

if [[ "${SIGN_IDENTITY}" == "-" ]]; then
  codesign --force --deep --sign - --timestamp=none "${APP_BUNDLE}"
else
  codesign --force --deep --sign "${SIGN_IDENTITY}" --options runtime --timestamp "${APP_BUNDLE}"
fi
codesign --verify --verbose=2 --deep "${APP_BUNDLE}"
rm -f "${APP_BUNDLE}/Contents/MacOS/macKinect.d"

mkdir -p "${DIST_DIR}"
rm -f "${ZIP_PATH}"
rm -f "${DMG_PATH}"
rm -rf "${STAGING_DIR}"

mkdir -p "${STAGING_DIR}"
ditto "${APP_BUNDLE}" "${STAGING_DIR}/macKinect.app"

cat > "${STAGING_DIR}/README.txt" <<'EOF'
macKinect
=========

Quick Start
-----------
1. Connect Kinect hardware and power.
2. Open macKinect.app.
3. Click Refresh, select your Kinect, then Connect and Start Stream.

System Integration
------------------
Use the app's "Install System Integration" button.
macOS will request administrator authorization.

No Homebrew is required to run this package.

Support:
https://buymeacoffee.com/einnovoeg
EOF

cp "${SCRIPT_DIR}/install-system-integration.sh" "${STAGING_DIR}/InstallSystemIntegration.command"
chmod +x "${STAGING_DIR}/InstallSystemIntegration.command"

/usr/bin/xattr -cr "${STAGING_DIR}" 2>/dev/null || true
COPYFILE_DISABLE=1 ditto -c -k --keepParent --norsrc "${STAGING_DIR}" "${ZIP_PATH}"
DMG_CREATED=0
if hdiutil create -volname "macKinect" -srcfolder "${STAGING_DIR}" -format UDZO "${DMG_PATH}"; then
  DMG_CREATED=1
else
  echo "Warning: DMG creation failed in this environment. ZIP artifact is available."
fi

rm -rf "${STAGING_DIR}"

echo "Packaged app:"
echo "  ${ZIP_PATH}"
if [[ "${DMG_CREATED}" -eq 1 ]]; then
  echo "  ${DMG_PATH}"
fi
echo "Signing identity: ${SIGN_IDENTITY}"
