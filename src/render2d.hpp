#pragma once
#include <glad/glad.h>

struct Render2D {
    GLuint fbo=0, color=0, depth=0;
    GLuint vao=0, vbo=0, ibo=0;
    GLuint prog_bright=0, prog_blur=0, prog_comp=0;
    int W=0, H=0;

    void make_quad(){
        static const float QUAD_POS[8] = {-1,-1, 1,-1, 1,1, -1,1};
        static const unsigned QUAD_IDX[6]={0,1,2,0,2,3};
        glGenVertexArrays(1,&vao);
        glGenBuffers(1,&vbo);
        glGenBuffers(1,&ibo);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER,vbo);
        glBufferData(GL_ARRAY_BUFFER,sizeof(QUAD_POS),QUAD_POS,GL_STATIC_DRAW);
        glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,8,(void*)0);
        glEnableVertexAttribArray(0);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER,sizeof(QUAD_IDX),QUAD_IDX,GL_STATIC_DRAW);
        glBindVertexArray(0);
    }

    void alloc_targets(int w,int h){
        W=w; H=h;
        glGenFramebuffers(1,&fbo);
        glBindFramebuffer(GL_FRAMEBUFFER,fbo);

        glGenTextures(1,&color);
        glBindTexture(GL_TEXTURE_2D,color);
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA16F,W,H,0,GL_RGBA,GL_FLOAT,nullptr);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
        glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,color,0);

        glGenRenderbuffers(1,&depth);
        glBindRenderbuffer(GL_RENDERBUFFER,depth);
        glRenderbufferStorage(GL_RENDERBUFFER,GL_DEPTH24_STENCIL8,W,H);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_DEPTH_STENCIL_ATTACHMENT,GL_RENDERBUFFER,depth);
        glBindFramebuffer(GL_FRAMEBUFFER,0);
    }

    void init(int w,int h){
        make_quad();
        alloc_targets(w,h);

        const char* VS = R"(#version 330 core
        layout(location=0) in vec2 aPos; out vec2 v;
        void main(){ v=(aPos+1.0)*0.5; gl_Position=vec4(aPos,0,1); })";

        const char* FS_BRIGHT = R"(#version 330 core
        in vec2 v; out vec4 o; uniform sampler2D tex;
        const float THRESH = 0.65;  // highlight threshold
        const float KNEE   = 0.20;  // soft knee
        void main(){
            vec3 c = texture(tex, v).rgb;
            float l = max(max(c.r,c.g),c.b);
            float t = max(l - THRESH, 0.0);
            float soft = t*t/(KNEE + 1e-6 + t);
            float w = max(t, soft) / max(l, 1e-6);
            o = vec4(c*w,1.0);
        })";

        const char* FS_BLUR = R"(#version 330 core
        in vec2 v; out vec4 o; uniform sampler2D tex; uniform vec2 dir;
        void main(){
            vec2 px = 1.0/vec2(textureSize(tex,0));
            vec3 s=vec3(0);
            float w0=0.204164, w1=0.304005, w2=0.193783, w3=0.073377, w4=0.024671;
            s += texture(tex, v + dir*px*0.0).rgb * w0;
            s += texture(tex, v + dir*px*1.5).rgb * w1;
            s += texture(tex, v - dir*px*1.5).rgb * w1;
            s += texture(tex, v + dir*px*3.5).rgb * w2;
            s += texture(tex, v - dir*px*3.5).rgb * w2;
            s += texture(tex, v + dir*px*6.5).rgb * w3;
            s += texture(tex, v - dir*px*6.5).rgb * w3;
            s += texture(tex, v + dir*px*10.0).rgb * w4;
            s += texture(tex, v - dir*px*10.0).rgb * w4;
            o = vec4(s,1);
        })";

        const char* FS_COMP = R"(#version 330 core
        in vec2 v; out vec4 o;
        uniform sampler2D baseTex;
        uniform sampler2D blurH;
        uniform sampler2D blurV;
        uniform vec2 ringNDC; uniform float lensK;
        void main(){
            vec2 ndc = v*2.0-1.0;
            vec2 d = ndc - ringNDC;
            float r = length(d);
            float s = 1.0 + lensK/(1.0 + 60.0*r*r);
            vec2 uv = (ringNDC + d*s)*0.5 + 0.5;

            vec3 base  = texture(baseTex, clamp(uv,0.0,1.0)).rgb;
            vec3 bloom = (texture(blurH, clamp(uv,0.0,1.0)).rgb
                        + texture(blurV, clamp(uv,0.0,1.0)).rgb) * 0.55;

            vec2 nv = ndc * vec2(1.0, 1.08) - vec2(0.0, 0.06);
            float vign = smoothstep(1.70, 0.45, length(nv));

            vec3 col = (base + bloom) * vign;
            col *= vec3(1.03,1.00,0.97);
            o = vec4(col,1);
        })";

        auto mk = [](GLenum st,const char* src){ GLuint s=glCreateShader(st); glShaderSource(s,1,&src,nullptr); glCompileShader(s); return s; };
        auto mkp=[&](const char* vs,const char* fs){ GLuint v=mk(GL_VERTEX_SHADER,vs), f=mk(GL_FRAGMENT_SHADER,fs);
            GLuint p=glCreateProgram(); glAttachShader(p,v); glAttachShader(p,f); glLinkProgram(p);
            glDeleteShader(v); glDeleteShader(f); return p; };

        prog_bright = mkp(VS,FS_BRIGHT);
        prog_blur   = mkp(VS,FS_BLUR);
        prog_comp   = mkp(VS,FS_COMP);
    }

    void resize(int w,int h){
        if(w==W && h==H) return;
        if(depth){ glDeleteRenderbuffers(1,&depth); depth=0; }
        if(color){ glDeleteTextures(1,&color); color=0; }
        if(!fbo){ glGenFramebuffers(1,&fbo); }
        alloc_targets(w,h);
    }

    void begin(){ glBindFramebuffer(GL_FRAMEBUFFER,fbo); glViewport(0,0,W,H); }
    void end(){ glBindFramebuffer(GL_FRAMEBUFFER,0); }

    void bright_and_blur(GLuint src, GLuint tmpH, GLuint tmpV){
        glBindFramebuffer(GL_FRAMEBUFFER,fbo);
        glViewport(0,0,W,H);

        glUseProgram(prog_bright);
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, src);
        glUniform1i(glGetUniformLocation(prog_bright,"tex"),0);
        glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,tmpH,0);
        glBindVertexArray(vao); glDrawElements(GL_TRIANGLES,6,GL_UNSIGNED_INT,0);

        glUseProgram(prog_blur);
        glUniform2f(glGetUniformLocation(prog_blur,"dir"),1,0);
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D,tmpH);
        glUniform1i(glGetUniformLocation(prog_blur,"tex"),0);
        glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,tmpV,0);
        glDrawElements(GL_TRIANGLES,6,GL_UNSIGNED_INT,0);

        glUniform2f(glGetUniformLocation(prog_blur,"dir"),0,1);
        glBindTexture(GL_TEXTURE_2D,tmpV);
        glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,tmpH,0);
        glDrawElements(GL_TRIANGLES,6,GL_UNSIGNED_INT,0);

        glBindVertexArray(0); glUseProgram(0); glBindFramebuffer(GL_FRAMEBUFFER,0);
    }

    void composite(GLuint baseTex, GLuint blurH, GLuint blurV, float cx_ndc, float cy_ndc, float lensK, int fbW, int fbH){
        glViewport(0,0,fbW,fbH);                 // << fix quarter-screen on high-DPI
        glUseProgram(prog_comp);
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, baseTex);
        glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, blurH);
        glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, blurV);
        glUniform1i(glGetUniformLocation(prog_comp,"baseTex"),0);
        glUniform1i(glGetUniformLocation(prog_comp,"blurH"),1);
        glUniform1i(glGetUniformLocation(prog_comp,"blurV"),2);
        glUniform2f(glGetUniformLocation(prog_comp,"ringNDC"), cx_ndc, cy_ndc);
        glUniform1f(glGetUniformLocation(prog_comp,"lensK"), lensK);
        glBindVertexArray(vao);
        glDrawElements(GL_TRIANGLES,6,GL_UNSIGNED_INT,0);
        glBindVertexArray(0);
        glUseProgram(0);
    }

    void shutdown(){
        if(ibo) glDeleteBuffers(1,&ibo);
        if(vbo) glDeleteBuffers(1,&vbo);
        if(vao) glDeleteVertexArrays(1,&vao);
        if(depth) glDeleteRenderbuffers(1,&depth);
        if(color) glDeleteTextures(1,&color);
        if(fbo) glDeleteFramebuffers(1,&fbo);
        if(prog_bright) glDeleteProgram(prog_bright);
        if(prog_blur)   glDeleteProgram(prog_blur);
        if(prog_comp)   glDeleteProgram(prog_comp);
        vao=vbo=ibo=fbo=color=depth=prog_bright=prog_blur=prog_comp=0;
    }
};
