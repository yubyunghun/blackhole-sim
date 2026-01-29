@echo off
setlocal
REM Build main.exe in the repo root: src\*.cpp + GLAD + Dear ImGui, linked
REM against GLFW. Shaders are loaded at runtime from src\shaders\.
REM Run this from: "x64 Native Tools Command Prompt for VS 2022"
REM (Alternatively use CMake, see README.)

set CLFLAGS=/nologo /O2 /MD /std:c++17 /EHsc ^
 /Ithird_party\glad\include ^
 /Ithird_party\glfw\include ^
 /Ithird_party\glm ^
 /Ithird_party\imgui ^
 /Ithird_party\imgui\backends

set IMGUI=third_party\imgui\imgui.cpp ^
 third_party\imgui\imgui_draw.cpp ^
 third_party\imgui\imgui_tables.cpp ^
 third_party\imgui\imgui_widgets.cpp ^
 third_party\imgui\backends\imgui_impl_glfw.cpp ^
 third_party\imgui\backends\imgui_impl_opengl3.cpp

set LFLAGS=/SUBSYSTEM:CONSOLE ^
 /LIBPATH:third_party\glfw\lib-vc2022 ^
 glfw3.lib opengl32.lib gdi32.lib shell32.lib user32.lib

if not exist third_party\imgui\imgui.cpp (
    echo Dear ImGui is missing. Run: git submodule update --init
    exit /b 1
)

del /q *.obj 2>nul

cl %CLFLAGS% src\main.cpp src\physics.cpp src\render.cpp src\ui.cpp third_party\glad\src\glad.c %IMGUI% /link %LFLAGS% /out:main.exe
if errorlevel 1 exit /b 1

REM Physics self-test (no GPU needed): run bh_tests.exe
del /q *.obj 2>nul
cl %CLFLAGS% /Isrc tests\test_physics.cpp src\physics.cpp /link /SUBSYSTEM:CONSOLE /out:bh_tests.exe
endlocal
