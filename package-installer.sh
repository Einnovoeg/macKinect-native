#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build-control-center"
APP_BUNDLE="${BUILD_DIR}/macKinect.app"
DIST_DIR="${SCRIPT_DIR}/dist"
STAMP="$(date +%Y%m%d-%H%M%S)"
PKG_ROOT="${DIST_DIR}/pkgroot-${STAMP}"
PKG_SCRIPTS="${DIST_DIR}/pkgscripts-${STAMP}"
PKG_PATH="${DIST_DIR}/macKinect-Installer-${STAMP}.pkg"
MODULE_CACHE_DIR="${BUILD_DIR}/clang-module-cache"
APP_SIGN_IDENTITY="${MACOS_SIGN_IDENTITY:--}"
PKG_SIGN_IDENTITY="${MACOS_PKG_SIGN_IDENTITY:-}"

cmake -S "${SCRIPT_DIR}" -B "${BUILD_DIR}" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_Swift_FLAGS="-module-cache-path ${MODULE_CACHE_DIR}"
mkdir -p "${MODULE_CACHE_DIR}"
CLANG_MODULE_CACHE_PATH="${MODULE_CACHE_DIR}" cmake --build "${BUILD_DIR}" --target macKinect -j4

if [[ "${APP_SIGN_IDENTITY}" == "-" ]]; then
  /usr/bin/codesign --force --deep --sign - --timestamp=none "${APP_BUNDLE}"
else
  /usr/bin/codesign --force --deep --sign "${APP_SIGN_IDENTITY}" --options runtime --timestamp "${APP_BUNDLE}"
fi
/usr/bin/codesign --verify --verbose=2 --deep "${APP_BUNDLE}"
/bin/rm -f "${APP_BUNDLE}/Contents/MacOS/macKinect.d"

/bin/mkdir -p "${DIST_DIR}"
/bin/rm -f "${PKG_PATH}"
/bin/rm -rf "${PKG_ROOT}" "${PKG_SCRIPTS}"

/bin/mkdir -p "${PKG_ROOT}/Applications"
/bin/mkdir -p "${PKG_ROOT}/Library/Audio/Plug-Ins/HAL"
/bin/mkdir -p "${PKG_ROOT}/Library/CoreMediaIO/Plug-Ins/DAL"
/usr/bin/ditto "${APP_BUNDLE}" "${PKG_ROOT}/Applications/macKinect.app"

HAL_SRC="${PKG_ROOT}/Applications/macKinect.app/Contents/PlugIns/HAL/KinectAudioHAL.driver"
HAL_DST="${PKG_ROOT}/Library/Audio/Plug-Ins/HAL/KinectAudioHAL.driver"
DAL_SRC="${PKG_ROOT}/Applications/macKinect.app/Contents/PlugIns/DAL/KinectCameraDAL.plugin"
DAL_DST="${PKG_ROOT}/Library/CoreMediaIO/Plug-Ins/DAL/KinectCameraDAL.plugin"
APP_FRAMEWORKS_DIR="${PKG_ROOT}/Applications/macKinect.app/Contents/Frameworks"

/usr/bin/ditto "${HAL_SRC}" "${HAL_DST}"
/usr/bin/ditto "${DAL_SRC}" "${DAL_DST}"

DAL_FRAMEWORKS_DIR="${DAL_DST}/Contents/Frameworks"
/bin/mkdir -p "${DAL_FRAMEWORKS_DIR}"
for pattern in "libfreenect.0*.dylib" "libfreenect2*.dylib" "libusb-1.0*.dylib" "libturbojpeg*.dylib"; do
  for lib in "${APP_FRAMEWORKS_DIR}"/${pattern}; do
    [[ -e "${lib}" ]] || continue
    /usr/bin/ditto "${lib}" "${DAL_FRAMEWORKS_DIR}/$(basename "${lib}")"
  done
done

DAL_BIN="${DAL_DST}/Contents/MacOS/KinectCameraDAL"
if [[ ! -f "${DAL_BIN}" && -f "${DAL_DST}/KinectCameraDAL" ]]; then
  DAL_BIN="${DAL_DST}/KinectCameraDAL"
  DAL_FRAMEWORKS_DIR="${DAL_DST}/Frameworks"
  /bin/mkdir -p "${DAL_FRAMEWORKS_DIR}"
fi

if [[ -f "${DAL_BIN}" ]]; then
  /usr/bin/install_name_tool -add_rpath "@loader_path/../Frameworks" "${DAL_BIN}" >/dev/null 2>&1 || true
  /usr/bin/install_name_tool -change "/opt/homebrew/opt/libusb/lib/libusb-1.0.0.dylib" "@rpath/libusb-1.0.0.dylib" "${DAL_BIN}" || true
  /usr/bin/install_name_tool -change "/opt/homebrew/opt/jpeg-turbo/lib/libturbojpeg.0.dylib" "@rpath/libturbojpeg.0.dylib" "${DAL_BIN}" || true
fi

if [[ -f "${DAL_FRAMEWORKS_DIR}/libfreenect.0.7.5.dylib" ]]; then
  /usr/bin/install_name_tool -id "@rpath/libfreenect.0.7.5.dylib" "${DAL_FRAMEWORKS_DIR}/libfreenect.0.7.5.dylib" || true
  /usr/bin/install_name_tool -change "@executable_path/../Frameworks/libusb-1.0.0.dylib" "@rpath/libusb-1.0.0.dylib" "${DAL_FRAMEWORKS_DIR}/libfreenect.0.7.5.dylib" || true
fi
if [[ -f "${DAL_FRAMEWORKS_DIR}/libfreenect2.0.2.0.dylib" ]]; then
  /usr/bin/install_name_tool -id "@rpath/libfreenect2.0.2.0.dylib" "${DAL_FRAMEWORKS_DIR}/libfreenect2.0.2.0.dylib" || true
  /usr/bin/install_name_tool -change "@executable_path/../Frameworks/libusb-1.0.0.dylib" "@rpath/libusb-1.0.0.dylib" "${DAL_FRAMEWORKS_DIR}/libfreenect2.0.2.0.dylib" || true
  /usr/bin/install_name_tool -change "@executable_path/../Frameworks/libturbojpeg.0.dylib" "@rpath/libturbojpeg.0.dylib" "${DAL_FRAMEWORKS_DIR}/libfreenect2.0.2.0.dylib" || true
fi
if [[ -f "${DAL_FRAMEWORKS_DIR}/libusb-1.0.0.dylib" ]]; then
  /usr/bin/install_name_tool -id "@rpath/libusb-1.0.0.dylib" "${DAL_FRAMEWORKS_DIR}/libusb-1.0.0.dylib" || true
fi
if [[ -f "${DAL_FRAMEWORKS_DIR}/libturbojpeg.0.4.0.dylib" ]]; then
  /usr/bin/install_name_tool -id "@rpath/libturbojpeg.0.4.0.dylib" "${DAL_FRAMEWORKS_DIR}/libturbojpeg.0.4.0.dylib" || true
fi

/usr/bin/codesign --force --deep --sign - --timestamp=none "${HAL_DST}"
/usr/bin/codesign --force --deep --sign - --timestamp=none "${DAL_DST}"

/bin/mkdir -p "${PKG_SCRIPTS}"
cat > "${PKG_SCRIPTS}/postinstall" <<'EOF'
#!/bin/bash
/usr/bin/killall coreaudiod >/dev/null 2>&1 || true
/usr/bin/killall VDCAssistant AppleCameraAssistant >/dev/null 2>&1 || true
exit 0
EOF
/bin/chmod +x "${PKG_SCRIPTS}/postinstall"

if [[ -n "${PKG_SIGN_IDENTITY}" ]]; then
  /usr/bin/pkgbuild \
    --root "${PKG_ROOT}" \
    --scripts "${PKG_SCRIPTS}" \
    --identifier "com.mackinect.installer" \
    --version "1.0.0" \
    --install-location "/" \
    --sign "${PKG_SIGN_IDENTITY}" \
    "${PKG_PATH}"
else
  /usr/bin/pkgbuild \
    --root "${PKG_ROOT}" \
    --scripts "${PKG_SCRIPTS}" \
    --identifier "com.mackinect.installer" \
    --version "1.0.0" \
    --install-location "/" \
    "${PKG_PATH}"
fi

/bin/rm -rf "${PKG_ROOT}" "${PKG_SCRIPTS}"

echo "Installer package:"
echo "  ${PKG_PATH}"
