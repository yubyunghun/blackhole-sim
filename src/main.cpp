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
// T: spacetime grid mode. 1 = ray traced (lensed, occluded), 0 = off,
// 2 = flat overlay drawn behind the image (the reference's original method)
static int   gGridMode = 1;
// [ / ]: black hole spin chi = a/M (0 = Schwarzschild, ->1 = maximally spinning)
static double gSpin = 0.9;
static const char* kGridModeNames[] = { "off", "ray traced", "flat overlay (reference)" };

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
// HDR output (values may exceed 1); tone mapping + bloom happen afterwards
layout(binding=0, rgba16f) writeonly uniform image2D outImage;

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
    vec4 disk; // x=r1, y=r2 (metres), z=rs (metres), w=time (seconds)
    vec4 kerr; // x=a (spin parameter, rs units, 0..0.5), y=r+ (horizon, rs units)
};

// Objects UBO (masses packed into 4 vec4s => up to 16 masses)
layout(std140, binding=3) uniform Objects {
    int  numObjects; vec3 _padX;
    vec4 objPosRadius[16];
    vec4 objColor[16];
    vec4 mass4[4]; // masses[i] = mass4[i/4][i%4]
};

// ---------------------------------------------------------------------------
// Null geodesics in Kerr spacetime (rotating black hole), Kerr-Schild form.
//
// Kerr-Schild coordinates are Cartesian and regular across the horizon:
//     g_uv = eta_uv + f k_u k_v
//     f = 2 M r^3 / (r^4 + a^2 z^2)
//     k = ( (r x + a y)/(r^2 + a^2), (r y - a x)/(r^2 + a^2), z / r ),  k_t = 1
// where r is the Boyer-Lindquist radius, found from
//     r^4 - (R^2 - a^2) r^2 - a^2 z^2 = 0,   R^2 = x^2 + y^2 + z^2.
// The inverse metric is just eta - f k k, so the photon Hamiltonian is
//     H = 1/2 ( -1 + |p|^2 - f S^2 ),   S = 1 + k.p   (p_t = -1)
// and Hamilton's equations give
//     dx/dl =  dH/dp = p - f S k
//     dp/dl = -dH/dx = 1/2 ( S^2 grad f + 2 f S grad S )
// with grad r, grad f, grad S written out analytically below.
//
// With a = 0 this is Schwarzschild in Eddington-Finkelstein coordinates,
// whose spatial coordinates are the Schwarzschild ones, so the image is
// identical to the old Binet-equation tracer.
//
// Conventions:
//  - units of rs (rs = 2M = 1, so M = 0.5) to keep r^4 etc. in float range
//  - "local" coords have the spin axis along +Z; world has it along +y:
//        local = world.zxy,   world = local.yzx   (cyclic, so right-handed)
//  - We trace from the camera backwards in time. Kerr with spin +a run
//    backwards in time is Kerr with spin -a run forwards, so we integrate
//    with A = -a. (This also keeps ingoing Kerr-Schild regular at the
//    horizon for the rays we trace.)
// ---------------------------------------------------------------------------

const float STEP_FRAC = 0.02;   // step = 2% of current radius near the hole...
const float STEP_FAR  = 0.08;   // ...growing to 8% far away, where paths are nearly straight
// Stop outgoing rays here and use their direction for the sky. Everything in
// the scene (stars at ~31 rs, grid within ~15 rs) is inside this, and the
// bending left beyond it is < 1 degree.
const float ESCAPE_R  = 80.0;
const int   MAX_STEPS = 3000;   // rays still bound after this are captured
const float MASS      = 0.5;    // M in units of rs

float RS;      // Schwarzschild radius in metres (from Disk UBO)
float A;       // spin parameter used for integration (= -a, see above)

// Boyer-Lindquist r at a Kerr-Schild point
float ksR(vec3 x){
    float a2 = A*A;
    float w = dot(x,x) - a2;
    return sqrt(0.5 * (w + sqrt(w*w + 4.0*a2*x.z*x.z)));
}
// metric function f (depends only on a^2)
float ksF(vec3 x){
    float r = ksR(x), r2 = r*r;
    return 2.0*MASS*r2*r / (r2*r2 + A*A*x.z*x.z);
}
vec3 ksK(vec3 x, float r){
    float P = r*r + A*A;
    return vec3((r*x.x + A*x.y)/P, (r*x.y - A*x.x)/P, x.z/r);
}

void kerrRHS(vec3 x, vec3 p, out vec3 dx, out vec3 dp){
    float a2 = A*A;
    float R2 = dot(x,x);
    float w  = R2 - a2;
    float r2 = 0.5 * (w + sqrt(w*w + 4.0*a2*x.z*x.z));
    float r  = sqrt(r2);
    float P  = r2 + a2;
    float Q  = r2*r2 + a2*x.z*x.z;
    float f  = 2.0*MASS*r2*r / Q;
    vec3  k  = vec3((r*x.x + A*x.y)/P, (r*x.y - A*x.x)/P, x.z/r);
    float S  = 1.0 + dot(k, p);

    dx = p - f*S*k;

    // grad r (implicit differentiation of the quartic)
    float den = 2.0*r2 - R2 + a2;
    vec3 gr = vec3(x.x*r, x.y*r, x.z*(r2 + a2)/r) / den;
    // grad f
    vec3 gQ = 4.0*r2*r*gr + vec3(0.0, 0.0, 2.0*a2*x.z);
    vec3 gf = 2.0*MASS * (3.0*r2*Q*gr - r2*r*gQ) / (Q*Q);
    // grad S = sum_j p_j grad k_j
    float c = (p.x*x.x + p.y*x.y)/P
            - 2.0*r*(p.x*(r*x.x + A*x.y) + p.y*(r*x.y - A*x.x))/(P*P)
            - p.z*x.z/r2;
    vec3 gS = c*gr + vec3((r*p.x - A*p.y)/P, (A*p.x + r*p.y)/P, p.z/r);

    dp = 0.5 * (S*S*gf + 2.0*f*S*gS);
}

// Classic 4-stage Runge-Kutta on (x, p)
void rk4(inout vec3 x, inout vec3 p, float h){
    vec3 k1x, k1p, k2x, k2p, k3x, k3p, k4x, k4p;
    kerrRHS(x,             p,             k1x, k1p);
    kerrRHS(x + 0.5*h*k1x, p + 0.5*h*k1p, k2x, k2p);
    kerrRHS(x + 0.5*h*k2x, p + 0.5*h*k2p, k3x, k3p);
    kerrRHS(x + h*k3x,     p + h*k3p,     k4x, k4p);
    x += h/6.0 * (k1x + 2.0*k2x + 2.0*k3x + k4x);
    p += h/6.0 * (k1p + 2.0*k2p + 2.0*k3p + k4p);
}

// Initial covariant momentum for a photon whose coordinate velocity points
// along unit vector d (so the camera's pixel directions are exactly the
// directions rays leave in, like the old tracer). Solving dx/dl || d and H = 0:
//     p = beta d + gamma k,  beta = 1/sqrt(1 - f(1 - c^2)),
//     gamma = f (1 + beta c)/(1 - f),  c = k.d    (k null => |k| = 1)
vec3 initMomentum(vec3 x, vec3 d){
    float r = ksR(x);
    float f = ksF(x);
    vec3  k = ksK(x, r);
    float c = dot(k, d);
    float beta  = 1.0 / sqrt(max(1.0 - f*(1.0 - c*c), 1e-6));
    float gamma = f * (1.0 + beta*c) / max(1.0 - f, 1e-6);
    return beta*d + gamma*k;
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

)" R"(
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

// ---------------------------------------------------------------------------
// Gas texture: 3D value-noise fBm sampled in polar coords, so features are
// stretched along the orbit (streaks) and tile seamlessly around the ring.
// ---------------------------------------------------------------------------
float vnoise(vec3 p){
    vec3 i = floor(p), f = fract(p);
    f = f*f*(3.0 - 2.0*f);
    return mix(mix(mix(hash13(i+vec3(0,0,0)), hash13(i+vec3(1,0,0)), f.x),
                   mix(hash13(i+vec3(0,1,0)), hash13(i+vec3(1,1,0)), f.x), f.y),
               mix(mix(hash13(i+vec3(0,0,1)), hash13(i+vec3(1,0,1)), f.x),
                   mix(hash13(i+vec3(0,1,1)), hash13(i+vec3(1,1,1)), f.x), f.y), f.z);
}
float fbm(vec3 p){
    float s = 0.0, a = 0.5;
    for (int i = 0; i < 5; ++i) { s += a * vnoise(p); p = p * 2.03 + 11.7; a *= 0.5; }
    return s;
}
float gasAt(float r, float phi){
    // ring radius 2.6 -> clumps around the orbit; r*4 -> finer radial banding
    vec3 q = vec3(cos(phi) * 2.6, sin(phi) * 2.6, r * 4.0);
    return fbm(q);
}

// Differential (Keplerian-like) rotation: omega ~ r^-1.5, inner gas laps outer.
// Shear would wind the pattern into ever-tighter spirals, so we use the
// flow-map trick: two copies with staggered resets, cross-faded so each one
// resets while invisible.
const float SPIN = 4.0;      // angular speed scale (rad/s at r = 1 rs)
const float FLOW_P = 4.0;    // seconds per flow cycle
float gasDensity(float r, float phi, float t){
    float omega = SPIN * pow(r, -1.5);
    float tA = mod(t, FLOW_P);
    float tB = mod(t + 0.5*FLOW_P, FLOW_P);
    float wA = 1.0 - abs(2.0*tA/FLOW_P - 1.0);
    // gas moves toward decreasing phi (matches Doppler velocity), so sample
    // where the material was: phi + omega*t
    float nA = gasAt(r, phi + omega*tA);
    float nB = gasAt(r + 0.37, phi + omega*tB + 1.3);   // decorrelate the 2 copies
    float n = mix(nB, nA, wA);
    // contrast: dark lanes and bright filaments
    return smoothstep(0.30, 0.75, n);
}

)" R"(
// Redshift for gas on a prograde circular (Keplerian) Kerr orbit, seen by a
// static camera:   g = 1 / ( u^t_cam * u^t (1 - Omega * L) )
//   Omega = sqrt(M) / (r^1.5 + a sqrt(M))
//   u^t   = (r^1.5 + a sqrt(M)) / ( r^0.75 sqrt(r^1.5 - 3M sqrt(r) + 2a sqrt(M)) )
//   L     = photon's angular momentum about the spin axis (energy = 1)
//   u^t_cam = 1/sqrt(1 - f_cam)   (static observer in Kerr-Schild: g_tt = -1 + f)
// This one number contains Doppler beaming, frame dragging and gravitational
// redshift. For a = 0 it reduces to the special-relativistic Doppler factor
// times sqrt(1 - rs/r) used before.
float kerrDiskG(float r, float aPhys, float Lphoton, float fCam){
    float sM  = sqrt(MASS);
    float r15 = r * sqrt(r);
    float Om  = sM / (r15 + aPhys*sM);
    float ut  = (r15 + aPhys*sM) /
                (pow(r, 0.75) * sqrt(max(r15 - 3.0*MASS*sqrt(r) + 2.0*aPhys*sM, 1e-4)));
    float uCam = 1.0 / sqrt(max(1.0 - fCam, 1e-4));
    return 1.0 / (uCam * ut * max(1.0 - Om*Lphoton, 1e-3));
}

// r: Boyer-Lindquist radius of the hit; pc: hit point (world, rs units)
vec4 diskColorRelativistic(vec3 pc, float r, float g, float r1, float r2, float t){

    // emitted temperature falls off ~ r^-3/4 (thin-disk profile)
    // profiles are anchored at a fixed radius (2.2 rs), not the inner edge, so
    // spin (which moves the ISCO inward) adds hotter gas instead of dimming the rest
    const float R_REF = 2.2;
    float T = 4500.0 * pow(r / R_REF, -0.75);
    vec3 col = blackbody(T * g);

    float gas = gasDensity(r, atan(pc.z, pc.x), t);
    float I = pow(g, 4.0) * pow(R_REF / r, 1.5) * (0.25 + 1.6 * gas);
    vec3 c = col * I * 1.5;                       // linear HDR; tone mapped later
    // opaque disk with soft inner/outer edges (premultiplied alpha)
    float edge = smoothstep(r2, r2 - 0.6, r) * smoothstep(r1, r1 + 0.15, r);
    return vec4(c * edge, edge);
}

)" R"(
// ---------------------------------------------------------------------------
// Ray-traced spacetime grid: Flamm's paraboloid, the embedding diagram of the
// Schwarzschild geometry:  y(d) = Y0 + 2*sqrt(rs*(d - rs)),  d = horizontal
// distance from the hole. Light rays hit it like any other surface, so the
// grid is lensed, wraps around the shadow, and is hidden behind the hole.
// Only the lines are drawn; rays pass through the gaps and continue.
// ---------------------------------------------------------------------------
const float GRID_SPACING_M = 1e10;   // line spacing (metres), same as the old CPU grid
const float GRID_HALF      = 13.0;   // half-extent in cells
const float GRID_Y0_M      = -9e10;  // height of the funnel's throat (metres)
const float GRID_LINE_PX   = 1.1;    // line width in pixels
const vec3  GRID_COLOR     = vec3(0.5);
const float GRID_ALPHA     = 0.7;

// signed height above the surface (rs units, rs = 1)
float gridF(vec3 p, float y0){
    float d = max(length(p.xz), 1.0);
    return p.y - (y0 + 2.0 * sqrt(d - 1.0));
}

void main(){
    int WIDTH  = CAM.ires.x;
    int HEIGHT = CAM.ires.y;
    ivec2 pix = ivec2(gl_GlobalInvocationID.xy);
    if (pix.x >= WIDTH || pix.y >= HEIGHT) return;
    bool rtGrid = CAM.ires.z == 1;

    int flags = int(CAM.params.w + 0.5);
    bool realDisk = (flags & 1) != 0;
    bool showStars = (flags & 2) != 0;

    RS = disk.z;
    float r1 = disk.x / RS, r2 = disk.y / RS;   // Boyer-Lindquist radii (rs units)
    float aPhys = kerr.x;
    float rPlus = kerr.y;
    A = -aPhys;                                 // backwards in time == opposite spin

    float u = (2.0*(pix.x+0.5)/WIDTH - 1.0) * CAM.params.y * CAM.params.x;
    float v_ = (1.0 - 2.0*(pix.y+0.5)/HEIGHT) * CAM.params.x;
    vec3 dir = normalize(u * CAM.camRight.xyz - v_ * CAM.camUp.xyz + CAM.camFwd.xyz);

    vec3 x  = CAM.camPos.xyz / RS;  // world position, rs units (used for hit tests)
    vec3 v  = dir;                  // world direction of travel
    vec3 xl = x.zxy;                // local Kerr-Schild position (spin axis = Z)
    float fCam = ksF(xl);
    if (fCam >= 0.999) {            // camera inside the horizon: nothing to see
        imageStore(outImage, pix, vec4(0,0,0,1));
        return;
    }
    vec3 pl = initMomentum(xl, dir.zxy);
    // photon angular momentum about the spin axis (conserved). The traced ray
    // runs backwards in time, so the physical photon's is the negative.
    float Lphys = -(xl.x*pl.y - xl.y*pl.x);

    vec4 color = vec4(0);           // what the ray finally hits (premultiplied)
    vec4 front = vec4(0);           // grid lines crossed on the way (front-to-back)
    bool done = false;

    float gS   = GRID_SPACING_M / RS;
    float gY0  = GRID_Y0_M / RS;
    float gExt = GRID_HALF * gS;
    float pixAngle = 2.0 * CAM.params.x / float(HEIGHT);   // radians per pixel
    float travel = 0.0;                                     // distance along the ray
    // highest point of the grid (its corners): steps entirely above it skip the test
    float gTop = gY0 + 2.0 * sqrt(1.4143 * gExt - 1.0);

    for (int i = 0; i < MAX_STEPS && !done; ++i){
        float R = length(x);
        float h = mix(STEP_FRAC, STEP_FAR, smoothstep(6.0, 30.0, R)) * R;
        vec3 p0 = x;
        rk4(xl, pl, h);
        x = xl.yzx;                       // back to world coords
        vec3 p1 = x;
        float segLen = length(p1 - p0);
        if (segLen > 0.0) v = (p1 - p0) / segLen;

        // --- earliest event along this step ---
        float bestT = 2.0;
        int   hit   = 0;            // 1 = disk, 2 = object
        int   hitObj = -1;

        // accretion disk: crossing of the y=0 plane, interpolated
        // strict sign change: a ray starting exactly on the plane (camera at y=0) isn't a hit
        if (p0.y * p1.y < 0.0) {
            float t = p0.y / (p0.y - p1.y);
            vec3 pc = mix(p0, p1, t);
            // in the equatorial plane, Kerr-Schild R^2 = r^2 + a^2
            float rd = sqrt(max(dot(pc.xz, pc.xz) - aPhys*aPhys, 0.0));
            if (rd >= r1 && rd <= r2 && t < bestT) { bestT = t; hit = 1; }
        }

        // spheres (stars)
        for (int k = 0; k < numObjects; ++k){
            float t;
            vec3  c = objPosRadius[k].xyz / RS;
            float R = objPosRadius[k].w   / RS;
            if (segSphere(p0, p1, c, R, t) && t < bestT) { bestT = t; hit = 2; hitObj = k; }
        }

        // grid surface: only counts if it's in front of whatever else this step hit
        // cheap bounding-box rejection first: above the top, below the throat,
        // or outside the square footprint on the same side
        bool nearGrid = min(p0.y, p1.y) <= gTop && max(p0.y, p1.y) >= gY0 &&
                        !(p0.x >  gExt && p1.x >  gExt) && !(p0.x < -gExt && p1.x < -gExt) &&
                        !(p0.z >  gExt && p1.z >  gExt) && !(p0.z < -gExt && p1.z < -gExt);
        if (rtGrid && nearGrid && front.a < 0.99) {
            float f0 = gridF(p0, gY0), f1 = gridF(p1, gY0);
            if (f0 * f1 < 0.0) {
                float t = f0 / (f0 - f1);
                vec3 P = mix(p0, p1, t);
                if (t < bestT && length(P.xz) >= 1.0 &&
                    abs(P.x) <= gExt && abs(P.z) <= gExt) {
                    // distance to the nearest grid line, and line width at this distance
                    vec2 g = abs(fract(P.xz / gS + 0.5) - 0.5) * gS;
                    float dl = min(g.x, g.y);
                    float w  = GRID_LINE_PX * pixAngle * (travel + t * segLen);
                    float cov = 1.0 - smoothstep(0.0, w, dl);
                    float a = GRID_ALPHA * cov;
                    front += (1.0 - front.a) * vec4(GRID_COLOR * a, a);
                }
            }
        }
        travel += segLen;

        if (hit == 1) {
            vec3 pc = mix(p0, p1, bestT);
            float rBL = sqrt(max(dot(pc.xz, pc.xz) - aPhys*aPhys, 0.0));
            if (realDisk) {
                float g = kerrDiskG(rBL, aPhys, Lphys, fCam);
                color = diskColorRelativistic(pc, rBL, g, r1, r2, disk.w);
            } else {
                float ro = rBL / r2;
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
        } else if (ksR(xl) <= rPlus) {
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

    // grid lines in front, everything else behind (premultiplied "over")
    color = front + (1.0 - front.a) * color;
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
    data.ires = ivec4(cw, ch, gGridMode, 0);   // z = grid mode (1 = ray traced)

    glBindBuffer(GL_UNIFORM_BUFFER, gUBOCamera);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(CamUBO), &data);
}

// ---------- Kerr helpers (units of M) ----------
// Prograde innermost stable circular orbit (Bardeen, Press & Teukolsky 1972)
static double iscoOverM(double chi){
    double z1 = 1.0 + std::cbrt(1.0 - chi*chi) * (std::cbrt(1.0 + chi) + std::cbrt(1.0 - chi));
    double z2 = std::sqrt(3.0*chi*chi + z1*z1);
    return 3.0 + z2 - std::sqrt((3.0 - z1) * (3.0 + z1 + 2.0*z2));
}

struct DiskUBO { vec4 disk; vec4 kerr; }; // (r1, r2, rs, time), (a, r+, -, -)
static void uploadDiskUBO(){
    DiskUBO d{};
    // w = animation time; wrapped so float precision stays good in long sessions
    // (wrap period is a multiple of the 4 s flow cycle, so it's seamless for the flow)
    float t = float(std::fmod(glfwGetTime(), 4000.0));
    // In rs units M = 0.5. Spin parameter a = chi*M, horizon r+ = M + sqrt(M^2 - a^2).
    const double M = 0.5;
    double a     = gSpin * M;
    double rPlus = M + std::sqrt(M*M - a*a);
    // Inner edge: the reference's 2.2 rs, or the ISCO if spin pulls it closer
    double r1 = std::min(2.2, iscoOverM(gSpin) * M);
    d.disk = vec4(float(SAG_RS*r1), float(SAG_RS*5.2), float(SAG_RS), t);
    d.kerr = vec4(float(a), float(rPlus), 0.0f, 0.0f);
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
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, cw, ch, 0, GL_RGBA, GL_FLOAT, nullptr);
        texW = cw; texH = ch;
    }

    glUseProgram(gProgComp);
    // Bind UBOs
    glBindBufferBase(GL_UNIFORM_BUFFER, 1, gUBOCamera);
    glBindBufferBase(GL_UNIFORM_BUFFER, 2, gUBODisk);
    glBindBufferBase(GL_UNIFORM_BUFFER, 3, gUBOObjs);

    // bind image
    glBindImageTexture(0, gTexOut, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);

    GLuint gx = (GLuint) ((cw + 15) / 16);
    GLuint gy = (GLuint) ((ch + 15) / 16);
    glDispatchCompute(gx, gy, 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
}

// ---------- Post-processing: bloom + tone mapping ----------
// HDR image -> bright pass (half res) -> separable Gaussian blur at two
// sizes (half and quarter res) -> composite: tonemap(hdr + bloom), blended
// over the grid. Bloom has alpha 0, so its glow adds on top of grid/stars.
static const char* FS_BRIGHT = R"(
#version 330 core
in vec2 vUV; out vec4 o;
uniform sampler2D uTex; uniform float uThreshold;
void main(){
    vec3 c = texture(uTex, vUV).rgb;
    float l = max(c.r, max(c.g, c.b));
    // soft threshold: keep only the part of the brightness above uThreshold
    o = vec4(c * (max(l - uThreshold, 0.0) / max(l, 1e-4)), 1.0);
}
)";
static const char* FS_BLUR = R"(
#version 330 core
in vec2 vUV; out vec4 o;
uniform sampler2D uTex; uniform vec2 uStep;   // one texel along the blur axis
void main(){
    const float w[5] = float[](0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216);
    vec3 s = texture(uTex, vUV).rgb * w[0];
    for (int i = 1; i < 5; ++i){
        s += texture(uTex, vUV + uStep * float(i)).rgb * w[i];
        s += texture(uTex, vUV - uStep * float(i)).rgb * w[i];
    }
    o = vec4(s, 1.0);
}
)";
static const char* FS_COMPOSITE = R"(
#version 330 core
in vec2 vUV; out vec4 o;
uniform sampler2D uHDR, uBloomA, uBloomB;
uniform float uBloom;      // bloom strength (0 = off)
uniform int   uTonemap;    // 1 = ACES tone map, 0 = plain clamp (reference look)
uniform float uExposure;
vec3 aces(vec3 x){ return clamp((x*(2.51*x + 0.03)) / (x*(2.43*x + 0.59) + 0.14), 0.0, 1.0); }
void main(){
    vec4 h = texture(uHDR, vUV);
    vec3 b = texture(uBloomA, vUV).rgb * 0.6 + texture(uBloomB, vUV).rgb * 0.9;
    vec3 c = h.rgb + b * uBloom;
    if (uTonemap == 1) c = aces(c * uExposure);
    o = vec4(c, h.a);        // premultiplied: glow (alpha 0) adds over the grid
}
)";

static GLuint gProgBright = 0, gProgBlur = 0, gProgComposite = 0;
struct RT { GLuint fbo = 0, tex = 0; int w = 0, h = 0; };
static RT gBloomA1, gBloomA2, gBloomB1, gBloomB2;   // A: half res, B: quarter res
static bool  gBloom = true;          // L key
static float gBloomStrength = 0.9f;
static float gExposure = 1.0f;

static void makeRT(RT& rt, int w, int h){
    rt.w = w; rt.h = h;
    glGenTextures(1, &rt.tex);
    glBindTexture(GL_TEXTURE_2D, rt.tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &rt.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, rt.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt.tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) die("bloom FBO incomplete");
    glClearColor(0,0,0,0);
    glClear(GL_COLOR_BUFFER_BIT);   // start black, not undefined (avoids NaN when bloom is off)
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}
static void initBloom(){
    gProgBright    = makeProg(VS_BLIT, FS_BRIGHT);
    gProgBlur      = makeProg(VS_BLIT, FS_BLUR);
    gProgComposite = makeProg(VS_BLIT, FS_COMPOSITE);
    makeRT(gBloomA1, gWinW/2, gWinH/2); makeRT(gBloomA2, gWinW/2, gWinH/2);
    makeRT(gBloomB1, gWinW/4, gWinH/4); makeRT(gBloomB2, gWinW/4, gWinH/4);
}

// draw a fullscreen pass sampling `src` into `dst`
static void pass(GLuint prog, GLuint src, const RT& dst){
    glBindFramebuffer(GL_FRAMEBUFFER, dst.fbo);
    glViewport(0, 0, dst.w, dst.h);
    glUseProgram(prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, src);
    glUniform1i(glGetUniformLocation(prog, "uTex"), 0);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}
static void blur(RT& a, RT& tmp, int iterations){
    for (int i = 0; i < iterations; ++i){
        glUseProgram(gProgBlur);
        glUniform2f(glGetUniformLocation(gProgBlur, "uStep"), 1.0f / a.w, 0.0f);
        pass(gProgBlur, a.tex, tmp);
        glUniform2f(glGetUniformLocation(gProgBlur, "uStep"), 0.0f, 1.0f / a.h);
        pass(gProgBlur, tmp.tex, a);
    }
}

// Runs the bloom passes into offscreen targets (call before drawing the grid)
static void runBloom(){
    if (!gBloom) return;
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glBindVertexArray(gVAOQuad);

    glUseProgram(gProgBright);
    glUniform1f(glGetUniformLocation(gProgBright, "uThreshold"), 0.8f);
    pass(gProgBright, gTexOut, gBloomA1);    // bright pass, downsample to 1/2
    blur(gBloomA1, gBloomA2, 2);             // tight glow
    pass(gProgBlit, gBloomA1.tex, gBloomB1); // downsample to 1/4
    blur(gBloomB1, gBloomB2, 3);             // wide haze

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, gWinW, gWinH);
    glBindVertexArray(0);
    glEnable(GL_DEPTH_TEST);
}

// ---------- Draw ----------
static void drawQuad(){
    glUseProgram(gProgComposite);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, gTexOut);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, gBloomA1.tex);
    glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, gBloomB1.tex);
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(glGetUniformLocation(gProgComposite,"uHDR"), 0);
    glUniform1i(glGetUniformLocation(gProgComposite,"uBloomA"), 1);
    glUniform1i(glGetUniformLocation(gProgComposite,"uBloomB"), 2);
    glUniform1f(glGetUniformLocation(gProgComposite,"uBloom"), gBloom ? gBloomStrength : 0.0f);
    glUniform1i(glGetUniformLocation(gProgComposite,"uTonemap"), gRealDisk ? 1 : 0);
    glUniform1f(glGetUniformLocation(gProgComposite,"uExposure"), gExposure);
    glBindVertexArray(gVAOQuad);
    glDisable(GL_DEPTH_TEST);
    // Blend the ray-traced image over the grid: empty space (alpha 0) lets the
    // warped grid show through, like the reference. Colours are premultiplied,
    // so faint stars aren't darkened twice and bloom adds as pure glow.
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
    if((action==GLFW_PRESS || action==GLFW_REPEAT) &&
       (key==GLFW_KEY_LEFT_BRACKET || key==GLFW_KEY_RIGHT_BRACKET)){
        double step = (key==GLFW_KEY_RIGHT_BRACKET) ? 0.1 : -0.1;
        gSpin = std::clamp(std::round((gSpin + step) * 10.0) / 10.0, 0.0, 0.99);
        std::cout << "[INFO] Spin a/M = " << gSpin << std::endl;
    }
    if(action==GLFW_PRESS && key==GLFW_KEY_T){
        gGridMode = (gGridMode == 1) ? 0 : (gGridMode == 0 ? 2 : 1);   // ray traced -> off -> overlay
        std::cout << "[INFO] Grid: " << kGridModeNames[gGridMode] << std::endl;
    }
    if(action==GLFW_PRESS && key==GLFW_KEY_L){
        gBloom = !gBloom;
        std::cout << "[INFO] Bloom " << (gBloom ? "ON" : "OFF") << std::endl;
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
    initBloom();
    generateGrid();

    glGenBuffers(1,&gUBOCamera);
    glBindBuffer(GL_UNIFORM_BUFFER, gUBOCamera);
    glBufferData(GL_UNIFORM_BUFFER, sizeof(CamUBO), nullptr, GL_DYNAMIC_DRAW);

    glGenBuffers(1,&gUBODisk);
    glBindBuffer(GL_UNIFORM_BUFFER, gUBODisk);
    glBufferData(GL_UNIFORM_BUFFER, sizeof(DiskUBO), nullptr, GL_DYNAMIC_DRAW);

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
        else if(a == "--no-bloom")   gBloom = false;
        else if(a == "--spin" && i+1 < argc) gSpin = std::clamp(std::atof(argv[++i]), 0.0, 0.99);
        else if(a == "--grid" && i+1 < argc) {
            std::string m = argv[++i];
            gGridMode = (m == "off") ? 0 : (m == "overlay" ? 2 : 1);
        }
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
            std::snprintf(title, sizeof(title), "Black Hole  |  %.0f FPS  |  spin a/M = %.2f  |  gravity %s",
                          fpsFrames / (frameT - fpsT0), gSpin, gGravity ? "ON" : "off");
            glfwSetWindowTitle(gWin, title);
            fpsFrames = 0; fpsT0 = frameT;
        }

        // physics + rebuild warped grid (objects may have moved)
        stepGravity(dt);
        if (gGridMode == 2) generateGrid();

        // upload UBOs
        uploadCameraUBO();
        uploadDiskUBO();
        uploadObjsUBO();

        // compute
        dispatchCompute();
        runBloom();

        // scene grid (for depth feel)
        vec3 camPos = gCam.position();
        mat4 V = lookAt(camPos, gCam.target, vec3(0,1,0));
        // 60deg FOV to match the ray tracer's tanHalfFov, so grid and image line up
        mat4 P = perspective(radians(60.0f), float(gWinW)/float(gWinH), 1e9f, 1e14f);
        if (gGridMode == 2) drawGrid(P*V);   // modes 0/1: grid is off or inside the ray tracer

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
