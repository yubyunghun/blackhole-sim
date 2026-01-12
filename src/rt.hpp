#pragma once
#include <glad/glad.h>

struct RT {
    GLuint fbo=0, colorTex=0, depthRb=0;
    int w=0, h=0;

    void init(int W, int H);
    void begin();
    void end();       // just unbind to default FBO
    void shutdown();
};
