#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>

struct LensWarp {
    GLuint vao=0, vbo=0, ibo=0, prog=0;
    GLint loc_uBg=-1, loc_uInvRes=-1, loc_uBHuv=-1, loc_uRadPx=-1, loc_uK=-1, loc_uShadow=-1, loc_uTint=-1;

    void init();
    // draw: samples bgTex, warps around BH center (bhUV in [0,1]), radius in pixels
    void draw(GLuint bgTex, int W, int H, glm::vec2 bhUV, float radPx, float strengthK, float shadowHardness, float tintMix);
    void shutdown();
};
