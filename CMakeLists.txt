cmake_minimum_required(VERSION 3.24)
project(Clip LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static C++ runtime: Clip.exe runs without the Visual C++ Redistributable installed.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)
FetchContent_Declare(tomlplusplus GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git GIT_TAG v3.4.0)
FetchContent_MakeAvailable(tomlplusplus)
# WebView2 SDK for the settings window; the runtime ships with Windows 11. Loader linked statically.
FetchContent_Declare(webview2
  URL https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/1.0.4129.50/microsoft.web.webview2.1.0.4129.50.nupkg
  URL_HASH SHA256=D3934F482D484B89FB4825DF720C710664E1143A1E90F7B3A60794EF33F473D2
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(webview2)

# An LGPL shared FFmpeg build with include/, lib/ and bin/ (CI downloads one from BtbN/FFmpeg-Builds).
set(FFMPEG_DIR "${CMAKE_SOURCE_DIR}/ffmpeg" CACHE PATH "FFmpeg shared build")

add_executable(Clip WIN32 src/main.cpp src/session.cpp src/ring.cpp src/video.cpp src/gpu.cpp src/watermark.cpp src/export.cpp src/discord.cpp src/games.cpp src/voice.cpp src/audio.cpp src/overlay.cpp src/input.cpp src/editor.cpp src/settings.cpp src/clip.manifest src/clip.rc)
set_source_files_properties(src/clip.rc PROPERTIES OBJECT_DEPENDS "${CMAKE_SOURCE_DIR}/src/ui/index.html;${CMAKE_SOURCE_DIR}/src/clip.ico")
target_compile_definitions(Clip PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(Clip PRIVATE /utf-8 /W3 /permissive-)
set(FFMPEG_LIBS "${FFMPEG_DIR}/lib/avcodec.lib" "${FFMPEG_DIR}/lib/avformat.lib" "${FFMPEG_DIR}/lib/avutil.lib")
target_include_directories(Clip PRIVATE "${FFMPEG_DIR}/include" "${webview2_SOURCE_DIR}/build/native/include")
target_link_libraries(Clip PRIVATE
  tomlplusplus::tomlplusplus ${FFMPEG_LIBS} "${webview2_SOURCE_DIR}/build/native/x64/WebView2LoaderStatic.lib"
  d3d11 dxgi d2d1 dwrite d3dcompiler dwmapi version urlmon comctl32 windowsapp mmdevapi winmm shell32 ole32 user32 advapi32)

file(GLOB FFMPEG_DLLS "${FFMPEG_DIR}/bin/*.dll")
add_custom_command(TARGET Clip POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${FFMPEG_DLLS} $<TARGET_FILE_DIR:Clip>)

# What recording costs a game: cmake --build build --config Release --target clip_bench
add_executable(clip_bench EXCLUDE_FROM_ALL tools/bench.cpp src/ring.cpp src/video.cpp src/gpu.cpp src/watermark.cpp src/export.cpp src/voice.cpp src/overlay.cpp src/input.cpp)
target_compile_definitions(clip_bench PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(clip_bench PRIVATE /utf-8 /W3 /permissive-)
target_include_directories(clip_bench PRIVATE "${FFMPEG_DIR}/include")
target_link_libraries(clip_bench PRIVATE ${FFMPEG_LIBS} d3d11 dxgi d2d1 dwrite d3dcompiler windowsapp user32)
add_dependencies(clip_bench Clip)  # FFmpeg DLLs land next to Clip.exe

enable_testing()
add_executable(fifo_test tests/fifo_test.cpp)
add_test(NAME fifo COMMAND fifo_test)
add_executable(trail_test tests/trail_test.cpp)
add_test(NAME trail COMMAND trail_test)
add_executable(ring_test tests/ring_test.cpp src/ring.cpp)  # FFmpeg DLLs sit next to it, copied for Clip
target_compile_options(ring_test PRIVATE /utf-8)
target_include_directories(ring_test PRIVATE "${FFMPEG_DIR}/include")
target_link_libraries(ring_test PRIVATE ${FFMPEG_LIBS})
add_dependencies(ring_test Clip)
add_test(NAME ring COMMAND ring_test)
