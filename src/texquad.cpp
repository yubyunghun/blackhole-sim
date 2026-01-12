#include "texquad.hpp"
#include <glad/glad.h>

static GLuint make_prog(const char* vs, const char* fs){
    GLuint v=glCreateShader(GL_VERTEX_SHADER); glShaderSource(v,1,&vs,nullptr); glCompileShader(v);
    GLuint f=glCreateShader(GL_FRAGMENT_SHADER); glShaderSource(f,1,&fs,nullptr); glCompileShader(f);
    GLuint p=glCreateProgram(); glAttachShader(p,v); glAttachShader(p,f); glLinkProgram(p);
    glDeleteShader(v); glDeleteShader(f); return p;
}

bool TexQuad::init(unsigned w, unsigned h, bool linear){
    glGenTextures(1,&tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear?GL_LINEAR:GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear?GL_LINEAR:GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)w, (GLsizei)h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

    const char* vs =
        "#version 330 core\n"
        "layout(location=0) in vec2 aPos;\n"
        "layout(location=1) in vec2 aUV;\n"
        "uniform mat4 uVP;\n"
        "out vec2 vUV;\n"
        "void main(){ vUV=aUV; gl_Position=uVP*vec4(aPos,0,1); }\n";
    const char* fs =
        "#version 330 core\n"
        "in vec2 vUV; out vec4 o;\n"
        "uniform sampler2D uTex; uniform float uAlpha;\n"
        "void main(){ vec4 c=texture(uTex,vUV); o=vec4(c.rgb, c.a*uAlpha); }\n";
    prog = make_prog(vs,fs);
    locVP = glGetUniformLocation(prog,"uVP");
    locAlpha = glGetUniformLocation(prog,"uAlpha");
    locSampler = glGetUniformLocation(prog,"uTex");

    float verts[] = {
        -1, -1, 0,0,
         1, -1, 1,0,
         1,  1, 1,1,
        -1,  1, 0,1
    };
    unsigned short idx[] = {0,1,2, 0,2,3};

    glGenVertexArrays(1,&vao);
    glGenBuffers(1,&vbo);
    glGenBuffers(1,&ebo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER,vbo);
    glBufferData(GL_ARRAY_BUFFER,sizeof(verts),verts,GL_STATIC_DRAW);
    glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,4*sizeof(float),(void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1,2,GL_FLOAT,GL_FALSE,4*sizeof(float),(void*)(2*sizeof(float)));
    glEnableVertexAttribArray(1);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,sizeof(idx),idx,GL_STATIC_DRAW);
    glBindVertexArray(0);
    return true;
}

void TexQuad::update(const uint32_t* rgba_abgr){
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    // size is already set; we just sub-update the whole thing
    // caller must have used same WxH as init
    // We can't know size here, but GL allows full-size subimage if bound level has storage
    // Use GL to query width/height
    int w=0,h=0; glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_WIDTH,&w);
    glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_HEIGHT,&h);
    glTexSubImage2D(GL_TEXTURE_2D,0,0,0,w,h,GL_RGBA,GL_UNSIGNED_BYTE,rgba_abgr);
}

void TexQuad::draw_ndc(float x, float y, float w, float h, float alpha){
    float L=x, B=y, R=x+w, T=y+h;
    float ortho[16] = {
        1,0,0,0,
        0,1,0,0,
        0,0,1,0,
        0,0,0,1
    };
    float verts[] = {
        L, B, 0,0,
        R, B, 1,0,
        R, T, 1,1,
        L, T, 0,1
    };
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);

    glUseProgram(prog);
    glUniformMatrix4fv(locVP,1,GL_FALSE,ortho);
    glUniform1f(locAlpha, alpha);
    glUniform1i(locSampler, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);

    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, 0);
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    glBindVertexArray(0);
}

void TexQuad::shutdown(){
    if(ebo){ glDeleteBuffers(1,&ebo); ebo=0; }
    if(vbo){ glDeleteBuffers(1,&vbo); vbo=0; }
    if(vao){ glDeleteVertexArrays(1,&vao); vao=0; }
    if(prog){ glDeleteProgram(prog); prog=0; }
    if(tex){ glDeleteTextures(1,&tex); tex=0; }
}
