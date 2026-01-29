cmake_minimum_required(VERSION 3.16)
project(blackhole_sim VERSION 1.0.0 LANGUAGES C CXX)

# Windows release builds: link the C++ runtime statically so the .exe runs
# without the Visual C++ redistributable (uses GLFW's glfw3_mt.lib).
option(BH_STATIC_RUNTIME "MSVC: link the C++ runtime statically" OFF)
if(MSVC AND BH_STATIC_RUNTIME)
    cmake_policy(SET CMP0091 NEW)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

if(NOT EXISTS "${CMAKE_SOURCE_DIR}/third_party/imgui/imgui.cpp"
   OR NOT EXISTS "${CMAKE_SOURCE_DIR}/third_party/glm/glm/glm.hpp")
    message(FATAL_ERROR "Submodules missing. Run: git submodule update --init")
endif()

set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/third_party/imgui)

add_executable(blackhole_sim
    src/main.cpp
    src/physics.cpp
    src/render.cpp
    src/ui.cpp
    third_party/glad/src/glad.c
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)

target_include_directories(blackhole_sim PRIVATE
    src
    third_party/glad/include
    third_party/glm
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)

if(MSVC)
    # Windows: use the prebuilt GLFW that ships in third_party/
    target_include_directories(blackhole_sim PRIVATE third_party/glfw/include)
    if(BH_STATIC_RUNTIME)
        set(GLFW_LIB ${CMAKE_SOURCE_DIR}/third_party/glfw/lib-vc2022/glfw3_mt.lib)
    else()
        set(GLFW_LIB ${CMAKE_SOURCE_DIR}/third_party/glfw/lib-vc2022/glfw3.lib)
    endif()
    target_link_libraries(blackhole_sim PRIVATE ${GLFW_LIB} opengl32 gdi32 shell32 user32)
    target_compile_options(blackhole_sim PRIVATE /EHsc)
else()
    # Linux and others: use the system GLFW (e.g. apt install libglfw3-dev)
    find_package(glfw3 3.3 REQUIRED)
    find_package(OpenGL REQUIRED)
    target_link_libraries(blackhole_sim PRIVATE glfw OpenGL::GL ${CMAKE_DL_LIBS})
endif()

# Physics self-test: CPU only, no window or GPU needed (runs in CI)
enable_testing()
add_executable(bh_tests tests/test_physics.cpp src/physics.cpp)
target_include_directories(bh_tests PRIVATE src third_party/glad/include third_party/glm)
if(MSVC)
    target_include_directories(bh_tests PRIVATE third_party/glfw/include)
else()
    target_link_libraries(bh_tests PRIVATE glfw)   # only for the GLFW header path
endif()
add_test(NAME physics COMMAND bh_tests)

# Shaders are loaded at runtime from shaders/ next to the executable
add_custom_command(TARGET blackhole_sim POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/src/shaders $<TARGET_FILE_DIR:blackhole_sim>/shaders)
