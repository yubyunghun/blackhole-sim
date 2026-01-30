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

// Gram-Schmidt of the observer's frame under g = eta + f l l. The observer is
// static (u along the time direction) unless a 4-velocity is given.
CameraTetrad cameraTetrad(dvec3 x, dvec3 fwd, dvec3 right, dvec3 up, double A, const dvec4* uIn){
    const double M = 0.5;
    CameraTetrad T{};
    double a2 = A*A;
    double w = dot(x,x) - a2;
    double r = std::sqrt(0.5*(w + std::sqrt(w*w + 4.0*a2*x.z*x.z)));
    T.f = 2.0*M*r*r*r / (r*r*r*r + a2*x.z*x.z);
    T.k = dvec3((r*x.x + A*x.y)/(r*r + a2), (r*x.y - A*x.x)/(r*r + a2), x.z/r);
    if (uIn) {
        T.ok = true;            // a moving (e.g. falling) observer can be anywhere
        T.u = *uIn;
    } else {
        T.ok = T.f < 0.98;      // static observers exist only outside the ergosphere
        if (!T.ok) return T;
        T.u = dvec4(1.0 / std::sqrt(1.0 - T.f), 0, 0, 0);   // g(u,u) = -1
    }
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

// ---------- Geodesics on the CPU (double precision) ----------
// Same Kerr-Schild Hamiltonian as the shader, for a particle of energy E
// (E = 1 for a photon normalised to unit energy):
//   H = 1/2 (-E^2 + |p|^2 - f S^2),  S = E + k.p
//   dx/dtau = p - f S k,   dt/dtau = E + f S,
//   dp/dtau = 1/2 (S^2 grad f + 2 f S grad S)
// H = 0 for light, -1/2 for a massive particle (tau = proper time).
struct KS { double r, f; dvec3 k; };
static KS ksAt(dvec3 x, double A){
    const double M = 0.5;
    double a2 = A*A, w = dot(x,x) - a2;
    double r2 = 0.5*(w + std::sqrt(w*w + 4.0*a2*x.z*x.z)), r = std::sqrt(r2);
    KS s; s.r = r;
    s.f = 2.0*M*r2*r / (r2*r2 + a2*x.z*x.z);
    s.k = dvec3((r*x.x + A*x.y)/(r2 + a2), (r*x.y - A*x.x)/(r2 + a2), x.z/r);
    return s;
}
static void geodesicRHS(dvec3 x, dvec3 p, double E, double A, dvec3& dx, dvec3& dp){
    const double M = 0.5;
    double a2 = A*A, R2 = dot(x,x), w = R2 - a2;
    double r2 = 0.5*(w + std::sqrt(w*w + 4.0*a2*x.z*x.z)), r = std::sqrt(r2);
    double P = r2 + a2, Q = r2*r2 + a2*x.z*x.z;
    double f = 2.0*M*r2*r / Q;
    dvec3  k((r*x.x + A*x.y)/P, (r*x.y - A*x.x)/P, x.z/r);
    double S = E + dot(k, p);
    dx = p - f*S*k;
    double den = 2.0*r2 - R2 + a2;
    dvec3 gr = dvec3(x.x*r, x.y*r, x.z*(r2 + a2)/r) / den;
    dvec3 gQ = 4.0*r2*r*gr + dvec3(0.0, 0.0, 2.0*a2*x.z);
    dvec3 gf = 2.0*M * (3.0*r2*Q*gr - r2*r*gQ) / (Q*Q);
    double c = (p.x*x.x + p.y*x.y)/P
             - 2.0*r*(p.x*(r*x.x + A*x.y) + p.y*(r*x.y - A*x.x))/(P*P)
             - p.z*x.z/r2;
    dvec3 gS = c*gr + dvec3((r*p.x - A*p.y)/P, (A*p.x + r*p.y)/P, p.z/r);
    dp = 0.5 * (S*S*gf + 2.0*f*S*gS);
}
static void geodesicRK4(dvec3& x, dvec3& p, double E, double A, double h){
    dvec3 k1x,k1p,k2x,k2p,k3x,k3p,k4x,k4p;
    geodesicRHS(x,            p,            E, A, k1x, k1p);
    geodesicRHS(x + 0.5*h*k1x, p + 0.5*h*k1p, E, A, k2x, k2p);
    geodesicRHS(x + 0.5*h*k2x, p + 0.5*h*k2p, E, A, k3x, k3p);
    geodesicRHS(x + h*k3x,    p + h*k3p,    E, A, k4x, k4p);
    x += h/6.0 * (k1x + 2.0*k2x + 2.0*k3x + k4x);
    p += h/6.0 * (k1p + 2.0*k2p + 2.0*k3p + k4p);
}

// ---------- Free fall ----------
static double spinA(){ return gSpin * 0.5; }          // a in rs units (M = 0.5)
static dvec3 camLocal(){ dvec3 w = dvec3(gCam.position()) / SAG_RS; return dvec3(w.z, w.x, w.y); }
static void setCamFromLocal(dvec3 x){
    dvec3 w(x.y, x.z, x.x);                            // world = local.yzx
    double R = length(w);
    gCam.radius    = float(R * SAG_RS);
    gCam.elevation = float(std::acos(std::clamp(w.y / R, -1.0, 1.0)));
    gCam.azimuth   = float(std::atan2(w.z, w.x));
    gCam.moving    = true;                             // fast preview while falling
}

double fallRadius(){ return ksAt(gFall.active ? gFall.x : camLocal(), spinA()).r; }

void startFall(){
    // start at rest = the static observer at the camera: u = (1/sqrt(1-f), 0)
    dvec3 x = camLocal();
    KS s = ksAt(x, spinA());
    if (s.f >= 0.98) return;                           // can't hover here
    double ut = 1.0 / std::sqrt(1.0 - s.f);
    gFall = FreeFall{};
    gFall.active = true;
    gFall.savedCam = gCam;
    gFall.x = x;
    gFall.E = (1.0 - s.f) * ut;                        // -p_t = sqrt(1 - f)
    gFall.p = s.f * ut * s.k;                          // p_i = f k_i (l.u), l.u = u^t
    gFall.rStart = s.r;
}

dvec4 fallVelocity(){
    KS s = ksAt(gFall.x, spinA());
    double S = gFall.E + dot(s.k, gFall.p);
    dvec3 dx = gFall.p - s.f * S * s.k;
    return dvec4(gFall.E + s.f * S, dx.x, dx.y, dx.z);
}

void advanceFall(double dTau){
    // adaptive steps: small near the hole (and the singularity)
    while (dTau > 0.0) {
        double r = ksAt(gFall.x, spinA()).r;
        double h = std::min(dTau, 0.004 * std::max(r, 0.05));
        geodesicRK4(gFall.x, gFall.p, gFall.E, spinA(), h);
        gFall.tau += h; dTau -= h;
    }
    setCamFromLocal(gFall.x);
}

void stepFall(double realDt){
    if (!gFall.active || gFall.paused) return;
    advanceFall(std::min(realDt, 0.05) * gFallSpeed);
    // end the dive at the singularity (a = 0) or just before the inner horizon
    // (a > 0), where the Kerr interior stops being physically meaningful
    double a = spinA(), M = 0.5;
    double rMinus = M - std::sqrt(std::max(M*M - a*a, 0.0));
    double rEnd = (a > 1e-6) ? rMinus * 1.05 + 0.01 : 0.03;
    double r = ksAt(gFall.x, a).r;
    if (r <= rEnd) {
        char buf[128];
        std::snprintf(buf, sizeof buf, "%s at r = %.2f rs after %.1f rs/c of proper time",
                      a > 1e-6 ? "reached the inner horizon" : "hit the singularity", r, gFall.tau);
        gFall.endReason = buf;
        gFall.active = false;
        std::cout << "[INFO] Dive ended: " << buf << std::endl;
        gCam = gFall.savedCam;     // back to where the dive started
    }
}

// ---------- Star motion: Kerr geodesics ----------
// Stars are test particles moving on timelike geodesics of the hole's
// spacetime (same Hamiltonian and integrator as the falling camera). Their
// pull on each other is ignored: each is ~1e-6 of the hole's mass. Compared
// with Newtonian gravity this gives perihelion precession (~3*pi*rs/r per
// orbit), the ISCO, plunging orbits and frame dragging.
static dvec3 worldToLocal(dvec3 w){ return dvec3(w.z, w.x, w.y); }
static dvec3 localToWorld(dvec3 l){ return dvec3(l.y, l.z, l.x); }

// From posRadius (m) and velocity (m/s, coordinate velocity dx/dt):
// u = u^t (1, v/c), normalised so g(u,u) = -1, then lowered: p_mu = g_mu_nu u^nu.
bool initStarGeodesic(ObjectData& o){
    const double A = spinA();
    dvec3 x = worldToLocal(dvec3(vec3(o.posRadius))) / SAG_RS;
    dvec3 v = worldToLocal(dvec3(o.velocity)) / C_LIGHT;
    KS s = ksAt(x, A);
    double lV = 1.0 + dot(s.k, v);                         // l_mu V^mu with V = (1, v)
    double gVV = -1.0 + dot(v, v) + s.f * lV * lV;         // g(V, V)
    if (gVV >= -1e-9) return false;                        // not timelike (inside the ergoregion at rest, or v >= c)
    double ut = 1.0 / std::sqrt(-gVV);
    double lu = ut * lV;
    o.geoE = ut - s.f * lu;                                // E = -p_t = u^t - f (l.u)
    o.geoP = ut * v + s.f * s.k * lu;                      // p_i = u^i + f k_i (l.u)
    o.geoX = x;
    o.geoInit = true;
    return true;
}

// Advance by coordinate time dtCoord (rs/c), stepping in proper time with
// dt/dtau = E + f S, small steps near the hole.
void advanceStar(ObjectData& o, double dtCoord){
    const double A = spinA();
    double done = 0.0;
    int guard = 0;
    while (done < dtCoord && guard++ < 100000) {
        KS s = ksAt(o.geoX, A);
        double S = o.geoE + dot(s.k, o.geoP);
        double dtdtau = std::max(o.geoE + s.f * S, 1e-9);
        double h = 0.01 * std::max(s.r, 0.05);              // proper-time step
        h = std::min(h, (dtCoord - done) / dtdtau);
        geodesicRK4(o.geoX, o.geoP, o.geoE, A, h);
        done += h * dtdtau;
    }
    // world position (m) and coordinate velocity (m/s) for the renderer / UI
    KS s = ksAt(o.geoX, A);
    double S = o.geoE + dot(s.k, o.geoP);
    dvec3 dxdtau = o.geoP - s.f * S * s.k;
    dvec3 v = dxdtau / std::max(o.geoE + s.f * S, 1e-9);
    o.posRadius = vec4(vec3(localToWorld(o.geoX) * SAG_RS), o.posRadius.w);
    o.velocity  = vec3(localToWorld(v) * C_LIGHT);
}

void stepGravity(double realDt){
    if(!gGravity) return;
    realDt = std::min(realDt, 0.05);                       // avoid huge jumps after a stall
    double dtCoord = realDt * gTimeScale * C_LIGHT / SAG_RS; // simulated seconds -> rs/c
    double rPlus = kerrInfo(gSpin).rPlus;
    for (auto& o : gObjs) {
        if (o.pinned) continue;
        if (!o.geoInit && !initStarGeodesic(o)) continue;
        advanceStar(o, dtCoord);
    }
    // stars whose centre crosses the event horizon are swallowed
    gObjs.erase(std::remove_if(gObjs.begin(), gObjs.end(), [&](const ObjectData& o){
        return !o.pinned && o.geoInit && ksAt(o.geoX, spinA()).r <= rPlus;
    }), gObjs.end());
}

// Circular equatorial orbit (prograde) at Kerr-Schild cylindrical radius R:
// the Boyer-Lindquist radius is r = sqrt(R^2 - a^2), the coordinate angular
// velocity Omega = sqrt(M) / (r^1.5 + a sqrt(M)), and in Kerr-Schild
// coordinates the motion is a rigid rotation, so the speed is Omega * R (c).
double circularOrbitSpeed(double R, double chi){
    const double M = 0.5, a = chi * M;
    double r = std::sqrt(std::max(R*R - a*a, 1e-12));
    return std::sqrt(M) / (r * std::sqrt(r) + a * std::sqrt(M)) * R;
}

// Give every star the velocity of a circular orbit around the black hole
// (prograde, same direction as the disk) and switch gravity on.
void orbitStars(){
    for (auto& o : gObjs) {
        if (o.pinned) continue;
        vec3 r = vec3(o.posRadius);
        double d = length(r);
        if (d <= 0.0) continue;
        double v = circularOrbitSpeed(d / SAG_RS, gSpin) * C_LIGHT;
        vec3 dir = normalize(cross(vec3(0,1,0), r));
        if (!std::isfinite(dir.x)) dir = vec3(1,0,0);   // star on the spin axis
        o.velocity = dir * float(v);
        o.geoInit = false;                               // re-derive the geodesic state
    }
    gGravity = true;
}
