# Server d'incontro di Project Trazyn: elenco delle lobby, codici a 6 cifre, apertura dei router, ponte di riserva.
# Si compila col resto del progetto (bersaglio TrazynServer) oppure da solo, per esempio su un server Linux:
#   cmake -S Tools/TrazynServer -B build-server -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-server
#   ./build-server/TrazynServer 27640
cmake_minimum_required(VERSION 3.16)
project(TrazynServer CXX C)

if(NOT TARGET enet)
    include(FetchContent)
    FetchContent_Declare(enet GIT_REPOSITORY https://github.com/lsalzman/enet.git GIT_TAG v1.3.18)
    FetchContent_GetProperties(enet)
    if(NOT enet_POPULATED)
        FetchContent_Populate(enet)
    endif()
    add_library(enet STATIC
        ${enet_SOURCE_DIR}/callbacks.c ${enet_SOURCE_DIR}/compress.c ${enet_SOURCE_DIR}/host.c ${enet_SOURCE_DIR}/list.c
        ${enet_SOURCE_DIR}/packet.c ${enet_SOURCE_DIR}/peer.c ${enet_SOURCE_DIR}/protocol.c ${enet_SOURCE_DIR}/unix.c ${enet_SOURCE_DIR}/win32.c)
    target_include_directories(enet PUBLIC ${enet_SOURCE_DIR}/include)
    if(WIN32)
        target_link_libraries(enet PUBLIC ws2_32 winmm)
    else()
        target_compile_definitions(enet PRIVATE HAS_SOCKLEN_T=1)
    endif()
endif()

add_executable(TrazynServer main.cpp)
target_compile_features(TrazynServer PRIVATE cxx_std_17)
target_include_directories(TrazynServer PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../Engine/Net")
target_link_libraries(TrazynServer PRIVATE enet)
