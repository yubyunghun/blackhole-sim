#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>

struct Disk3D {
    // GL objects
    GLuint vao=0, vbo=0, ibo=0, prog=0;

    // Uniform locations
    GLint loc_uVP=-1, loc_uM=-1, loc_uCam=-1, loc_uTime=-1;
    GLint loc_uParams1=-1, loc_uParams2=-1, loc_uParams3=-1, loc_uFlags=-1;

    // Cached params (optional, used for live tweaks & debugging)
    float diskRadius    = 0.38f;
    float ringRadius    = 0.58f;
    float ringThickness = 0.14f;
    float spinSpeed     = 0.85f;
    float beaming       = 1.25f;
    bool  billboard     = true;

    // Crescent shaping
    float occlBias      = 0.10f;
    float occlPower     = 1.15f;
    float rimWidth      = 0.006f;
    float rimStrength   = 1.10f;

    void init();
    void draw(const glm::mat4& VP,
              const glm::mat4& M,
              float diskR, float ringR, float ringT,
              float spinSpd, float beamAmt, bool billboardOn,
              float occBias, float occPow, float rimW, float rimK);
    void shutdown();
};
