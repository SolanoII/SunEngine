cmake_minimum_required(VERSION 3.20)

project(SunEngine LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_definitions(GLM_ENABLE_EXPERIMENTAL)

include(FetchContent)

# ==========================
# GLFW
# ==========================
FetchContent_Declare(glfw GIT_REPOSITORY https://github.com/glfw/glfw.git GIT_TAG 3.4)
FetchContent_MakeAvailable(glfw)

# ==========================
# EnTT
# ==========================
FetchContent_Declare(entt GIT_REPOSITORY https://github.com/skypjack/entt.git GIT_TAG v3.14.0)
FetchContent_MakeAvailable(entt)

# ==========================
# Recast & Detour
# ==========================
set(RECASTNAVIGATION_DEMO OFF CACHE BOOL "" FORCE)
set(RECASTNAVIGATION_TESTS OFF CACHE BOOL "" FORCE)
set(RECASTNAVIGATION_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    recastnavigation
    GIT_REPOSITORY https://github.com/recastnavigation/recastnavigation.git
    GIT_TAG v1.6.0
)
FetchContent_MakeAvailable(recastnavigation)

# ==========================
# Assimp (Importatore Modelli 3D)
# ==========================
set(ASSIMP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(ASSIMP_INSTALL_PDB OFF CACHE BOOL "" FORCE)
set(ASSIMP_INJECT_DEBUG_POSTFIX OFF CACHE BOOL "" FORCE)

set(ASSIMP_BUILD_EXPORTERS OFF CACHE BOOL "" FORCE)
set(ASSIMP_NO_EXPORT ON CACHE BOOL "" FORCE)

set(ASSIMP_INSTALL OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_INSTALL OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

set(ASSIMP_BUILD_ALL_IMPORTERS_BY_DEFAULT OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_GLTF2_IMPORTER ON CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_FBX_IMPORTER ON CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_OBJ_IMPORTER ON CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_STL_IMPORTER ON CACHE BOOL "" FORCE)

FetchContent_Declare(
    assimp
    GIT_REPOSITORY https://github.com/assimp/assimp.git
    GIT_TAG        v5.4.3
)
FetchContent_MakeAvailable(assimp)

# ==========================
# spdlog
# ==========================
FetchContent_Declare(spdlog GIT_REPOSITORY https://github.com/gabime/spdlog.git GIT_TAG v1.15.3)
FetchContent_MakeAvailable(spdlog)

# ==========================
# GLM
# ==========================
FetchContent_Declare(
    glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG        1.0.1
)
FetchContent_MakeAvailable(glm)

# ==========================
# nlohmann_json
# ==========================
FetchContent_Declare(nlohmann_json GIT_REPOSITORY https://github.com/nlohmann/json.git GIT_TAG v3.11.3)
FetchContent_MakeAvailable(nlohmann_json)

# ==========================
# miniaudio
# ==========================
FetchContent_Declare(miniaudio GIT_REPOSITORY https://github.com/mackron/miniaudio.git GIT_TAG 0.11.21)
FetchContent_MakeAvailable(miniaudio)

# ==========================
# ENet (UDP affidabile per squadra, lobby e peer to peer: Engine/Net)
# Compilata da qui con i suoi sorgenti: il suo CMakeLists e' troppo vecchio per le versioni recenti di CMake.
# ==========================
FetchContent_Declare(enet GIT_REPOSITORY https://github.com/lsalzman/enet.git GIT_TAG v1.3.18)
FetchContent_GetProperties(enet)
if(NOT enet_POPULATED)
    FetchContent_Populate(enet)
    add_library(enet STATIC
        ${enet_SOURCE_DIR}/callbacks.c
        ${enet_SOURCE_DIR}/compress.c
        ${enet_SOURCE_DIR}/host.c
        ${enet_SOURCE_DIR}/list.c
        ${enet_SOURCE_DIR}/packet.c
        ${enet_SOURCE_DIR}/peer.c
        ${enet_SOURCE_DIR}/protocol.c
        ${enet_SOURCE_DIR}/unix.c
        ${enet_SOURCE_DIR}/win32.c
    )
    target_include_directories(enet PUBLIC ${enet_SOURCE_DIR}/include)
    if(WIN32)
        target_link_libraries(enet PUBLIC ws2_32 winmm)
    else()
        target_compile_definitions(enet PRIVATE HAS_SOCKLEN_T=1)
    endif()
endif()

# ==========================
# STB (header-only: stb_image, stb_image_write)
# ==========================
FetchContent_Declare(
    stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG        master
)
FetchContent_MakeAvailable(stb)

# STB image implementation (va compilata una sola volta in tutto il progetto)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/stb_image_impl.c"
"#define STB_IMAGE_IMPLEMENTATION\n#include \"stb_image.h\"\n"
)
add_library(stb_image STATIC "${CMAKE_CURRENT_BINARY_DIR}/stb_image_impl.c" "Engine/Renderer/PostFX/AutoExposure.h")
target_include_directories(stb_image PUBLIC ${stb_SOURCE_DIR})

find_package(OpenGL REQUIRED)

# ==========================
# Dear ImGui
# ==========================
FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git GIT_TAG v1.90.8)
FetchContent_GetProperties(imgui)
if(NOT imgui_POPULATED)
    FetchContent_Populate(imgui)
    add_library(imgui STATIC
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/imgui_demo.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
     "Engine/Renderer/PostFX/AutoExposure.h")
    target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
    target_link_libraries(imgui PUBLIC glfw OpenGL::GL)
endif()

# ==========================
# Jolt Physics
# ==========================
set(TARGET_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(TARGET_SAMPLES OFF CACHE BOOL "" FORCE)
set(TARGET_HELLO_WORLD OFF CACHE BOOL "" FORCE)
set(TARGET_PERFORMANCE_TEST OFF CACHE BOOL "" FORCE)
set(TARGET_JOLT_VIEWER OFF CACHE BOOL "" FORCE)
set(USE_DX12 OFF CACHE BOOL "" FORCE)
set(USE_VK OFF CACHE BOOL "" FORCE)
set(USE_HLSL OFF CACHE BOOL "" FORCE)
set(USE_STATIC_MSVC_RUNTIME_LIBRARY OFF CACHE BOOL "" FORCE)

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/External/JoltPhysics/Build" EXCLUDE_FROM_ALL)

# ==========================
# Engine (Libreria Statica)
# ==========================
file(GLOB_RECURSE ENGINE_SOURCES CONFIGURE_DEPENDS "Engine/*.cpp" "Engine/*.c" "Engine/*.h" "Engine/*.hpp")
add_library(SunEngine STATIC ${ENGINE_SOURCES} "Engine/Renderer/PostFX/AutoExposure.h")

target_include_directories(SunEngine PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/Engine/Platform/GLAD/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/External/JoltPhysics"
    ${miniaudio_SOURCE_DIR}
    ${entt_SOURCE_DIR}/src
    ${recastnavigation_SOURCE_DIR}/Recast/Include
    ${recastnavigation_SOURCE_DIR}/Detour/Include
)

target_link_libraries(SunEngine PUBLIC 
    glfw 
    spdlog 
    OpenGL::GL 
    glm::glm 
    EnTT::EnTT 
    Jolt 
    nlohmann_json::nlohmann_json
    enet 
    imgui
    Detour 
    Recast 
    assimp
    stb_image
)

# ==========================
# Gioco (Eseguibile)
# ==========================
file(GLOB_RECURSE SANDBOX_SOURCES CONFIGURE_DEPENDS "Sandbox/*.cpp" "Sandbox/*.h" "Sandbox/*.hpp")
add_executable(ProjectTrazyn ${SANDBOX_SOURCES} "Engine/Renderer/PostFX/AutoExposure.h")
target_link_libraries(ProjectTrazyn PRIVATE SunEngine)

# ==========================
# Server d'incontro (programma a parte: lobby via internet senza aprire porte, vedi Tools/TrazynServer)
# ==========================
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/Tools/TrazynServer")

# ==========================
# OPZIONI DI COMPILAZIONE MIRATE MSVC
# ==========================
if (MSVC)
    target_compile_options(SunEngine PRIVATE /utf-8 /bigobj /MP)
    target_compile_options(ProjectTrazyn PRIVATE /utf-8 /bigobj /MP)
endif()

add_custom_command(TARGET ProjectTrazyn POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_CURRENT_SOURCE_DIR}/Sandbox/GameData"
    "$<TARGET_FILE_DIR:ProjectTrazyn>/GameData"
    COMMENT "Sincronizzazione automatica cartella GameData..."
)