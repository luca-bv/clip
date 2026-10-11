cmake_minimum_required(VERSION 3.24)
project(Clip LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(tomlplusplus GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git GIT_TAG v3.4.0)
FetchContent_MakeAvailable(tomlplusplus)

# An LGPL shared FFmpeg build with include/, lib/ and bin/ (CI downloads one from BtbN/FFmpeg-Builds).
set(FFMPEG_DIR "${CMAKE_SOURCE_DIR}/ffmpeg" CACHE PATH "FFmpeg shared build")

add_executable(Clip WIN32 src/main.cpp src/session.cpp src/ring.cpp src/video.cpp src/audio.cpp src/overlay.cpp src/editor.cpp src/clip.manifest)
target_compile_definitions(Clip PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(Clip PRIVATE /utf-8 /W3 /permissive-)
set(FFMPEG_LIBS "${FFMPEG_DIR}/lib/avcodec.lib" "${FFMPEG_DIR}/lib/avformat.lib" "${FFMPEG_DIR}/lib/avutil.lib")
target_include_directories(Clip PRIVATE "${FFMPEG_DIR}/include")
target_link_libraries(Clip PRIVATE
  tomlplusplus::tomlplusplus ${FFMPEG_LIBS}
  d3d11 dxgi d2d1 dwrite comctl32 windowsapp mmdevapi winmm shell32 ole32 user32 advapi32)

file(GLOB FFMPEG_DLLS "${FFMPEG_DIR}/bin/*.dll")
add_custom_command(TARGET Clip POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${FFMPEG_DLLS} $<TARGET_FILE_DIR:Clip>)

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
