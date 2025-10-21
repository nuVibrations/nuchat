# macOS flavor of nuChat.
#
# To Build:
#
# cmake -B build -GXcode
# cmake --build build --config Release
#

cmake_minimum_required(VERSION 3.20)
project(mac_voice_loopback LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(SRC vpio_loopback.cpp)

add_executable(mac_voice_loopback ${SRC})

# macOS frameworks
find_library(AUDIOTOOLBOX AudioToolbox)
find_library(AUDIOUNIT AudioUnit)
find_library(COREAUDIO CoreAudio)

target_link_libraries(mac_voice_loopback
    ${AUDIOTOOLBOX}
    ${AUDIOUNIT}
    ${COREAUDIO}
)
