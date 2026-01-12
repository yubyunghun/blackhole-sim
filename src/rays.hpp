#pragma once
#include <vector>

struct Ray {
    float x{}, y{};
    float dx{1.f}, dy{0.f};

    float r{}, phi{};
    float dr{}, dphi{};

    bool  alive{true};
    std::vector<float> trail;

    void step_geo(float dt, float c_ndc, float r_s);       // Euler (kept)
    void step_geo_rk4(float dt, float c_ndc, float r_s);   // RK4 (use)
    void step_straight(float dt, float c_ndc, float r_s);  // gravity off
};
