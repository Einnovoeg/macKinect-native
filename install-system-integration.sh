#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build-control-center"
APP_BUNDLE=""

if [[ $# -gt 0 ]]; then
  if [[ -d "$1" && "$1" == *.app ]]; then
    APP_BUNDLE="$1"
  elif [[ -d "$1/macKinect.app" ]]; then
    APP_BUNDLE="$1/macKinect.app"
  fi
fi

if [[ -z "${APP_BUNDLE}" && -d "${SCRIPT_DIR}/macKinect.app" ]]; then
  APP_BUNDLE="${SCRIPT_DIR}/macKinect.app"
fi

if [[ -z "${APP_BUNDLE}" ]]; then
  APP_BUNDLE="${BUILD_DIR}/macKinect.app"
fi

HAL_SRC="${APP_BUNDLE}/Contents/PlugIns/HAL/KinectAudioHAL.driver"
if [[ ! -d "${HAL_SRC}" && -d "${BUILD_DIR}/KinectAudioHAL.driver" ]]; then
  HAL_SRC="${BUILD_DIR}/KinectAudioHAL.driver"
fi
if [[ ! -d "${HAL_SRC}" && -d "${APP_BUNDLE}/Contents/PlugIns/HAL/KinectAudioHAL.driver" ]]; then
  HAL_SRC="${APP_BUNDLE}/Contents/PlugIns/HAL/KinectAudioHAL.driver"
fi

HAL_DST="/Library/Audio/Plug-Ins/HAL/KinectAudioHAL.driver"
DAL_SRC="${APP_BUNDLE}/Contents/PlugIns/DAL/KinectCameraDAL.plugin"
if [[ ! -d "${DAL_SRC}" && -d "${BUILD_DIR}/KinectCameraDAL.plugin" ]]; then
  DAL_SRC="${BUILD_DIR}/KinectCameraDAL.plugin"
fi
DAL_DST="/Library/CoreMediaIO/Plug-Ins/DAL/KinectCameraDAL.plugin"
APP_FRAMEWORKS_DIR="${APP_BUNDLE}/Contents/Frameworks"

CAN_PROMPT_SUDO=0
if [[ -t 0 && -t 1 ]]; then
  CAN_PROMPT_SUDO=1
fi

run_privileged() {
  if [[ "${CAN_PROMPT_SUDO}" -eq 1 ]]; then
    sudo "$@"
    return $?
  fi
  return 1
}

if [[ ! -d "${HAL_SRC}" ]]; then
  echo "HAL source bundle not found." >&2
  echo "Checked app: ${APP_BUNDLE}" >&2
  echo "Checked fallback build path: ${BUILD_DIR}/KinectAudioHAL.driver" >&2
  exit 1
fi

if [[ "${CAN_PROMPT_SUDO}" -ne 1 ]]; then
  echo "Cannot prompt for sudo in this non-interactive shell." >&2
  echo "Run this script from an interactive Terminal session so dependency fixups are applied:" >&2
  echo "  \"${SCRIPT_DIR}/install-system-integration.sh\" \"${APP_BUNDLE}\"" >&2
  exit 2
fi

echo "Installing Audio HAL:"
echo "  ${HAL_SRC}"
echo "  -> ${HAL_DST}"
run_privileged ditto "${HAL_SRC}" "${HAL_DST}"

if [[ -d "${DAL_SRC}" ]]; then
  TMP_DAL_ROOT="$(mktemp -d /tmp/KinectCameraDAL-install.XXXXXX)"
  TMP_DAL="${TMP_DAL_ROOT}/KinectCameraDAL.plugin"
  TMP_DAL_FRAMEWORKS="${TMP_DAL}/Frameworks"
  TMP_DAL_BIN="${TMP_DAL}/KinectCameraDAL"
  /usr/bin/ditto "${DAL_SRC}" "${TMP_DAL}"

  if [[ -d "${APP_FRAMEWORKS_DIR}" ]]; then
    mkdir -p "${TMP_DAL_FRAMEWORKS}"
    for pattern in "libfreenect.0*.dylib" "libfreenect2*.dylib" "libusb-1.0*.dylib" "libturbojpeg*.dylib"; do
      for lib in "${APP_FRAMEWORKS_DIR}"/${pattern}; do
        [[ -e "${lib}" ]] || continue
        /usr/bin/ditto "${lib}" "${TMP_DAL_FRAMEWORKS}/$(basename "${lib}")"
      done
    done
  fi

  if [[ -f "${TMP_DAL_BIN}" ]]; then
    /usr/bin/install_name_tool -add_rpath "@loader_path/Frameworks" "${TMP_DAL_BIN}" >/dev/null 2>&1 || true
    /usr/bin/install_name_tool -change "/opt/homebrew/opt/libusb/lib/libusb-1.0.0.dylib" "@rpath/libusb-1.0.0.dylib" "${TMP_DAL_BIN}" || true
    /usr/bin/install_name_tool -change "/opt/homebrew/opt/jpeg-turbo/lib/libturbojpeg.0.dylib" "@rpath/libturbojpeg.0.dylib" "${TMP_DAL_BIN}" || true
  fi

  if [[ -f "${TMP_DAL_FRAMEWORKS}/libfreenect.0.7.5.dylib" ]]; then
    /usr/bin/install_name_tool -id "@loader_path/libfreenect.0.7.5.dylib" "${TMP_DAL_FRAMEWORKS}/libfreenect.0.7.5.dylib" || true
    /usr/bin/install_name_tool -change "@executable_path/../Frameworks/libusb-1.0.0.dylib" "@loader_path/libusb-1.0.0.dylib" "${TMP_DAL_FRAMEWORKS}/libfreenect.0.7.5.dylib" || true
  fi
  if [[ -f "${TMP_DAL_FRAMEWORKS}/libfreenect2.0.2.0.dylib" ]]; then
    /usr/bin/install_name_tool -id "@loader_path/libfreenect2.0.2.0.dylib" "${TMP_DAL_FRAMEWORKS}/libfreenect2.0.2.0.dylib" || true
    /usr/bin/install_name_tool -change "@executable_path/../Frameworks/libusb-1.0.0.dylib" "@loader_path/libusb-1.0.0.dylib" "${TMP_DAL_FRAMEWORKS}/libfreenect2.0.2.0.dylib" || true
    /usr/bin/install_name_tool -change "@executable_path/../Frameworks/libturbojpeg.0.dylib" "@loader_path/libturbojpeg.0.dylib" "${TMP_DAL_FRAMEWORKS}/libfreenect2.0.2.0.dylib" || true
  fi
  if [[ -f "${TMP_DAL_FRAMEWORKS}/libusb-1.0.0.dylib" ]]; then
    /usr/bin/install_name_tool -id "@loader_path/libusb-1.0.0.dylib" "${TMP_DAL_FRAMEWORKS}/libusb-1.0.0.dylib" || true
  fi
  if [[ -f "${TMP_DAL_FRAMEWORKS}/libturbojpeg.0.4.0.dylib" ]]; then
    /usr/bin/install_name_tool -id "@loader_path/libturbojpeg.0.4.0.dylib" "${TMP_DAL_FRAMEWORKS}/libturbojpeg.0.4.0.dylib" || true
  fi

  /usr/bin/codesign --force --deep --sign - --timestamp=none "${TMP_DAL}"

  echo "Installing Camera DAL plugin:"
  echo "  ${TMP_DAL}"
  echo "  -> ${DAL_DST}"
  run_privileged ditto "${TMP_DAL}" "${DAL_DST}"
  run_privileged /usr/bin/codesign --force --deep --sign - --timestamp=none "${DAL_DST}"
  rm -rf "${TMP_DAL_ROOT}"
else
  echo "Camera DAL plugin not found at ${DAL_SRC}; skipping camera installation."
fi

echo "Restarting audio/camera services..."
run_privileged killall coreaudiod || true
run_privileged killall VDCAssistant AppleCameraAssistant || true

echo "Done. Open the app and click 'Re-check integration status'."
