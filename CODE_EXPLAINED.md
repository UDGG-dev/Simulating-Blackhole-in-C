# Black Hole Simulator — Full Code Walkthrough

This document explains **every file, every line**: what it is, why it's there,
and what it does. Read `bh_core.h` first — it holds all the physics and
rendering, and the other three files just call into it.

Files covered:
- `bh_core.h` — the physics + rendering engine (shared by everything)
- `bh_video.c` — renders an orbiting-camera animation as PPM frames
- `bh_live.c` — interactive SDL2 window
- `blackhole.c` — the original single-file still-image version

---

## PART 1: `bh_core.h` (the engine)

### 1.1 Header guard and includes

```c
#ifndef BH_CORE_H
#define BH_CORE_H
```
- **What:** an include guard. If the file gets included twice, the second
  time `BH_CORE_H` is already defined, so the compiler skips everything.
- **Why:** without it, a double include would define every function twice
  and fail to compile. Here it's just good habit, since each `.c` file
  includes it once.

```c
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
```
- `math.h` provides `sqrt`, `sin`, `cos`, `tan`, `atan2`, `acos`, `exp`,
  `log`, `pow`, `floor`.
- `stdint.h` provides fixed-size integer types: `uint8_t` (0–255, one color
  channel) and `uint32_t` (32-bit unsigned, used by the hash).
- `stdlib.h` provides `malloc`, `free`, `atoi`, `atof`. The header itself
  uses none of them, but the files that include it need them.

```c
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
```
- `M_PI` isn't part of strict C standard, though gcc normally provides it.
  This defines π only if missing, so the code compiles either way.

### 1.2 Constants

```c
#define MAX_STEPS 4000
```
- The most integration steps one ray may take. Some rays orbit near the
  photon sphere (r = 1.5) for a very long time. This caps the cost per ray.
  A ray that hits the cap stops with whatever color it collected so far.

```c
#define R_ESCAPE  60.0
```
- Beyond 60 rs from the hole we treat a ray as free (escaped), because
  gravity is negligible that far out. The bending angle at distance r falls
  off like 1/r.

```c
#define DISK_IN   3.0
#define DISK_OUT  14.0
```
- Disk inner and outer radius in rs. 3 rs is the innermost stable circular
  orbit (ISCO) for a non-spinning hole (6GM/c² = 3 rs). 14 is arbitrary; it
  just sets how big the disk looks.

```c
#define T_INNER   9000.0
#define EXPOSURE  1.6
```
- `T_INNER` is the disk temperature at its inner edge, in Kelvin. It's
  artistic: a real stellar-mass black hole disk is millions of K
  (X-rays), which we couldn't display.
- `EXPOSURE` is a brightness multiplier applied before tone mapping.

### 1.3 Data types

```c
typedef struct { double x, y, z; } vec3;
```
- A 3D vector of `double`s (64-bit floats). `typedef` lets us write `vec3`
  instead of `struct {...}`. Used for positions, directions and RGB colors.

```c
typedef struct {
    double az;       /* orbit angle around the spin axis (rad)      */
    double el;       /* elevation above the disk plane (rad)        */
    double dist;     /* distance from the hole in rs                */
    double time;     /* animation time (seconds), rotates the disk  */
    double fov_deg;  /* vertical field of view                      */
} BHCamera;
```
- Everything that defines a camera. It uses spherical coordinates around the
  black hole, so orbiting is just changing `az` and `el`. `time` isn't
  really camera state, but it's passed along so a frame is fully described
  by one struct. `fov_deg` is the vertical field of view in degrees.

### 1.4 Vector helpers

```c
static inline vec3 v3(double x, double y, double z) { return (vec3){x, y, z}; }
```
- A constructor. `(vec3){x, y, z}` is a C99 "compound literal", an unnamed
  struct value.
- `static` gives the function file-local linkage, so it's fine in a header.
  `inline` hints the compiler to paste the body at the call site, avoiding
  call overhead. These helpers run billions of times.

```c
static inline vec3 add(vec3 a, vec3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline vec3 sub(vec3 a, vec3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline vec3 mul(vec3 a, double s) { return v3(a.x * s, a.y * s, a.z * s); }
```
- Component-wise addition, subtraction, and scaling by a number. C has no
  operator overloading, so `a + b` for vectors needs functions.

```c
static inline double dot(vec3 a, vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
```
- The dot product, a·b = |a||b|cos(angle). It gives squared length when
  used as `dot(a,a)`, and the sign tells whether two vectors point the same
  way.

```c
static inline vec3 cross(vec3 a, vec3 b) {
    return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
```
- The cross product a×b: a vector perpendicular to both, with length
  |a||b|sin(angle). We use it for the angular momentum and for building the
  camera axes.

```c
static inline vec3 norm(vec3 a) { return mul(a, 1.0 / sqrt(dot(a, a))); }
```
- Normalize: divide by the length so the result has length 1.
  `sqrt(dot(a,a))` is the length. It's computed as one division and three
  multiplies, since division is slower.

```c
static inline double clampd(double x, double lo, double hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}
```
- Forces `x` into `[lo, hi]`. `? :` is the ternary operator: `cond ? a : b`.
  (The name has a `d` for double to avoid clashing with other `clamp`
  functions.)

```c
static inline double smooth(double a, double b, double x) {
    double t = clampd((x - a) / (b - a), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}
```
- The "smoothstep" function. It maps x from `[a, b]` to `[0, 1]` linearly
  (`t`), then applies 3t² − 2t³, an S-curve with zero slope at both ends.
  The result is 0 below `a`, 1 above `b`, and eases in between. We use it
  to fade the disk edges without a hard cut.

### 1.5 Blackbody color

```c
static vec3 blackbody(double T) {
    double t = clampd(T, 1000.0, 40000.0) / 100.0, r, g, b;
```
- Takes a temperature in Kelvin and returns an RGB color. It clamps T to
  1000–40000 K, the range where the fit below is valid, then divides by
  100 because the fit's formulas work in hundreds of Kelvin. `r, g, b` are
  declared here and assigned next.

```c
    r = (t <= 66.0) ? 255.0 : 329.698727446 * pow(t - 60.0, -0.1332047592);
    g = (t <= 66.0) ? 99.4708025861 * log(t) - 161.1195681661
                    : 288.1221695283 * pow(t - 60.0, -0.0755148492);
    b = (t >= 66.0) ? 255.0 : (t <= 19.0 ? 0.0 : 138.5177312231 * log(t - 10.0) - 305.0447927307);
```
- These constants come from Tanner Helland's curve fit to blackbody colors.
  It's an empirical approximation, not a Planck integral.
- Below about 6600 K, red is saturated at 255 and green and blue grow with
  temperature, so cool = red/orange. Above 6600 K, red and green decay
  slowly, so hot = blue-white.
- `log` is the natural log in C.

```c
    return v3(clampd(r, 0, 255) / 255.0, clampd(g, 0, 255) / 255.0, clampd(b, 0, 255) / 255.0);
}
```
- Clamps each channel to 0–255, scales to 0–1, and packs it into a `vec3`.
  This is only the color (chromaticity) of the light. The brightness is
  applied separately by whoever calls it.
- Small inaccuracy: the fit targets display-encoded sRGB values, but we
  later apply gamma again, so colors are slightly off physically. It looks
  right, which is the goal here.

### 1.6 Star field

```c
static uint32_t hash_u32(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU;
    x ^= x >> 15; x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}
```
- A "hash" scrambles a 32-bit integer so that nearby inputs give unrelated
  outputs. This is what makes the stars look random while being fully
  deterministic. The same input always gives the same star, which is
  required so a star stays in the same place from frame to frame.
- `x ^= x >> 16` XORs the high half into the low half. `x *= constant`
  multiplies by an odd constant, spreading low bits into high bits.
  Repeating the pattern makes every input bit affect every output bit
  ("avalanche").
- The `U` suffix marks the constants unsigned. Unsigned overflow wraps
  modulo 2³², which is well-defined in C and is what we want.

```c
static double rnd(uint32_t h) { return (h & 0xFFFFFF) / 16777216.0; }
```
- Turns a hash into a float in `[0, 1)`. `h & 0xFFFFFF` keeps the low 24
  bits (0 to 16,777,215), then divides by 2²⁴ = 16,777,216.

```c
static vec3 stars(vec3 d) {
    d = norm(d);
```
- Input: the direction a ray finally left the scene in. Output: the star
  color seen in that direction (black for empty sky). We normalize it
  because the velocity vector isn't exactly length 1 after gravity has
  acted on it.

```c
    const double cells = 1400.0;
    double theta = acos(clampd(d.y, -1.0, 1.0));
    double phi = atan2(d.z, d.x) + M_PI;
```
- We turn the direction into two sky angles.
- `theta = acos(d.y)` is the polar angle from the +y axis, in `[0, π]`.
  The `clampd` guards against `d.y` being 1.0000001 from rounding, which
  would make `acos` return NaN.
- `phi = atan2(z, x) + π` is the azimuth, in `[0, 2π]`. Adding π makes it
  non-negative, which matters for the unsigned cast below.
- `cells = 1400` means 1400 grid cells across π radians, so each cell is
  π/1400 ≈ 0.0022 rad ≈ 0.129° wide.

```c
    double fu = phi / M_PI * cells, fv = theta / M_PI * cells;
    int iu = (int)floor(fu), iv = (int)floor(fv);
```
- `fu`, `fv` are the continuous grid coordinates (`fu` runs 0 to 2800,
  `fv` 0 to 1400). `iu`, `iv` are the integer cell indices from rounding
  down.

```c
    uint32_t h = hash_u32((uint32_t)iu * 73856093u ^ (uint32_t)iv * 19349663u);
```
- We combine the two cell indices into one number and hash it. Multiplying
  each index by a different large prime and XORing them is a standard
  "spatial hash". Precedence: `*` binds tighter than `^`, so it's
  `(iu·p1) ^ (iv·p2)`. This gives every cell its own reproducible random
  number `h`.

```c
    if (rnd(h) > 0.015) return v3(0, 0, 0);
```
- Only 1.5% of cells contain a star. Every other cell returns black. With
  2800 × 1400 ≈ 3.9M cells, that's about 59,000 stars over the whole sky.

```c
    double cx = iu + 0.2 + 0.6 * rnd(hash_u32(h + 1));
    double cy = iv + 0.2 + 0.6 * rnd(hash_u32(h + 2));
```
- The star's center position within its cell. Hashing `h+1` and `h+2`
  gives independent random numbers. The position is kept in 0.2–0.8 of the
  cell so the glow isn't cut off at the cell border. We only look at our
  own cell, not the neighbors.

```c
    double dx = fu - cx, dy = fv - cy;
    double mag = rnd(hash_u32(h + 3));
    double b = exp(-(dx * dx + dy * dy) / 0.05) * (0.3 + 1.7 * mag * mag);
```
- `dx, dy` is the offset from this pixel's sample point to the star
  center, in cell units.
- `mag` is a random 0–1 brightness roll.
- `exp(-d²/0.05)` is a Gaussian blob, 1 at the center and falling to 1/e
  at 0.22 cells. That's what makes a star a soft dot rather than a single
  pixel.
- `(0.3 + 1.7·mag²)` scales brightness from 0.3 to 2.0. Squaring `mag`
  makes dim stars much more common than bright ones, like a real sky.

```c
    vec3 c = blackbody(3500.0 + 8000.0 * rnd(hash_u32(h + 4)));
    return mul(c, b);
}
```
- Each star gets a random temperature from 3500 K (orange) to 11,500 K
  (blue-white) via `h+4`, and we return its color times its brightness.
- Because we pass the bent ray direction into this function, gravitational
  lensing distorts the stars automatically: they smear into arcs near the
  hole.

### 1.7 `bh_trace`, the physics core

```c
static vec3 bh_trace(vec3 pos, vec3 vel, double time) {
    vec3 col = v3(0, 0, 0);
```
- `pos` is the ray's starting point (the camera). `vel` is its unit
  direction. `time` is the animation time. `col` accumulates the light
  this ray picks up, starting black.

```c
    vec3 hvec = cross(pos, vel);
    double h2 = dot(hvec, hvec);
```
- `hvec` is the ray's angular momentum per unit speed. Because `vel` has
  length 1, `|pos × vel|` equals the ray's impact parameter b (its
  closest-approach distance if the ray were straight). `h2 = b²`.
- It's a constant of motion for any central force, so we compute it once.
- Rays with b below 3√3/2 ≈ 2.598 rs get captured. That number is the
  black hole shadow radius.

```c
    for (int i = 0; i < MAX_STEPS; i++) {
        double r2 = dot(pos, pos), r = sqrt(r2);
```
- The integration loop. `r2` is the squared distance from the hole (the
  hole is at the origin) and `r` is the distance. We keep `r2` because the
  force needs it.

```c
        if (r < 1.0) break;                              /* fell into the horizon */
```
- Inside the event horizon (r = rs = 1) nothing escapes. We stop, and
  `col` stays at whatever it collected before (usually black). This
  produces the shadow.

```c
        if (r > R_ESCAPE && dot(pos, vel) > 0.0) {       /* escaped to the sky    */
            col = add(col, stars(vel));
            break;
        }
```
- If the ray is far away and moving outward, it's gone: we add the star
  color for its final direction and stop.
- `dot(pos, vel) > 0` means moving away from the hole. It's needed because
  the camera may start beyond 60 rs, and a ray moving inward there must
  not be counted as escaped.

```c
        double dt = clampd(0.02 * r, 0.01, 0.5);
```
- The step size is 2% of the current distance, limited to 0.01–0.5. Steps
  are small near the hole, where bending is strong, and big far away,
  where rays are nearly straight. This is an adaptive step.

```c
        double r5 = r2 * r2 * r;
        vec3 a = mul(pos, -1.5 * h2 / r5);               /* Schwarzschild photon bending */
```
- The physics. The acceleration is `a = -1.5 · h² · pos / r⁵`, which is a
  vector pointing to the origin (the minus sign), of magnitude 1.5 h²/r⁴.
  It's an inverse-fourth-power attraction.
- Why this? For a photon in Schwarzschild geometry, the path in terms of
  u = 1/r obeys `u'' + u = 1.5 u²` (with rs = 1). The `1.5u²` term is the
  general-relativity correction. A central force `a = -k·pos/r⁵` gives,
  via Binet's equation, `u'' + u = k u²/h²`. Setting k = 1.5·h² makes the
  two match exactly. So a Newtonian-style force reproduces the
  relativistic light path.
- `r5 = r²·r²·r` is r⁵ with two multiplies instead of a slow `pow`.

```c
        vel = add(vel, mul(a, dt));
        vec3 np = add(pos, mul(vel, dt));
```
- We update the velocity first (`v += a·dt`), then the position with the
  new velocity (`x += v·dt`). This is semi-implicit Euler, more stable
  than the plain version. `np` is the new position, kept in a temporary
  because we still need the old `pos` for the disk crossing test.

```c
        if ((pos.y > 0.0) != (np.y > 0.0)) {
```
- The disk lies in the plane y = 0. If `y` changed sign this step, the ray
  crossed the plane. `(a>0) != (b>0)` is true only when the two are on
  opposite sides.

```c
            double t = pos.y / (pos.y - np.y);
            vec3 p = add(pos, mul(sub(np, pos), t));
```
- We find where along the step the crossing happened, by linear
  interpolation. `t` is between 0 and 1, and `p = pos + t·(np − pos)` is
  the point where y = 0.

```c
            double rd = sqrt(p.x * p.x + p.z * p.z);
            if (rd > DISK_IN && rd < DISK_OUT) {
```
- `rd` is the crossing point's distance from the spin axis (the y axis).
  If it's within the disk's radial range, we hit the disk.

```c
                double beta = sqrt(0.5 / (rd - 1.0));            /* Keplerian v/c */
```
- The orbital speed of the disk gas as a fraction of c, from the
  circular-orbit speed in Schwarzschild geometry, `v = sqrt(GM/(r−2GM))`.
  In units of rs = 2GM/c² = 1, GM/c² = 0.5. At r = 3 it's
  `sqrt(0.5/2) = 0.5`, so half the speed of light.

```c
                vec3 tang = v3(p.z / rd, 0.0, -p.x / rd);
```
- The unit vector along the gas's direction of motion: tangent to the
  circle, in the disk plane. It picks the rotation direction: at
  (x=1, z=0) it points along −z, toward the camera, so the right side of
  the screen approaches and looks brighter.

```c
                vec3 to_obs = mul(norm(vel), -1.0);
```
- The direction light travels from the disk to the camera. We trace
  backward, so the physical light direction is the opposite of `vel`.

```c
                double gamma = 1.0 / sqrt(1.0 - beta * beta);
                double D = 1.0 / (gamma * (1.0 - beta * dot(tang, to_obs)));  /* Doppler */
```
- `gamma` is the Lorentz factor.
- `D` is the relativistic Doppler factor `1/(γ(1 − β cosθ))`, where
  `cosθ = tang · to_obs`. Gas moving toward the camera gives D > 1
  (blueshift, brighter). Moving away gives D < 1 (redshift, dimmer). At
  β = 0.5, D ranges from about 0.58 to 1.73.

```c
                double g = sqrt(1.0 - 1.0 / rd);                              /* grav. redshift */
                double s = D * g;
```
- `g` is the gravitational redshift factor `sqrt(1 − rs/r)`. Light
  climbing out of the well loses energy, so it's below 1 and gets smaller
  as you approach the horizon. `s = D·g` is the total frequency-shift
  factor, the ratio of observed to emitted frequency.
- Approximation: it treats our bent-ray coordinate direction as the local
  direction. Good enough for looks.

```c
                double phase = atan2(p.z, p.x) + time * 1.5 * pow(DISK_IN / rd, 1.5);
```
- The angle around the disk (`atan2`), plus a term that grows with time.
  Kepler's law says angular speed ∝ r^−1.5, so inner gas spins faster.
  It's 1.5 rad/s at r = 3. This makes the pattern turn in the direction
  the gas orbits, with the inside faster than the outside (differential
  rotation).

```c
                double tex = 0.75 + 0.25 * sin(18.0 * rd + 3.0 * phase) * sin(7.0 * phase - 5.0 * rd);
```
- A procedural texture: a product of two sines, giving spiral-ish bands.
  The product ranges over [−1, 1], so `tex` ranges over [0.5, 1.0]. It's
  purely decorative, since without it the disk would be a featureless
  smooth gradient and rotation would be invisible.

```c
                double edge = smooth(DISK_IN, DISK_IN + 0.4, rd) *
                              (1.0 - smooth(DISK_OUT - 4.0, DISK_OUT, rd));
```
- A fade factor: 0→1 across the inner 0.4 rs, and 1→0 across the outer
  4 rs. It prevents hard circular cutoffs.

```c
                double T = T_INNER * pow(DISK_IN / rd, 0.75) * s;
```
- The observed color temperature. A standard thin-disk model has
  T ∝ r^(−3/4). Multiplying by `s` applies the Doppler and gravitational
  shift to the color.

```c
                double I = tex * edge * pow(DISK_IN / rd, 1.5) * pow(s, 3.5);
```
- The brightness: texture × edge fade × a radial falloff × a
  Doppler/redshift boost. The exponent 3.5 on `s` is a compromise. Strict
  physics says bolometric surface brightness scales as s⁴, and I used 3.5
  to keep the bright side from blowing out. The r^−1.5 falloff is also
  artistic: pure blackbody would be T⁴ ∝ r⁻³, which would make the outer
  disk almost invisible.

```c
                col = add(col, mul(blackbody(T), I));
            }
        }
        pos = np;
    }
    return col;
}
```
- We add the disk's color × brightness to `col`. Adding, not overwriting,
  means a ray can cross the disk several times and collect light each
  time, which produces the extra lensed images. It treats the disk as thin
  and see-through.
- `pos = np` commits the step, and the loop repeats. After the loop, we
  return the collected color.

### 1.8 `bh_render`, the camera and pixel loop

```c
static void bh_render(uint8_t *img, int W, int H, int aa, const BHCamera *c) {
```
- Renders a W×H frame into `img`, which is a byte buffer with 3 bytes
  (R, G, B) per pixel. `aa` is the supersampling factor per axis. `c` is a
  pointer to a camera struct; `const` promises we don't modify it.

```c
    double ce = cos(c->el), se = sin(c->el);
    vec3 cam = v3(c->dist * ce * sin(c->az), c->dist * se, -c->dist * ce * cos(c->az));
```
- We convert (distance, elevation, azimuth) to xyz. At az = 0 and el = 0
  the camera sits at (0, 0, −dist), looking toward +z. Elevation lifts it
  along y. Azimuth swings it around the y axis.

```c
    vec3 fwd = norm(mul(cam, -1.0));
```
- The forward direction: from the camera to the origin, normalized.

```c
    vec3 right = norm(cross(v3(0, 1, 0), fwd));
    vec3 up = cross(fwd, right);
```
- We build the camera's local axes. `right` is perpendicular to both
  world-up and forward. `up` is perpendicular to forward and right.
  Together `fwd`, `right`, `up` form an orthonormal basis. This breaks
  down if the camera looks exactly along y, where the cross product is
  zero, which is why the live viewer limits elevation to ±80°.

```c
    double tan_half = tan(c->fov_deg * 0.5 * M_PI / 180.0);
    double aspect = (double)W / H;
    double time = c->time;
```
- `tan_half` is the tangent of half the field of view. A ray hitting the
  image's top edge tilts by that angle, so the tangent is the scale factor
  for image-plane coordinates. `aspect` is the width/height ratio (the
  `(double)` cast avoids integer division). `time` is copied into a local
  variable for the loop.

```c
    #pragma omp parallel for schedule(dynamic, 2)
```
- An OpenMP directive: split the loop below across all CPU threads. It's
  ignored, harmlessly, if you compile without `-fopenmp`.
- `dynamic, 2` means threads grab 2 rows at a time as they finish. Rows
  cost very different amounts, because rays that hit the shadow or orbit
  take many steps, so dynamic handing-out balances the load. Static
  splitting would leave some threads idle.

```c
    for (int j = 0; j < H; j++) {
        for (int i = 0; i < W; i++) {
            vec3 acc = v3(0, 0, 0);
```
- Row `j`, column `i`. `acc` accumulates the color samples for this pixel.
  Variables declared inside the parallel loop are private to each thread,
  so there are no race conditions.

```c
            for (int sy = 0; sy < aa; sy++)
                for (int sx = 0; sx < aa; sx++) {
```
- The supersampling loops: `aa × aa` sub-pixel samples.

```c
                    double u = ((i + (sx + 0.5) / aa) / W * 2.0 - 1.0) * aspect * tan_half;
                    double v = (1.0 - (j + (sy + 0.5) / aa) / H * 2.0) * tan_half;
```
- We map the sample position to image-plane coordinates. `i + (sx+0.5)/aa`
  is the sample's x position (with a 0.5 offset to sample sub-pixel
  centers). Dividing by `W` gives 0–1, and `*2 − 1` gives −1 to +1.
  Multiplying by `aspect * tan_half` scales it to the field of view.
- For `v`, the `1.0 −` flips vertically, since image row 0 is at the top
  but "up" is positive.

```c
                    vec3 d = norm(add(fwd, add(mul(right, u), mul(up, v))));
                    acc = add(acc, bh_trace(cam, d, time));
                }
```
- The ray direction is `fwd + u·right + v·up`, normalized. We trace it and
  add the result to `acc`.

```c
            acc = mul(acc, 1.0 / (aa * aa));
```
- We average the samples.

```c
            double ch[3] = {acc.x, acc.y, acc.z};
            for (int k = 0; k < 3; k++) {
                double m = 1.0 - exp(-ch[k] * EXPOSURE);   /* tone map */
                m = pow(m, 1.0 / 2.2);                     /* gamma    */
                img[((size_t)j * W + i) * 3 + k] = (uint8_t)(clampd(m, 0.0, 1.0) * 255.0 + 0.5);
            }
```
- We copy the channels into an array so we can loop over them.
- **Tone mapping** `1 − e^(−x·exposure)`: the raw light values can be far
  above 1 (the Doppler-boosted side especially). This curve is roughly
  linear for small values and smoothly saturates at 1, so bright areas
  roll off instead of clipping.
- **Gamma** `^(1/2.2)`: monitors expect gamma-encoded values, so we
  brighten the mid-tones to display linear light correctly.
- The array index is `(row·W + col)·3 + channel`. The `(size_t)` cast
  avoids integer overflow on huge images. We clamp, scale to 0–255, add
  0.5 to round instead of truncating, and store the byte.

---

## PART 2: `bh_video.c`

```c
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include "bh_core.h"
```
- `stdio.h` provides `fprintf`, `fopen`, `fwrite`, `snprintf`.
  `sys/stat.h` provides `mkdir`. `time.h` provides `time()` and
  `difftime()` for the ETA. `"bh_core.h"` uses quotes, not angle brackets,
  meaning "look in this folder first."

```c
static BHCamera camera_at(double u, double t) {   /* u in [0,1) = loop progress */
    double s = 0.5 * (1.0 - cos(2.0 * M_PI * u)); /* 0 -> 1 -> 0 */
```
- Camera position as a function of `u`, the fraction of the video
  completed. `s` is a smooth "bump" that starts at 0 for u = 0, peaks at 1
  for u = 0.5, and returns to 0 at u = 1. Because it's a cosine, it eases
  in and out with no sudden speed changes.

```c
    BHCamera c;
    c.az = 2.0 * M_PI * u;
    c.el = (4.0 + 44.0 * s) * M_PI / 180.0;
    c.dist = 30.0 - 8.0 * s;
    c.time = t;
    c.fov_deg = 40.0;
    return c;
}
```
- `az` goes once around (0 to 2π). `el` goes 4° → 48° → 4°, converted to
  radians. `dist` goes 30 → 22 → 30, so the camera moves closer while
  looking from higher up. `time` is real elapsed seconds, which drives the
  disk rotation.

```c
int main(int argc, char **argv) {
    int W = argc > 1 ? atoi(argv[1]) : 1280;
    int H = argc > 2 ? atoi(argv[2]) : 720;
    int fps = argc > 3 ? atoi(argv[3]) : 30;
    double secs = argc > 4 ? atof(argv[4]) : 8.0;
    int aa = argc > 5 ? atoi(argv[5]) : 2;
```
- `argc` is the argument count (the program name counts as 1) and `argv`
  holds them as text. Each line says "if the user gave this argument,
  convert it (`atoi` to int, `atof` to double), otherwise use the
  default."

```c
    if (W < 16 || H < 16 || fps < 1 || secs <= 0 || aa < 1) {
        fprintf(stderr, "usage: %s [width height fps seconds aa]\n", argv[0]);
        return 1;
    }
    int n = (int)(fps * secs + 0.5);
```
- Input validation. A nonzero return code tells the shell it failed. `n`
  is the total frame count, rounded to the nearest integer.

```c
    mkdir("frames", 0755);
    uint8_t *img = malloc((size_t)W * H * 3);
    if (!img) { fprintf(stderr, "out of memory\n"); return 1; }
```
- We create the output directory. The `0755` is Unix permissions (owner
  read/write/execute, others read/execute), and if the folder already
  exists the call fails harmlessly and we ignore it. Then we allocate one
  frame's worth of bytes on the heap and check that it worked.

```c
    time_t t0 = time(NULL);
    for (int f = 0; f < n; f++) {
        BHCamera c = camera_at((double)f / n, (double)f / fps);
        bh_render(img, W, H, aa, &c);
```
- We record the start time. Then for each frame `f`, the loop-progress is
  `f/n` (never reaching 1, so the last frame is just before the first, and
  the video loops seamlessly in camera motion) and the time is `f/fps`
  seconds. We render into `img`.

```c
        char path[64];
        snprintf(path, sizeof path, "frames/frame_%04d.ppm", f);
        FILE *fp = fopen(path, "wb");
        if (!fp) { perror(path); return 1; }
        fprintf(fp, "P6\n%d %d\n255\n", W, H);
        fwrite(img, 3, (size_t)W * H, fp);
        fclose(fp);
```
- `snprintf` builds a safe file name like `frames/frame_0007.ppm`
  (`%04d` zero-pads to 4 digits, so file names sort correctly). `"wb"`
  opens for writing in binary mode.
- The PPM (P6) format is a text header, then raw RGB bytes. `P6` is the
  magic number for binary color, followed by width and height and the max
  value 255. `fwrite(ptr, size, count, file)` dumps `W·H` items of 3 bytes
  each.

```c
        double el = difftime(time(NULL), t0);
        double eta = el / (f + 1) * (n - f - 1);
        fprintf(stderr, "\rframe %d/%d  elapsed %.0fs  eta %.0fs   ", f + 1, n, el, eta);
    }
    free(img);
```
- Progress: elapsed seconds, and ETA = (average time per frame so far) ×
  (frames remaining). `\r` returns the cursor to the line start so the
  text updates in place. We print to `stderr` so it's unbuffered. Then we
  free the buffer.

```c
    fprintf(stderr, "\n\nDone. Encode with ONE of:\n"
        "  ffmpeg -framerate %d -i frames/frame_%%04d.ppm -c:v libvpx-vp9 -crf 24 -b:v 0 blackhole.webm\n"
        ...
```
- Prints the encode commands. `%%` in a `printf` format prints a literal
  `%`, which ffmpeg needs for its `%04d` pattern.
- In the ffmpeg commands: `-framerate` sets the input rate (it must come
  before `-i`), `-c:v` picks the video codec, `-crf` is the constant-quality
  level (lower = better and larger), `-b:v 0` tells VP9 to use pure quality
  mode, and `-pix_fmt yuv420p` makes the H.264 file playable everywhere.

---

## PART 3: `bh_live.c`

```c
#include <SDL2/SDL.h>
```
- SDL2 is a library for windows, input and pixel display. Here it gives us
  a window, mouse/keyboard events, and a way to push our pixel buffer to
  the screen.

```c
static BHCamera default_camera(void) {
    BHCamera c = { 0.0, 10.0 * M_PI / 180.0, 30.0, 0.0, 40.0 };
    return c;
}
```
- The starting camera. Positional initialization fills the struct fields
  in order: `az`=0, `el`=10° (in radians), `dist`=30, `time`=0,
  `fov_deg`=40. It's a function because the reset key needs it too.

```c
static void set_title(SDL_Window *w, const char *mode, double ms, int anim) {
    char t[160];
    snprintf(t, sizeof t, "Black hole | %s %.0f ms | drag: orbit  wheel: zoom  space: %s  r: reset",
             mode, ms, anim ? "stop spin" : "spin disk");
    SDL_SetWindowTitle(w, t);
}
```
- We build a status string in a 160-byte buffer (`snprintf` never overruns
  it) and set it as the window title. It shows the render mode, the render
  time in ms, and the controls. It's a cheap way to show performance
  without drawing text.

```c
int main(int argc, char **argv) {
    int W = argc > 1 ? atoi(argv[1]) : 960;
    int H = argc > 2 ? atoi(argv[2]) : 540;
    int S = argc > 3 ? atoi(argv[3]) : 3;
    if (W < 64 || H < 64) { fprintf(stderr, "window too small\n"); return 1; }
    if (S < 1) S = 1;
    int lw = W / S, lh = H / S;
```
- Arguments: width, height, and `S`, the preview divisor. `lw`/`lh` are
  the low-resolution preview dimensions. At 1920×1080 with S = 4, that's
  480×270, which is 16× fewer rays.

```c
    if (SDL_Init(SDL_INIT_VIDEO) != 0) { ... return 1; }
```
- Initializes SDL's video subsystem. It returns 0 on success and nonzero
  on failure, in which case `SDL_GetError()` says why.

```c
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");   /* linear upscaling */
```
- A hint that scaled textures should use linear (smooth) interpolation,
  not nearest-neighbor. Without it the low-res preview would look blocky.
  It must be set before creating the textures.

```c
    SDL_Window *win = SDL_CreateWindow("Black hole", SDL_WINDOWPOS_CENTERED,
                                       SDL_WINDOWPOS_CENTERED, W, H, SDL_WINDOW_SHOWN);
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, 0);
```
- We create the window (title, position centered on the screen, size,
  visible). Then we create a renderer, SDL's drawing context for that
  window. `-1` means "use the first driver that works" and `0` means no
  special flags.

```c
    SDL_Texture *tex_hi = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB24,
                                            SDL_TEXTUREACCESS_STREAMING, W, H);
    SDL_Texture *tex_lo = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB24,
                                            SDL_TEXTUREACCESS_STREAMING, lw, lh);
```
- A texture is an image in GPU memory. `RGB24` means 3 bytes per pixel, in
  exactly the layout our renderer writes. `STREAMING` means we plan to
  overwrite its contents frequently from the CPU. We make two, one per
  resolution, so we never have to recreate them.

```c
    uint8_t *buf_hi = malloc((size_t)W * H * 3);
    uint8_t *buf_lo = malloc((size_t)lw * lh * 3);
    if (!tex_hi || !tex_lo || !buf_hi || !buf_lo) { ...; return 1; }
```
- The CPU-side pixel buffers our ray tracer writes into, then a check that
  none of the four allocations failed.

```c
    BHCamera cam = default_camera();
    int running = 1, dirty = 1, hi_done = 0, animate = 0;
    Uint32 last_input = SDL_GetTicks(), last_tick = last_input;
    double freq = (double)SDL_GetPerformanceFrequency();
```
- Program state:
  - `running`: the loop continues while it's 1.
  - `dirty`: the image is stale and needs a preview render. It starts at
    1 so we draw immediately.
  - `hi_done`: the full-res image for the current view exists.
  - `animate`: the disk is spinning.
  - `last_input` / `last_tick`: millisecond timestamps (`SDL_GetTicks`
    counts ms since SDL started).
  - `freq`: ticks per second of the high-precision timer, used to time
    renders.

```c
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
```
- The main loop. Each pass first drains the event queue: `SDL_PollEvent`
  fetches one pending event (mouse, keyboard, window) into `e` and returns
  0 when the queue is empty.

```c
            case SDL_QUIT:
                running = 0;
                break;
```
- The user clicked the window's close button.

```c
            case SDL_KEYDOWN:
                if (e.key.keysym.sym == SDLK_ESCAPE || e.key.keysym.sym == SDLK_q) running = 0;
                else if (e.key.keysym.sym == SDLK_SPACE) animate = !animate;
                else if (e.key.keysym.sym == SDLK_r) { double t = cam.time; cam = default_camera(); cam.time = t; }
                dirty = 1; hi_done = 0; last_input = SDL_GetTicks();
                break;
```
- Keyboard handling. Esc or Q quits, Space toggles the spin (`!` flips
  0↔1), and R resets the camera while preserving `cam.time`, so the disk
  pattern doesn't jump. For any key, we mark the image stale, mark the
  full-res image invalid, and record the time of this input.

```c
            case SDL_MOUSEMOTION:
                if (e.motion.state & SDL_BUTTON_LMASK) {
                    cam.az -= e.motion.xrel * 0.006;
                    cam.el = clampd(cam.el + e.motion.yrel * 0.006, -1.4, 1.4);
                    dirty = 1; hi_done = 0; last_input = SDL_GetTicks();
                }
                break;
```
- Mouse movement counts only while the left button is held
  (`state & LMASK`). `xrel` and `yrel` are the pixel deltas since the last
  event. We convert them to angles at 0.006 rad ≈ 0.34° per pixel, so a
  1000-pixel drag is about 340°. Elevation is clamped to ±1.4 rad (±80°)
  to avoid the straight-up/down pole, where the camera-basis cross product
  becomes degenerate.

```c
            case SDL_MOUSEWHEEL:
                cam.dist = clampd(cam.dist * pow(0.9, e.wheel.y), 5.0, 50.0);
                dirty = 1; hi_done = 0; last_input = SDL_GetTicks();
                break;
```
- `wheel.y` is +1 per notch up, −1 per notch down. `0.9^y` multiplies
  distance by 0.9 (zoom in 10%) or 1.11 (zoom out). Multiplicative zoom
  feels uniform at any distance. Clamped to 5–50 rs: closer than 5 puts
  you inside the disk region, and beyond 50 approaches the escape radius.

```c
        Uint32 now = SDL_GetTicks();
        double dt = (now - last_tick) / 1000.0;
        last_tick = now;
        if (animate) {
            cam.time += clampd(dt, 0.0, 0.1);
            dirty = 1; hi_done = 0;
        }
```
- We compute the seconds since the last loop pass. If animating, we
  advance the disk time by that much, capped at 0.1 s so one slow frame
  doesn't cause a big jump, and force a re-render every pass.

```c
        if (dirty) {
            Uint64 a = SDL_GetPerformanceCounter();
            bh_render(buf_lo, lw, lh, 1, &cam);
            SDL_UpdateTexture(tex_lo, NULL, buf_lo, lw * 3);
            SDL_RenderClear(ren);
            SDL_RenderCopy(ren, tex_lo, NULL, NULL);
            SDL_RenderPresent(ren);
            set_title(win, "preview", (SDL_GetPerformanceCounter() - a) * 1000.0 / freq, animate);
            dirty = 0;
```
- **Preview render.** We note the start time, render at low resolution
  with no antialiasing (`aa = 1`), upload the pixels to the texture
  (`lw*3` is the bytes per row, the "pitch"), clear the screen, and copy
  the whole texture (`NULL, NULL` = full source to full window) so SDL
  stretches it to window size. `SDL_RenderPresent` shows the frame. Then
  we show the elapsed ms in the title and clear `dirty`.

```c
        } else if (!animate && !hi_done && now - last_input > 250) {
            Uint64 a = SDL_GetPerformanceCounter();
            bh_render(buf_hi, W, H, 2, &cam);
            ...
            hi_done = 1;
```
- **Full-res render.** It happens only if we're not animating, haven't
  yet rendered full-res for this view, and 250 ms have passed since the
  last input. It's the same steps at full resolution. (In your build the
  antialiasing argument is `1`, not `2`, after the earlier edit.)
  `hi_done = 1` prevents re-rendering the same view forever.

```c
        } else {
            SDL_Delay(5);
        }
    }
```
- If there's nothing to do, sleep 5 ms so the loop doesn't spin at 100%
  of a core doing nothing.

```c
    free(buf_hi); free(buf_lo);
    SDL_DestroyTexture(tex_hi); SDL_DestroyTexture(tex_lo);
    SDL_DestroyRenderer(ren); SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
```
- Cleanup in reverse order of creation. The OS would reclaim it all at
  exit anyway, but doing it explicitly is good practice.

---

## PART 4: `blackhole.c` (the first version)

It's the same physics as `bh_core.h`, in one file. The differences:

- `AA`, `CAM_DIST`, `FOV_DEG` are `#define` constants, not runtime
  parameters or a struct.
- `trace()` has no `time` argument, so the disk texture is static.
- The camera comes from just the elevation argument
  (`cam = (0, d·sin(el), −d·cos(el))`), with no azimuth.
- `main` does the pixel loop and the PPM write in one place. That's what
  `bh_render` and `bh_video`'s file writing later split apart.
- It writes one fixed file, `blackhole.ppm`, and prints a single
  "wrote…" line.

If you understood Part 1, you understand the trace and render code in this
file too.
