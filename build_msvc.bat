@echo off
setlocal
REM Build only the clean-room main + glad, and link GLFW.
REM Run this from: "x64 Native Tools Command Prompt for VS 2022"

set CLFLAGS=/nologo /O2 /MD /std:c++17 /EHsc ^
 /Ithird_party\glad\include ^
 /Ithird_party\glfw\include ^
 /Ithird_party\glm

set LFLAGS=/SUBSYSTEM:CONSOLE ^
 /LIBPATH:third_party\glfw\lib-vc2022 ^
 glfw3.lib opengl32.lib gdi32.lib shell32.lib user32.lib

del /q *.obj 2>nul

cl %CLFLAGS% src\main.cpp third_party\glad\src\glad.c /link %LFLAGS% /out:main.exe
endlocal
