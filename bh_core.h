/*
 * bh_core.h - shared Schwarzschild black hole ray tracer core.
 * Units: Schwarzschild radius rs = 1. Include from bh_video.c / bh_live.c.
 * Compile with -fopenmp for multithreading.
 */
#ifndef BH_CORE_H
#define BH_CORE_H

#include <math.h>
#include <stdint.h>
#include <stdlib.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MAX_STEPS 4000
#define R_ESCAPE  60.0
#define DISK_IN   3.0      /* ISCO = 3 rs */
#define DISK_OUT  14.0
#define T_INNER   9000.0   /* K at inner edge (artistic) */
#define EXPOSURE  1.6

typedef struct { double x, y, z; } vec3;

typedef struct {
    double az;       /* orbit angle around the spin axis (rad)      */
    double el;       /* elevation above the disk plane (rad)        */
    double dist;     /* distance from the hole in rs                */
    double time;     /* animation time (seconds), rotates the disk  */
    double fov_deg;  /* vertical field of view                      */
} BHCamera;

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

/* Approximate blackbody color (Tanner Helland fit), T in Kelvin */
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

/* ---- trace one ray backwards from the camera; returns linear HDR color ---- */
static vec3 bh_trace(vec3 pos, vec3 vel, double time) {
    vec3 col = v3(0, 0, 0);
    vec3 hvec = cross(pos, vel);
    double h2 = dot(hvec, hvec);   /* conserved angular momentum^2 */

    for (int i = 0; i < MAX_STEPS; i++) {
        double r2 = dot(pos, pos), r = sqrt(r2);

        if (r < 1.0) break;                              /* fell into the horizon */
        if (r > R_ESCAPE && dot(pos, vel) > 0.0) {       /* escaped to the sky    */
            col = add(col, stars(vel));
            break;
        }

        double dt = clampd(0.02 * r, 0.01, 0.5);
        double r5 = r2 * r2 * r;
        vec3 a = mul(pos, -1.5 * h2 / r5);               /* Schwarzschild photon bending */
        vel = add(vel, mul(a, dt));
        vec3 np = add(pos, mul(vel, dt));

        /* equatorial-plane crossing -> accretion disk */
        if ((pos.y > 0.0) != (np.y > 0.0)) {
            double t = pos.y / (pos.y - np.y);
            vec3 p = add(pos, mul(sub(np, pos), t));
            double rd = sqrt(p.x * p.x + p.z * p.z);
            if (rd > DISK_IN && rd < DISK_OUT) {
                double beta = sqrt(0.5 / (rd - 1.0));            /* Keplerian v/c */
                vec3 tang = v3(p.z / rd, 0.0, -p.x / rd);
                vec3 to_obs = mul(norm(vel), -1.0);
                double gamma = 1.0 / sqrt(1.0 - beta * beta);
                double D = 1.0 / (gamma * (1.0 - beta * dot(tang, to_obs)));  /* Doppler */
                double g = sqrt(1.0 - 1.0 / rd);                              /* grav. redshift */
                double s = D * g;

                /* pattern co-rotates with the disk (inner parts spin faster) */
                double phase = atan2(p.z, p.x) + time * 1.5 * pow(DISK_IN / rd, 1.5);
                double tex = 0.75 + 0.25 * sin(18.0 * rd + 3.0 * phase) * sin(7.0 * phase - 5.0 * rd);
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

/* Render a W x H RGB24 frame with aa x aa supersampling. */
static void bh_render(uint8_t *img, int W, int H, int aa, const BHCamera *c) {
    double ce = cos(c->el), se = sin(c->el);
    vec3 cam = v3(c->dist * ce * sin(c->az), c->dist * se, -c->dist * ce * cos(c->az));
    vec3 fwd = norm(mul(cam, -1.0));
    vec3 right = norm(cross(v3(0, 1, 0), fwd));
    vec3 up = cross(fwd, right);
    double tan_half = tan(c->fov_deg * 0.5 * M_PI / 180.0);
    double aspect = (double)W / H;
    double time = c->time;

    #pragma omp parallel for schedule(dynamic, 2)
    for (int j = 0; j < H; j++) {
        for (int i = 0; i < W; i++) {
            vec3 acc = v3(0, 0, 0);
            for (int sy = 0; sy < aa; sy++)
                for (int sx = 0; sx < aa; sx++) {
                    double u = ((i + (sx + 0.5) / aa) / W * 2.0 - 1.0) * aspect * tan_half;
                    double v = (1.0 - (j + (sy + 0.5) / aa) / H * 2.0) * tan_half;
                    vec3 d = norm(add(fwd, add(mul(right, u), mul(up, v))));
                    acc = add(acc, bh_trace(cam, d, time));
                }
            acc = mul(acc, 1.0 / (aa * aa));

            double ch[3] = {acc.x, acc.y, acc.z};
            for (int k = 0; k < 3; k++) {
                double m = 1.0 - exp(-ch[k] * EXPOSURE);   /* tone map */
                m = pow(m, 1.0 / 2.2);                     /* gamma    */
                img[((size_t)j * W + i) * 3 + k] = (uint8_t)(clampd(m, 0.0, 1.0) * 255.0 + 0.5);
            }
        }
    }
}

#endif
