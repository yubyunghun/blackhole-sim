#include "postfx.hpp"
#include <glm/gtc/type_ptr.hpp>

static const float QUAD_POS[4*2]  = { -1.f,-1.f,  1.f,-1.f,  1.f, 1.f,  -1.f, 1.f };
static const unsigned QUAD_IDX[6] = {0,1,2, 0,2,3};

static GLuint make_shader(GLenum st, const char* src){
    GLuint s=glCreateShader(st);
    glShaderSource(s,1,&src,nullptr);
    glCompileShader(s);
#if _DEBUG
    GLint ok=0; glGetShaderiv(s,GL_COMPILE_STATUS,&ok);
    if(!ok){ char log[4096]; GLsizei n=0; glGetShaderInfoLog(s,4096,&n,log); OutputDebugStringA(log); }
#endif
    return s;
}
static GLuint make_prog(const char* vs, const char* fs){
    GLuint v=make_shader(GL_VERTEX_SHADER,vs);
    GLuint f=make_shader(GL_FRAGMENT_SHADER,fs);
    GLuint p=glCreateProgram(); glAttachShader(p,v); glAttachShader(p,f); glLinkProgram(p);
    glDeleteShader(v); glDeleteShader(f);
#if _DEBUG
    GLint ok=0; glGetProgramiv(p,GL_LINK_STATUS,&ok);
    if(!ok){ char log[4096]; GLsizei n=0; glGetProgramInfoLog(p,4096,&n,log); OutputDebugStringA(log); }
#endif
    return p;
}

void PostFX::init(int W, int H){
    w=W; h=H;

    // FBO
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

#if _DEBUG
    if(glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE){
        OutputDebugStringA("PostFX FBO incomplete\n");
    }
#endif
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // fullscreen quad + shader
    const char* VS = R"(#version 330 core
    layout(location=0) in vec2 aPos;
    out vec2 vUV;
    void main(){
        vUV = aPos*0.5 + 0.5;
        gl_Position = vec4(aPos,0,1);
    })";

    // One-pass cheap bloom + vignette + exposure
    const char* FS = R"(#version 330 core
    in vec2 vUV; out vec4 o;
    uniform sampler2D uTex;
    uniform vec2  uInvRes;
    uniform float uExposure;
    uniform float uBloomK;
    uniform float uVignK;
    uniform float uThresh;

    void main(){
        vec3 base = texture(uTex, vUV).rgb;

        // bright pass
        vec3 bright = max(base - vec3(uThresh), vec3(0.0));

        // small 13-tap blur around vUV (isotropic)
        vec2 px = uInvRes;
        vec2 offs[12] = vec2[](
            vec2( 1,0), vec2(-1,0), vec2(0, 1), vec2(0,-1),
            vec2( 1,1), vec2(-1,1), vec2(1,-1), vec2(-1,-1),
            vec2( 2,0), vec2(-2,0), vec2(0, 2), vec2(0,-2)
        );
        float w0 = 0.24;
        float w1 = 0.08;
        float w2 = 0.04;
        vec3 blur = w0*bright;
        for(int i=0;i<4;i++)  blur += w1 * max(texture(uTex, vUV + offs[i]*px).rgb - vec3(uThresh), vec3(0));
        for(int i=4;i<12;i++) blur += w2 * max(texture(uTex, vUV + offs[i]*px).rgb - vec3(uThresh), vec3(0));

        vec3 bloomed = base + uBloomK * blur;

        // simple exposure
        bloomed = 1.0 - exp(-bloomed * uExposure);

        // vignette (radial falloff in NDC)
        vec2 ndc = vUV*2.0-1.0;
        float r2 = dot(ndc,ndc);
        float vig = 1.0 - uVignK * smoothstep(0.2, 1.0, r2);
        vec3 finalCol = bloomed * vig;

        o = vec4(finalCol, 1.0);
    })";

    prog = make_prog(VS, FS);
    loc_uTex     = glGetUniformLocation(prog,"uTex");
    loc_uInvRes  = glGetUniformLocation(prog,"uInvRes");
    loc_uExposure= glGetUniformLocation(prog,"uExposure");
    loc_uBloomK  = glGetUniformLocation(prog,"uBloomK");
    loc_uVignK   = glGetUniformLocation(prog,"uVignK");
    loc_uThresh  = glGetUniformLocation(prog,"uThresh");

    glGenVertexArrays(1,&vao);
    glGenBuffers(1,&vbo);
    glGenBuffers(1,&ibo);

    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(QUAD_POS), QUAD_POS, GL_STATIC_DRAW);
    glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,sizeof(float)*2,(void*)0);
    glEnableVertexAttribArray(0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(QUAD_IDX), QUAD_IDX, GL_STATIC_DRAW);
    glBindVertexArray(0);
}

void PostFX::begin(){
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0,0,w,h);
    glClearColor(0.02f,0.02f,0.03f,1.0f);
    glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
}

void PostFX::end_and_draw(float exposure, float bloomK, float vignK, float threshold){
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    // draw FS quad
    glDisable(GL_DEPTH_TEST);
    glUseProgram(prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, colorTex);
    glUniform1i(loc_uTex, 0);
    glUniform2f(loc_uInvRes, 1.0f/float(w), 1.0f/float(h));
    glUniform1f(loc_uExposure, exposure);
    glUniform1f(loc_uBloomK,   bloomK);
    glUniform1f(loc_uVignK,    vignK);
    glUniform1f(loc_uThresh,   threshold);

    glBindVertexArray(vao);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
    glBindVertexArray(0);
    glUseProgram(0);
    glEnable(GL_DEPTH_TEST);
}

void PostFX::shutdown(){
    if(ibo){ glDeleteBuffers(1,&ibo); ibo=0; }
    if(vbo){ glDeleteBuffers(1,&vbo); vbo=0; }
    if(vao){ glDeleteVertexArrays(1,&vao); vao=0; }
    if(prog){ glDeleteProgram(prog); prog=0; }

    if(depthRb){ glDeleteRenderbuffers(1,&depthRb); depthRb=0; }
    if(colorTex){ glDeleteTextures(1,&colorTex); colorTex=0; }
    if(fbo){ glDeleteFramebuffers(1,&fbo); fbo=0; }
}
