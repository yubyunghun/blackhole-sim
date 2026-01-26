cmake_minimum_required(VERSION 3.16)
project(blackhole_sim LANGUAGES C CXX)

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
    target_link_libraries(blackhole_sim PRIVATE
        ${CMAKE_SOURCE_DIR}/third_party/glfw/lib-vc2022/glfw3.lib
        opengl32 gdi32 shell32 user32)
    target_compile_options(blackhole_sim PRIVATE /EHsc)
else()
    # Linux and others: use the system GLFW (e.g. apt install libglfw3-dev)
    find_package(glfw3 3.3 REQUIRED)
    find_package(OpenGL REQUIRED)
    target_link_libraries(blackhole_sim PRIVATE glfw OpenGL::GL ${CMAKE_DL_LIBS})
endif()

# Shaders are loaded at runtime from shaders/ next to the executable
add_custom_command(TARGET blackhole_sim POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/src/shaders $<TARGET_FILE_DIR:blackhole_sim>/shaders)
