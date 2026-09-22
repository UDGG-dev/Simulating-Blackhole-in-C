/*
 * bh_live.c - interactive black hole viewer (CPU ray tracing, SDL2 window).
 *
 * Build (Fedora):  sudo dnf install SDL2-devel gcc
 *   gcc -O3 -march=native -fopenmp bh_live.c -o bh_live $(pkg-config --cflags --libs sdl2) -lm
 * Run:  ./bh_live [width height lowres_divisor]
 *       ./bh_live 960 540 3        (defaults)
 *
 * Controls:
 *   left-drag   orbit the camera
 *   mouse wheel zoom in / out
 *   SPACE       toggle disk rotation animation
 *   R           reset camera
 *   Q / ESC     quit
 *
 * While you interact (or animate) it renders at 1/divisor resolution for
 * speed. About 250 ms after you stop, it re-renders once at full resolution
 * with 2x2 supersampling. Increase the divisor if it feels laggy.
 */
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include "bh_core.h"

static BHCamera default_camera(void) {
    BHCamera c = { 0.0, 10.0 * M_PI / 180.0, 30.0, 0.0, 40.0 };
    return c;
}

static void set_title(SDL_Window *w, const char *mode, double ms, int anim) {
    char t[160];
    snprintf(t, sizeof t, "Black hole | %s %.0f ms | drag: orbit  wheel: zoom  space: %s  r: reset",
             mode, ms, anim ? "stop spin" : "spin disk");
    SDL_SetWindowTitle(w, t);
}

int main(int argc, char **argv) {
    int W = argc > 1 ? atoi(argv[1]) : 960;
    int H = argc > 2 ? atoi(argv[2]) : 540;
    int S = argc > 3 ? atoi(argv[3]) : 3;
    if (W < 64 || H < 64) { fprintf(stderr, "window too small\n"); return 1; }
    if (S < 1) S = 1;
    int lw = W / S, lh = H / S;

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");   /* linear upscaling */
    SDL_Window *win = SDL_CreateWindow("Black hole", SDL_WINDOWPOS_CENTERED,
                                       SDL_WINDOWPOS_CENTERED, W, H, SDL_WINDOW_SHOWN);
    if (!win) { fprintf(stderr, "window: %s\n", SDL_GetError()); return 1; }
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, 0);
    if (!ren) { fprintf(stderr, "renderer: %s\n", SDL_GetError()); return 1; }
    SDL_Texture *tex_hi = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB24,
                                            SDL_TEXTUREACCESS_STREAMING, W, H);
    SDL_Texture *tex_lo = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB24,
                                            SDL_TEXTUREACCESS_STREAMING, lw, lh);
    uint8_t *buf_hi = malloc((size_t)W * H * 3);
    uint8_t *buf_lo = malloc((size_t)lw * lh * 3);
    if (!tex_hi || !tex_lo || !buf_hi || !buf_lo) {
        fprintf(stderr, "allocation failed\n");
        return 1;
    }

    BHCamera cam = default_camera();
    int running = 1, dirty = 1, hi_done = 0, animate = 0;
    Uint32 last_input = SDL_GetTicks(), last_tick = last_input;
    double freq = (double)SDL_GetPerformanceFrequency();

    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_QUIT:
                running = 0;
                break;
            case SDL_KEYDOWN:
                if (e.key.keysym.sym == SDLK_ESCAPE || e.key.keysym.sym == SDLK_q) running = 0;
                else if (e.key.keysym.sym == SDLK_SPACE) animate = !animate;
                else if (e.key.keysym.sym == SDLK_r) { double t = cam.time; cam = default_camera(); cam.time = t; }
                dirty = 1; hi_done = 0; last_input = SDL_GetTicks();
                break;
            case SDL_MOUSEMOTION:
                if (e.motion.state & SDL_BUTTON_LMASK) {
                    cam.az -= e.motion.xrel * 0.006;
                    cam.el = clampd(cam.el + e.motion.yrel * 0.006, -1.4, 1.4);
                    dirty = 1; hi_done = 0; last_input = SDL_GetTicks();
                }
                break;
            case SDL_MOUSEWHEEL:
                cam.dist = clampd(cam.dist * pow(0.9, e.wheel.y), 5.0, 50.0);
                dirty = 1; hi_done = 0; last_input = SDL_GetTicks();
                break;
            }
        }

        Uint32 now = SDL_GetTicks();
        double dt = (now - last_tick) / 1000.0;
        last_tick = now;
        if (animate) {
            cam.time += clampd(dt, 0.0, 0.1);
            dirty = 1; hi_done = 0;
        }

        if (dirty) {
            Uint64 a = SDL_GetPerformanceCounter();
            bh_render(buf_lo, lw, lh, 1, &cam);
            SDL_UpdateTexture(tex_lo, NULL, buf_lo, lw * 3);
            SDL_RenderClear(ren);
            SDL_RenderCopy(ren, tex_lo, NULL, NULL);
            SDL_RenderPresent(ren);
            set_title(win, "preview", (SDL_GetPerformanceCounter() - a) * 1000.0 / freq, animate);
            dirty = 0;
        } else if (!animate && !hi_done && now - last_input > 250) {
            Uint64 a = SDL_GetPerformanceCounter();
            bh_render(buf_hi, W, H, 1, &cam);
            SDL_UpdateTexture(tex_hi, NULL, buf_hi, W * 3);
            SDL_RenderClear(ren);
            SDL_RenderCopy(ren, tex_hi, NULL, NULL);
            SDL_RenderPresent(ren);
            set_title(win, "full-res", (SDL_GetPerformanceCounter() - a) * 1000.0 / freq, animate);
            hi_done = 1;
        } else {
            SDL_Delay(5);
        }
    }

    free(buf_hi); free(buf_lo);
    SDL_DestroyTexture(tex_hi); SDL_DestroyTexture(tex_lo);
    SDL_DestroyRenderer(ren); SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
