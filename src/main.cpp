// Real-time Kerr black hole renderer.
// Started as a port of kavan010/black_hole; see README.md for what's changed.
//
// This file: window + input, scene actions, command line, and the main loop.
// See common.hpp for how the other files fit together.
#include "common.hpp"

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include <cctype>
#include <ctime>
#include <filesystem>

// ---------- Scene actions ----------
void resetScene(){
    gObjs = gInitialObjs;
    gCam  = Camera{};
    gGravity = false;
}

// "Video look": reproduce the reference video. Saves the user's settings so
// turning it off puts everything back.
struct SavedLook { double spin; bool realDisk, stars, bloom, relCam; int grid; };
static SavedLook gSavedLook;

void setVideoLook(bool on){
    if (on == gVideoLook) return;
    if (on) {
        gSavedLook = { gSpin, gRealDisk, gShowStars, gBloom, gRelCamera, gGridMode };
        gSpin = 0.0;            // the video's black hole doesn't spin
        gRelCamera = false;     // the original uses plain coordinate directions
        gRealDisk = false;      // flat disk, no Doppler
        gShowStars = false;
        gBloom = false;
        gGridMode = 2;          // original flat overlay grid
        // camera: ~18 rs out, ~26 degrees above the disk, red star up-right
        gCam.radius    = 2.35e11f;
        gCam.elevation = float(M_PI) * 0.5f - 0.45f;
        gCam.azimuth   = -float(M_PI) * 0.5f - 0.35f;
    } else {
        gSpin = gSavedLook.spin; gRealDisk = gSavedLook.realDisk;
        gShowStars = gSavedLook.stars; gBloom = gSavedLook.bloom; gGridMode = gSavedLook.grid;
        gRelCamera = gSavedLook.relCam;
    }
    gVideoLook = on;
}

// ---------- Window size / fullscreen ----------
static void framebufferSizeCB(GLFWwindow*, int w, int h){
    gWinW = w; gWinH = h;          // 0 x 0 while minimised; the loop pauses then
    gResized = true;
}

// F11: borderless fullscreen on the current monitor, and back
static int gWindowedX = 100, gWindowedY = 100, gWindowedW = 1280, gWindowedH = 720;
void setFullscreen(bool on){
    if (on == gFullscreen) return;
    if (on) {
        glfwGetWindowPos(gWin, &gWindowedX, &gWindowedY);
        glfwGetWindowSize(gWin, &gWindowedW, &gWindowedH);
        GLFWmonitor* mon = glfwGetPrimaryMonitor();
        const GLFWvidmode* mode = glfwGetVideoMode(mon);
        glfwSetWindowMonitor(gWin, mon, 0, 0, mode->width, mode->height, mode->refreshRate);
    } else {
        glfwSetWindowMonitor(gWin, nullptr, gWindowedX, gWindowedY, gWindowedW, gWindowedH, 0);
    }
    glfwSwapInterval(1);           // keep vsync after the mode switch
    gFullscreen = on;
}

// ---------- Input ----------
// True when the mouse/keyboard is being used by the UI panel, so the scene
// shouldn't react (e.g. dragging a slider must not orbit the camera).
static bool uiWantsMouse(){ return ImGui::GetCurrentContext() && ImGui::GetIO().WantCaptureMouse; }
static bool uiWantsKeys(){  return ImGui::GetCurrentContext() && ImGui::GetIO().WantCaptureKeyboard; }

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
    // presses over the panel belong to the UI; releases always end a drag
    if(action==GLFW_PRESS && uiWantsMouse()) return;
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
    if(uiWantsMouse()) return;
    gCam.radius -= float(yoff) * float(gCam.zoomSpeed);
    // stay outside the ergosphere (1 rs ~ 1.27e10 m), where a static camera can exist
    gCam.radius = std::clamp(gCam.radius, 2e10f, 1e12f);
    gCam.moving = true;
}
static void keyCB(GLFWwindow* win, int key, int, int action, int){
    if(uiWantsKeys()) return;             // typing into a UI field
    if(action==GLFW_PRESS && key==GLFW_KEY_ESCAPE) glfwSetWindowShouldClose(win,1);
    if(action==GLFW_PRESS && key==GLFW_KEY_H) gShowUI = !gShowUI;
    if(action==GLFW_PRESS && key==GLFW_KEY_V) setVideoLook(!gVideoLook);
    if(action==GLFW_PRESS && key==GLFW_KEY_F11) setFullscreen(!gFullscreen);
    if(action==GLFW_PRESS && key==GLFW_KEY_F5) reloadShaders();
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

// ---------- Init ----------
static void initGL(){
    if(!glfwInit()) die("GLFW init failed");
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR,4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR,3);
    glfwWindowHint(GLFW_OPENGL_PROFILE,GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

    gWin = glfwCreateWindow(gWinW, gWinH, "Black Hole (compute)", nullptr, nullptr);
    if(!gWin){ die("GLFW window failed (needs OpenGL 4.3)"); }
    glfwMakeContextCurrent(gWin);
    glfwSwapInterval(1);
    // work in framebuffer pixels (can differ from window size with display scaling)
    glfwGetFramebufferSize(gWin, &gWinW, &gWinH);
    updateRenderSizes();

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
    glfwSetFramebufferSizeCallback(gWin, framebufferSizeCB);
    glfwSetKeyCallback(gWin, keyCB);

    // Dear ImGui. Installed after our callbacks so it chains to them.
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;     // don't write imgui.ini next to the exe
    ImGui::StyleColorsDark();
    ImGui::GetStyle().WindowRounding = 6.0f;
    ImGui_ImplGlfw_InitForOpenGL(gWin, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    initRenderer();
}

// --physics: print the Kerr numbers for a few spins
static void printPhysicsTable(){
    std::printf("%-6s %-8s %-8s %-8s %-8s %-8s %-8s %-7s %s\n", "a/M", "horizon", "ergo",
                "ph-pro", "ph-ret", "ISCO", "F-peak", "eff%", "horizon period");
    for (double chi : {0.0, 0.5, 0.9, 0.99}) {
        KerrInfo K = kerrInfo(chi);
        std::printf("%-6.2f %-8.3f %-8.3f %-8.3f %-8.3f %-8.3f %-8.3f %-7.1f %.0f s\n", chi, K.rPlus,
                    K.rErgoEq, K.rPhPro, K.rPhRetro, K.rIsco, K.rFluxPeak, 100.0*K.efficiency,
                    K.horizonPeriod);
    }
}

int main(int argc, char** argv){
    {   // folder of the executable, for finding shaders/
        std::error_code ec;
        auto exe = std::filesystem::absolute(argv[0], ec);
        if (!ec) gExeDir = exe.parent_path().string();
    }

    // Command line (see README). --screenshot renders N frames (default 20),
    // saves the last one, and exits.
    const char* autoShot = nullptr;
    int autoFrames = 20;
    bool shotWithUI = false;     // screenshots normally leave the panel out
    bool startOrbit = false;     // --orbit: start with the stars on circular orbits
    bool startVideo = false;     // --video-look
    bool startFull  = false;     // --fullscreen
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
        else if(a == "--no-ui")      gShowUI = false;
        else if(a == "--orbit")      startOrbit = true;
        else if(a == "--video-look") startVideo = true;
        else if(a == "--fullscreen") startFull = true;
        else if(a == "--coord-camera") gRelCamera = false;
        else if(a == "--physics")    { printPhysicsTable(); return 0; }
        else if(a == "--size" && i+1 < argc) {           // e.g. --size 1600x900
            int w = 0, h = 0;
            if (std::sscanf(argv[++i], "%dx%d", &w, &h) == 2 && w >= 64 && h >= 64) { gWinW = w; gWinH = h; }
        }
        else if(a == "--shot-ui")    shotWithUI = true;
        else if(a == "--grid" && i+1 < argc) {
            std::string m = argv[++i];
            gGridMode = (m == "off") ? 0 : (m == "overlay" ? 2 : 1);
        }
    }

    initGL();
    gInitialObjs = gObjs;
    if (startOrbit) orbitStars();
    if (startVideo) setVideoLook(true);
    if (startFull)  setFullscreen(true);
    int frameCount = 0;

    double lastMoveT = glfwGetTime();
    double lastFrameT = glfwGetTime();
    while(!glfwWindowShouldClose(gWin)){
        // minimised: nothing to draw, wait for the window to come back
        if (gWinW == 0 || gWinH == 0) { glfwWaitEvents(); lastFrameT = glfwGetTime(); continue; }
        if (gResized) { applyWindowSize(); gResized = false; }

        // background: black, or the video's dark slate (~45,45,50)
        if (gVideoLook) glClearColor(0.18f, 0.18f, 0.20f, 1.0f);
        else            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        double frameT = glfwGetTime();
        double dt = frameT - lastFrameT;
        lastFrameT = frameT;

        // FPS in the title bar, updated twice a second
        static int fpsFrames = 0; static double fpsT0 = frameT;
        ++fpsFrames;
        if (frameT - fpsT0 >= 0.5) {
            char title[128];
            std::snprintf(title, sizeof(title), "Black Hole  |  %.0f FPS  |  %d spp  |  spin a/M = %.2f  |  gravity %s",
                          fpsFrames / (frameT - fpsT0), progressiveSamples(), gSpin, gGravity ? "ON" : "off");
            glfwSetWindowTitle(gWin, title);
            fpsFrames = 0; fpsT0 = frameT;
        }

        // UI first, so slider changes apply to this frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        drawUI();
        ImGui::Render();

        stepGravity(dt);     // physics
        renderScene();       // ray trace, bloom, grid, composite

        // decay "moving" flag after a short idle
        double now = glfwGetTime();
        if (gCam.dragging) lastMoveT = now;
        if ((now - lastMoveT) > 0.15) gCam.moving = false;

        // screenshots: P key / button, or automatic with --screenshot.
        // Taken before the panel is drawn, unless --shot-ui.
        ++frameCount;
        auto drawPanel = [](){ ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData()); };
        if (shotWithUI) drawPanel();
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
        if (!shotWithUI) drawPanel();

        glfwSwapBuffers(gWin);
        glfwPollEvents();
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(gWin);
    glfwTerminate();
    return 0;
}
