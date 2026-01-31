// Rendering: shader programs, render targets, the ray-tracer dispatch
// (progressive refinement, physics uniforms), bloom, the overlay grid and
// screenshots. The ray tracer itself is shaders/geodesic.comp.
#include "common.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <cstring>

// ---------- GL objects (private to this file) ----------
static GLuint gProgBlit = 0, gProgGrid = 0, gProgComp = 0;
static GLuint gProgBright = 0, gProgBlur = 0, gProgComposite = 0;

static GLuint gVAOQuad = 0, gVBOQuad = 0;
static GLuint gGridVAO = 0, gGridVBO = 0, gGridEBO = 0; static int gGridIndexCount = 0;
static GLuint gUBOCamera = 0, gUBODisk = 0, gUBOObjs = 0;

// Three images the ray tracer can write to; currentTarget() picks one per frame
// and is the single source of truth for its size (camera UBO + dispatch).
static GLuint gTexOut   = 0;       // moving camera: half-res preview
static GLuint gTexAccum = 0;       // still camera: full-res progressive image
static GLuint gTexLow   = 0;       // video look: 200x150, nearest-filtered (pixelated)
static GLuint gTexCur   = 0;       // whichever image is shown this frame
static const int gLowW = 200, gLowH = 150;   // the reference repo's resolution

struct RT { GLuint fbo = 0, tex = 0; int w = 0, h = 0; };
static RT gBloomA1, gBloomA2, gBloomB1, gBloomB2;   // A: half res, B: quarter res

// ---------- Shader loading ----------
// Shaders live in shaders/ next to the executable (CMake copies them there),
// or in src/shaders/ (build_msvc.bat puts main.exe in the repo root).
static bool readShader(const char* name, std::string& out){
    namespace fs = std::filesystem;
    const fs::path exe(gExeDir);
    const fs::path dirs[] = { exe / "shaders", exe / "src" / "shaders",
                              fs::path("shaders"), fs::path("src") / "shaders" };
    for (const auto& d : dirs) {
        std::ifstream in(d / name, std::ios::binary);
        if (in) {
            std::stringstream ss; ss << in.rdbuf(); out = ss.str();
            // Some Windows editors save UTF-8 with a byte-order mark, which GLSL
            // compilers reject before #version. Strip it.
            if (out.size() >= 3 && (unsigned char)out[0] == 0xEF &&
                (unsigned char)out[1] == 0xBB && (unsigned char)out[2] == 0xBF) out.erase(0, 3);
            return true;
        }
    }
    std::cerr << "[Shader] could not find " << name << " (looked in shaders/ and src/shaders/)\n";
    return false;
}

// Compile/link. On failure: print the log and return 0 (the caller decides
// whether that is fatal: at startup it is, for F5 reloads it isn't).
static GLuint compile(GLenum type, const char* file){
    std::string src;
    if (!readShader(file, src)) return 0;
    const char* p = src.c_str();
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &p, nullptr);
    glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLint n = 0; glGetShaderiv(s, GL_INFO_LOG_LENGTH, &n);
        std::vector<char> log(std::max(n, 1)); glGetShaderInfoLog(s, n, nullptr, log.data());
        std::cerr << "[Shader compile error] " << file << "\n" << log.data() << "\n";
        glDeleteShader(s);
        return 0;
    }
    return s;
}
static GLuint linkProgram(GLuint a, GLuint b = 0){
    if (!a) { if (b) glDeleteShader(b); return 0; }
    GLuint p = glCreateProgram(); glAttachShader(p, a); if (b) glAttachShader(p, b);
    glLinkProgram(p); glDetachShader(p, a); if (b) glDetachShader(p, b);
    glDeleteShader(a); if (b) glDeleteShader(b);
    GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLint n = 0; glGetProgramiv(p, GL_INFO_LOG_LENGTH, &n);
        std::vector<char> log(std::max(n, 1)); glGetProgramInfoLog(p, n, nullptr, log.data());
        std::cerr << "[Program link error]\n" << log.data() << "\n";
        glDeleteProgram(p);
        return 0;
    }
    return p;
}
static GLuint makeProg(const char* vs, const char* fs){
    GLuint v = compile(GL_VERTEX_SHADER, vs);
    GLuint f = compile(GL_FRAGMENT_SHADER, fs);
    if (!v || !f) { if (v) glDeleteShader(v); if (f) glDeleteShader(f); return 0; }
    return linkProgram(v, f);
}
static GLuint makeComp(const char* cs){ return linkProgram(compile(GL_COMPUTE_SHADER, cs)); }

// Build every program. Only swaps them in if all succeed, so a typo during
// an F5 reload leaves the running programs untouched.
static bool buildPrograms(){
    GLuint p[6] = {
        makeProg("blit.vert", "blit.frag"),
        makeProg("grid.vert", "grid.frag"),
        makeComp("geodesic.comp"),
        makeProg("blit.vert", "bright.frag"),
        makeProg("blit.vert", "blur.frag"),
        makeProg("blit.vert", "composite.frag"),
    };
    bool ok = true;
    for (GLuint x : p) ok = ok && x != 0;
    if (!ok) { for (GLuint x : p) if (x) glDeleteProgram(x); return false; }
    GLuint* slots[6] = { &gProgBlit, &gProgGrid, &gProgComp, &gProgBright, &gProgBlur, &gProgComposite };
    for (int i = 0; i < 6; ++i) { if (*slots[i]) glDeleteProgram(*slots[i]); *slots[i] = p[i]; }
    return true;
}

bool reloadShaders(){
    bool ok = buildPrograms();
    std::cout << (ok ? "[INFO] Shaders reloaded" : "[INFO] Shader reload failed; keeping the old ones")
              << std::endl;
    return ok;
}

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
}

// The reference's flat overlay grid (grid mode 2), warped on the CPU
static void generateGrid(){
    // the video's grid runs off the edges of the screen, so make it bigger there
    const int gridSize = gVideoLook ? 36 : 26;
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

// ---------- Resolution ----------
float effectiveRenderScale(){
    if (!gAutoScale) return gRenderScale;
    double px = double(gWinW) * double(gWinH);
    return float(std::min(1.0, std::sqrt(AUTO_MAX_PIXELS / std::max(px, 1.0))));
}
void updateRenderSizes(){
    float s = effectiveRenderScale();
    gStillW  = std::max(16, int(gWinW * s + 0.5f));
    gStillH  = std::max(16, int(gWinH * s + 0.5f));
    gMovingW = std::max(16, gStillW / 2);
    gMovingH = std::max(16, gStillH / 2);
}

// Horizontal/vertical view aspect. The reference video shows a 4:3 render
// stretched to a 16:9 window (everything 4/3 wider), so the video look
// squeezes the field of view horizontally by that factor.
float viewAspect(){
    float a = float(gWinW) / float(gWinH);
    return gVideoLook ? a / ((16.0f/9.0f) / (4.0f/3.0f)) : a;
}

struct Target { GLuint tex; int w, h; bool progressive; };
static Target currentTarget(){
    if (gVideoLook) return { gTexLow, gLowW, gLowH, false };
    // Stars moving under gravity would leave trails in the running average,
    // so treat that like a moving camera (fresh low-res image every frame).
    if (gCam.moving || (gGravity && gObjs.size() > 1)) return { gTexOut, gMovingW, gMovingH, false };
    return { gTexAccum, gStillW, gStillH, true };
}

// ---------- UBO upload ----------
struct CamUBO {
    vec4 camPos, camRight, camUp, camFwd;
    vec4 params;    // x=tanHalfFov, y=aspect, z=moving(0/1), w=flags
    ivec4 ires;     // x=WIDTH, y=HEIGHT, z=grid mode, w unused
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
    float flags = float((gRealDisk ? 1 : 0) | (gShowStars ? 2 : 0) | (gVideoLook ? 4 : 0));
    data.params   = vec4(tan(radians(60.0f*0.5f)), viewAspect(), gCam.moving?1.0f:0.0f, flags);

    Target t = currentTarget();
    data.ires = ivec4(t.w, t.h, gGridMode, 0);   // z = grid mode (1 = ray traced)

    glBindBuffer(GL_UNIFORM_BUFFER, gUBOCamera);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(CamUBO), &data);
}

struct DiskUBO { vec4 disk; vec4 kerr; }; // (r1, r2, rs, time), (a, r+, -, -)
static void uploadDiskUBO(){
    DiskUBO d{};
    // w = animation time; wrapped so float precision stays good in long sessions
    // (wrap period is a multiple of the 4 s flow cycle, so it's seamless for the flow)
    double now = (gTimeOverride >= 0.0) ? gTimeOverride : glfwGetTime();
    float t = float(std::fmod(now, 4000.0));
    // In rs units M = 0.5. Spin parameter a = chi*M, horizon r+ = M + sqrt(M^2 - a^2).
    const double M = 0.5;
    double a     = gSpin * M;
    double rPlus = M + std::sqrt(M*M - a*a);
    // Inner edge: the relativistic (Novikov-Thorne) disk starts exactly at the
    // ISCO. The reference/video looks keep the original 2.2 rs (or the ISCO if
    // spin pulls it closer).
    double isco = iscoOverM(gSpin) * M;
    double r1 = (gRealDisk && !gVideoLook) ? isco : std::min(2.2, isco);
    d.disk = vec4(float(SAG_RS*r1), float(SAG_RS*gDiskOuter), float(SAG_RS), t);
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
    // Only the stars go to the ray tracer. The black hole itself (pinned) is
    // handled by the horizon test; drawing it as a sphere of radius rs would
    // wrongly cover part of the shadow for a spinning hole, whose horizon is
    // smaller than rs.
    vector<const ObjectData*> drawn;
    for (const auto& o : gObjs) if (!o.pinned && drawn.size() < 16) drawn.push_back(&o);
    ObjsUBO u{}; u.num = int(drawn.size());
    for(int i=0;i<u.num;++i){
        u.posR[i] = drawn[i]->posRadius;
        u.color[i]= drawn[i]->color;
    }
    // pack masses into mass4[4]
    float temp[16] = {};
    for(int i=0;i<u.num;++i) temp[i] = drawn[i]->mass;
    for(int g=0; g<4; ++g){
        u.mass4[g] = vec4( temp[g*4+0], temp[g*4+1], temp[g*4+2], temp[g*4+3] );
    }
    glBindBuffer(GL_UNIFORM_BUFFER, gUBOObjs);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(ObjsUBO), &u);
}

// ---------- Progressive rendering ----------
// Moving camera: trace every pixel of a low-res image each frame (gTexOut).
// Still camera:  full-res image (gTexAccum), but each frame traces only one
// pixel of every 2x2 block, cycling through the four. Every visit uses a new
// sub-pixel jitter and is blended into a running average -> anti-aliasing.
// The average keeps a minimum weight for new samples (PROG_BLEND_MIN) so
// animated gas and moving stars stay sharp instead of smearing.
static const int   PROG_STRIDE    = 2;
static const float PROG_BLEND_MIN = 0.2f;   // ~5-sample moving average at most
static int  gPassIndex = 0;        // still-camera passes since the last reset
static bool gAccumReset = true;

// (Re)allocate the window-sized images at the current render sizes.
static void resizeRenderTargets(){
    glBindTexture(GL_TEXTURE_2D, gTexOut);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, gMovingW, gMovingH, 0, GL_RGBA, GL_FLOAT, nullptr);
    glBindTexture(GL_TEXTURE_2D, gTexAccum);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, gStillW, gStillH, 0, GL_RGBA, GL_FLOAT, nullptr);
    gAccumReset = true;                      // old samples don't fit the new size
}

static void initRenderTargets(){
    glGenTextures(1, &gTexOut);
    glBindTexture(GL_TEXTURE_2D, gTexOut);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glGenTextures(1, &gTexAccum);
    glBindTexture(GL_TEXTURE_2D, gTexAccum);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    resizeRenderTargets();
    glGenTextures(1, &gTexLow);
    glBindTexture(GL_TEXTURE_2D, gTexLow);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, gLowW, gLowH, 0, GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);   // blocky, like the video
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    gTexCur = gTexAccum;
}

// Anything that changes the picture (other than the animated gas, which the
// moving average handles) restarts the accumulation.
static bool sceneChanged(){
    static double last[16] = {};
    double now[16] = { gCam.radius, gCam.azimuth, gCam.elevation, gSpin,
                       double(gRealDisk), double(gShowStars), double(gGridMode),
                       double(gObjs.size()), double(gWinW), double(gWinH),
                       double(gStillW), gDiskTemp, gDiskGain, gGasSpeed, gDiskOuter,
                       double(gRelCamera) };
    bool changed = std::memcmp(last, now, sizeof(now)) != 0;
    std::memcpy(last, now, sizeof(now));
    return changed;
}

static float halton(int i, int b){
    float f = 1.0f, r = 0.0f;
    while (i > 0) { f /= float(b); r += f * float(i % b); i /= b; }
    return r;
}

// Effective samples per pixel (for the title bar). Once the blend weight hits
// its floor w, the running average behaves like ~(2 - w) / w samples.
int progressiveSamples(){
    if (!currentTarget().progressive) return 1;
    int n = std::max(1, gPassIndex / (PROG_STRIDE*PROG_STRIDE));
    int cap = int((2.0f - PROG_BLEND_MIN) / PROG_BLEND_MIN + 0.5f);
    return std::min(n, cap);
}

// ---------- Physics uniforms: Novikov-Thorne disk + relativistic camera ----------
// Camera tetrad in local (ingoing) Kerr-Schild coords (t,X,Y,Z) for the
// black hole's spin a. Built by Gram-Schmidt under g = eta + f l l, l = (1, k).
// Embedding-diagram table for the ray-traced grid (see geodesic.comp),
// rebuilt only when the spin changes.
static void uploadGridTable(GLuint P){
    static double cachedSpin = -1.0;
    static float H[128];
    static float dMin = 1.0f, sMax = 1.0f, hMax = 0.0f;
    if (gSpin != cachedSpin) {
        cachedSpin = gSpin;
        const double M = 0.5, a = gSpin * M;
        double rPlus = M + std::sqrt(M*M - a*a);
        double d0 = std::sqrt(rPlus*rPlus + a*a);            // horizon, Kerr-Schild cylindrical radius
        double dMax = 13.0 * 1e10 / SAG_RS * 1.4143;          // grid corners (GRID_HALF cells)
        double s1 = std::sqrt(dMax - d0);
        for (int i = 0; i < 128; ++i) {
            double s = s1 * i / 127.0;
            double d = d0 + s*s;
            double r = std::sqrt(std::max(d*d - a*a, rPlus*rPlus));   // Boyer-Lindquist r
            H[i] = float(kerrEmbeddingHeight(r, gSpin));
        }
        dMin = float(d0); sMax = float(s1); hMax = H[127];
    }
    glUniform1fv(glGetUniformLocation(P, "uGridH"), 128, H);
    glUniform3f(glGetUniformLocation(P, "uGridP"), dMin, sMax, hMax);
}

static void setPhysicsUniforms(){
    GLuint P = gProgComp;
    uploadGridTable(P);
    KerrInfo K = kerrInfo(gSpin);
    glUniform4f(glGetUniformLocation(P, "uNT"), float(gSpin),
                float(std::sqrt(K.rIsco / 0.5)), float(1.0 / std::max(K.fluxMax, 1e-30)), 0.0f);

    // camera basis in world space (same as uploadCameraUBO)
    dvec3 pos = dvec3(gCam.position()) / SAG_RS;
    dvec3 fwd = normalize(dvec3(gCam.target) / SAG_RS - pos);
    dvec3 right = normalize(cross(fwd, dvec3(0,1,0)));
    dvec3 up = cross(right, fwd);
    auto toLocal = [](dvec3 w){ return dvec3(w.z, w.x, w.y); };   // local = world.zxy

    // Camera tetrad (physics.cpp): a static observer, or the falling camera's
    // own 4-velocity during a dive (which can be inside the horizon)
    dvec4 uFall;
    if (gFall.active) uFall = fallVelocity();
    CameraTetrad T = cameraTetrad(toLocal(pos), toLocal(fwd), toLocal(right), toLocal(up), gSpin * 0.5,
                                  gFall.active ? &uFall : nullptr);
    bool rel = (gRelCamera || gFall.active) && T.ok;
    glUniform1i(glGetUniformLocation(P, "uRelCamera"), rel ? 1 : 0);
    if (!T.ok) return;

    auto set4 = [&](const char* n, dvec4 v){
        glUniform4f(glGetUniformLocation(P, n), float(v.x), float(v.y), float(v.z), float(v.w)); };
    set4("uCamU", T.u); set4("uCamEF", T.eF); set4("uCamER", T.eR); set4("uCamEU", T.eU);
    glUniform1f(glGetUniformLocation(P, "uCamF"), float(T.f));
    glUniform3f(glGetUniformLocation(P, "uCamK"), float(T.k.x), float(T.k.y), float(T.k.z));
}

// ---------- Compute dispatch ----------
static void dispatchCompute(){
    glUseProgram(gProgComp);
    glBindBufferBase(GL_UNIFORM_BUFFER, 1, gUBOCamera);
    glBindBufferBase(GL_UNIFORM_BUFFER, 2, gUBODisk);
    glBindBufferBase(GL_UNIFORM_BUFFER, 3, gUBOObjs);

    int stride = 1, offX = 0, offY = 0, fill = 0;
    float blend = 1.0f, jx = 0.0f, jy = 0.0f;
    bool changed = sceneChanged();
    Target t = currentTarget();
    GLuint target = t.tex;
    int w = t.w, h = t.h;
    if (!t.progressive) {
        gAccumReset = true;                    // start fresh once we go back to still
    } else {
        if (changed || gAccumReset) { gPassIndex = 0; gAccumReset = false; }
        const int cells = PROG_STRIDE * PROG_STRIDE;
        static const int order[4][2] = { {0,0}, {1,1}, {1,0}, {0,1} };  // spread out
        int k = gPassIndex % cells;
        int n = gPassIndex / cells;            // samples this pixel already has
        stride = PROG_STRIDE;
        offX = order[k][0]; offY = order[k][1];
        blend = (n == 0) ? 1.0f : std::max(1.0f / float(n + 1), PROG_BLEND_MIN);
        if (n > 0) { jx = halton(n, 2) - 0.5f; jy = halton(n, 3) - 0.5f; }
        fill = (gPassIndex == 0) ? 1 : 0;      // first pass: no holes
        ++gPassIndex;
    }

    glUniform1i(glGetUniformLocation(gProgComp, "uStride"), stride);
    glUniform2i(glGetUniformLocation(gProgComp, "uOffset"), offX, offY);
    glUniform2f(glGetUniformLocation(gProgComp, "uJitter"), jx, jy);
    glUniform1f(glGetUniformLocation(gProgComp, "uBlend"), blend);
    glUniform1i(glGetUniformLocation(gProgComp, "uFillBlock"), fill);
    glUniform1f(glGetUniformLocation(gProgComp, "uDiskTemp"), gDiskTemp);
    glUniform1f(glGetUniformLocation(gProgComp, "uDiskGain"), gDiskGain);
    glUniform1f(glGetUniformLocation(gProgComp, "uGasSpeed"), gGasSpeed);
    setPhysicsUniforms();

    glBindImageTexture(0, target, 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA16F);

    int tw = (w + stride - 1) / stride, th = (h + stride - 1) / stride;
    glDispatchCompute(GLuint((tw + 15) / 16), GLuint((th + 15) / 16), 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
    gTexCur = target;
}

// ---------- Auto exposure (dives only) ----------
// A diving camera sees the disk blueshifted by orders of magnitude, with huge
// contrast, so expose for the highlights: read a coarse (~30x16) level of the
// HDR image's mip chain (local averages) and scale so the brightest region
// lands just below white. Adapts smoothly, like an eye or a camera.
static void updateAutoExposure(double dt){
    if (!gFall.active || !gRealDisk) {           // normal viewing: fixed exposure
        gAutoExposure = 1.0f;
        return;
    }
    glBindTexture(GL_TEXTURE_2D, gTexCur);
    glGenerateMipmap(GL_TEXTURE_2D);
    GLint w = 0, h = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH,  &w);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
    int top = int(std::floor(std::log2(double(std::max(w, h)))));
    int level = std::max(top - 5, 0);                            // ~32 texels across
    GLint lw = 0, lh = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, level, GL_TEXTURE_WIDTH,  &lw);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, level, GL_TEXTURE_HEIGHT, &lh);
    std::vector<float> px(size_t(std::max(lw, 1)) * std::max(lh, 1) * 4);
    glGetTexImage(GL_TEXTURE_2D, level, GL_RGBA, GL_FLOAT, px.data());
    float lum = 0.0f;                                            // brightest local average
    for (size_t i = 0; i + 3 < px.size(); i += 4)
        lum = std::max(lum, 0.2126f*px[i] + 0.7152f*px[i+1] + 0.0722f*px[i+2]);
    // ACES handles highlights up to ~6 gracefully, so only start reducing the
    // exposure beyond that (normal views keep their usual look)
    float target = std::clamp(6.0f / std::max(lum, 1e-6f), 1e-5f, 1.0f);
    float k = float(1.0 - std::exp(-dt * 3.0));                 // ~1/3 s adaptation
    gAutoExposure += (target - gAutoExposure) * k;
    if (std::getenv("BH_DEBUG_EXPOSURE"))
        std::fprintf(stderr, "[exposure] r=%.3f max local lum=%g level=%d (%dx%d) exp=%g\n",
                     fallRadius(), lum, level, lw, lh, gAutoExposure);
}

// ---------- Post-processing: bloom + tone mapping ----------
// HDR image -> bright pass (half res) -> separable Gaussian blur at two
// sizes (half and quarter res) -> composite: tonemap(hdr + bloom), blended
// over the grid. Bloom has alpha 0, so its glow adds on top of grid/stars.
static void makeRT(RT& rt, int w, int h){
    w = std::max(w, 1); h = std::max(h, 1);
    rt.w = w; rt.h = h;
    if (!rt.tex) glGenTextures(1, &rt.tex);       // reused on resize
    glBindTexture(GL_TEXTURE_2D, rt.tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (!rt.fbo) glGenFramebuffers(1, &rt.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, rt.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt.tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) die("bloom FBO incomplete");
    glClearColor(0,0,0,0);
    glClear(GL_COLOR_BUFFER_BIT);   // start black, not undefined (avoids NaN when bloom is off)
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}
// bloom buffers are half / quarter of the ray-traced (still) resolution
static void resizeBloom(){
    makeRT(gBloomA1, gStillW/2, gStillH/2); makeRT(gBloomA2, gStillW/2, gStillH/2);
    makeRT(gBloomB1, gStillW/4, gStillH/4); makeRT(gBloomB2, gStillW/4, gStillH/4);
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
    pass(gProgBright, gTexCur, gBloomA1);    // bright pass, downsample to 1/2
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
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, gTexCur);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, gBloomA1.tex);
    glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, gBloomB1.tex);
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(glGetUniformLocation(gProgComposite,"uHDR"), 0);
    glUniform1i(glGetUniformLocation(gProgComposite,"uBloomA"), 1);
    glUniform1i(glGetUniformLocation(gProgComposite,"uBloomB"), 2);
    glUniform1f(glGetUniformLocation(gProgComposite,"uBloom"), gBloom ? gBloomStrength : 0.0f);
    glUniform1i(glGetUniformLocation(gProgComposite,"uTonemap"), gRealDisk ? 1 : 0);
    glUniform1f(glGetUniformLocation(gProgComposite,"uExposure"), gExposure * gAutoExposure);
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
    // grey like the reference code, or the pinkish grey (188,162,168) seen in the video
    if (gVideoLook) glUniform4f(glGetUniformLocation(gProgGrid,"uColor"), 0.74f, 0.64f, 0.66f, 0.85f);
    else            glUniform4f(glGetUniformLocation(gProgGrid,"uColor"), 0.5f, 0.5f, 0.5f, 0.7f);
    glBindVertexArray(gGridVAO);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDrawElements(GL_LINES, gGridIndexCount, GL_UNSIGNED_INT, 0);
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    glBindVertexArray(0);
}

// ---------- Public entry points ----------
void initRenderer(){
    if (!buildPrograms()) die("could not build the shader programs (see errors above)");
    createQuad();
    initRenderTargets();
    resizeBloom();
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

// Recompute render sizes and reallocate every size-dependent image.
void applyWindowSize(){
    updateRenderSizes();
    resizeRenderTargets();
    resizeBloom();
    glViewport(0, 0, gWinW, gWinH);
}

// One frame of the scene (everything except the UI panel)
void renderScene(){
    if (gGridMode == 2) generateGrid();   // overlay grid follows moving stars

    uploadCameraUBO();
    uploadDiskUBO();
    uploadObjsUBO();

    dispatchCompute();                    // ray trace
    static double lastT = glfwGetTime();
    double now = glfwGetTime();
    // (held dives, e.g. --dive-to, adapt instantly so the first frame is right)
    updateAutoExposure(gFall.paused ? 10.0 : now - lastT);
    lastT = now;
    runBloom();

    // overlay grid (mode 2): 60 deg FOV to match the ray tracer, so they line up
    if (gGridMode == 2) {
        mat4 V = lookAt(gCam.position(), gCam.target, vec3(0,1,0));
        mat4 P = perspective(radians(60.0f), viewAspect(), 1e9f, 1e14f);
        drawGrid(P*V);
    }
    drawQuad();                           // composite over the grid
}

// ---------- Screenshot (24-bit BMP, no dependencies) ----------
bool saveScreenshotBMP(const char* path){
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
    std::cout << "[INFO] Saved " << path << std::endl;
    return true;
}
