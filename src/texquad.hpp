#pragma once
#include <glad/glad.h>
#include <cstdint>

struct TexQuad {
    GLuint tex = 0;
    GLuint vao = 0, vbo = 0, ebo = 0, prog = 0;
    GLint  locVP = -1, locAlpha = -1, locSampler = -1;

    bool init(unsigned w, unsigned h, bool linear);
    void update(const uint32_t* rgba_abgr);
    void draw_ndc(float x, float y, float w, float h, float alpha);
    void shutdown();
};
