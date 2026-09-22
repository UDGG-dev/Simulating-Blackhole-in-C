/*
 * blackhole.c - Schwarzschild black hole ray tracer (no dependencies)
 *
 * Physics:
 *   - Units: Schwarzschild radius rs = 1 (event horizon at r = 1).
 *   - Light rays are traced BACKWARDS from the camera through curved spacetime.
 *     For a photon in Schwarzschild geometry, the trajectory obeys
 *         a = -1.5 * h^2 * x / |x|^5 ,   h = |x cross v|  (conserved)
 *     which reproduces the exact photon orbit equation u'' = -u + 1.5 u^2.
 *   - Ray falls inside r < 1   -> black (shadow)
 *   - Ray reaches r > R_ESCAPE -> procedural starfield (gravitationally lensed)
 *   - Ray crosses the equatorial plane between DISK_IN and DISK_OUT
 *     -> thin accretion disk with temperature ~ r^-3/4, Keplerian Doppler
 *     beaming and gravitational redshift. Higher-order images (the photon
 *     ring / disk wrapping over the top) appear naturally.
 *
 * Build:  gcc -O3 -march=native -fopenmp blackhole.c -o blackhole -lm
 * Run:    ./blackhole [width height elevation_deg]
 *         ./blackhole 1920 1080 8        -> writes blackhole.ppm
 * View:   magick blackhole.ppm blackhole.png   (or any image viewer)
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define AA         2        /* supersampling per axis (AA*AA rays/pixel) */
#define MAX_STEPS  4000
#define R_ESCAPE   60.0
#define DISK_IN    3.0      /* ISCO = 3 rs */
#define DISK_OUT   14.0
#define CAM_DIST   30.0
#define FOV_DEG    40.0
#define EXPOSURE   1.6
#define T_INNER    9000.0   /* Kelvin at inner edge (artistic) */

typedef struct { double x, y, z; } vec3;

static inline vec3 v3(double x, double y, double z) { return (vec3){x, y, z}; }
static inline vec3 add(vec3 a, vec3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline vec3 sub(vec3 a, vec3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline vec3 mul(vec3 a, double s) { return v3(a.x * s, a.y * s, a.z * s); }
static inline double dot(vec3 a, vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline vec3 cross(vec3 a, vec3 b) {
    return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
static inline vec3 norm(vec3 a) { return mul(a, 1.0 / sqrt(dot(a, a))); }
static inline double clampd(double x, double lo, double hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}
static inline double smooth(double a, double b, double x) {
    double t = clampd((x - a) / (b - a), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

/* Approximate blackbody color (Tanner Helland fit), T in Kelvin -> linear-ish RGB */
static vec3 blackbody(double T) {
    double t = clampd(T, 1000.0, 40000.0) / 100.0, r, g, b;
    r = (t <= 66.0) ? 255.0 : 329.698727446 * pow(t - 60.0, -0.1332047592);
    g = (t <= 66.0) ? 99.4708025861 * log(t) - 161.1195681661
                    : 288.1221695283 * pow(t - 60.0, -0.0755148492);
    b = (t >= 66.0) ? 255.0 : (t <= 19.0 ? 0.0 : 138.5177312231 * log(t - 10.0) - 305.0447927307);
    return v3(clampd(r, 0, 255) / 255.0, clampd(g, 0, 255) / 255.0, clampd(b, 0, 255) / 255.0);
}

/* ---- procedural starfield ---- */
static uint32_t hash_u32(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU;
    x ^= x >> 15; x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}
static double rnd(uint32_t h) { return (h & 0xFFFFFF) / 16777216.0; }

static vec3 stars(vec3 d) {
    d = norm(d);
    const double cells = 1400.0;
    double theta = acos(clampd(d.y, -1.0, 1.0));
    double phi = atan2(d.z, d.x) + M_PI;
    double fu = phi / M_PI * cells, fv = theta / M_PI * cells;
    int iu = (int)floor(fu), iv = (int)floor(fv);
    uint32_t h = hash_u32((uint32_t)iu * 73856093u ^ (uint32_t)iv * 19349663u);
    if (rnd(h) > 0.015) return v3(0, 0, 0);
    double cx = iu + 0.2 + 0.6 * rnd(hash_u32(h + 1));
    double cy = iv + 0.2 + 0.6 * rnd(hash_u32(h + 2));
    double dx = fu - cx, dy = fv - cy;
    double mag = rnd(hash_u32(h + 3));
    double b = exp(-(dx * dx + dy * dy) / 0.05) * (0.3 + 1.7 * mag * mag);
    vec3 c = blackbody(3500.0 + 8000.0 * rnd(hash_u32(h + 4)));
    return mul(c, b);
}

/* ---- trace one ray, return linear HDR color ---- */
static vec3 trace(vec3 pos, vec3 vel) {
    vec3 col = v3(0, 0, 0);
    vec3 hvec = cross(pos, vel);
    double h2 = dot(hvec, hvec);

    for (int i = 0; i < MAX_STEPS; i++) {
        double r2 = dot(pos, pos), r = sqrt(r2);

        if (r < 1.0) break;                              /* swallowed by horizon */
        if (r > R_ESCAPE && dot(pos, vel) > 0.0) {       /* escaped to the sky   */
            col = add(col, stars(vel));
            break;
        }

        double dt = clampd(0.02 * r, 0.01, 0.5);
        double r5 = r2 * r2 * r;
        vec3 a = mul(pos, -1.5 * h2 / r5);
        vel = add(vel, mul(a, dt));
        vec3 np = add(pos, mul(vel, dt));

        /* equatorial-plane crossing -> accretion disk */
        if ((pos.y > 0.0) != (np.y > 0.0)) {
            double t = pos.y / (pos.y - np.y);
            vec3 p = add(pos, mul(sub(np, pos), t));
            double rd = sqrt(p.x * p.x + p.z * p.z);
            if (rd > DISK_IN && rd < DISK_OUT) {
                /* Keplerian orbital speed (fraction of c) and direction */
                double beta = sqrt(0.5 / (rd - 1.0));
                vec3 tang = v3(p.z / rd, 0.0, -p.x / rd);
                vec3 to_obs = mul(norm(vel), -1.0);
                double gamma = 1.0 / sqrt(1.0 - beta * beta);
                double D = 1.0 / (gamma * (1.0 - beta * dot(tang, to_obs)));
                double g = sqrt(1.0 - 1.0 / rd);         /* gravitational redshift */
                double s = D * g;                        /* total frequency shift  */

                double ang = atan2(p.z, p.x);
                double tex = 0.75 + 0.25 * sin(18.0 * rd + 3.0 * ang) * sin(7.0 * ang - 5.0 * rd);
                double edge = smooth(DISK_IN, DISK_IN + 0.4, rd) *
                              (1.0 - smooth(DISK_OUT - 4.0, DISK_OUT, rd));
                double T = T_INNER * pow(DISK_IN / rd, 0.75) * s;
                double I = tex * edge * pow(DISK_IN / rd, 1.5) * pow(s, 3.5);
                col = add(col, mul(blackbody(T), I));
            }
        }
        pos = np;
    }
    return col;
}

int main(int argc, char **argv) {
    int W = argc > 1 ? atoi(argv[1]) : 1280;
    int H = argc > 2 ? atoi(argv[2]) : 720;
    double elev = (argc > 3 ? atof(argv[3]) : 10.0) * M_PI / 180.0;
    if (W < 1 || H < 1) { fprintf(stderr, "bad size\n"); return 1; }

    vec3 cam = v3(0.0, CAM_DIST * sin(elev), -CAM_DIST * cos(elev));
    vec3 fwd = norm(mul(cam, -1.0));
    vec3 right = norm(cross(v3(0, 1, 0), fwd));
    vec3 up = cross(fwd, right);
    double tan_half = tan(FOV_DEG * 0.5 * M_PI / 180.0);
    double aspect = (double)W / H;

    uint8_t *img = malloc((size_t)W * H * 3);
    if (!img) { fprintf(stderr, "out of memory\n"); return 1; }

    #pragma omp parallel for schedule(dynamic, 4)
    for (int j = 0; j < H; j++) {
        for (int i = 0; i < W; i++) {
            vec3 acc = v3(0, 0, 0);
            for (int sy = 0; sy < AA; sy++)
                for (int sx = 0; sx < AA; sx++) {
                    double u = ((i + (sx + 0.5) / AA) / W * 2.0 - 1.0) * aspect * tan_half;
                    double v = (1.0 - (j + (sy + 0.5) / AA) / H * 2.0) * tan_half;
                    vec3 d = norm(add(fwd, add(mul(right, u), mul(up, v))));
                    acc = add(acc, trace(cam, d));
                }
            acc = mul(acc, 1.0 / (AA * AA));

            double c[3] = {acc.x, acc.y, acc.z};
            for (int k = 0; k < 3; k++) {
                double m = 1.0 - exp(-c[k] * EXPOSURE);       /* tone map */
                m = pow(m, 1.0 / 2.2);                        /* gamma    */
                img[((size_t)j * W + i) * 3 + k] = (uint8_t)(clampd(m, 0.0, 1.0) * 255.0 + 0.5);
            }
        }
    }

    FILE *f = fopen("blackhole.ppm", "wb");
    if (!f) { perror("blackhole.ppm"); return 1; }
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    fwrite(img, 3, (size_t)W * H, f);
    fclose(f);
    free(img);
    fprintf(stderr, "wrote blackhole.ppm (%dx%d, elevation %.1f deg)\n", W, H, elev * 180.0 / M_PI);
    return 0;
}
