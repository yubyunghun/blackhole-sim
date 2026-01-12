#include "disk3d.hpp"
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/matrix_transform.hpp>

static const float QUAD_POS[8] = {
    -1.f,-1.f,  1.f,-1.f,  1.f, 1.f,  -1.f, 1.f
};
static const unsigned QUAD_IDX[6] = {0,1,2, 0,2,3};

static GLuint sh(GLenum type,const char* src){
    GLuint s=glCreateShader(type);
    glShaderSource(s,1,&src,nullptr);
    glCompileShader(s);
#if defined(_DEBUG)
    GLint ok=0; glGetShaderiv(s,GL_COMPILE_STATUS,&ok);
    if(!ok){ char log[4096]; GLsizei n=0; glGetShaderInfoLog(s,4096,&n,log); OutputDebugStringA(log); }
#endif
    return s;
}
static GLuint make_program(const char* vs,const char* fs){
    GLuint v=sh(GL_VERTEX_SHADER,vs), f=sh(GL_FRAGMENT_SHADER,fs);
    GLuint p=glCreateProgram(); glAttachShader(p,v); glAttachShader(p,f); glLinkProgram(p);
    glDeleteShader(v); glDeleteShader(f);
#if defined(_DEBUG)
    GLint ok=0; glGetProgramiv(p,GL_LINK_STATUS,&ok);
    if(!ok){ char log[4096]; GLsizei n=0; glGetProgramInfoLog(p,4096,&n,log); OutputDebugStringA(log); }
#endif
    return p;
}

void Disk3D::init(){
    const char* VS = R"(#version 330 core
    layout(location=0) in vec2 aPos;
    uniform mat4 uVP, uM;
    out vec3 vWorldPos;
    void main(){
        vec3 p = vec3(aPos,0.0);
        vec4 w = uM * vec4(p,1.0);
        vWorldPos = w.xyz;
        gl_Position = uVP * w;
    })";

    // Annulus (abs(r - R0)) + crescent shaping + built-in halo; alpha is ring-only.
    const char* FS = R"(#version 330 core
    in vec3 vWorldPos; out vec4 o;

    uniform mat4  uM;
    uniform vec3  uCam;
    uniform float uTime;

    // x: diskRadius, y: ringRadius, z: ringThickness, w: spinSpeed
    uniform vec4 uParams1;
    // x: beaming, y: outerSoft, z: innerSoft
    uniform vec4 uParams2;
    // x: occlBias, y: occlPower, z: rimWidth, w: rimStrength
    uniform vec4 uParams3;

    float G(float x,float s){ return exp(-(x*x)/(2.0*s*s)); }

    void main(){
        vec3 Px = normalize(vec3(uM[0].xyz));
        vec3 Py = normalize(vec3(uM[1].xyz));
        vec3 P0 = vec3(uM[3].xyz);

        vec3 dW = vWorldPos - P0;
        float x = dot(dW,Px);
        float y = dot(dW,Py);
        float r = length(vec2(x,y));
        float th = atan(y,x);

        float R_disk = uParams1.x;
        float R0     = uParams1.y;
        float Rt     = max(0.001, uParams1.z);
        float spin   = uParams1.w;

        float beta   = uParams2.x;
        float softO  = max(0.001, uParams2.y);
        float softI  = max(0.001, uParams2.z);

        float ocBias = uParams3.x;
        float ocPow  = max(0.5, uParams3.y);
        float rimW   = max(0.001, uParams3.z);
        float rimK   = max(0.0,  uParams3.w);

        // --- Correct ANNUlUS mask: 1 inside |r - R0| < Rt, 0 outside (soft edges) ---
        float s = abs(r - R0);
        float ringBody = 1.0 - smoothstep(Rt - softO, Rt + softO, s);

        // Horizon dims color only (never cuts alpha)
        float horizon = smoothstep(R_disk - softI, R_disk + softI, r);

        // Flow & beaming
        vec3 T = normalize(-sin(th)*Px + cos(th)*Py);
        vec3 V = normalize(uCam - vWorldPos);
        float approach = clamp(dot(T,V) + ocBias, -1.0, 1.0);
        float boost = pow(max(0.0, 0.5 + 0.5*approach), 1.0 + beta);

        // Palette and shading
        vec3 baseA = vec3(1.10, 0.72, 0.32);
        vec3 baseB = vec3(0.98, 0.86, 0.58);
        vec3 base  = mix(baseA, baseB, 0.5 + 0.5*sin(th*0.6));

        float cross    = G(s, Rt*0.52);
        float hotspot  = 0.60 + 0.40*G(sin(th + uTime*spin), 0.33);

        float occl = smoothstep(-0.30, 0.60, approach);
        occl = pow(occl, ocPow);
        occl = max(0.25, occl);

        float innerEdge = 1.0 - smoothstep(R0 - Rt, R0 - Rt + rimW, r);
        float nearMask  = smoothstep(0.0, 0.18, approach);
        float rimGlow   = (rimK * 1.25) * innerEdge * nearMask;

        // Built-in outer halo (always visible)
        float haloW   = max(Rt*1.4, 0.02);
        float halo    = 1.0 - smoothstep(R0 + Rt, R0 + Rt + haloW, r);
        vec3  haloCol = vec3(1.25, 1.05, 0.90);

        float shade = clamp(0.40 + 0.85*(0.55*cross + 0.35*hotspot)*boost, 0.0, 1.0);
        vec3 col = base * shade;
        col *= mix(1.0, 1.30, boost);
        col *= mix(0.65, 1.00, horizon);
        col *= (0.55 + 0.45*occl);
        col += rimGlow * vec3(1.30, 1.08, 0.95);
        col += halo * 0.55 * haloCol;

        // soften extreme outer edge a bit
        float edge = smoothstep(R0+Rt-0.003, R0+Rt+softO, r);
        col *= (1.0 - 0.22*edge);

        // Alpha = ring only (prevents “white pie” fill)
        float alpha = ringBody;
        if(alpha < 0.002) discard;
        o = vec4(col, alpha);
    })";

    prog = make_program(VS,FS);
    loc_uVP      = glGetUniformLocation(prog,"uVP");
    loc_uM       = glGetUniformLocation(prog,"uM");
    loc_uCam     = glGetUniformLocation(prog,"uCam");
    loc_uTime    = glGetUniformLocation(prog,"uTime");
    loc_uParams1 = glGetUniformLocation(prog,"uParams1");
    loc_uParams2 = glGetUniformLocation(prog,"uParams2");
    loc_uParams3 = glGetUniformLocation(prog,"uParams3");
    loc_uFlags   = -1;

    glGenVertexArrays(1,&vao);
    glGenBuffers(1,&vbo);
    glGenBuffers(1,&ibo);

    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(QUAD_POS), QUAD_POS, GL_STATIC_DRAW);
    glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE, sizeof(float)*2, (void*)0);
    glEnableVertexAttribArray(0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(QUAD_IDX), QUAD_IDX, GL_STATIC_DRAW);
    glBindVertexArray(0);
}

void Disk3D::draw(const glm::mat4& VP,
                  const glm::mat4& M,
                  float diskR, float ringR, float ringT,
                  float spinSpd, float beamAmt, bool billboardOn,
                  float occBiasIn, float occPowIn, float rimWIn, float rimKIn)
{
    diskRadius    = diskR;  ringRadius = ringR;  ringThickness = ringT;
    spinSpeed     = spinSpd; beaming   = beamAmt; billboard = billboardOn;
    occlBias      = occBiasIn; occlPower = occPowIn; rimWidth = rimWIn; rimStrength = rimKIn;

    glUseProgram(prog);
    glUniformMatrix4fv(loc_uVP,1,GL_FALSE, glm::value_ptr(VP));
    glUniformMatrix4fv(loc_uM ,1,GL_FALSE, glm::value_ptr(M));
    glUniform4f(loc_uParams1, diskR, ringR, ringT, spinSpd);
    glUniform4f(loc_uParams2, beamAmt, ringR*0.02f, diskR*0.015f, 0.0f);
    glUniform4f(loc_uParams3, occBiasIn, occPowIn, rimWIn, rimKIn);

    glBindVertexArray(vao);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
    glBindVertexArray(0);
    glUseProgram(0);
}

void Disk3D::shutdown(){
    if(ibo){ glDeleteBuffers(1,&ibo); ibo=0; }
    if(vbo){ glDeleteBuffers(1,&vbo); vbo=0; }
    if(vao){ glDeleteVertexArrays(1,&vao); vao=0; }
    if(prog){ glDeleteProgram(prog); prog=0; }
}
