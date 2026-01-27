# Third-party libraries

| Library | Use | License | Notice |
|---|---|---|---|
| [GLFW](https://www.glfw.org/) 3.4 | Window, input, OpenGL context | zlib/libpng | In the headers in `glfw/include/GLFW/` |
| [GLAD](https://glad.dav1d.de/) | OpenGL function loader (generated for GL 3.3 core) | Generated code: public domain / CC0. The Khronos `khrplatform.h` is MIT-style. | In `glad/include/KHR/khrplatform.h` |
| [GLM](https://github.com/g-truc/glm) | Vector/matrix math (git submodule) | Happy Bunny License or MIT | `glm/copying.txt` |
| [Dear ImGui](https://github.com/ocornut/imgui) v1.91.5 | Control panel (git submodule) | MIT | `imgui/LICENSE.txt` |
| stb_easy_font | (bundled, currently unused) | Public domain / MIT | In `stb/stb_easy_font.h` |

`glfw/lib-vc2022/` holds prebuilt GLFW binaries for Visual Studio 2022
(covered by GLFW's zlib license).
