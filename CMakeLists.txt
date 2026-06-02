cmake_minimum_required(VERSION 3.20)
project(sundial VERSION 1.1.1 LANGUAGES CXX)

# App version shown in the About box. Defaults to the project() version; CI and
# tools/release.ps1 override it with the release tag (-DSUNDIAL_VERSION=x.y.z).
set(SUNDIAL_VERSION "${PROJECT_VERSION}" CACHE STRING "Sundial version string")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WIN32)
  message(FATAL_ERROR "Sundial is Windows-only.")
endif()

# Use the static CRT across the whole statically-linked graph (sundial, imgui,
# libultrahdr core). libultrahdr's bundled libjpeg-turbo defaults to the static
# CRT (/MT), so matching it here avoids an LNK4098 CRT-mismatch. Velopack is a
# DLL import lib and CRT-neutral. Set before FetchContent so fetched
# subprojects inherit it (CMP0091 is NEW at our cmake_minimum_required).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)
FetchContent_Declare(imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG        v1.91.0
)
FetchContent_MakeAvailable(imgui)

add_library(imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
)
target_include_directories(imgui PUBLIC
  ${imgui_SOURCE_DIR}
  ${imgui_SOURCE_DIR}/backends
)
if(MSVC)
  target_compile_options(imgui PRIVATE /W0)
endif()

# --- Ultra HDR JPEG export (libultrahdr) ------------------------------------
# Fetched and built from source (it builds its own libjpeg-turbo with SIMD
# disabled, so no NASM is required). Turn off to build without Ultra HDR
# export - SaveUltraHdrJpeg() then throws an explanatory error instead.
option(SUNDIAL_ENABLE_ULTRAHDR "Enable Ultra HDR JPEG export via libultrahdr" ON)
if(SUNDIAL_ENABLE_ULTRAHDR)
  set(UHDR_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(UHDR_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(UHDR_BUILD_BENCHMARK OFF CACHE BOOL "" FORCE)
  set(UHDR_BUILD_JAVA OFF CACHE BOOL "" FORCE)
  set(UHDR_BUILD_DEPS ON CACHE BOOL "" FORCE)     # build bundled libjpeg-turbo
  set(UHDR_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  set(UHDR_ENABLE_LOGS OFF CACHE BOOL "" FORCE)
  # Write the gain map metadata in BOTH the XMP/GContainer packet (default off)
  # and the ISO 21496-1 packet (default on). The XMP form is what Chrome,
  # Windows Photos, and Android key off; ISO-only is recognised by a much
  # narrower/newer set of viewers - so without this the file decodes as plain
  # SDR almost everywhere.
  set(UHDR_WRITE_XMP ON CACHE BOOL "" FORCE)
  set(UHDR_WRITE_ISO ON CACHE BOOL "" FORCE)
  # Build uhdr as a static lib so there's no DLL to ship beside the exe. A
  # plain (non-cache) variable scopes this to the fetched subproject.
  set(BUILD_SHARED_LIBS OFF)
  FetchContent_Declare(libultrahdr
    GIT_REPOSITORY https://github.com/google/libultrahdr.git
    GIT_TAG        v1.4.0
  )
  FetchContent_MakeAvailable(libultrahdr)
endif()

add_executable(sundial WIN32
  src/main.cpp
  src/HdrCapture.cpp
  src/Encoder.cpp
  src/Tonemap.cpp
  src/Clipboard.cpp
  src/Toast.cpp
  src/Settings.cpp
  src/ImageOps.cpp
  src/ShaderTonemap.cpp
  src/VideoRecorder.cpp
  src/Toolbar.cpp
  src/Editor.cpp
  src/TrayIcon.cpp
  src/ShellIntegration.cpp
  src/Updater.cpp
  src/About.cpp
  src/Version.cpp
  src/sundial.rc
)

target_include_directories(sundial PRIVATE src)

target_link_libraries(sundial PRIVATE
  imgui
  d3d11.lib
  dxgi.lib
  dxguid.lib
  d3dcompiler.lib
  windowscodecs.lib
  mfplat.lib
  mfreadwrite.lib
  mfuuid.lib
  user32.lib
  gdi32.lib
  comctl32.lib
  shell32.lib
  shcore.lib
  ole32.lib
  comdlg32.lib
)

target_compile_definitions(sundial PRIVATE
  UNICODE _UNICODE
  WIN32_LEAN_AND_MEAN NOMINMAX
  SUNDIAL_VERSION="${SUNDIAL_VERSION}"
)

if(MSVC)
  target_compile_options(sundial PRIVATE /W4 /permissive- /Zc:__cplusplus)
endif()

if(SUNDIAL_ENABLE_ULTRAHDR AND TARGET uhdr)
  message(STATUS "Sundial: Ultra HDR JPEG export enabled (libultrahdr)")
  target_link_libraries(sundial PRIVATE uhdr)
  target_include_directories(sundial PRIVATE "${libultrahdr_SOURCE_DIR}")
  target_compile_definitions(sundial PRIVATE SUNDIAL_HAS_ULTRAHDR=1)
endif()

# --- Auto-update support (Velopack) -----------------------------------------
# Enabled automatically once velopack_libc has been vendored into
# third_party/velopack. Fetch it with:  pwsh tools/setup-velopack.ps1
# Until then the app builds exactly as before, with the updater stubbed out.
option(SUNDIAL_ENABLE_UPDATER "Enable Velopack auto-update support" ON)

set(VELOPACK_DIR "${CMAKE_SOURCE_DIR}/third_party/velopack")
if(SUNDIAL_ENABLE_UPDATER AND EXISTS "${VELOPACK_DIR}/include")
  file(GLOB VELOPACK_LIBS "${VELOPACK_DIR}/lib/*.lib")
  if(VELOPACK_LIBS)
    message(STATUS "Sundial: Velopack updater enabled (${VELOPACK_DIR})")
    target_compile_definitions(sundial PRIVATE SUNDIAL_HAS_UPDATER=1)
    target_include_directories(sundial PRIVATE "${VELOPACK_DIR}/include")
    target_link_libraries(sundial PRIVATE ${VELOPACK_LIBS})

    # Copy the velopack_libc DLL next to the exe so it's found at runtime.
    file(GLOB VELOPACK_DLLS "${VELOPACK_DIR}/bin/*.dll")
    foreach(_dll ${VELOPACK_DLLS})
      add_custom_command(TARGET sundial POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_dll}" "$<TARGET_FILE_DIR:sundial>")
    endforeach()
  else()
    message(WARNING
      "Sundial: third_party/velopack/include found but no lib/*.lib. "
      "Re-run tools/setup-velopack.ps1. Building without the updater.")
  endif()
else()
  message(STATUS
    "Sundial: Velopack updater NOT enabled "
    "(run tools/setup-velopack.ps1 to fetch velopack_libc). "
    "Building with the updater stubbed out.")
endif()
