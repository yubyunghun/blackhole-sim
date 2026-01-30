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
