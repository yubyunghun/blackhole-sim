#include "gl_program.hpp"
#include <glad/glad.h>
#include <fstream>
#include <sstream>

std::string read_text_file(const char* path){
    std::ifstream f(path, std::ios::binary);
    if(!f) return {};
    std::ostringstream ss; ss << f.rdbuf();
    return ss.str();
}

bool GLProgram::load_compute_from_file(const char* path){
    std::string src = read_text_file(path);
    if(src.empty()) return false;
    unsigned sh = glCreateShader(GL_COMPUTE_SHADER);
    const char* p = src.c_str();
    glShaderSource(sh,1,&p,nullptr);
    glCompileShader(sh);
    int ok=0; glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if(!ok){ glDeleteShader(sh); return false; }
    prog = glCreateProgram();
    glAttachShader(prog, sh);
    glLinkProgram(prog);
    glDeleteShader(sh);
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if(!ok){ glDeleteProgram(prog); prog=0; return false; }
    return true;
}

void GLProgram::use() const { glUseProgram(prog); }
void GLProgram::destroy(){ if(prog){ glDeleteProgram(prog); prog=0; } }
