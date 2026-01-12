#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>

struct PostFX {
    // offscreen target
    GLuint fbo=0, colorTex=0, depthRb=0;
    int w=0, h=0;

    // fs-quad
    GLuint vao=0, vbo=0, ibo=0, prog=0;
    GLint loc_uTex=-1, loc_uInvRes=-1, loc_uExposure=-1, loc_uBloomK=-1, loc_uVignK=-1, loc_uThresh=-1;

    void init(int W, int H);
    void begin(); // bind FBO + viewport
    void end_and_draw(float exposure, float bloomK, float vignK, float threshold); // composite to backbuffer
    void shutdown();
};
