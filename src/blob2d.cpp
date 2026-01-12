#include "blob2d.hpp"
#include <glm/gtc/type_ptr.hpp>

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

void Blob2D::init(){
    const char* VS = R"(#version 330 core
    layout(location=0) in vec2 aPos;
    uniform mat4 uVP, uM;
    out vec2 vLocal;                 // [-1,1]^2 quad coords
    void main(){
        vec3 p = vec3(aPos,0.0);
        vLocal = aPos;
        gl_Position = uVP * (uM * vec4(p,1.0));
    })";

    // -> Circular alpha mask + additive-with-alpha friendly output
    const char* FS = R"(#version 330 core
    in vec2 vLocal; out vec4 o;
    uniform vec3  uColor;
    uniform float uRadius;   // kept for API symmetry (not needed here)
    uniform float uSoft;     // edge softness (0..~0.3)
    uniform float uGain;     // brightness

    void main(){
        float r    = length(vLocal);                       // 0 center … ~1 edge
        float soft = max(0.0001, uSoft);
        // alpha: 1 at r<=1, falls to 0 by r>=1+soft (kills the square)
        float a    = 1.0 - smoothstep(1.0, 1.0 + soft, r);
        if(a <= 0.001) discard;

        // gentle Gaussian hotspot inside
        float g = exp(-3.0 * r*r);
        float intensity = uGain * (0.65*a + 0.45*g);

        vec3 col = uColor * intensity;
        o = vec4(col, clamp(intensity, 0.0, 1.0) * a);     // premult-style alpha weight
    })";

    prog = prog_from(VS,FS);

    loc_uVP    = glGetUniformLocation(prog,"uVP");
    loc_uM     = glGetUniformLocation(prog,"uM");
    loc_uColor = glGetUniformLocation(prog,"uColor");
    loc_uRadius= glGetUniformLocation(prog,"uRadius");
    loc_uSoft  = glGetUniformLocation(prog,"uSoft");
    loc_uGain  = glGetUniformLocation(prog,"uGain");

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

void Blob2D::draw(const glm::mat4& VP, const glm::mat4& M,
                  const glm::vec3& color, float radius, float soft, float gain)
{
    glUseProgram(prog);
    glUniformMatrix4fv(loc_uVP,1,GL_FALSE, glm::value_ptr(VP));
    glUniformMatrix4fv(loc_uM ,1,GL_FALSE, glm::value_ptr(M));
    glUniform3f(loc_uColor, color.x, color.y, color.z);
    glUniform1f(loc_uRadius, radius);
    glUniform1f(loc_uSoft,   soft);
    glUniform1f(loc_uGain,   gain);

    glBindVertexArray(vao);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, 0);
    glBindVertexArray(0);
    glUseProgram(0);
}

void Blob2D::shutdown(){
    if(ibo){ glDeleteBuffers(1,&ibo); ibo=0; }
    if(vbo){ glDeleteBuffers(1,&vbo); vbo=0; }
    if(vao){ glDeleteVertexArrays(1,&vao); vao=0; }
    if(prog){ glDeleteProgram(prog); prog=0; }
}
