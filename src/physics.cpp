// Physics on the CPU: Kerr black hole formulas, the Novikov-Thorne disk flux
// (for normalisation and the panel readout), and the N-body star gravity.
// The light-ray physics itself runs on the GPU in shaders/geodesic.comp.
#include "common.hpp"

// ---------- Kerr helpers ----------
// Prograde innermost stable circular orbit (Bardeen, Press & Teukolsky 1972), units of M
double iscoOverM(double chi){
    double z1 = 1.0 + std::cbrt(1.0 - chi*chi) * (std::cbrt(1.0 + chi) + std::cbrt(1.0 - chi));
    double z2 = std::sqrt(3.0*chi*chi + z1*z1);
    return 3.0 + z2 - std::sqrt((3.0 - z1) * (3.0 + z1 + 2.0*z2));
}

// Novikov-Thorne flux (same formula as the shader, in double; arbitrary units)
static double ntFluxCPU(double rOverM, double aS, double x0){
    aS = std::max(aS, 1e-4);
    double x = std::sqrt(rOverM);
    if (x <= x0) return 0.0;
    double th = std::acos(aS);
    double x1 =  2.0*std::cos((th - M_PI)/3.0);
    double x2 =  2.0*std::cos((th + M_PI)/3.0);
    double x3 = -2.0*std::cos(th/3.0);
    double s = x - x0 - 1.5*aS*std::log(x/x0)
             - 3.0*(x1-aS)*(x1-aS)/(x1*(x1-x2)*(x1-x3))*std::log((x-x1)/(x0-x1))
             - 3.0*(x2-aS)*(x2-aS)/(x2*(x2-x1)*(x2-x3))*std::log((x-x2)/(x0-x2))
             - 3.0*(x3-aS)*(x3-aS)/(x3*(x3-x1)*(x3-x2))*std::log((x-x3)/(x0-x3));
    return std::max(s, 0.0) / (x*x*x*x * (x*x*x - 3.0*x + 2.0*aS));
}

double ntFlux(double rOverM, double chi){
    return ntFluxCPU(rOverM, chi, std::sqrt(iscoOverM(chi)));
}

// ---------- Camera tetrad ----------
double ksDot(const CameraTetrad& T, const dvec4& p, const dvec4& q){
    dvec4 l(1.0, T.k.x, T.k.y, T.k.z);
    double eta = -p.x*q.x + p.y*q.y + p.z*q.z + p.w*q.w;
    return eta + T.f * dot(l, p) * dot(l, q);
}

// Gram-Schmidt of the static observer's frame under g = eta + f l l.
CameraTetrad cameraTetrad(dvec3 x, dvec3 fwd, dvec3 right, dvec3 up, double A){
    const double M = 0.5;
    CameraTetrad T{};
    double a2 = A*A;
    double w = dot(x,x) - a2;
    double r = std::sqrt(0.5*(w + std::sqrt(w*w + 4.0*a2*x.z*x.z)));
    T.f = 2.0*M*r*r*r / (r*r*r*r + a2*x.z*x.z);
    T.k = dvec3((r*x.x + A*x.y)/(r*r + a2), (r*x.y - A*x.x)/(r*r + a2), x.z/r);
    T.ok = T.f < 0.98;      // static observers exist only outside the ergosphere
    if (!T.ok) return T;

    T.u = dvec4(1.0 / std::sqrt(1.0 - T.f), 0, 0, 0);   // g(u,u) = -1
    auto spatial = [&](dvec3 v){ dvec4 V(0.0, v.x, v.y, v.z); return V + ksDot(T, T.u, V) * T.u; };
    auto unit = [&](dvec4 v){ return v / std::sqrt(ksDot(T, v, v)); };
    T.eF = unit(spatial(fwd));
    T.eR = spatial(right); T.eR = unit(T.eR - ksDot(T, T.eR, T.eF) * T.eF);
    T.eU = spatial(up);    T.eU = unit(T.eU - ksDot(T, T.eU, T.eF) * T.eF - ksDot(T, T.eU, T.eR) * T.eR);
    return T;
}

KerrInfo kerrInfo(double chi){
    const double M = 0.5;
    KerrInfo k{};
    k.a        = chi * M;
    k.rPlus    = M + std::sqrt(M*M - k.a*k.a);
    k.rErgoEq  = 2.0 * M;
    k.rPhPro   = 2.0*M * (1.0 + std::cos(2.0/3.0 * std::acos(-chi)));
    k.rPhRetro = 2.0*M * (1.0 + std::cos(2.0/3.0 * std::acos( chi)));
    double riM = iscoOverM(chi);
    k.rIsco    = riM * M;
    k.efficiency = 1.0 - std::sqrt(1.0 - 2.0/(3.0*riM));
    // peak flux: scan outward from the ISCO
    double x0 = std::sqrt(riM), best = 0.0, bestR = riM;
    for (int i = 1; i <= 4000; ++i) {
        double rM = riM * std::pow(40.0, i / 4000.0);
        double F = ntFluxCPU(rM, chi, x0);
        if (F > best) { best = F; bestR = rM; }
    }
    k.fluxMax   = best;
    k.rFluxPeak = bestR * M;
    // horizon angular velocity  Omega_H M = chi / (2 (1 + sqrt(1 - chi^2)))
    // converted to seconds with the time unit GM/c^3 (~21 s for Sgr A*)
    double tM = G_GRAV * SAG_MASS / (C_LIGHT*C_LIGHT*C_LIGHT);
    double omegaHM = chi / (2.0 * (1.0 + std::sqrt(1.0 - chi*chi)));
    k.horizonPeriod = (omegaHM > 0.0) ? 2.0*M_PI * tM / omegaHM : 0.0;
    return k;
}

// ---------- N-body gravity ----------
// Two-pass (compute all accelerations, then integrate) so the result doesn't
// depend on object order, with a real time step so speed doesn't depend on
// frame rate. Semi-implicit Euler with substeps for stability near the hole.
void stepGravity(double realDt){
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

// Give every star the velocity of a circular orbit around the black hole
// (prograde, same direction as the disk) and switch gravity on.
void orbitStars(){
    for (auto& o : gObjs) {
        if (o.pinned) continue;
        vec3 r = vec3(o.posRadius);
        double d = length(r);
        if (d <= 0.0) continue;
        double v = std::sqrt(G_GRAV * SAG_MASS / d);
        vec3 dir = normalize(cross(vec3(0,1,0), r));
        if (!std::isfinite(dir.x)) dir = vec3(1,0,0);   // star on the spin axis
        o.velocity = dir * float(v);
    }
    gGravity = true;
}
