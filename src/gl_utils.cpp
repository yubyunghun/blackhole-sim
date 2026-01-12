#include "gl_utils.hpp"

GLuint compile_shader(GLenum type, const char* src){
    GLuint s=glCreateShader(type);
    glShaderSource(s,1,&src,nullptr);
    glCompileShader(s);
    GLint ok=0; glGetShaderiv(s,GL_COMPILE_STATUS,&ok);
    if(!ok){ glDeleteShader(s); return 0; }
    return s;
}

GLuint link_program(GLuint vs, GLuint fs){
    GLuint p=glCreateProgram();
    glAttachShader(p,vs);
    glAttachShader(p,fs);
    glLinkProgram(p);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok=0; glGetProgramiv(p,GL_LINK_STATUS,&ok);
    if(!ok){ glDeleteProgram(p); return 0; }
    return p;
}
