#include "rt.hpp"

void RT::init(int W, int H){
    w=W; h=H;
    glGenFramebuffers(1,&fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);

    glGenTextures(1,&colorTex);
    glBindTexture(GL_TEXTURE_2D, colorTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colorTex, 0);

    glGenRenderbuffers(1,&depthRb);
    glBindRenderbuffer(GL_RENDERBUFFER, depthRb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, depthRb);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void RT::begin(){
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0,0,w,h);
    glClearColor(0.02f,0.02f,0.03f,1.0f);
    glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
}

void RT::end(){
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void RT::shutdown(){
    if(depthRb){ glDeleteRenderbuffers(1,&depthRb); depthRb=0; }
    if(colorTex){ glDeleteTextures(1,&colorTex); colorTex=0; }
    if(fbo){ glDeleteFramebuffers(1,&fbo); fbo=0; }
}
