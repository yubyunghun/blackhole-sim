#include "render2d.hpp"
#include "gl_utils.hpp"
#include <vector>
#include <cmath>

bool Render2D::init(){
    const char* vsSrc = R"(#version 330 core
    layout(location=0) in vec2 aPos;
    void main(){ gl_Position = vec4(aPos, 0.0, 1.0); })";

    const char* fsSrc = R"(#version 330 core
    uniform vec4 uColor;
    out vec4 FragColor;
    void main(){ FragColor = uColor; })";

    GLuint vs = compile_shader(GL_VERTEX_SHADER, vsSrc);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fsSrc);
    prog = link_program(vs, fs);
    uColor = glGetUniformLocation(prog, "uColor");

    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    return (prog && vao && vbo);
}

void Render2D::shutdown(){
    if(vbo) glDeleteBuffers(1,&vbo);
    if(vao) glDeleteVertexArrays(1,&vao);
    if(prog) glDeleteProgram(prog);
    vbo=vao=prog=0;
}

void Render2D::draw_circle(float cx,float cy,float r,int segments,float rc,float gc,float bc){
    std::vector<float> verts; verts.reserve((segments+2)*2);
    verts.push_back(cx); verts.push_back(cy);
    for(int i=0;i<=segments;i++){
        float a = 6.28318530718f * (float(i)/segments);
        verts.push_back(cx + r*std::cos(a));
        verts.push_back(cy + r*std::sin(a));
    }
    glUseProgram(prog);
    glUniform4f(uColor, rc, gc, bc, 1.0f);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float)*verts.size(), verts.data(), GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2*sizeof(float), (void*)0);
    glDrawArrays(GL_TRIANGLE_FAN, 0, (GLsizei)(verts.size()/2));
    glBindVertexArray(0);
}

void Render2D::draw_polyline(const std::vector<float>& xy, float r, float g, float b, float a){
    if(xy.size() < 4) return;
    glUseProgram(prog);
    glUniform4f(uColor, r, g, b, a);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float)*xy.size(), xy.data(), GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2*sizeof(float), (void*)0);
    glLineWidth(1.5f);
    glDrawArrays(GL_LINE_STRIP, 0, (GLsizei)(xy.size()/2));
    glBindVertexArray(0);
}

void Render2D::enable_blend(){
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}
