cmake_minimum_required(VERSION 3.20)
project(sundial VERSION 1.3.0 LANGUAGES CXX)

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

# --- HDR AVIF import/export (libavif) ---------------------------------------
# Fetched and built from source. libavif's AVIF_CODEC_AOM=LOCAL pulls and
# builds libaom (v3.12.0) for both encode and decode, so no system codec is
# needed. We disable aom's assembly (AOM_TARGET_CPU=generic / ENABLE_NASM=OFF)
# so the build needs no NASM - the same SIMD-free trade-off libultrahdr makes
# above; aom is only ever encoding/decoding single still screenshots, so the
# slower scalar path is fine. Turn off to build without AVIF - SaveAvifHdr()
# then throws and .avif files fall back to the WIC decode path (which, absent
# the OS AV1 Image Extension, simply fails to open them).
option(SUNDIAL_ENABLE_AVIF "Enable HDR AVIF import/export via libavif" ON)
if(SUNDIAL_ENABLE_AVIF)
  # aom (built by libavif's LOCAL mechanism): no assembly, no test/example/doc
  # baggage. These are read by aom's CMake when libavif FetchContent's it.
  set(AOM_TARGET_CPU "generic" CACHE STRING "" FORCE)  # disables all aom SIMD
  set(ENABLE_NASM OFF CACHE BOOL "" FORCE)
  set(ENABLE_TESTS OFF CACHE BOOL "" FORCE)
  set(ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(ENABLE_TOOLS OFF CACHE BOOL "" FORCE)
  set(ENABLE_DOCS OFF CACHE BOOL "" FORCE)

  # libavif itself: build aom locally for codec duties, skip the CLI apps /
  # tests / examples, and don't require system libyuv (libavif has a built-in
  # fallback for the colour conversions we use). Gain-map encode/decode is part
  # of the stable API as of libavif 1.2.0, so no extra flag is required.
  set(AVIF_CODEC_AOM "LOCAL" CACHE STRING "" FORCE)
  set(AVIF_LIBYUV "OFF" CACHE STRING "" FORCE)
  set(AVIF_LIBSHARPYUV "OFF" CACHE STRING "" FORCE)
  # Gain-map encode/decode is in the stable API as of 1.2.0; this flag is a
  # no-op there but harmless, and guards us if the implementation is ever gated.
  set(AVIF_ENABLE_EXPERIMENTAL_GAIN_MAP ON CACHE BOOL "" FORCE)
  set(AVIF_BUILD_APPS OFF CACHE BOOL "" FORCE)
  set(AVIF_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(AVIF_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(AVIF_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
  # libaom's CMake hard-requires Perl (even with assembly disabled). Rather than
  # make Perl a separate install prerequisite, fall back to the copy Git for
  # Windows already ships (we depend on git anyway). CI runners have Perl on
  # PATH, so this only kicks in on dev machines without a standalone Perl.
  find_package(Perl QUIET)
  if(NOT PERL_FOUND)
    find_program(GIT_EXECUTABLE git)
    if(GIT_EXECUTABLE)
      get_filename_component(_git_bin "${GIT_EXECUTABLE}" DIRECTORY)   # ...\cmd
      get_filename_component(_git_root "${_git_bin}" DIRECTORY)         # ...\Git
      find_program(PERL_EXECUTABLE perl
        HINTS "${_git_root}/usr/bin" "${_git_root}/mingw64/bin")
    endif()
    if(NOT PERL_EXECUTABLE)
      find_program(PERL_EXECUTABLE perl
        HINTS "C:/Program Files/Git/usr/bin"
              "C:/Program Files (x86)/Git/usr/bin"
              "$ENV{LOCALAPPDATA}/Programs/Git/usr/bin")
    endif()
    if(PERL_EXECUTABLE)
      message(STATUS "Sundial: using Git-bundled Perl for libaom: "
                     "${PERL_EXECUTABLE}")
      set(PERL_EXECUTABLE "${PERL_EXECUTABLE}" CACHE FILEPATH "" FORCE)
    else()
      message(WARNING
        "Sundial: Perl not found - libaom (HDR AVIF) configure will fail. "
        "Install Strawberry Perl, or build with -DSUNDIAL_ENABLE_AVIF=OFF.")
    endif()
  endif()

  # Static lib so there's no DLL to ship beside the exe (scoped to the fetched
  # subproject by the plain BUILD_SHARED_LIBS variable above).
  set(BUILD_SHARED_LIBS OFF)
  FetchContent_Declare(libavif
    GIT_REPOSITORY https://github.com/AOMediaCodec/libavif.git
    GIT_TAG        v1.2.0
  )
  FetchContent_MakeAvailable(libavif)
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
  # Target Windows 10: needed for SetProcessDpiAwarenessContext and the newer
  # WIC pixel-format GUIDs (64bppPRGBAHalf, 96bppRGBFloat, ...). Pin it
  # explicitly so the fetched libaom subproject can't lower it globally.
  _WIN32_WINNT=0x0A00 WINVER=0x0A00
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

if(SUNDIAL_ENABLE_AVIF AND TARGET avif)
  message(STATUS "Sundial: HDR AVIF import/export enabled (libavif + aom)")
  target_link_libraries(sundial PRIVATE avif)
  target_include_directories(sundial PRIVATE "${libavif_SOURCE_DIR}/include")
  target_compile_definitions(sundial PRIVATE SUNDIAL_HAS_AVIF=1)
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
