// The on-screen control panel (Dear ImGui).
#include "common.hpp"
#include "imgui.h"

void drawUI(){
    if (!gShowUI) return;
    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.78f);
    ImGui::Begin("Controls", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::PushItemWidth(120.0f);   // keep the panel narrow so it covers less of the view

    int resW = gCam.moving ? gMovingW : gStillW, resH = gCam.moving ? gMovingH : gStillH;
    ImGui::Text("%.0f FPS   %d spp   %dx%d", ImGui::GetIO().Framerate,
                progressiveSamples(), resW, resH);
    ImGui::TextDisabled("H hides this panel");
    bool video = gVideoLook;
    if (ImGui::Checkbox("Video look (V)", &video)) setVideoLook(video);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Match the reference video: flat orange disk,\n200x150 pixels, overlay grid, no spin");

    if (ImGui::CollapsingHeader("Black hole", ImGuiTreeNodeFlags_DefaultOpen)) {
        float spin = float(gSpin);
        if (ImGui::SliderFloat("Spin a/M", &spin, 0.0f, 0.99f, "%.2f")) gSpin = spin;
        ImGui::Checkbox("Relativistic camera", &gRelCamera);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Pixel directions measured in a static observer's\n"
                              "local frame (correct aberration), instead of\n"
                              "plain coordinate directions");
        if (ImGui::TreeNode("Physics")) {
            KerrInfo K = kerrInfo(gSpin);
            ImGui::TextDisabled("radii in rs (Schwarzschild radius)");
            ImGui::Text("Event horizon      %.3f", K.rPlus);
            ImGui::Text("Ergosphere (eq.)   %.3f", K.rErgoEq);
            ImGui::Text("Photon orbit pro   %.3f", K.rPhPro);
            ImGui::Text("Photon orbit retro %.3f", K.rPhRetro);
            ImGui::Text("ISCO (disk edge)   %.3f", K.rIsco);
            ImGui::Text("Peak disk flux     %.3f", K.rFluxPeak);
            ImGui::Text("Radiative eff.     %.1f %%", 100.0 * K.efficiency);
            if (K.horizonPeriod > 0.0)
                ImGui::Text("Horizon spin period %.0f s", K.horizonPeriod);
            else
                ImGui::Text("Horizon spin period  -");
            ImGui::TextDisabled("(period for Sgr A*'s mass, 4.3e6 Msun)");
            ImGui::TreePop();
        }
    }

    if (ImGui::CollapsingHeader("Accretion disk", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("Relativistic (D)", &gRealDisk);
        ImGui::BeginDisabled(!gRealDisk);
        ImGui::SliderFloat("Peak temp K", &gDiskTemp, 2000.0f, 15000.0f, "%.0f");
        ImGui::SliderFloat("Brightness", &gDiskGain, 0.2f, 6.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
        ImGui::SliderFloat("Gas speed", &gGasSpeed, 0.0f, 12.0f, "%.1f");
        ImGui::EndDisabled();
        ImGui::SliderFloat("Outer radius rs", &gDiskOuter, 3.0f, 10.0f, "%.1f");
    }

    if (ImGui::CollapsingHeader("View")) {
        ImGui::Checkbox("Stars (B)", &gShowStars);
        ImGui::SameLine();
        ImGui::Checkbox("Bloom (L)", &gBloom);
        ImGui::Combo("Grid (T)", &gGridMode, kGridModeNames, 3);
        ImGui::TextDisabled("drag: orbit   scroll: zoom");
        ImGui::BeginDisabled(!gBloom);
        ImGui::SliderFloat("Bloom strength", &gBloomStrength, 0.0f, 3.0f, "%.2f");
        ImGui::EndDisabled();
        ImGui::BeginDisabled(!gRealDisk);
        ImGui::SliderFloat("Exposure", &gExposure, 0.2f, 4.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
        ImGui::EndDisabled();
    }

    if (ImGui::CollapsingHeader("Gravity")) {
        ImGui::Checkbox("Gravity on (G)", &gGravity);
        float ts = float(gTimeScale);
        if (ImGui::SliderFloat("Time scale", &ts, 100.0f, 100000.0f, "%.0fx", ImGuiSliderFlags_Logarithmic))
            gTimeScale = ts;
        int stars = 0; for (auto& o : gObjs) if (!o.pinned) ++stars;
        ImGui::TextDisabled("%d star%s left", stars, stars == 1 ? "" : "s");
        if (ImGui::Button("Orbit stars")) orbitStars();
        ImGui::SameLine();
        if (ImGui::Button("Reset scene")) resetScene();
    }

    if (ImGui::CollapsingHeader("Display")) {
        bool fs = gFullscreen;
        if (ImGui::Checkbox("Fullscreen (F11)", &fs)) setFullscreen(fs);
        bool autoS = gAutoScale;
        if (ImGui::Checkbox("Auto resolution", &autoS)) {
            if (!autoS) gRenderScale = effectiveRenderScale();   // no jump when unticking
            gAutoScale = autoS;
            gResized = true;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Keeps the ray-traced image under ~0.5 megapixels");
        ImGui::BeginDisabled(gAutoScale);
        float sc = effectiveRenderScale();
        if (ImGui::SliderFloat("Render scale", &sc, 0.25f, 1.0f, "%.2f")) { gRenderScale = sc; gResized = true; }
        ImGui::EndDisabled();
        ImGui::TextDisabled("window %dx%d  render %dx%d", gWinW, gWinH, gStillW, gStillH);
        if (ImGui::Button("Reload shaders (F5)")) reloadShaders();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Re-read the shader files without restarting.\nIf a shader has an error, the old one keeps running.");
    }

    ImGui::Separator();
    if (ImGui::Button("Screenshot (P)")) gScreenshotRequested = true;

    ImGui::PopItemWidth();
    ImGui::End();
}
