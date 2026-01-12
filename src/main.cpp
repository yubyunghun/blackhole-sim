// Clean-room port of kavan010/black_hole with small structural changes:
// - GLAD instead of GLEW
// - All shaders embedded (no external files)
// - std140-safe UBO layouts (booleans -> ints; masses packed as vec4[4])
// - "moving" low-res compute while orbiting; full-res when idle
// - Simple orbit camera centered on black hole
// - Grid warp preview + full-screen blit of compute output

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#define _USE_MATH_DEFINES
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <string>
#include <cmath>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <algorithm>
#include <iostream>
#include <cctype>
#include <ctime>

using namespace glm;
using std::vector;

// ---------- GL 4.3 compute entry points ----------
// third_party/glad was generated for GL 3.3 core, which lacks compute shaders.
// Load the few 4.2/4.3 functions we need ourselves after the context is created.
#ifndef GL_COMPUTE_SHADER
#define GL_COMPUTE_SHADER 0x91B9
#endif
#ifndef GL_SHADER_IMAGE_ACCESS_BARRIER_BIT
#define GL_SHADER_IMAGE_ACCESS_BARRIER_BIT 0x00000020
#endif
typedef void (APIENTRYP PFN_BindImageTexture)(GLuint, GLuint, GLint, GLboolean, GLint, GLenum, GLenum);
typedef void (APIENTRYP PFN_DispatchCompute)(GLuint, GLuint, GLuint);
typedef void (APIENTRYP PFN_MemoryBarrier)(GLbitfield);
static PFN_BindImageTexture pglBindImageTexture = nullptr;
static PFN_DispatchCompute  pglDispatchCompute  = nullptr;
static PFN_MemoryBarrier    pglMemoryBarrier    = nullptr;
#define glBindImageTexture pglBindImageTexture
#define glDispatchCompute  pglDispatchCompute
#define glMemoryBarrier    pglMemoryBarrier

// ---------- Physics constants ----------
static const double C_LIGHT = 299792458.0;
static const double G_GRAV  = 6.67430e-11;

// Sagittarius A* mass (kg) and r_s
static const double SAG_MASS = 8.54e36;
static const double SAG_RS   = 2.0 * G_GRAV * SAG_MASS / (C_LIGHT*C_LIGHT);

// ---------- Window / Compute ----------
// Matches the reference: 800x600 window, 200x150 ray-traced image (4:3 everywhere)
static int   gWinW = 800, gWinH = 600;
// Adaptive-step RK4 is ~100x cheaper per ray than the old fixed-step loop,
// so we can afford much higher resolution. Lower these if your GPU struggles.
static int   gMovingW = 400, gMovingH = 300;   // res while orbiting
static int   gStillW  = 800, gStillH  = 600;   // res when idle (full window)

// Simulated seconds per real second for the N-body gravity.
// (A star at 4e11 m around Sgr A* has a ~18 h orbit; 1e4 makes that ~7 s.)
static double gTimeScale = 1e4;

// Gravity toggle: G key toggles, holding right mouse enables while held
static bool  gGravity = false;
static bool  gScreenshotRequested = false;   // set by the P key
static bool  gRealDisk  = true;   // D: relativistic (Doppler/redshift) disk vs reference flat disk
static bool  gShowStars = true;   // B: lensed starfield background

// ---------- Camera ----------
struct Camera {
    vec3 target = vec3(0.0f);
    // Start ~12 rs out and ~6 deg above the disk plane. (The reference starts at
    // 5 rs exactly in the plane, i.e. *inside* the disk, so half the screen is disk.)
    float radius = 1.5e11f;
    float azimuth = 0.0f;
    float elevation = float(M_PI) * 0.5f - 0.1f;
    float orbitSpeed = 0.01f;
    double zoomSpeed = 25e9;

    bool dragging = false;
    bool moving   = false;
    double lastX  = 0.0, lastY = 0.0;

    vec3 position() const {
        float el = glm::clamp(elevation, 0.01f, float(M_PI)-0.01f);
        return vec3(
            radius * sin(el)*cos(azimuth),
            radius * cos(el),
            radius * sin(el)*sin(azimuth)
        );
    }
} gCam;

// ---------- Scene objects ----------
struct ObjectData {
    vec4 posRadius; // xyz center, w radius
    vec4 color;     // rgb color, a unused
    float mass;     // kg (packed later into vec4[4] for std140)
    vec3  velocity; // m/s
    bool  pinned;   // never moves (the black hole: the ray tracer assumes it sits at the origin)
};
static vector<ObjectData> gObjs = {
    { vec4(4e11f, 0.0f, 0.0f, 4e10f), vec4(1,1,0,1), 1.98892e30f, vec3(0), false },
    { vec4(0.0f, 0.0f, 4e11f, 4e10f), vec4(1,0,0,1), 1.98892e30f, vec3(0), false },
    { vec4(0.0f, 0.0f, 0.0f, float(SAG_RS)), vec4(0,0,0,1), float(SAG_MASS), vec3(0), true }
};

// ---------- GL resources ----------
static GLFWwindow* gWin = nullptr;

static GLuint gProgBlit = 0;     // fullscreen quad program
static GLuint gProgGrid = 0;     // grid program
static GLuint gProgComp = 0;     // compute shader

static GLuint gVAOQuad = 0, gVBOQuad = 0, gTexOut = 0;
static GLuint gGridVAO=0, gGridVBO=0, gGridEBO=0; int gGridIndexCount=0;

// UBOs
static GLuint gUBOCamera=0, gUBODisk=0, gUBOObjs=0;

// ------------ Shaders (embedded) ------------

// Blit (fullscreen) shaders
static const char* VS_BLIT = R"(
#version 330 core
layout(location=0) in vec2 aPos;
layout(location=1) in vec2 aUV;
out vec2 vUV;
void main(){
    vUV = aUV;
    gl_Position = vec4(aPos,0,1);
}
)";

static const char* FS_BLIT = R"(
#version 330 core
in vec2 vUV;
out vec4 o;
uniform sampler2D uTex;
void main(){
    o = texture(uTex, vUV);
}
)";

// Grid shaders
static const char* VS_GRID = R"(
#version 330 core
layout(location=0) in vec3 aPos;
uniform mat4 uVP;
void main(){ gl_Position = uVP * vec4(aPos,1.0); }
)";

static const char* FS_GRID = R"(
#version 330 core
out vec4 o;
void main(){ o = vec4(0.5,0.5,0.5,0.7); }
)";

// Compute shader (based on repo, refactored for std140 + params packing)
static const char* CS_GEOD = R"(
#version 430
layout(local_size_x=16, local_size_y=16) in;

// Output image
layout(binding=0, rgba8) writeonly uniform image2D outImage;

// Camera UBO: std140-safe packing
layout(std140, binding=1) uniform Camera {
    vec4 camPos;      // xyz, pad
    vec4 camRight;    // xyz, pad
    vec4 camUp;       // xyz, pad
    vec4 camFwd;      // xyz, pad
    vec4 params;      // x=tanHalfFov, y=aspect, z=movingFlag(0/1), w=unused
    ivec4 ires;       // x=WIDTH, y=HEIGHT, z,w unused
} CAM;

// Disk UBO
layout(std140, binding=2) uniform Disk {
    vec4 disk; // x=r1, y=r2 (metres), z=rs (metres), w=unused
};

// Objects UBO (masses packed into 4 vec4s => up to 16 masses)
layout(std140, binding=3) uniform Objects {
    int  numObjects; vec3 _padX;
    vec4 objPosRadius[16];
    vec4 objColor[16];
    vec4 mass4[4]; // masses[i] = mass4[i/4][i%4]
};

// ---------------------------------------------------------------------------
// Null geodesics in Schwarzschild spacetime, integrated in Cartesian coords.
//
// For a photon, the spatial path obeys (Binet form, exact in GR):
//     d2x/dl2 = -1.5 * rs * h^2 * x / r^5,   h = |x cross dx/dl| (conserved)
// Same trajectories as the spherical-coordinate geodesic equations, but with
// no coordinate singularity at the poles and an easy RK4.
//
// Everything below works in units of rs (rs == 1) so r^5 never overflows
// 32-bit floats (in metres r^5 ~ 1e55 > FLT_MAX).
// ---------------------------------------------------------------------------

const float STEP_FRAC = 0.02;   // step = 2% of current radius (adaptive)
const float ESCAPE_R  = 2000.0; // rs units (~2.5e13 m, well beyond the scene)
const int   MAX_STEPS = 3000;   // rays still bound after this are captured

float RS;      // Schwarzschild radius in metres (from Disk UBO)
float H2;      // conserved h^2 for the current ray

vec3 accel(vec3 x){
    float r2 = dot(x,x);
    float r5 = r2*r2*sqrt(r2);
    return -1.5 * H2 * x / r5;
}

// Classic 4-stage Runge-Kutta on (x, v)
void rk4(inout vec3 x, inout vec3 v, float h){
    vec3 k1x = v;              vec3 k1v = accel(x);
    vec3 k2x = v + 0.5*h*k1v;  vec3 k2v = accel(x + 0.5*h*k1x);
    vec3 k3x = v + 0.5*h*k2v;  vec3 k3v = accel(x + 0.5*h*k2x);
    vec3 k4x = v + h*k3v;      vec3 k4v = accel(x + h*k3x);
    x += h/6.0 * (k1x + 2.0*k2x + 2.0*k3x + k4x);
    v += h/6.0 * (k1v + 2.0*k2v + 2.0*k3v + k4v);
}

// First intersection of segment p0->p1 with sphere (c,R); t in [0,1]
bool segSphere(vec3 p0, vec3 p1, vec3 c, float R, out float t){
    vec3 d = p1 - p0;
    vec3 m = p0 - c;
    float a = dot(d,d);
    float b = dot(m,d);
    float cc = dot(m,m) - R*R;
    if (cc <= 0.0) { t = 0.0; return true; }      // started inside
    if (b > 0.0) return false;                    // moving away
    float disc = b*b - a*cc;
    if (disc < 0.0) return false;
    t = (-b - sqrt(disc)) / a;
    return t <= 1.0;
}

// ---------------------------------------------------------------------------
// Background: procedural starfield + faint galactic band, looked up by the
// ray's final (escape) direction, so it is lensed for free.
// ---------------------------------------------------------------------------
float hash13(vec3 p){
    p = fract(p * 0.1031);
    p += dot(p, p.zyx + 31.32);
    return fract((p.x + p.y) * p.z);
}

vec3 starLayer(vec3 d, float scale, float density, float seed){
    vec3 p = d * scale;
    vec3 c = floor(p);
    float h = hash13(c + seed);
    if (h > density) return vec3(0);
    // star position jittered inside its cell
    vec3 sp = c + vec3(hash13(c + seed + 1.7), hash13(c + seed + 3.1), hash13(c + seed + 5.3));
    float d2 = dot(p - sp, p - sp);
    float b = exp(-d2 * 40.0) * (0.4 + 0.6 * fract(h * 97.0));
    // colour: mostly white, some blue-white, some orange
    float t = fract(h * 53.0);
    vec3 col = t < 0.2 ? vec3(1.0, 0.75, 0.5) : (t < 0.45 ? vec3(0.7, 0.8, 1.0) : vec3(1.0));
    return col * b;
}

vec3 sky(vec3 d){
    // galactic band along a tilted great circle
    vec3 n = normalize(vec3(0.25, 1.0, 0.35));
    float band = exp(-pow(dot(d, n) * 5.0, 2.0));
    vec3 col = vec3(0.10, 0.09, 0.12) * band;
    col += starLayer(d, 120.0, 0.06, 0.0);
    col += starLayer(d, 260.0, 0.05 + 0.10 * band, 17.0) * 0.7;
    col += starLayer(d, 520.0, 0.04 + 0.20 * band, 41.0) * 0.5;
    return col;
}

// ---------------------------------------------------------------------------
// Accretion disk with relativistic effects.
// Gas on circular orbits at the local orbital speed  beta = sqrt(rs / 2(r - rs)).
// Observed/emitted frequency ratio g = Doppler * gravitational:
//     g = 1 / (gamma (1 - beta cos a)) * sqrt(1 - rs/r) / sqrt(1 - rs/r_cam)
// Observed intensity ~ g^4 (beaming), observed temperature = g * T_emit.
// ---------------------------------------------------------------------------
vec3 blackbody(float T){                // Kelvin -> rgb (Tanner Helland fit)
    float t = T / 100.0;
    vec3 c;
    c.r = t <= 66.0 ? 1.0 : 1.292936 * pow(t - 60.0, -0.1332047);
    c.g = t <= 66.0 ? 0.3900816 * log(t) - 0.6318414 : 1.129891 * pow(t - 60.0, -0.0755148);
    c.b = t >= 66.0 ? 1.0 : (t <= 19.0 ? 0.0 : 0.5432068 * log(t - 10.0) - 1.1962541);
    return clamp(c, 0.0, 1.0);
}

vec4 diskColorRelativistic(vec3 pc, vec3 rayDir, float r1, float r2, float rCam){
    float r = length(pc.xz);
    // orbital velocity: counter-clockwise seen from +y
    vec3 vdir = normalize(cross(vec3(0,1,0), vec3(pc.x, 0.0, pc.z)));
    float beta = clamp(sqrt(0.5 / (r - 1.0)), 0.0, 0.99);
    float gamma = 1.0 / sqrt(1.0 - beta*beta);
    vec3 nPhoton = -normalize(rayDir);          // photon travels from disk to camera
    float gDopp = 1.0 / (gamma * (1.0 - beta * dot(vdir, nPhoton)));
    float gGrav = sqrt(1.0 - 1.0/r) / sqrt(1.0 - 1.0/rCam);
    float g = gDopp * gGrav;

    // emitted temperature falls off ~ r^-3/4 (thin-disk profile)
    float T = 4500.0 * pow(r / r1, -0.75);
    vec3 col = blackbody(T * g);

    float I = pow(g, 4.0) * pow(r1 / r, 1.5);
    vec3 c = 1.0 - exp(-col * I * 3.0);           // tone map into [0,1)
    // opaque disk with soft inner/outer edges (premultiplied alpha)
    float edge = smoothstep(r2, r2 - 0.6, r) * smoothstep(r1, r1 + 0.15, r);
    return vec4(c * edge, edge);
}

void main(){
    int WIDTH  = CAM.ires.x;
    int HEIGHT = CAM.ires.y;
    ivec2 pix = ivec2(gl_GlobalInvocationID.xy);
    if (pix.x >= WIDTH || pix.y >= HEIGHT) return;

    int flags = int(CAM.params.w + 0.5);
    bool realDisk = (flags & 1) != 0;
    bool showStars = (flags & 2) != 0;

    RS = disk.z;
    float r1 = disk.x / RS, r2 = disk.y / RS;

    float u = (2.0*(pix.x+0.5)/WIDTH - 1.0) * CAM.params.y * CAM.params.x;
    float v_ = (1.0 - 2.0*(pix.y+0.5)/HEIGHT) * CAM.params.x;
    vec3 dir = normalize(u * CAM.camRight.xyz - v_ * CAM.camUp.xyz + CAM.camFwd.xyz);

    vec3 x = CAM.camPos.xyz / RS;   // position in rs units
    float rCam = length(x);
    vec3 v = dir;                   // |v| = 1
    vec3 hv = cross(x, v);
    H2 = dot(hv, hv);

    vec4 color = vec4(0);           // default: escaped -> transparent (grid shows)
    bool done = false;

    for (int i = 0; i < MAX_STEPS && !done; ++i){
        float r = length(x);
        float h = STEP_FRAC * r;
        vec3 p0 = x;
        rk4(x, v, h);
        vec3 p1 = x;

        // --- earliest event along this step ---
        float bestT = 2.0;
        int   hit   = 0;            // 1 = disk, 2 = object
        int   hitObj = -1;

        // accretion disk: crossing of the y=0 plane, interpolated
        // strict sign change: a ray starting exactly on the plane (camera at y=0) isn't a hit
        if (p0.y * p1.y < 0.0) {
            float t = p0.y / (p0.y - p1.y);
            vec3 pc = mix(p0, p1, t);
            float rd = length(pc.xz);
            if (rd >= r1 && rd <= r2 && t < bestT) { bestT = t; hit = 1; }
        }

        // spheres (stars)
        for (int k = 0; k < numObjects; ++k){
            float t;
            vec3  c = objPosRadius[k].xyz / RS;
            float R = objPosRadius[k].w   / RS;
            if (segSphere(p0, p1, c, R, t) && t < bestT) { bestT = t; hit = 2; hitObj = k; }
        }

        if (hit == 1) {
            vec3 pc = mix(p0, p1, bestT);
            if (realDisk) {
                color = diskColorRelativistic(pc, v, r1, r2, rCam);
            } else {
                float ro = length(pc.xz) / r2;
                color = vec4(vec3(1.0, ro, 0.2) * ro, ro);  // reference look (premultiplied)
            }
            done = true;
        } else if (hit == 2) {
            vec3 P = mix(p0, p1, bestT);
            vec3 C = objPosRadius[hitObj].xyz / RS;
            vec3 N = normalize(P - C);        // normal of the sphere we hit
            vec3 V = normalize(-v);           // back along the (bent) ray
            float amb = 0.1, diff = max(dot(N, V), 0.0);
            vec4 oc = objColor[hitObj];
            color = vec4(oc.rgb * (amb + (1.0-amb)*diff), oc.a);
            done = true;
        } else if (length(x) <= 1.0) {
            color = vec4(0,0,0,1);            // crossed the event horizon
            done = true;
        } else if (length(x) > ESCAPE_R && dot(x, v) > 0.0) {
            // escaped to infinity: sample the sky in the final direction
            if (showStars) {
                vec3 s = sky(normalize(v));
                color = vec4(s, clamp(max(s.r, max(s.g, s.b)), 0.0, 1.0));
            }
            done = true;
        }
    }
    if (!done) color = vec4(0,0,0,1);         // trapped near photon sphere

    imageStore(outImage, pix, color);
}
)";

// ---------- GL helpers ----------
static void die(const char* msg){ std::cerr << msg << "\n"; std::exit(1); }

static GLuint compile(GLenum type, const char* src){
    GLuint s = glCreateShader(type);
    glShaderSource(s,1,&src,nullptr);
    glCompileShader(s);
    GLint ok=0; glGetShaderiv(s,GL_COMPILE_STATUS,&ok);
    if(!ok){ GLint n=0; glGetShaderiv(s,GL_INFO_LOG_LENGTH,&n);
        std::vector<char> log(n); glGetShaderInfoLog(s,n,nullptr,log.data());
        std::cerr<<"[Shader compile error]\n"<<log.data()<<"\n"; die("compile failed"); }
    return s;
}
static GLuint linkProgram(GLuint a, GLuint b=0){
    GLuint p=glCreateProgram(); glAttachShader(p,a); if(b) glAttachShader(p,b);
    glLinkProgram(p); glDetachShader(p,a); if(b) glDetachShader(p,b);
    GLint ok=0; glGetProgramiv(p,GL_LINK_STATUS,&ok);
    if(!ok){ GLint n=0; glGetProgramiv(p,GL_INFO_LOG_LENGTH,&n);
        std::vector<char> log(n); glGetProgramInfoLog(p,n,nullptr,log.data());
        std::cerr<<"[Program link error]\n"<<log.data()<<"\n"; die("link failed"); }
    glDeleteShader(a); if(b) glDeleteShader(b);
    return p;
}
static GLuint makeProg(const char* vs, const char* fs){
    return linkProgram(compile(GL_VERTEX_SHADER,vs), compile(GL_FRAGMENT_SHADER,fs));
}
static GLuint makeComp(const char* cs){ return linkProgram(compile(GL_COMPUTE_SHADER, cs)); }

// ---------- Geometry ----------
static void createQuad(){
    const float quad[] = {
        // pos      // uv
        -1,  1,   0,1,
        -1, -1,   0,0,
         1, -1,   1,0,
        -1,  1,   0,1,
         1, -1,   1,0,
         1,  1,   1,1
    };
    glGenVertexArrays(1,&gVAOQuad);
    glGenBuffers(1,&gVBOQuad);
    glBindVertexArray(gVAOQuad);
    glBindBuffer(GL_ARRAY_BUFFER,gVBOQuad);
    glBufferData(GL_ARRAY_BUFFER,sizeof(quad),quad,GL_STATIC_DRAW);
    glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,4*sizeof(float),(void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1,2,GL_FLOAT,GL_FALSE,4*sizeof(float),(void*)(2*sizeof(float)));
    glEnableVertexAttribArray(1);
    glBindVertexArray(0);

    glGenTextures(1,&gTexOut);
    glBindTexture(GL_TEXTURE_2D, gTexOut);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
}

static void generateGrid(){
    const int gridSize = 26;
    const float spacing = 1e10f;

    vector<vec3> verts;
    vector<GLuint> inds;
    verts.reserve((gridSize+1)*(gridSize+1));
    auto addY = [&](float x, float z){
        float y = 0.0f;
        for(const auto& o: gObjs){
            vec3 c = vec3(o.posRadius);
            double rs = 2.0 * G_GRAV * double(o.mass) / (C_LIGHT*C_LIGHT);
            double dx = double(x) - double(c.x);
            double dz = double(z) - double(c.z);
            double d  = std::sqrt(dx*dx+dz*dz);
            if (d > rs) y += float(2.0 * std::sqrt(rs * (d - rs)) - 3e10);
            else        y += float(2.0 * std::sqrt(rs*rs) - 3e10);
        }
        verts.emplace_back(x,y,z);
    };
    for(int z=0; z<=gridSize; ++z){
        for(int x=0; x<=gridSize; ++x){
            addY( (x - gridSize/2)*spacing, (z - gridSize/2)*spacing );
        }
    }
    for(int z=0; z<gridSize; ++z){
        for(int x=0; x<gridSize; ++x){
            int i = z*(gridSize+1)+x;
            inds.push_back(i); inds.push_back(i+1);
            inds.push_back(i); inds.push_back(i+gridSize+1);
        }
    }
    if(!gGridVAO){ glGenVertexArrays(1,&gGridVAO); glGenBuffers(1,&gGridVBO); glGenBuffers(1,&gGridEBO); }
    glBindVertexArray(gGridVAO);
    glBindBuffer(GL_ARRAY_BUFFER,gGridVBO);
    glBufferData(GL_ARRAY_BUFFER, verts.size()*sizeof(vec3), verts.data(), GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,gGridEBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, inds.size()*sizeof(GLuint), inds.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,sizeof(vec3),(void*)0);
    glBindVertexArray(0);
    gGridIndexCount = int(inds.size());
}

// ---------- UBO upload ----------
struct CamUBO {
    vec4 camPos, camRight, camUp, camFwd;
    vec4 params;    // x=tanHalfFov, y=aspect, z=moving(0/1), w=0
    ivec4 ires;     // x=WIDTH, y=HEIGHT, z,w unused
};
static void uploadCameraUBO(){
    CamUBO data{};
    vec3 pos = gCam.position();
    vec3 upW = vec3(0,1,0);
    vec3 fwd = normalize(gCam.target - pos);
    vec3 right = normalize(cross(fwd, upW));
    vec3 up = cross(right, fwd);

    data.camPos   = vec4(pos,0);
    data.camRight = vec4(right,0);
    data.camUp    = vec4(up,0);
    data.camFwd   = vec4(fwd,0);
    float flags = float((gRealDisk ? 1 : 0) | (gShowStars ? 2 : 0));
    data.params   = vec4(tan(radians(60.0f*0.5f)), float(gWinW)/float(gWinH), gCam.moving?1.0f:0.0f, flags);

    int cw = gCam.moving ? gMovingW : gStillW;
    int ch = gCam.moving ? gMovingH : gStillH;
    data.ires = ivec4(cw, ch, 0, 0);

    glBindBuffer(GL_UNIFORM_BUFFER, gUBOCamera);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(CamUBO), &data);
}

struct DiskUBO { vec4 disk; }; // r1, r2, rs, -
static void uploadDiskUBO(){
    DiskUBO d{};
    d.disk = vec4(float(SAG_RS*2.2), float(SAG_RS*5.2), float(SAG_RS), 0.0f);
    glBindBuffer(GL_UNIFORM_BUFFER, gUBODisk);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(DiskUBO), &d);
}

struct ObjsUBO {
    // mirror std140 pack: int + pad + arrays + mass4[4]
    int num; float pad[3];
    vec4 posR[16];
    vec4 color[16];
    vec4 mass4[4]; // 16 floats packed
};
static void uploadObjsUBO(){
    ObjsUBO u{}; u.num = (int)std::min<size_t>(gObjs.size(), 16);
    for(int i=0;i<u.num;++i){
        u.posR[i] = gObjs[i].posRadius;
        u.color[i]= gObjs[i].color;
    }
    // pack masses into mass4[4]
    float temp[16] = {};
    for(int i=0;i<u.num;++i) temp[i] = gObjs[i].mass;
    for(int g=0; g<4; ++g){
        u.mass4[g] = vec4( temp[g*4+0], temp[g*4+1], temp[g*4+2], temp[g*4+3] );
    }
    glBindBuffer(GL_UNIFORM_BUFFER, gUBOObjs);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(ObjsUBO), &u);
}

// ---------- Compute dispatch ----------
static void dispatchCompute(){
    // ensure output texture matches compute size
    int cw = gCam.moving ? gMovingW : gStillW;
    int ch = gCam.moving ? gMovingH : gStillH;

    // only (re)allocate when the resolution actually changes
    static int texW = 0, texH = 0;
    glBindTexture(GL_TEXTURE_2D, gTexOut);
    if (cw != texW || ch != texH) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, cw, ch, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        texW = cw; texH = ch;
    }

    glUseProgram(gProgComp);
    // Bind UBOs
    glBindBufferBase(GL_UNIFORM_BUFFER, 1, gUBOCamera);
    glBindBufferBase(GL_UNIFORM_BUFFER, 2, gUBODisk);
    glBindBufferBase(GL_UNIFORM_BUFFER, 3, gUBOObjs);

    // bind image
    glBindImageTexture(0, gTexOut, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);

    GLuint gx = (GLuint) ((cw + 15) / 16);
    GLuint gy = (GLuint) ((ch + 15) / 16);
    glDispatchCompute(gx, gy, 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
}

// ---------- Draw ----------
static void drawQuad(){
    glUseProgram(gProgBlit);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, gTexOut);
    glUniform1i(glGetUniformLocation(gProgBlit,"uTex"), 0);
    glBindVertexArray(gVAOQuad);
    glDisable(GL_DEPTH_TEST);
    // Blend the ray-traced image over the grid: empty space (alpha 0) lets the
    // warped grid show through, like the reference. The shader outputs
    // premultiplied colour, so faint stars aren't darkened twice.
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    glBindVertexArray(0);
}

static void drawGrid(const mat4& VP){
    glUseProgram(gProgGrid);
    glUniformMatrix4fv(glGetUniformLocation(gProgGrid,"uVP"),1,GL_FALSE,value_ptr(VP));
    glBindVertexArray(gGridVAO);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDrawElements(GL_LINES, gGridIndexCount, GL_UNSIGNED_INT, 0);
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    glBindVertexArray(0);
}

// ---------- Callbacks ----------
static void cursorPosCB(GLFWwindow*, double x, double y){
    if(!gCam.dragging){ gCam.lastX=x; gCam.lastY=y; return; }
    float dx = float(x - gCam.lastX);
    float dy = float(y - gCam.lastY);
    gCam.azimuth   += dx * gCam.orbitSpeed;
    gCam.elevation -= dy * gCam.orbitSpeed;
    gCam.elevation = std::clamp(gCam.elevation, 0.01f, float(M_PI)-0.01f);
    gCam.lastX=x; gCam.lastY=y;
    gCam.moving = true;
}
static void mouseCB(GLFWwindow* w, int button, int action, int){
    if(button==GLFW_MOUSE_BUTTON_LEFT){
        if(action==GLFW_PRESS){ gCam.dragging=true; gCam.moving=true; glfwGetCursorPos(w,&gCam.lastX,&gCam.lastY); }
        else gCam.dragging=false;
    }
    if(button==GLFW_MOUSE_BUTTON_RIGHT){
        if(action==GLFW_PRESS)        gGravity = true;
        else if(action==GLFW_RELEASE) gGravity = false;
    }
}
static void scrollCB(GLFWwindow*, double, double yoff){
    gCam.radius -= float(yoff) * float(gCam.zoomSpeed);
    gCam.radius = std::clamp(gCam.radius, 1e10f, 1e12f);
    gCam.moving = true;
}
static void keyCB(GLFWwindow* win, int key, int, int action, int){
    if(action==GLFW_PRESS && key==GLFW_KEY_ESCAPE) glfwSetWindowShouldClose(win,1);
    if(action==GLFW_PRESS && key==GLFW_KEY_P) gScreenshotRequested = true;
    if(action==GLFW_PRESS && key==GLFW_KEY_D){
        gRealDisk = !gRealDisk;
        std::cout << "[INFO] Disk: " << (gRealDisk ? "relativistic" : "reference (flat)") << std::endl;
    }
    if(action==GLFW_PRESS && key==GLFW_KEY_B){
        gShowStars = !gShowStars;
        std::cout << "[INFO] Starfield " << (gShowStars ? "ON" : "OFF") << std::endl;
    }
    if(action==GLFW_PRESS && key==GLFW_KEY_G){
        gGravity = !gGravity;
        std::cout << "[INFO] Gravity turned " << (gGravity ? "ON" : "OFF") << std::endl;
    }
}

// ---------- N-body gravity ----------
// Two-pass (compute all accelerations, then integrate) so the result doesn't
// depend on object order, with a real time step so speed doesn't depend on
// frame rate. Semi-implicit Euler with substeps for stability near the hole.
static void stepGravity(double realDt){
    if(!gGravity) return;
    realDt = std::min(realDt, 0.05);              // avoid huge jumps after a stall
    const int SUB = 50;
    const double h = realDt * gTimeScale / SUB;   // simulated seconds per substep

    vector<ObjectData> holes;
    for(const auto& b : gObjs) if(b.pinned) holes.push_back(b);
    // anything whose centre falls inside a pinned body's event horizon is swallowed.
    // Checked every substep: otherwise a star can fall through the centre between
    // checks and get flung out by the (Newtonian) 1/r^2 singularity.
    auto swallow = [&](){
        gObjs.erase(std::remove_if(gObjs.begin(), gObjs.end(), [&](const ObjectData& o){
            if(o.pinned) return false;
            for(const auto& b : holes){
                double rs = 2.0 * G_GRAV * double(b.mass) / (C_LIGHT*C_LIGHT);
                if(length(vec3(o.posRadius) - vec3(b.posRadius)) < rs) return true;
            }
            return false;
        }), gObjs.end());
    };

    vector<dvec3> acc;
    for(int s=0; s<SUB; ++s){
        const size_t n = gObjs.size();
        acc.assign(n, dvec3(0.0));
        for(size_t i=0;i<n;++i){
            acc[i] = dvec3(0.0);
            if(gObjs[i].pinned) continue;
            dvec3 pi = dvec3(vec3(gObjs[i].posRadius));
            for(size_t j=0;j<n;++j){
                if(i==j) continue;
                dvec3 d = dvec3(vec3(gObjs[j].posRadius)) - pi;
                double dist = length(d);
                if(dist <= 0.0) continue;
                acc[i] += d * (G_GRAV * double(gObjs[j].mass) / (dist*dist*dist));
            }
        }
        for(size_t i=0;i<n;++i){
            auto& o = gObjs[i];
            if(o.pinned) continue;
            dvec3 v = dvec3(o.velocity) + acc[i]*h;
            dvec3 p = dvec3(vec3(o.posRadius)) + v*h;
            o.velocity = vec3(v);
            o.posRadius = vec4(vec3(p), o.posRadius.w);
        }
        swallow();
    }
}

// ---------- Screenshot (24-bit BMP, no dependencies) ----------
static bool saveScreenshotBMP(const char* path){
    int w = gWinW, h = gWinH;
    int rowBytes = (w*3 + 3) & ~3;                    // BMP rows pad to 4 bytes
    vector<unsigned char> px(size_t(rowBytes) * h);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, w, h, GL_BGR, GL_UNSIGNED_BYTE, px.data());  // bottom-up == BMP order

    FILE* f = std::fopen(path, "wb");
    if(!f) return false;
    unsigned int dataSize = unsigned(px.size()), fileSize = 54 + dataSize;
    unsigned char hdr[54] = { 'B','M' };
    auto put32 = [&](int off, unsigned int v){ for(int i=0;i<4;++i) hdr[off+i] = (v >> (8*i)) & 0xFF; };
    put32(2, fileSize); put32(10, 54); put32(14, 40);
    put32(18, unsigned(w)); put32(22, unsigned(h));
    hdr[26] = 1; hdr[28] = 24; put32(34, dataSize);
    std::fwrite(hdr, 1, 54, f);
    std::fwrite(px.data(), 1, px.size(), f);
    std::fclose(f);
    std::cout << "[INFO] Saved " << path << std::endl;    return true;
}
// ---------- Init ----------
static void initGL(){
    if(!glfwInit()) die("GLFW init failed");
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR,4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR,3);
    glfwWindowHint(GLFW_OPENGL_PROFILE,GLFW_OPENGL_CORE_PROFILE);

    gWin = glfwCreateWindow(gWinW, gWinH, "Black Hole (compute)", nullptr, nullptr);
    if(!gWin){ die("GLFW window failed"); }
    glfwMakeContextCurrent(gWin);

    if(!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) die("GLAD load failed");
    pglBindImageTexture = (PFN_BindImageTexture)glfwGetProcAddress("glBindImageTexture");
    pglDispatchCompute  = (PFN_DispatchCompute) glfwGetProcAddress("glDispatchCompute");
    pglMemoryBarrier    = (PFN_MemoryBarrier)   glfwGetProcAddress("glMemoryBarrier");
    if(!pglBindImageTexture || !pglDispatchCompute || !pglMemoryBarrier)
        die("OpenGL 4.3 compute functions not available on this GPU/driver");
    std::cout << "OpenGL " << glGetString(GL_VERSION) << "\n";
    glViewport(0,0,gWinW,gWinH);
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);

    glfwSetCursorPosCallback(gWin, cursorPosCB);
    glfwSetMouseButtonCallback(gWin, mouseCB);
    glfwSetScrollCallback(gWin, scrollCB);
    glfwSetKeyCallback(gWin, keyCB);

    gProgBlit = makeProg(VS_BLIT, FS_BLIT);
    gProgGrid = makeProg(VS_GRID, FS_GRID);
    gProgComp = makeComp(CS_GEOD);

    createQuad();
    generateGrid();

    glGenBuffers(1,&gUBOCamera);
    glBindBuffer(GL_UNIFORM_BUFFER, gUBOCamera);
    glBufferData(GL_UNIFORM_BUFFER, sizeof(CamUBO), nullptr, GL_DYNAMIC_DRAW);

    glGenBuffers(1,&gUBODisk);
    glBindBuffer(GL_UNIFORM_BUFFER, gUBODisk);
    glBufferData(GL_UNIFORM_BUFFER, sizeof(vec4), nullptr, GL_DYNAMIC_DRAW);

    glGenBuffers(1,&gUBOObjs);
    glBindBuffer(GL_UNIFORM_BUFFER, gUBOObjs);
    glBufferData(GL_UNIFORM_BUFFER, sizeof(ObjsUBO), nullptr, GL_DYNAMIC_DRAW);
}

int main(int argc, char** argv){
    // Optional: main.exe --screenshot out.bmp [frames] [--gravity]
    // renders N frames (default 20), saves the last one, and exits.
    const char* autoShot = nullptr;
    int autoFrames = 20;
    for(int i=1;i<argc;++i){
        std::string a = argv[i];
        if(a == "--screenshot" && i+1 < argc){
            autoShot = argv[++i];
            if(i+1 < argc && std::isdigit((unsigned char)argv[i+1][0])) autoFrames = std::atoi(argv[++i]);
        } else if(a == "--gravity")  gGravity = true;
        else if(a == "--flat-disk")  gRealDisk = false;
        else if(a == "--no-stars")   gShowStars = false;
    }

    initGL();
    int frameCount = 0;

    double lastMoveT = glfwGetTime();
    double lastFrameT = glfwGetTime();
    while(!glfwWindowShouldClose(gWin)){
        glClearColor(0.0f,0.0f,0.0f,1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        double frameT = glfwGetTime();
        double dt = frameT - lastFrameT;
        lastFrameT = frameT;

        // FPS in the title bar, updated twice a second
        static int fpsFrames = 0; static double fpsT0 = frameT;
        ++fpsFrames;
        if (frameT - fpsT0 >= 0.5) {
            char title[128];
            std::snprintf(title, sizeof(title), "Black Hole  |  %.0f FPS  |  gravity %s",
                          fpsFrames / (frameT - fpsT0), gGravity ? "ON" : "off");
            glfwSetWindowTitle(gWin, title);
            fpsFrames = 0; fpsT0 = frameT;
        }

        // physics + rebuild warped grid (objects may have moved)
        stepGravity(dt);
        generateGrid();

        // upload UBOs
        uploadCameraUBO();
        uploadDiskUBO();
        uploadObjsUBO();

        // compute
        dispatchCompute();

        // scene grid (for depth feel)
        vec3 camPos = gCam.position();
        mat4 V = lookAt(camPos, gCam.target, vec3(0,1,0));
        // 60deg FOV to match the ray tracer's tanHalfFov, so grid and image line up
        mat4 P = perspective(radians(60.0f), float(gWinW)/float(gWinH), 1e9f, 1e14f);
        drawGrid(P*V);

        // blit
        drawQuad();

        // decay "moving" flag after a short idle
        double now = glfwGetTime();
        if (gCam.dragging) lastMoveT = now;
        if ((now - lastMoveT) > 0.15) gCam.moving = false;

        // screenshots: P key, or automatic with --screenshot
        ++frameCount;
        if (gScreenshotRequested) {
            char name[64];
            std::snprintf(name, sizeof(name), "screenshot_%lld.bmp", (long long)std::time(nullptr));
            saveScreenshotBMP(name);
            gScreenshotRequested = false;
        }
        if (autoShot && frameCount >= autoFrames) {
            saveScreenshotBMP(autoShot);
            glfwSetWindowShouldClose(gWin, 1);
        }

        glfwSwapBuffers(gWin);
        glfwPollEvents();
    }

    glfwDestroyWindow(gWin);
    glfwTerminate();
    return 0;
}
