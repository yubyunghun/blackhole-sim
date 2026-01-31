// Physics self-test (no GPU needed): checks the CPU-side Kerr formulas and the
// relativistic camera against published values and exact identities.
// Run:  ctest   (CMake)   or   bh_tests.exe   (build_msvc.bat)
#include "common.hpp"

static int gFails = 0, gChecks = 0;

static void check(const char* what, double got, double want, double tol){
    ++gChecks;
    bool ok = std::fabs(got - want) <= tol;
    if (!ok) ++gFails;
    std::printf("%s %-46s got %-12.6g want %-12.6g (tol %g)\n", ok ? "[ ok ]" : "[FAIL]", what, got, want, tol);
}

int main(){
    const double M = 0.5;   // in rs units

    std::puts("-- Kerr radii (units of rs; literature values in units of M x 0.5)");
    {
        KerrInfo k0 = kerrInfo(0.0);
        check("a=0    horizon = 2M",              k0.rPlus,    1.0,          1e-12);
        check("a=0    photon sphere = 3M",        k0.rPhPro,   1.5,          1e-12);
        check("a=0    ISCO = 6M",                 k0.rIsco,    3.0,          1e-12);
        check("a=0    efficiency = 1 - sqrt(8/9)", k0.efficiency, 1.0 - std::sqrt(8.0/9.0), 1e-12);
        check("a=0    NT flux peak ~ 9.55M",      k0.rFluxPeak, 9.55 * M,    0.05 * M);

        check("a=0.5  ISCO = 4.2330M",            kerrInfo(0.5).rIsco,   4.2330 * M, 5e-4 * M);
        check("a=0.9  ISCO = 2.3209M",            kerrInfo(0.9).rIsco,   2.3209 * M, 5e-4 * M);
        check("a=0.99 ISCO = 1.4545M",            kerrInfo(0.99).rIsco,  1.4545 * M, 5e-4 * M);
        check("a=0.9  horizon = M(1+sqrt(1-a^2))", kerrInfo(0.9).rPlus,  M * (1.0 + std::sqrt(1.0 - 0.81)), 1e-12);
        check("a=0.9  efficiency 15.6%",          kerrInfo(0.9).efficiency,  0.1558, 5e-4);
        check("a=0.99 efficiency 26.4%",          kerrInfo(0.99).efficiency, 0.2640, 5e-4);
        check("a->1   prograde photon orbit -> M", kerrInfo(0.9999).rPhPro,   1.0 * M, 0.05 * M);
        check("a->1   retrograde photon orbit -> 4M", kerrInfo(0.9999).rPhRetro, 4.0 * M, 1e-3 * M);
        check("ergosphere at equator = 2M",       kerrInfo(0.7).rErgoEq,  1.0, 1e-12);
    }

    std::puts("-- Novikov-Thorne flux");
    for (double chi : {0.0, 0.9}) {
        double ri = iscoOverM(chi);
        char buf[64];
        std::snprintf(buf, sizeof buf, "a=%.1f  flux at the ISCO is zero", chi);
        check(buf, ntFlux(ri, chi), 0.0, 1e-15);
        // far out F ~ r^-3 (1 - sqrt(r_isco/r)) plus log corrections that fade
        // as ~ln(x)/x, so test very far out where doubling r divides F by ~8
        std::snprintf(buf, sizeof buf, "a=%.1f  flux ~ r^-3 far out (1e6 M)", chi);
        check(buf, ntFlux(1e6, chi) / ntFlux(2e6, chi), 8.0, 0.03);
    }

    std::puts("-- Relativistic camera (static-observer tetrad)");
    for (double chi : {0.0, 0.9}) {
        // camera at 11.8 rs, a bit above the disk plane, looking at the hole
        dvec3 x(11.8, 0.3, 1.2);                      // local Kerr-Schild coords
        dvec3 fwd = normalize(-x);
        dvec3 right = normalize(cross(fwd, dvec3(0, 0, 1)));
        dvec3 up = cross(right, fwd);
        CameraTetrad T = cameraTetrad(x, fwd, right, up, -chi * M);
        char buf[64];
        std::snprintf(buf, sizeof buf, "a=%.1f  tetrad exists outside ergosphere", chi);
        check(buf, T.ok ? 1.0 : 0.0, 1.0, 0.0);
        const dvec4* e[4] = { &T.u, &T.eF, &T.eR, &T.eU };
        double worst = 0.0;
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j) {
                double want = (i == j) ? (i == 0 ? -1.0 : 1.0) : 0.0;
                worst = std::max(worst, std::fabs(ksDot(T, *e[i], *e[j]) - want));
            }
        std::snprintf(buf, sizeof buf, "a=%.1f  tetrad orthonormal (max error)", chi);
        check(buf, worst, 0.0, 1e-12);
        // any photon built from the tetrad is null: g(p,p) = 0
        dvec3 n = normalize(dvec3(0.3, -0.2, 1.0));
        dvec4 p = T.u + n.x * T.eR + n.y * T.eU + n.z * T.eF;
        std::snprintf(buf, sizeof buf, "a=%.1f  pixel photon is null", chi);
        check(buf, ksDot(T, p, p), 0.0, 1e-12);
    }
    {   // gravitational blueshift: a photon with energy 1 at infinity, measured
        // by a static observer, has energy u^t = 1/sqrt(1 - rs/r)  (a = 0).
        // (This is the camera factor in the disk redshift g = E_cam / E_emit.)
        dvec3 x(6.0, 0.0, 0.0);
        CameraTetrad T = cameraTetrad(x, dvec3(-1,0,0), dvec3(0,1,0), dvec3(0,0,1), 0.0);
        dvec3 n = normalize(dvec3(0.2, 0.1, 1.0));
        dvec4 P = T.u - (n.x * T.eR + n.y * T.eU + n.z * T.eF);   // arriving photon
        double lp = P.x + dot(T.k, dvec3(P.y, P.z, P.w));
        double pt = -P.x + T.f * lp;                               // covariant p_t
        dvec4 Pn = P / (-pt);                                      // energy at infinity = 1
        check("a=0    static observer sees photon blueshifted", -ksDot(T, Pn, T.u),
              1.0 / std::sqrt(1.0 - 1.0 / 6.0), 1e-12);
    }
    {   // free fall from rest conserves the mass shell g(u,u) = -1
        Camera saved = gCam;
        double savedSpin = gSpin;
        gSpin = 0.9;
        gCam = Camera{};
        startFall();
        advanceFall(20.0);
        dvec4 u = fallVelocity();
        dvec3 xl(gFall.x);
        CameraTetrad T = cameraTetrad(xl, dvec3(1,0,0), dvec3(0,1,0), dvec3(0,0,1), gSpin * M, &u);
        check("a=0.9  free fall keeps g(u,u) = -1", ksDot(T, u, u), -1.0, 1e-8);
        check("a=0.9  free fall moves inward", fallRadius() < gFall.rStart ? 1.0 : 0.0, 1.0, 0.0);
        gFall = FreeFall{}; gCam = saved; gSpin = savedSpin;
    }
    {   // a star on a circular Kerr orbit keeps its radius and returns after
        // one period T = 2 pi / Omega (coordinate time)
        double savedSpin = gSpin;
        gSpin = 0.9;
        const double r = 10.0;                                   // rs
        double v = circularOrbitSpeed(r, gSpin);                 // units of c
        ObjectData star{ vec4(float(r * SAG_RS), 0, 0, 1e9f), vec4(1), 1e30f,
                         vec3(0.0f, 0.0f, float(-v * C_LIGHT)), false };
        bool ok = initStarGeodesic(star);
        check("a=0.9  circular star orbit initialises", ok ? 1.0 : 0.0, 1.0, 0.0);
        double T = 2.0 * M_PI * r / v;
        double rMin = 1e9, rMax = 0.0;
        for (int i = 0; i < 100; ++i) {
            advanceStar(star, T / 100.0);
            double rr = length(dvec3(vec3(star.posRadius))) / SAG_RS;
            rMin = std::min(rMin, rr); rMax = std::max(rMax, rr);
        }
        check("a=0.9  circular orbit radius drift (fraction)", (rMax - rMin) / r, 0.0, 1e-3);
        double back = length(dvec3(vec3(star.posRadius)) / SAG_RS - dvec3(r, 0, 0));
        check("a=0.9  back at the start after one period (rs)", back, 0.0, 0.01 * r);
        gSpin = savedSpin;
    }
    {   // perihelion precession (a = 0): an eccentric orbit launched at apoapsis
        // (40 rs). Simulated: angle between successive apoapses minus 2 pi.
        // Exact: dphi = 2 * int_{rp}^{ra} L / (r^2 sqrt(R(r))) dr - 2 pi with
        // R(r) = E^2 - (1 - 2M/r)(1 + L^2/r^2), E and L the orbit's constants.
        double savedSpin = gSpin;
        gSpin = 0.0;
        const double ra = 40.0;
        double v = 0.85 * circularOrbitSpeed(ra, 0.0);
        ObjectData star{ vec4(float(ra * SAG_RS), 0, 0, 1e9f), vec4(1), 1e30f,
                         vec3(0.0f, 0.0f, float(-v * C_LIGHT)), false };
        initStarGeodesic(star);
        const double E = star.geoE;
        const double L = std::fabs(star.geoX.x * star.geoP.y - star.geoX.y * star.geoP.x);

        // simulate to the next apoapsis, accumulating the angle travelled
        double dt = 2.0 * M_PI * ra / v / 4000.0, phi = 0.0, prevAng = 0.0;
        double r1 = ra, r2 = ra, phiApo = 0.0;
        bool wentIn = false;
        for (int i = 0; i < 20000 && phiApo == 0.0; ++i) {
            advanceStar(star, dt);
            dvec3 w = dvec3(vec3(star.posRadius)) / SAG_RS;
            double rr = length(w), ang = std::atan2(w.z, w.x);
            double d = ang - prevAng;
            if (d >  M_PI) d -= 2.0 * M_PI;
            if (d < -M_PI) d += 2.0 * M_PI;
            phi += d; prevAng = ang;
            if (rr < 0.9 * ra) wentIn = true;
            if (wentIn && r1 > r2 && r1 > rr) phiApo = std::fabs(phi);   // r peaked: apoapsis
            r2 = r1; r1 = rr;
        }

        // exact value. r^3 R(r) = (1 - E^2)(ra - r)(r - rp)(r - r3): find the
        // turning points ra, rp from E and L, r3 from Vieta (product of roots
        // = 2 M L^2 / (1 - E^2)). With r = (ra+rp)/2 + (ra-rp)/2 cos(chi) the
        // factor sqrt((ra-r)(r-rp)) cancels against dr, leaving a smooth integrand.
        const double M = 0.5;
        auto R = [&](double r){ return E*E - (1.0 - 2.0*M/r) * (1.0 + L*L/(r*r)); };
        auto root = [&](double lo, double hi){            // R(lo), R(hi) of opposite sign
            bool loPos = R(lo) > 0.0;
            for (int i = 0; i < 200; ++i) { double mid = 0.5*(lo+hi); ((R(mid) > 0.0) == loPos ? lo : hi) = mid; }
            return 0.5*(lo+hi);
        };
        // brackets: R > 0 at 0.75 ra (between the turning points for this orbit),
        // R < 0 at 2 ra and at the photon sphere 3M
        double raX = root(0.75 * ra, 2.0 * ra);           // outer turning point (~40)
        double rp  = root(3.0 * M, 0.75 * ra);            // inner turning point
        double r3  = 2.0*M*L*L / ((1.0 - E*E) * raX * rp);
        double sum = 0.0;
        const int N = 20000;
        for (int i = 0; i < N; ++i) {
            double chi = M_PI * (i + 0.5) / N;
            double r = 0.5*(raX + rp) + 0.5*(raX - rp)*std::cos(chi);
            sum += L / (r*r * std::sqrt((1.0 - E*E) * (r - r3) / (r*r*r))) * (M_PI / N);
        }
        double exact = 2.0 * sum - 2.0 * M_PI;
        check("a=0    perihelion precession vs exact GR (rad)", phiApo - 2.0 * M_PI, exact, 0.01 * exact);
        gSpin = savedSpin;
    }
    {   // embedding diagram: a = 0 is Flamm's paraboloid z = 2 sqrt(r - 1) (rs units)
        check("a=0    embedding height at r=2 (Flamm)",  kerrEmbeddingHeight(2.0, 0.0), 2.0, 2e-3);
        check("a=0    embedding height at r=10 (Flamm)", kerrEmbeddingHeight(10.0, 0.0), 6.0, 2e-3);
        // with spin, far from the hole the geometry is nearly the same
        check("a=0.9  embedding slope ~ Flamm far out",
              kerrEmbeddingHeight(41.0, 0.9) - kerrEmbeddingHeight(37.0, 0.9),
              2.0*std::sqrt(40.0) - 2.0*std::sqrt(36.0), 0.01);
    }
    {   // a = 0: Kerr-Schild f reduces to Schwarzschild rs/r
        dvec3 x(5.0, 2.0, -3.0);
        CameraTetrad T = cameraTetrad(x, dvec3(1,0,0), dvec3(0,1,0), dvec3(0,0,1), 0.0);
        check("a=0    Kerr-Schild f = rs/r", T.f, 1.0 / length(x), 1e-12);
    }
    {   // analytic shadow size seen by a static observer (Schwarzschild)
        double r = 1.5e11 / SAG_RS;   // default camera distance in rs
        double sinA = (3.0 * std::sqrt(3.0) / 2.0) / r * std::sqrt(1.0 - 1.0 / r);
        double px = std::tan(std::asin(sinA)) / std::tan(M_PI / 6.0) * 360.0;
        check("shadow radius at default view (px, 720 tall)", px, 134.2, 0.15);
    }

    std::printf("\n%d checks, %d failed\n", gChecks, gFails);
    return gFails == 0 ? 0 : 1;
}
