cmake_minimum_required(VERSION 3.16)
project(esca-voice VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(GNUInstallDirs)
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(SDBUSCPP REQUIRED IMPORTED_TARGET sdbus-c++)

# The audio framework, and the shared client for the AI server.
find_library(ESCA_AUDCLIENT_LIB NAMES esca-audclient REQUIRED)
find_path(ESCA_AUDIO_INCLUDE_DIR esca/audio/client.hpp REQUIRED)
find_library(ESCA_AICLIENT_LIB NAMES esca-aiclient REQUIRED)
find_path(ESCA_AI_INCLUDE_DIR esca/ai/client.hpp REQUIRED)

add_executable(esca-voiced src/main.cpp src/vad.cpp)
target_include_directories(esca-voiced PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src ${ESCA_AUDIO_INCLUDE_DIR} ${ESCA_AI_INCLUDE_DIR})
target_link_libraries(esca-voiced PRIVATE
    PkgConfig::SDBUSCPP ${ESCA_AICLIENT_LIB} ${ESCA_AUDCLIENT_LIB} Threads::Threads)

install(TARGETS esca-voiced RUNTIME DESTINATION ${CMAKE_INSTALL_SBINDIR})
install(FILES data/vn.sparc.esca.Voice1.conf
        DESTINATION ${CMAKE_INSTALL_DATADIR}/dbus-1/system.d)
install(FILES data/esca-voiced.service
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/systemd/system)
