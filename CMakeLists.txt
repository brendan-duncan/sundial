cmake_minimum_required(VERSION 3.20)
project(sundial LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WIN32)
  message(FATAL_ERROR "Sundial is Windows-only.")
endif()

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
  shell32.lib
  shcore.lib
  ole32.lib
  comdlg32.lib
)

target_compile_definitions(sundial PRIVATE
  UNICODE _UNICODE
  WIN32_LEAN_AND_MEAN NOMINMAX
)

if(MSVC)
  target_compile_options(sundial PRIVATE /W4 /permissive- /Zc:__cplusplus)
endif()
