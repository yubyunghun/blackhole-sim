#include "rays.hpp"
#include <cmath>

static inline void cart_from_polar(float r, float phi, float& x, float& y){
    x = r * std::cos(phi);
    y = r * std::sin(phi);
}

// φ¨ = (2/r) ṙ φ̇
// r¨ = c^2 * r_s / (2 r^2) + r * φ̇^2
static inline void geo_accels(float r, float dr, float dphi, float c, float r_s,
                              float& dd_r, float& dd_phi)
{
    if (r < 1e-6f) r = 1e-6f;
    const float rinv  = 1.0f / r;
    const float rinv2 = rinv * rinv;
    dd_phi = 2.0f * rinv * dr * dphi;
    dd_r   = (c*c) * (r_s * 0.5f) * rinv2 + r * (dphi*dphi);
}

void Ray::step_geo(float dt, float c_ndc, float r_s){
    if(!alive) return;
    if(r <= r_s){ alive=false; return; }

    float dd_r=0.f, dd_phi=0.f;
    geo_accels(r, dr, dphi, c_ndc, r_s, dd_r, dd_phi);

    dr   += dd_r   * dt;
    dphi += dd_phi * dt;
    r    += dr     * dt;
    phi  += dphi   * dt;

    cart_from_polar(r, phi, x, y);
    trail.push_back(x); trail.push_back(y);
    if(trail.size() > 2*2000) trail.erase(trail.begin(), trail.begin()+2);
}

void Ray::step_geo_rk4(float dt, float c_ndc, float r_s){
    if(!alive) return;
    if(r <= r_s){ alive=false; return; }

    auto deriv = [&](float R, float P, float dR, float dP,
                     float& oR, float& oP, float& odR, float& odP){
        float dd_r=0.f, dd_phi=0.f;
        geo_accels(R, dR, dP, c_ndc, r_s, dd_r, dd_phi);
        oR  = dR;
        oP  = dP;
        odR = dd_r;
        odP = dd_phi;
    };

    float k1_r,k1_p,k1_dr,k1_dp;
    deriv(r, phi, dr, dphi, k1_r, k1_p, k1_dr, k1_dp);

    float r2=r+0.5f*dt*k1_r, p2=phi+0.5f*dt*k1_p;
    float dr2=dr+0.5f*dt*k1_dr, dp2=dphi+0.5f*dt*k1_dp;
    float k2_r,k2_p,k2_dr,k2_dp;
    deriv(r2, p2, dr2, dp2, k2_r, k2_p, k2_dr, k2_dp);

    float r3=r+0.5f*dt*k2_r, p3=phi+0.5f*dt*k2_p;
    float dr3=dr+0.5f*dt*k2_dr, dp3=dphi+0.5f*dt*k2_dp;
    float k3_r,k3_p,k3_dr,k3_dp;
    deriv(r3, p3, dr3, dp3, k3_r, k3_p, k3_dr, k3_dp);

    float r4=r+dt*k3_r, p4=phi+dt*k3_p;
    float dr4=dr+dt*k3_dr, dp4=dphi+dt*k3_dp;
    float k4_r,k4_p,k4_dr,k4_dp;
    deriv(r4, p4, dr4, dp4, k4_r, k4_p, k4_dr, k4_dp);

    r   += (dt/6.0f) * (k1_r  + 2.0f*k2_r  + 2.0f*k3_r  + k4_r);
    phi += (dt/6.0f) * (k1_p  + 2.0f*k2_p  + 2.0f*k3_p  + k4_p);
    dr  += (dt/6.0f) * (k1_dr + 2.0f*k2_dr + 2.0f*k3_dr + k4_dr);
    dphi+= (dt/6.0f) * (k1_dp + 2.0f*k2_dp + 2.0f*k3_dp + k4_dp);

    if(r <= r_s){ alive=false; return; }
    cart_from_polar(r, phi, x, y);
    trail.push_back(x); trail.push_back(y);
    if(trail.size() > 2*4000) trail.erase(trail.begin(), trail.begin()+2);
}

void Ray::step_straight(float dt, float c_ndc, float r_s){
    if(!alive) return;

    x += dx * c_ndc * dt;
    y += dy * c_ndc * dt;

    r   = std::hypot(x, y);
    phi = std::atan2(y, x);
    const float cp = std::cos(phi), sp = std::sin(phi);
    dr   = c_ndc * (dx*cp + dy*sp);
    dphi = (r > 1e-6f) ? ( c_ndc*(-dx*sp + dy*cp) / r ) : 0.0f;

    if(r <= r_s){ alive=false; return; }
    trail.push_back(x); trail.push_back(y);
    if(trail.size() > 2*2000) trail.erase(trail.begin(), trail.begin()+2);
}
