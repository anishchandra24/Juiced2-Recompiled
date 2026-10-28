# juiced2 - ReXGlue Recompiled Project (Juiced 2: Hot Import Nights)
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(juiced2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(Juiced2_SOURCES
    src/main.cpp
    src/juiced2_heap_fix.cpp
)

# Optional: embed the game's own icon into the exe. The icon is the game's
# artwork, so it is not part of this project. Run the game once, copy the
# juiced2.ico it writes next to the exe into res/, then rebuild.
if(WIN32 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/res/juiced2.ico")
    enable_language(RC)
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/juiced2_icon.rc"
         "1 ICON \"${CMAKE_CURRENT_SOURCE_DIR}/res/juiced2.ico\"\n")
    list(APPEND Juiced2_SOURCES "${CMAKE_CURRENT_BINARY_DIR}/juiced2_icon.rc")
endif()

if(WIN32)
    add_executable(juiced2 WIN32 ${Juiced2_SOURCES})
else()
    add_executable(juiced2 ${Juiced2_SOURCES})
endif()

rexglue_setup_target(juiced2)
