/*
 * bh_video.c - render an orbiting-camera black hole animation as PPM frames.
 *
 * Build:  gcc -O3 -march=native -fopenmp bh_video.c -o bh_video -lm
 * Run:    ./bh_video [width height fps seconds aa]
 *         ./bh_video 1280 720 30 8 2        (defaults)
 * Output: frames/frame_0000.ppm, frames/frame_0001.ppm, ...
 *
 * Camera path (one full loop): orbits 360 degrees around the spin axis while
 * the elevation swings from a near edge-on view up to ~48 degrees and back,
 * and the camera drifts slightly closer at the top of the arc. The disk
 * pattern rotates over time.
 *
 * Encode with ffmpeg (see the commands printed at the end).
 */
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include "bh_core.h"

static BHCamera camera_at(double u, double t) {   /* u in [0,1) = loop progress */
    double s = 0.5 * (1.0 - cos(2.0 * M_PI * u)); /* 0 -> 1 -> 0 */
    BHCamera c;
    c.az = 2.0 * M_PI * u;
    c.el = (4.0 + 44.0 * s) * M_PI / 180.0;
    c.dist = 30.0 - 8.0 * s;
    c.time = t;
    c.fov_deg = 40.0;
    return c;
}

int main(int argc, char **argv) {
    int W = argc > 1 ? atoi(argv[1]) : 1280;
    int H = argc > 2 ? atoi(argv[2]) : 720;
    int fps = argc > 3 ? atoi(argv[3]) : 30;
    double secs = argc > 4 ? atof(argv[4]) : 8.0;
    int aa = argc > 5 ? atoi(argv[5]) : 2;
    if (W < 16 || H < 16 || fps < 1 || secs <= 0 || aa < 1) {
        fprintf(stderr, "usage: %s [width height fps seconds aa]\n", argv[0]);
        return 1;
    }
    int n = (int)(fps * secs + 0.5);

    mkdir("frames", 0755);
    uint8_t *img = malloc((size_t)W * H * 3);
    if (!img) { fprintf(stderr, "out of memory\n"); return 1; }

    time_t t0 = time(NULL);
    for (int f = 0; f < n; f++) {
        BHCamera c = camera_at((double)f / n, (double)f / fps);
        bh_render(img, W, H, aa, &c);

        char path[64];
        snprintf(path, sizeof path, "frames/frame_%04d.ppm", f);
        FILE *fp = fopen(path, "wb");
        if (!fp) { perror(path); return 1; }
        fprintf(fp, "P6\n%d %d\n255\n", W, H);
        fwrite(img, 3, (size_t)W * H, fp);
        fclose(fp);

        double el = difftime(time(NULL), t0);
        double eta = el / (f + 1) * (n - f - 1);
        fprintf(stderr, "\rframe %d/%d  elapsed %.0fs  eta %.0fs   ", f + 1, n, el, eta);
    }
    free(img);

    fprintf(stderr,
        "\n\nDone. Encode with ONE of:\n"
        "  ffmpeg -framerate %d -i frames/frame_%%04d.ppm -c:v libvpx-vp9 -crf 24 -b:v 0 blackhole.webm\n"
        "  ffmpeg -framerate %d -i frames/frame_%%04d.ppm -c:v libx264 -pix_fmt yuv420p -crf 16 blackhole.mp4\n"
        "(the second needs the full ffmpeg from RPM Fusion on Fedora)\n", fps, fps);
    return 0;
}
