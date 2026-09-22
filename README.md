# Simulating-Blackhole-in-C
Hi 
Here's the full setup, from a fresh ~/Downloads folder with all files in it, to running each tool .

# 0) One time setup :  
  cd ~/Downloads
  Make sure these four files are all in this folder: blackhole.c, bh_core.h, bh_video.c, bh_live.c.

  Install the compiler and SDL2 with : sudo dnf install gcc SDL2-devel

# 1) Single still image (blackhole.c)
   gcc -O3 -march=native -fopenmp blackhole.c -o blackhole -lm
   ./blackhole 1920 1080 8
     #to open the file at resolution of your choice , The three numbers are width, height, camera elevation in degrees.   
   magick blackhole.ppm blackhole.png
   xdg-open blackhole.png
   
# 3) Live Interctive Viewer (bh_video.c)
   gcc -O3 -march=native -fopenmp bh_video.c -o bh_video -lm
   ./bh_video 1920 1080 30 8 2
     #Numbers are width, height, fps, seconds, antialiasing. This writes 240 frames into frames/ and can take several minutes; it prints an ETA as it runs. When      it finishes, encode the frames into a video:
     ffmpeg -framerate 30 -i frames/frame_%04d.ppm -c:v libvpx-vp9 -crf 24 -b:v 0 blackhole.webm
     xdg-open blackhole.webm

   # Quick Reference
  # still image
gcc -O3 -march=native -fopenmp blackhole.c -o blackhole -lm && ./blackhole 1920 1080 8 && magick blackhole.ppm blackhole.png

   # live viewer
gcc -O3 -march=native -fopenmp bh_live.c -o bh_live $(pkg-config --cflags --libs sdl2) -lm && ./bh_live 1920 1080 4

   # video
gcc -O3 -march=native -fopenmp bh_video.c -o bh_video -lm && ./bh_video 1920 1080 30 8 2 && ffmpeg -framerate 30 -i frames/frame_%04d.ppm -c:v libvpx-vp9 -  crf 24 -b:v 0 blackhole.webm
