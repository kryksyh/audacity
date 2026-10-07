cmake_minimum_required(VERSION 3.16)
project(pa_rtl CXX)

# Point at the PortAudio that Audacity itself uses, e.g.
#   -DPORTAUDIO_ROOT=<audacity build dir>/_deps/portaudio
set(PORTAUDIO_ROOT "" CACHE PATH "PortAudio install prefix (include/ and lib/)")

find_path(PORTAUDIO_INCLUDE_DIR portaudio.h HINTS ${PORTAUDIO_ROOT}/include REQUIRED)
find_library(PORTAUDIO_LIBRARY NAMES portaudio portaudio_x64 HINTS ${PORTAUDIO_ROOT}/lib REQUIRED)

add_executable(pa_rtl main.cpp)
target_compile_features(pa_rtl PRIVATE cxx_std_17)
target_include_directories(pa_rtl PRIVATE ${PORTAUDIO_INCLUDE_DIR})
target_link_libraries(pa_rtl PRIVATE ${PORTAUDIO_LIBRARY})

get_filename_component(PORTAUDIO_LIB_DIR ${PORTAUDIO_LIBRARY} DIRECTORY)
set_target_properties(pa_rtl PROPERTIES BUILD_RPATH ${PORTAUDIO_LIB_DIR})
