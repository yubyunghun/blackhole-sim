#include "grid3d.hpp"
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/matrix_transform.hpp>

static const float QUAD_POS[8] = { -1.f,-1.f,  1.f,-1.f,  1.f, 1.f,  -1.f, 1.f };
static const unsigned QUAD_IDX[6] = {0,1,2, 0,2,3};

static GLuint mk(GLenum t, const char* src){
    GLuint s=glCreateShader(t);
    glShaderSource(s,1,&src,nullptr);
    glCompileShader(s);
#if _DEBUG
    GLint ok=0; glGetShaderiv(s,GL_COMPILE_STATUS,&ok);
    if(!ok){ char log[2048]; GLsizei n=0; glGetShaderInfoLog(s,2048,&n,log); OutputDebugStringA(log); }
#endif
    return s;
}
static GLuint prog_from(const char* vs, const char* fs){
    GLuint v=mk(GL_VERTEX_SHADER,vs), f=mk(GL_FRAGMENT_SHADER,fs);
    GLuint p=glCreateProgram(); glAttachShader(p,v); glAttachShader(p,f); glLinkProgram(p);
    glDeleteShader(v); glDeleteShader(f);
#if _DEBUG
    GLint ok=0; glGetProgramiv(p,GL_LINK_STATUS,&ok);
    if(!ok){ char log[2048]; GLsizei n=0; glGetProgramInfoLog(p,2048,&n,log); OutputDebugStringA(log); }
#endif
    return p;
}

void Grid3D::init(){
    const char* VS = R"(#version 330 core
    layout(location=0) in vec2 aPos;
    uniform mat4 uVP, uM;
    out vec3 vWorldPos;
    void main(){
        // world plane in XZ at y=0, scaled large
        vec3 p = vec3(aPos * 50.0, 0.0);
        vec4 w = uM * vec4(p, 1.0);
        vWorldPos = w.xyz;
        gl_Position = uVP * w;
    })";

    // Grid drawn procedurally in world space, with a simple radial lensing warp
    const char* FS = R"(#version 330 core
    in vec3 vWorldPos; out vec4 o;

    uniform vec3  uColor;
    uniform vec3  uBH;       // black hole center (world)
    uniform float uSingR;    // event-horizon-ish radius (for falloff)
    uniform float uWarpK;    // lensing strength
    uniform float uTime;

    // signed distance to nearest grid line for x/z
    float lineMask(vec2 p, float scale, float width){
        vec2 f = abs(fract(p*scale) - 0.5);
        float d = min(f.x, f.y);
        return smoothstep(width, 0.0, d); // 1 on lines, 0 between
    }

    void main(){
        // position relative to BH (only use XZ for lensing)
        vec2 posXZ = vWorldPos.xz;
        vec2 toBH  = posXZ - uBH.xz;
        float r    = length(toBH) + 1e-5;

        // falloff (0 at center, 1 far away); emphasize near the hole
        float fall = smoothstep(uSingR*0.8, uSingR*6.0, r);

        // simple “attractive” warp: pull coordinates inward ~ 1/r^2
        vec2 dir = toBH / r;
        vec2 warped = posXZ - (uWarpK * (1.0 - fall)) * dir / (0.15 + r*r);

        // grid evaluation uses the warped coordinates
        float scale = 0.5;               // cell size density
        float lines = lineMask(warped, scale, 0.03);

        // base grid shade with gentle depth dim
        float vign = clamp(1.0 - 0.0009*dot(posXZ,posXZ), 0.0, 1.0);
        vec3 gridCol = uColor * (0.35 + 0.65*vign);

        // dark disk (shadow) at center — cosmetic
        float umbra = 1.0 - smoothstep(uSingR*0.9, uSingR*1.1, r);

        // blend: grid lines + slight fill + umbra
        vec3 col = gridCol * (0.18 + 0.82*lines);
        col *= mix(1.0, 0.55, umbra); // darken inside the hole

        o = vec4(col, 1.0);
    })";

    prog = prog_from(VS, FS);

    loc_uVP   = glGetUniformLocation(prog,"uVP");
    loc_uM    = glGetUniformLocation(prog,"uM");
    loc_uColor= glGetUniformLocation(prog,"uColor");
    loc_uBH   = glGetUniformLocation(prog,"uBH");
    loc_uSingR= glGetUniformLocation(prog,"uSingR");
    loc_uWarpK= glGetUniformLocation(prog,"uWarpK");
    loc_uTime = glGetUniformLocation(prog,"uTime");

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

void Grid3D::set_warp(const glm::vec3& bh, float r, float k){
    bhPos = bh; singR = r; warpK = k;
}

void Grid3D::draw(const glm::mat4& VP, const glm::vec3& color){
    glm::mat4 M = glm::mat4(1.0f); // plane already in XZ

    glUseProgram(prog);
    glUniformMatrix4fv(loc_uVP,1,GL_FALSE, glm::value_ptr(VP));
    glUniformMatrix4fv(loc_uM ,1,GL_FALSE, glm::value_ptr(M));
    glUniform3f(loc_uColor, color.x, color.y, color.z);
    glUniform3f(loc_uBH,    bhPos.x, bhPos.y, bhPos.z);
    glUniform1f(loc_uSingR, singR);
    glUniform1f(loc_uWarpK, warpK);
    glUniform1f(loc_uTime,  0.0f);

    glBindVertexArray(vao);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
    glBindVertexArray(0);
    glUseProgram(0);
}

void Grid3D::shutdown(){
    if(ibo){ glDeleteBuffers(1,&ibo); ibo=0; }
    if(vbo){ glDeleteBuffers(1,&vbo); vbo=0; }
    if(vao){ glDeleteVertexArrays(1,&vao); vao=0; }
    if(prog){ glDeleteProgram(prog); prog=0; }
}
