#pragma once
// Shared declarations for the black hole renderer.
//
// Settings and scene state are C++17 `inline` variables: every .cpp that
// includes this header sees the same single instance.
//
//   main.cpp     window, input, scene actions, command line, main loop
//   physics.cpp  Kerr formulas, Novikov-Thorne flux, N-body gravity
//   render.cpp   shaders, render targets, ray-tracer dispatch, bloom, grid
//   ui.cpp       Dear ImGui control panel
//   shaders/     GLSL (geodesic.comp is the ray tracer)

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#define _USE_MATH_DEFINES
#include <cmath>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

using namespace glm;
using std::vector;

// ---------- GL 4.3 compute entry points ----------
// third_party/glad was generated for GL 3.3 core, which lacks compute shaders.
// The few 4.2/4.3 functions we need are loaded by hand after context creation.
#ifndef GL_COMPUTE_SHADER
#define GL_COMPUTE_SHADER 0x91B9
#endif
#ifndef GL_SHADER_IMAGE_ACCESS_BARRIER_BIT
#define GL_SHADER_IMAGE_ACCESS_BARRIER_BIT 0x00000020
#endif
#ifndef GL_TEXTURE_FETCH_BARRIER_BIT
#define GL_TEXTURE_FETCH_BARRIER_BIT 0x00000008
#endif
typedef void (APIENTRYP PFN_BindImageTexture)(GLuint, GLuint, GLint, GLboolean, GLint, GLenum, GLenum);
typedef void (APIENTRYP PFN_DispatchCompute)(GLuint, GLuint, GLuint);
typedef void (APIENTRYP PFN_MemoryBarrier)(GLbitfield);
inline PFN_BindImageTexture pglBindImageTexture = nullptr;
inline PFN_DispatchCompute  pglDispatchCompute  = nullptr;
inline PFN_MemoryBarrier    pglMemoryBarrier    = nullptr;
#define glBindImageTexture pglBindImageTexture
#define glDispatchCompute  pglDispatchCompute
#define glMemoryBarrier    pglMemoryBarrier

// ---------- Physics constants ----------
inline constexpr double C_LIGHT = 299792458.0;
inline constexpr double G_GRAV  = 6.67430e-11;
// Sagittarius A* mass (kg) and Schwarzschild radius r_s
inline constexpr double SAG_MASS = 8.54e36;
inline constexpr double SAG_RS   = 2.0 * G_GRAV * SAG_MASS / (C_LIGHT*C_LIGHT);

// ---------- Window / resolution ----------
// The window is resizable; gWinW/gWinH track its framebuffer size.
inline int   gWinW = 1280, gWinH = 720;
// Ray-traced resolution = window size x render scale. "Auto" picks the scale
// so the image is at most ~0.5 megapixels, which keeps big or fullscreen
// windows smooth. The moving-camera preview is half that in each direction.
inline float gRenderScale = 1.0f;     // used when auto is off
inline bool  gAutoScale   = true;
inline constexpr double AUTO_MAX_PIXELS = 500000.0;
inline int   gMovingW = 400, gMovingH = 300;   // res while orbiting (set by updateRenderSizes)
inline int   gStillW  = 800, gStillH  = 600;   // res when idle
inline bool  gFullscreen = false;     // F11
inline bool  gResized    = true;      // re-apply sizes next frame (also on the first frame)

// ---------- Settings ----------
// Simulated seconds per real second for the N-body gravity.
// (A star at 4e11 m around Sgr A* has a ~18 h orbit; 1e4 makes that ~7 s.)
inline double gTimeScale = 1e4;
inline bool   gGravity   = false;     // G key; right mouse while held
inline bool   gScreenshotRequested = false;   // P key
inline bool   gRealDisk  = true;      // D: relativistic disk vs reference flat disk
inline bool   gShowStars = true;      // B: lensed starfield background
// T: spacetime grid mode. 1 = ray traced (lensed, occluded), 0 = off,
// 2 = flat overlay drawn behind the image (the reference's original method)
inline int    gGridMode  = 1;
inline const char* const kGridModeNames[] = { "off", "ray traced", "flat overlay (reference)" };
// [ / ]: black hole spin chi = a/M (0 = Schwarzschild, ->1 = maximally spinning)
inline double gSpin = 0.9;

// Disk look (UI panel)
inline float gDiskTemp  = 5500.0f;    // peak temperature (K), Novikov-Thorne profile
inline float gDiskGain  = 2.5f;       // brightness
inline float gGasSpeed  = 4.0f;       // gas rotation speed
inline float gDiskOuter = 5.2f;       // outer radius (rs)
inline bool  gShowUI    = true;       // H key
inline bool  gVideoLook = false;      // V key: match the look of the reference video
inline bool  gRelCamera = true;       // relativistic (static-observer tetrad) camera

// Post-processing
inline bool  gBloom = true;           // L key
inline float gBloomStrength = 0.9f;
inline float gExposure = 1.0f;

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
};
inline Camera gCam;

// ---------- Scene objects ----------
struct ObjectData {
    vec4 posRadius; // xyz center, w radius
    vec4 color;     // rgb color, a unused
    float mass;     // kg (packed later into vec4[4] for std140)
    vec3  velocity; // m/s
    bool  pinned;   // never moves (the black hole: the ray tracer assumes it sits at the origin)
};
inline vector<ObjectData> gObjs = {
    { vec4(4e11f, 0.0f, 0.0f, 4e10f), vec4(1,1,0,1), 1.98892e30f, vec3(0), false },
    { vec4(0.0f, 0.0f, 4e11f, 4e10f), vec4(1,0,0,1), 1.98892e30f, vec3(0), false },
    { vec4(0.0f, 0.0f, 0.0f, float(SAG_RS)), vec4(0,0,0,1), float(SAG_MASS), vec3(0), true }
};
inline vector<ObjectData> gInitialObjs;   // captured at startup for "Reset scene"

// Animation clock override (seconds). < 0: use the real clock. Recording
// (--record) sets it per frame so the gas moves smoothly and repeatably.
inline double gTimeOverride = -1.0;

inline GLFWwindow* gWin = nullptr;
inline std::string gExeDir;               // folder of the executable (for finding shaders/)

[[noreturn]] inline void die(const char* msg){ std::cerr << msg << "\n"; std::exit(1); }

// ---------- physics.cpp ----------
// Characteristic radii and numbers of the Kerr black hole. Radii in rs (= 2M).
struct KerrInfo {
    double a;          // spin parameter a (rs units) = chi * M
    double rPlus;      // event horizon
    double rErgoEq;    // ergosphere at the equator (always 2M)
    double rPhPro, rPhRetro;   // circular photon orbits (prograde / retrograde)
    double rIsco;      // innermost stable circular orbit (prograde)
    double rFluxPeak;  // radius of peak Novikov-Thorne flux
    double fluxMax;    // peak flux (for normalisation)
    double efficiency; // radiative efficiency 1 - E_isco
    double horizonPeriod; // rotation period of the horizon (s), for Sgr A*'s mass
};
double   iscoOverM(double chi);        // prograde ISCO in units of M
KerrInfo kerrInfo(double chi);
void     stepGravity(double realDt);   // N-body step (when gGravity is on)
void     orbitStars();                 // put stars on circular orbits, gravity on

// ---------- render.cpp ----------
void  initRenderer();                  // programs, buffers, render targets
bool  reloadShaders();                 // F5: rebuild every program from shaders/
float effectiveRenderScale();
void  updateRenderSizes();
void  applyWindowSize();               // reallocate everything size-dependent
void  renderScene();                   // ray trace + bloom + grid + composite
int   progressiveSamples();            // effective samples per pixel
float viewAspect();
bool  saveScreenshotBMP(const char* path);

// ---------- main.cpp ----------
void setVideoLook(bool on);
void setFullscreen(bool on);
void resetScene();

// ---------- ui.cpp ----------
void drawUI();
