#pragma once
#include <glad/glad.h>
GLuint compile_shader(GLenum type, const char* src);
GLuint link_program(GLuint vs, GLuint fs);
