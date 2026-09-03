cmake_minimum_required(VERSION 3.20)
project(SunEngine LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Opzioni di compilazione MSVC (posizionate all'inizio per tutti i target)
if (MSVC)
    add_compile_options(/utf-8)
endif()

add_compile_definitions(GLM_ENABLE_EXPERIMENTAL)

include(FetchContent)

# GLFW
FetchContent_Declare(glfw GIT_REPOSITORY https://github.com/glfw/glfw.git GIT_TAG 3.4)
FetchContent_MakeAvailable(glfw)

# EnTT (Singola dichiarazione pulita)
FetchContent_Declare(entt GIT_REPOSITORY https://github.com/skypjack/entt.git GIT_TAG v3.14.0)
FetchContent_MakeAvailable(entt)

# ==========================
# Recast & Detour (NavMesh 3D & Pathfinding)
# ==========================
set(RECASTNAVIGATION_DEMO OFF CACHE BOOL "" FORCE)     # Disabilita la Demo (rimuove la dipendenza da SDL2)
set(RECASTNAVIGATION_TESTS OFF CACHE BOOL "" FORCE)    # Disabilita i Test interni
set(RECASTNAVIGATION_EXAMPLES OFF CACHE BOOL "" FORCE) # Disabilita gli Esempi
FetchContent_Declare(
    recastnavigation
    GIT_REPOSITORY https://github.com/recastnavigation/recastnavigation.git
    GIT_TAG v1.6.0
)
FetchContent_MakeAvailable(recastnavigation)

# spdlog
FetchContent_Declare(spdlog GIT_REPOSITORY https://github.com/gabime/spdlog.git GIT_TAG v1.15.3)
FetchContent_MakeAvailable(spdlog)

# GLM
FetchContent_Declare(glm GIT_REPOSITORY https://github.com/g-truc/glm.git GIT_TAG 1.0.1)
FetchContent_MakeAvailable(glm)

# nlohmann_json
FetchContent_Declare(nlohmann_json GIT_REPOSITORY https://github.com/nlohmann/json.git GIT_TAG v3.11.3)
FetchContent_MakeAvailable(nlohmann_json)

# miniaudio
FetchContent_Declare(miniaudio GIT_REPOSITORY https://github.com/mackron/miniaudio.git GIT_TAG 0.11.21)
FetchContent_MakeAvailable(miniaudio)

find_package(OpenGL REQUIRED)

# Dear ImGui
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
    )
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
add_library(SunEngine STATIC ${ENGINE_SOURCES})

target_include_directories(SunEngine PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/Engine/Platform/GLAD/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/External/JoltPhysics"
    ${miniaudio_SOURCE_DIR}
    ${entt_SOURCE_DIR}/src
    ${recastnavigation_SOURCE_DIR}/Recast/Include  # <-- Include di Recast
    ${recastnavigation_SOURCE_DIR}/Detour/Include  # <-- Include di Detour
)

target_link_libraries(SunEngine PUBLIC 
    glfw 
    spdlog 
    OpenGL::GL 
    glm::glm 
    EnTT::EnTT 
    Jolt 
    nlohmann_json::nlohmann_json 
    imgui
    Detour  # <--- Tolto il prefisso RecastNavigation::
    Recast  # <--- Tolto il prefisso RecastNavigation::
)

# ==========================
# Gioco (Eseguibile)
# ==========================
file(GLOB_RECURSE SANDBOX_SOURCES CONFIGURE_DEPENDS "Sandbox/*.cpp" "Sandbox/*.h" "Sandbox/*.hpp")
add_executable(ProjectTrazyn ${SANDBOX_SOURCES})
target_link_libraries(ProjectTrazyn PRIVATE SunEngine)

add_custom_command(TARGET ProjectTrazyn POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_CURRENT_SOURCE_DIR}/Sandbox/GameData"
    "$<TARGET_FILE_DIR:ProjectTrazyn>/GameData"
    COMMENT "Sincronizzazione automatica cartella GameData..."
)