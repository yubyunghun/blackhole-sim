#include "lenswarp.hpp"
#include <glm/gtc/type_ptr.hpp>

static const float QUAD_POS[8]  = { -1.f,-1.f,  1.f,-1.f,  1.f, 1.f,  -1.f, 1.f };
static const unsigned QUAD_IDX[6] = {0,1,2, 0,2,3};

static GLuint mkShader(GLenum st,const char* src){
    GLuint s=glCreateShader(st);
    glShaderSource(s,1,&src,nullptr);
    glCompileShader(s);
#if _DEBUG
    GLint ok=0; glGetShaderiv(s,GL_COMPILE_STATUS,&ok);
    if(!ok){ char log[4096]; GLsizei n=0; glGetShaderInfoLog(s,4096,&n,log); OutputDebugStringA(log); }
#endif
    return s;
}
static GLuint mkProg(const char* vs,const char* fs){
    GLuint v=mkShader(GL_VERTEX_SHADER,vs);
    GLuint f=mkShader(GL_FRAGMENT_SHADER,fs);
    GLuint p=glCreateProgram(); glAttachShader(p,v); glAttachShader(p,f); glLinkProgram(p);
    glDeleteShader(v); glDeleteShader(f);
#if _DEBUG
    GLint ok=0; glGetProgramiv(p,GL_LINK_STATUS,&ok);
    if(!ok){ char log[4096]; GLsizei n=0; glGetProgramInfoLog(p,4096,&n,log); OutputDebugStringA(log); }
#endif
    return p;
}

void LensWarp::init(){
    const char* VS = R"(#version 330 core
    layout(location=0) in vec2 aPos;
    out vec2 vUV;
    void main(){ vUV=aPos*0.5+0.5; gl_Position=vec4(aPos,0,1); })";

    // Analytic, cheap lensing on the background only.
    // Uses inverse-square radial displacement around BH center.
    const char* FS = R"(#version 330 core
    in vec2 vUV; out vec4 o;
    uniform sampler2D uBg;
    uniform vec2  uInvRes;
    uniform vec2  uBHuv;
    uniform float uRadPx;
    uniform float uK;
    uniform float uShadow; // 0..1 hardness
    uniform float uTint;   // mixes warm tint slightly

    vec3 warm(vec3 c){ return mix(c, vec3(1.0,0.72,0.55), 0.20*uTint); }

    void main(){
        vec2 res = 1.0/uInvRes;
        vec2 p  = vUV*res;
        vec2 c  = uBHuv*res;
        vec2 d  = p - c;
        float r = length(d)+1e-4;

        // inverse-square displacement; clamp far out to avoid ringing
        float k = uK * (uRadPx*uRadPx) / (r*r + 0.25*uRadPx*uRadPx);
        vec2  q = p + d * k;

        // sample warped background
        vec3 col = texture(uBg, q/res).rgb;

        // hole shadow (acts like event horizon on background)
        float inner = smoothstep(uRadPx*(1.0+0.05*uShadow), uRadPx*(1.0-0.12*uShadow), r);
        col *= inner;

        col = warm(col);
        o = vec4(col,1);
    })";

    prog = mkProg(VS,FS);
    loc_uBg     = glGetUniformLocation(prog,"uBg");
    loc_uInvRes = glGetUniformLocation(prog,"uInvRes");
    loc_uBHuv   = glGetUniformLocation(prog,"uBHuv");
    loc_uRadPx  = glGetUniformLocation(prog,"uRadPx");
    loc_uK      = glGetUniformLocation(prog,"uK");
    loc_uShadow = glGetUniformLocation(prog,"uShadow");
    loc_uTint   = glGetUniformLocation(prog,"uTint");

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

void LensWarp::draw(GLuint bgTex, int W, int H, glm::vec2 bhUV, float radPx, float strengthK, float shadowHardness, float tintMix){
    glDisable(GL_DEPTH_TEST);
    glUseProgram(prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, bgTex);
    glUniform1i(loc_uBg,0);
    glUniform2f(loc_uInvRes, 1.0f/float(W), 1.0f/float(H));
    glUniform2f(loc_uBHuv,   bhUV.x, bhUV.y);
    glUniform1f(loc_uRadPx,  radPx);
    glUniform1f(loc_uK,      strengthK);
    glUniform1f(loc_uShadow, shadowHardness);
    glUniform1f(loc_uTint,   tintMix);

    glBindVertexArray(vao);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
    glBindVertexArray(0);
    glUseProgram(0);
    glEnable(GL_DEPTH_TEST);
}

void LensWarp::shutdown(){
    if(ibo){ glDeleteBuffers(1,&ibo); ibo=0; }
    if(vbo){ glDeleteBuffers(1,&vbo); vbo=0; }
    if(vao){ glDeleteVertexArrays(1,&vao); vao=0; }
    if(prog){ glDeleteProgram(prog); prog=0; }
}
