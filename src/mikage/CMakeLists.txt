# ArtemisRuntime.framework: openArtemis as an engine inside Mikage's iOS app
# (top-level option OA_MIKAGE_RUNTIME). The SDL host it runs under ships with
# Mikage (Engine/MikageSDLHost) and is compiled in here, as every Mikage SDL
# engine does; its symbols stay hidden.
set(MIKAGE_SDL_HOST_DIR "" CACHE PATH "Mikage's Engine/MikageSDLHost directory")
if(NOT EXISTS "${MIKAGE_SDL_HOST_DIR}/MikageSDLHost.cpp")
  message(FATAL_ERROR "MIKAGE_SDL_HOST_DIR has no MikageSDLHost.cpp: '${MIKAGE_SDL_HOST_DIR}'")
endif()

find_package(PNG CONFIG REQUIRED)
find_package(Freetype CONFIG REQUIRED)

add_library(ArtemisRuntime SHARED
  ${PROJECT_SOURCE_DIR}/src/app/main.cpp
  MikageArtemisRuntime.cpp
  MikagePlatform.cpp
  ${MIKAGE_SDL_HOST_DIR}/MikageSDLHost.cpp
  ${MIKAGE_SDL_HOST_DIR}/MikageSDLHostAudioSession.mm
  MikageArtemisRuntime.h)

# main.cpp without SDL's main(); Escape steps back instead of quitting.
target_compile_definitions(ArtemisRuntime PRIVATE OA_HOST_LIBRARY=1)
target_include_directories(ArtemisRuntime PRIVATE
  ${PROJECT_SOURCE_DIR}/src/app
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${MIKAGE_SDL_HOST_DIR})
set_source_files_properties(${MIKAGE_SDL_HOST_DIR}/MikageSDLHostAudioSession.mm
  PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
target_compile_options(ArtemisRuntime PRIVATE
  $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra>)
target_link_libraries(ArtemisRuntime PRIVATE
  libopenartemis
  SDL3::SDL3
  PNG::PNG
  Freetype::Freetype
  "-framework Foundation"
  "-framework UIKit"
  "-framework AVFoundation"
  "-framework AudioToolbox"
  "-framework CoreFoundation"
  "-Wl,-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/exported_symbols.txt")

set_target_properties(ArtemisRuntime PROPERTIES
  FRAMEWORK TRUE
  PUBLIC_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/MikageArtemisRuntime.h"
  MACOSX_FRAMEWORK_IDENTIFIER "moe.cheney233.mikage.artemis-runtime"
  XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "moe.cheney233.mikage.artemis-runtime"
  XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED "NO"
  XCODE_ATTRIBUTE_DEFINES_MODULE "YES"
  XCODE_ATTRIBUTE_CLANG_ENABLE_MODULES "YES"
  XCODE_ATTRIBUTE_MODULEMAP_FILE "${CMAKE_CURRENT_SOURCE_DIR}/module.modulemap"
  XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
  XCODE_ATTRIBUTE_LD_DYLIB_INSTALL_NAME "@rpath/ArtemisRuntime.framework/ArtemisRuntime"
  XCODE_ATTRIBUTE_LD_RUNPATH_SEARCH_PATHS "@executable_path/Frameworks @loader_path/..")
