cmake_minimum_required(VERSION 3.15)
project(macKinect LANGUAGES CXX C OBJCXX Swift)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(KINECT_BUILD_BUNDLED_LIBFREENECT2 "Build workspace libfreenect2 source if no package is found" ON)
option(KINECT_BUILD_AUDIO_HAL_PLUGIN "Build CoreAudio HAL virtual microphone plugin target" ON)
option(KINECT_BUILD_CAMERA_DAL_PLUGIN "Build CoreMediaIO DAL virtual camera plugin target" ON)

set(KINECT_V1_FIRMWARE_PATH "")
set(KINECT_V1_FIRMWARE_CANDIDATES
    "${CMAKE_SOURCE_DIR}/firmware/audios.bin"
    "${CMAKE_SOURCE_DIR}/../libfreenect/src/audios.bin"
    "${CMAKE_SOURCE_DIR}/../libfreenect/build/src/audios.bin"
)
foreach(_fw_candidate IN LISTS KINECT_V1_FIRMWARE_CANDIDATES)
    if(EXISTS "${_fw_candidate}")
        set(KINECT_V1_FIRMWARE_PATH "${_fw_candidate}")
        break()
    endif()
endforeach()
if(KINECT_V1_FIRMWARE_PATH)
    message(STATUS "Kinect v1 firmware found: ${KINECT_V1_FIRMWARE_PATH}")
else()
    message(WARNING "Kinect v1 audios.bin firmware not found. v1 camera/mic startup may fail until firmware is provided.")
endif()

# --- CoreAudio HAL virtual microphone plugin ---
if(APPLE AND KINECT_BUILD_AUDIO_HAL_PLUGIN)
    set(KINECT_HAL_EXECUTABLE_NAME "KinectAudioHAL")
    set(KINECT_HAL_BUNDLE_ID "com.mackinect.audiohal")
    set(KINECT_HAL_BUNDLE_VERSION "1.0.0")
    configure_file(
        "${CMAKE_SOURCE_DIR}/src/hal_plugin/Info.plist.in"
        "${CMAKE_BINARY_DIR}/KinectAudioHAL-Info.plist"
        @ONLY
    )

    add_library(KinectAudioHAL MODULE
        src/hal_plugin/KinectAudioHALPlugin.cpp
    )
    target_link_libraries(KinectAudioHAL PRIVATE
        "-framework CoreAudio"
        "-framework CoreFoundation"
    )
    target_compile_definitions(KinectAudioHAL PRIVATE GL_SILENCE_DEPRECATION)
    set_target_properties(KinectAudioHAL PROPERTIES
        BUNDLE TRUE
        BUNDLE_EXTENSION "driver"
        PREFIX ""
        OUTPUT_NAME "${KINECT_HAL_EXECUTABLE_NAME}"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_BINARY_DIR}/KinectAudioHAL-Info.plist"
    )

    add_custom_target(install-kinect-audio-hal
        COMMAND ${CMAKE_COMMAND} -E echo "Install with:"
        COMMAND ${CMAKE_COMMAND} -E echo "  sudo ditto $<TARGET_BUNDLE_DIR:KinectAudioHAL> /Library/Audio/Plug-Ins/HAL/KinectAudioHAL.driver"
        DEPENDS KinectAudioHAL
        VERBATIM
    )
endif()

# Find OpenGL and GLUT
find_package(OpenGL REQUIRED)
find_package(GLUT REQUIRED)

# Force ARM64 if on Apple Silicon to match libfreenect
set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "" FORCE)

# Find libfreenect (v1) - Try local paths first
set(LIBFREENECT_ROOT "${CMAKE_SOURCE_DIR}/../libfreenect" CACHE PATH "Path to libfreenect source/install")

find_path(FREENECT_INCLUDE_DIR libfreenect.h
    HINTS 
        ${LIBFREENECT_ROOT}/include
        ${LIBFREENECT_ROOT}/src
        /usr/local/include 
        /opt/homebrew/include
)

find_library(FREENECT_LIBRARY freenect
    HINTS 
        ${LIBFREENECT_ROOT}/build/lib
        ${LIBFREENECT_ROOT}/lib
        /usr/local/lib 
        /opt/homebrew/lib
)

if(FREENECT_LIBRARY)
    get_filename_component(FREENECT_LIBRARY_DIR "${FREENECT_LIBRARY}" DIRECTORY)
endif()

set(KINECT_HAVE_LIBFREENECT OFF)
set(KINECT_HAVE_LIBFREENECT2 OFF)

if(FREENECT_INCLUDE_DIR AND FREENECT_LIBRARY)
    set(KINECT_HAVE_LIBFREENECT ON)
endif()

# Find/build libfreenect2 (v2) - optional
set(LIBFREENECT2_TARGET "")
set(LIBFREENECT2_INCLUDE_DIRS "")
find_package(libfreenect2 QUIET CONFIG)
if(libfreenect2_FOUND)
    set(KINECT_HAVE_LIBFREENECT2 ON)
    set(LIBFREENECT2_TARGET libfreenect2::freenect2)
endif()

if(NOT KINECT_HAVE_LIBFREENECT2 AND KINECT_BUILD_BUNDLED_LIBFREENECT2 AND EXISTS "${CMAKE_SOURCE_DIR}/../libfreenect2/CMakeLists.txt")
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_OPENNI2_DRIVER OFF CACHE BOOL "" FORCE)
    set(BUILD_STREAMER_RECORDER OFF CACHE BOOL "" FORCE)
    set(ENABLE_CXX11 ON CACHE BOOL "" FORCE)
    set(ENABLE_OPENCL OFF CACHE BOOL "" FORCE)
    set(ENABLE_CUDA OFF CACHE BOOL "" FORCE)
    set(ENABLE_OPENGL OFF CACHE BOOL "" FORCE)
    set(ENABLE_VAAPI OFF CACHE BOOL "" FORCE)
    set(ENABLE_TEGRAJPEG OFF CACHE BOOL "" FORCE)
    add_subdirectory("${CMAKE_SOURCE_DIR}/../libfreenect2" "${CMAKE_BINARY_DIR}/third_party/libfreenect2")
    set(KINECT_HAVE_LIBFREENECT2 ON)
    set(LIBFREENECT2_TARGET freenect2)
    set(LIBFREENECT2_INCLUDE_DIRS
        "${CMAKE_SOURCE_DIR}/../libfreenect2/include"
        "${CMAKE_BINARY_DIR}/third_party/libfreenect2"
    )
endif()

# Sources
set(SOURCES
    src/main.cpp
    src/gui/app.cpp
    src/backends/freenect_v1_backend.cpp
    src/backends/freenect_v2_backend.cpp
)

set(CONTROL_CENTER_SOURCES
    src/control_center_v1.cpp
)

# Check backend availability
if(KINECT_HAVE_LIBFREENECT)
    message(STATUS "Kinect v1 support enabled. Include: ${FREENECT_INCLUDE_DIR}, Lib: ${FREENECT_LIBRARY}")
else()
    message(WARNING "libfreenect not found - v1 support disabled")
endif()

if(KINECT_HAVE_LIBFREENECT2)
    message(STATUS "Kinect v2 support enabled")
else()
    message(STATUS "Kinect v2 support disabled (libfreenect2 not found)")
endif()

# Silence macOS deprecation warnings for OpenGL
add_definitions(-DGL_SILENCE_DEPRECATION)

# --- Legacy C++ App ---
add_executable(KinectMacOsApp ${SOURCES})

target_include_directories(KinectMacOsApp PRIVATE
    src
    ${FREENECT_INCLUDE_DIR}
    ${LIBFREENECT2_INCLUDE_DIRS}
)

if(KINECT_HAVE_LIBFREENECT2)
    target_link_libraries(KinectMacOsApp PRIVATE ${LIBFREENECT2_TARGET})
endif()

if(KINECT_HAVE_LIBFREENECT)
    target_link_libraries(KinectMacOsApp PRIVATE ${FREENECT_LIBRARY})
endif()

target_link_libraries(KinectMacOsApp PRIVATE
    OpenGL::GL
    GLUT::GLUT
)
target_compile_definitions(KinectMacOsApp PRIVATE
    GL_SILENCE_DEPRECATION
    KINECT_HAVE_LIBFREENECT=$<BOOL:${KINECT_HAVE_LIBFREENECT}>
    KINECT_HAVE_LIBFREENECT2=$<BOOL:${KINECT_HAVE_LIBFREENECT2}>
)

# --- Kinect Control Center (native OpenGL v1 control app) ---
if(KINECT_HAVE_LIBFREENECT)
    get_filename_component(FREENECT_LIBRARY_DIR "${FREENECT_LIBRARY}" DIRECTORY)
    add_executable(kinect-control-center ${CONTROL_CENTER_SOURCES})
    target_include_directories(kinect-control-center PRIVATE ${FREENECT_INCLUDE_DIR})
    target_link_libraries(kinect-control-center PRIVATE
        ${FREENECT_LIBRARY}
        OpenGL::GL
        GLUT::GLUT
    )
    target_compile_definitions(kinect-control-center PRIVATE
        GL_SILENCE_DEPRECATION
        KINECT_HAVE_LIBFREENECT=1
        KINECT_HAVE_LIBFREENECT2=0
    )
    set_target_properties(kinect-control-center PROPERTIES
        BUILD_RPATH "${FREENECT_LIBRARY_DIR}"
        INSTALL_RPATH "${FREENECT_LIBRARY_DIR}"
    )
    message(STATUS "kinect-control-center target enabled")
else()
    message(WARNING "kinect-control-center target disabled because libfreenect was not found")
endif()

# --- CoreMediaIO DAL virtual camera plugin ---
if(APPLE AND KINECT_BUILD_CAMERA_DAL_PLUGIN)
    set(KINECT_DAL_EXECUTABLE_NAME "KinectCameraDAL")
    set(KINECT_DAL_BUNDLE_ID "com.mackinect.cameradal")
    set(KINECT_DAL_BUNDLE_VERSION "1.0.0")
    configure_file(
        "${CMAKE_SOURCE_DIR}/src/dal_plugin/Info.plist.in"
        "${CMAKE_BINARY_DIR}/KinectCameraDAL-Info.plist"
        @ONLY
    )

    add_library(KinectCameraDAL MODULE
        src/dal_plugin/KinectCameraDALPlugin.cpp
        src/backends/freenect_v1_backend.cpp
        src/backends/freenect_v2_backend.cpp
    )
    target_include_directories(KinectCameraDAL PRIVATE
        src
        ${FREENECT_INCLUDE_DIR}
        ${LIBFREENECT2_INCLUDE_DIRS}
    )
    if(KINECT_HAVE_LIBFREENECT)
        target_link_libraries(KinectCameraDAL PRIVATE ${FREENECT_LIBRARY})
    endif()
    if(KINECT_HAVE_LIBFREENECT2)
        target_link_libraries(KinectCameraDAL PRIVATE ${LIBFREENECT2_TARGET})
    endif()
    target_link_libraries(KinectCameraDAL PRIVATE
        "-framework CoreMediaIO"
        "-framework CoreMedia"
        "-framework CoreVideo"
        "-framework CoreFoundation"
        "-framework CoreAudio"
    )
    target_compile_definitions(KinectCameraDAL PRIVATE
        GL_SILENCE_DEPRECATION
        KINECT_HAVE_LIBFREENECT=$<BOOL:${KINECT_HAVE_LIBFREENECT}>
        KINECT_HAVE_LIBFREENECT2=$<BOOL:${KINECT_HAVE_LIBFREENECT2}>
    )
    set_target_properties(KinectCameraDAL PROPERTIES
        BUNDLE TRUE
        BUNDLE_EXTENSION "plugin"
        PREFIX ""
        OUTPUT_NAME "${KINECT_DAL_EXECUTABLE_NAME}"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_BINARY_DIR}/KinectCameraDAL-Info.plist"
        BUILD_RPATH "@loader_path/Frameworks;@loader_path/../../../Frameworks"
        INSTALL_RPATH "@loader_path/Frameworks;@loader_path/../../../Frameworks"
    )

    add_custom_target(install-kinect-camera-dal
        COMMAND ${CMAKE_COMMAND} -E echo "Install with:"
        COMMAND ${CMAKE_COMMAND} -E echo "  ${CMAKE_SOURCE_DIR}/install-system-integration.sh ${CMAKE_BINARY_DIR}/macKinect.app"
        DEPENDS KinectCameraDAL
        VERBATIM
    )
endif()

# --- Swift / Objective-C Bridge (Modern App) ---

enable_language(Swift)

if(CMAKE_Swift_COMPILER)
    message(STATUS "Swift compiler found: ${CMAKE_Swift_COMPILER}")
    
    set(SWIFT_SOURCES
        src/swift/KinectEntrypoint.swift
        src/swift/KinectApp.swift
        src/swift/ContentView.swift
        src/swift/KinectManager.swift
        src/swift/PreviewMovieRecorder.swift
    )
    
    set(BRIDGE_SOURCES
        src/bridge/KinectBridge.mm
        src/backends/freenect_v1_backend.cpp
        src/backends/freenect_v2_backend.cpp
    )
    set_source_files_properties(src/bridge/KinectBridge.mm PROPERTIES
        COMPILE_OPTIONS "-fobjc-arc"
    )

    add_executable(macKinect
        ${SWIFT_SOURCES}
        ${BRIDGE_SOURCES}
    )

    set_target_properties(macKinect PROPERTIES
        Swift_LANGUAGE_VERSION 5
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "com.mackinect.app"
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "macKinect"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.mackinect.app"
        MACOSX_BUNDLE_BUNDLE_VERSION "1.0.0"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0.0"
        SWIFT_OBJC_BRIDGING_HEADER "${CMAKE_SOURCE_DIR}/src/bridge/KinectBridge.h"
        XCODE_ATTRIBUTE_SWIFT_OBJC_BRIDGING_HEADER "${CMAKE_SOURCE_DIR}/src/bridge/KinectBridge.h"
    )

    target_include_directories(macKinect PRIVATE
        src
        ${FREENECT_INCLUDE_DIR}
        ${LIBFREENECT2_INCLUDE_DIRS}
    )

    if(KINECT_HAVE_LIBFREENECT)
        target_link_libraries(macKinect PRIVATE ${FREENECT_LIBRARY})
    endif()
    
    if(KINECT_HAVE_LIBFREENECT2)
        target_link_libraries(macKinect PRIVATE ${LIBFREENECT2_TARGET})
    endif()

    target_link_libraries(macKinect PRIVATE
        "-framework Foundation"
        "-framework AppKit"
        "-framework SwiftUI"
        "-framework AVFoundation"
        "-framework CoreMedia"
        "-framework CoreVideo"
        "-framework ImageIO"
        "-framework UniformTypeIdentifiers"
    )

    target_compile_options(macKinect PRIVATE
        "$<$<COMPILE_LANGUAGE:Swift>:-import-objc-header>"
        "$<$<COMPILE_LANGUAGE:Swift>:${CMAKE_SOURCE_DIR}/src/bridge/KinectBridge.h>"
    )
    target_compile_definitions(macKinect PRIVATE
        GL_SILENCE_DEPRECATION
        KINECT_HAVE_LIBFREENECT=$<BOOL:${KINECT_HAVE_LIBFREENECT}>
        KINECT_HAVE_LIBFREENECT2=$<BOOL:${KINECT_HAVE_LIBFREENECT2}>
    )

    set(KINECT_BUNDLE_SEARCH_DIRS
        "${FREENECT_LIBRARY_DIR}"
        "${CMAKE_BINARY_DIR}/third_party/libfreenect2/lib"
        "/opt/homebrew/lib"
        "/opt/homebrew/opt/libusb/lib"
        "/opt/homebrew/opt/jpeg-turbo/lib"
    )
    list(FILTER KINECT_BUNDLE_SEARCH_DIRS EXCLUDE REGEX "^$")
    list(JOIN KINECT_BUNDLE_SEARCH_DIRS "|" KINECT_BUNDLE_SEARCH_DIRS_JOINED)
    if(TARGET KinectAudioHAL)
        add_dependencies(macKinect KinectAudioHAL)
    endif()
    if(TARGET KinectCameraDAL)
        add_dependencies(macKinect KinectCameraDAL)
        add_custom_command(TARGET macKinect POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_BUNDLE_DIR:macKinect>/Contents/PlugIns/DAL
            COMMAND ${CMAKE_COMMAND} -E copy_directory $<TARGET_BUNDLE_DIR:KinectCameraDAL> $<TARGET_BUNDLE_DIR:macKinect>/Contents/PlugIns/DAL/KinectCameraDAL.plugin
            VERBATIM
        )
    endif()

    if(KINECT_V1_FIRMWARE_PATH)
        if(TARGET KinectAudioHAL)
            add_custom_command(TARGET macKinect POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_BUNDLE_DIR:macKinect>/Contents/PlugIns/HAL
                COMMAND ${CMAKE_COMMAND} -E copy_directory $<TARGET_BUNDLE_DIR:KinectAudioHAL> $<TARGET_BUNDLE_DIR:macKinect>/Contents/PlugIns/HAL/KinectAudioHAL.driver
                COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_BUNDLE_DIR:macKinect>/Contents/Resources/libfreenect
                COMMAND ${CMAKE_COMMAND} -E copy_if_different ${KINECT_V1_FIRMWARE_PATH} $<TARGET_BUNDLE_DIR:macKinect>/Contents/Resources/libfreenect/audios.bin
                COMMAND ${CMAKE_COMMAND}
                    -DAPP_BUNDLE=$<TARGET_BUNDLE_DIR:macKinect>
                    -DSEARCH_DIRS_PIPE=${KINECT_BUNDLE_SEARCH_DIRS_JOINED}
                    -P "${CMAKE_SOURCE_DIR}/cmake/fixup_mackinect.cmake"
                VERBATIM
            )
        else()
            add_custom_command(TARGET macKinect POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_BUNDLE_DIR:macKinect>/Contents/Resources/libfreenect
                COMMAND ${CMAKE_COMMAND} -E copy_if_different ${KINECT_V1_FIRMWARE_PATH} $<TARGET_BUNDLE_DIR:macKinect>/Contents/Resources/libfreenect/audios.bin
                COMMAND ${CMAKE_COMMAND}
                    -DAPP_BUNDLE=$<TARGET_BUNDLE_DIR:macKinect>
                    -DSEARCH_DIRS_PIPE=${KINECT_BUNDLE_SEARCH_DIRS_JOINED}
                    -P "${CMAKE_SOURCE_DIR}/cmake/fixup_mackinect.cmake"
                VERBATIM
            )
        endif()
    else()
        if(TARGET KinectAudioHAL)
            add_custom_command(TARGET macKinect POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_BUNDLE_DIR:macKinect>/Contents/PlugIns/HAL
                COMMAND ${CMAKE_COMMAND} -E copy_directory $<TARGET_BUNDLE_DIR:KinectAudioHAL> $<TARGET_BUNDLE_DIR:macKinect>/Contents/PlugIns/HAL/KinectAudioHAL.driver
                COMMAND ${CMAKE_COMMAND}
                    -DAPP_BUNDLE=$<TARGET_BUNDLE_DIR:macKinect>
                    -DSEARCH_DIRS_PIPE=${KINECT_BUNDLE_SEARCH_DIRS_JOINED}
                    -P "${CMAKE_SOURCE_DIR}/cmake/fixup_mackinect.cmake"
                VERBATIM
            )
        else()
            add_custom_command(TARGET macKinect POST_BUILD
                COMMAND ${CMAKE_COMMAND}
                    -DAPP_BUNDLE=$<TARGET_BUNDLE_DIR:macKinect>
                    -DSEARCH_DIRS_PIPE=${KINECT_BUNDLE_SEARCH_DIRS_JOINED}
                    -P "${CMAKE_SOURCE_DIR}/cmake/fixup_mackinect.cmake"
                VERBATIM
            )
        endif()
    endif()

else()
    message(WARNING "Swift compiler not found. Modern app will not be built.")
endif()
