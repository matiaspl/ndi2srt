cmake_minimum_required(VERSION 3.16)

project(ndi2srt VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

include(CheckIncludeFiles)

find_package(PkgConfig REQUIRED)

# Core GStreamer
pkg_check_modules(GST REQUIRED gstreamer-1.0>=1.20 gstreamer-video-1.0>=1.20)

add_executable(ndi2srt
    src/main.c
)

target_include_directories(ndi2srt PRIVATE
    ${GST_INCLUDE_DIRS}
)

target_link_directories(ndi2srt PRIVATE
    ${GST_LIBRARY_DIRS}
)

target_link_libraries(ndi2srt PRIVATE
    ${GST_LIBRARIES}
)

target_compile_options(ndi2srt PRIVATE
    ${GST_CFLAGS_OTHER}
)

if(APPLE)
    # Enable RPATH so the binary can find Homebrew-installed GStreamer dylibs at runtime.
    set(CMAKE_INSTALL_RPATH "@executable_path;@loader_path")
endif()

install(TARGETS ndi2srt RUNTIME DESTINATION bin)


