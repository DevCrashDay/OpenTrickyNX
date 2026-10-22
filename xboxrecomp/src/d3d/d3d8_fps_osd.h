/*
 * d3d8_fps_osd.h - the frame rate drawn over the picture
 *
 * Where there is no title bar to show it in (the Switch), "Show frame rate"
 * draws the count in the top-left corner of the picture instead. The text is
 * a 3x5 block font turned into rectangles, which each backend fills with its
 * own clear (glScissor + glClear, vkCmdClearAttachments): no texture, shader
 * or pipeline of its own, and nothing for the frame's state to restore.
 */
#ifndef D3D8_FPS_OSD_H
#define D3D8_FPS_OSD_H

#include <stdio.h>
#include <string.h>

typedef struct { int x, y, w, h; } FpsOsdRect;     /* y from the top */

enum { FPS_OSD_MAX_RECTS = 96 };

/* Presents per second over the last half second or so, from the backend's
 * frame counter; -1 until the first measurement. */
static inline int fps_osd_rate(long frames, unsigned now_ms)
{
    static long f0;
    static unsigned t0;
    static int fps = -1;
    if (!t0) { t0 = now_ms ? now_ms : 1; f0 = frames; return fps; }
    if (now_ms - t0 >= 500) {
        fps = (int)(((frames - f0) * 1000L + (long)(now_ms - t0) / 2) / (long)(now_ms - t0));
        t0 = now_ms ? now_ms : 1;
        f0 = frames;
    }
    return fps;
}

/* Rows of a glyph, top first, bit 2 = left column. */
static inline const unsigned char *fps_osd_glyph(char c)
{
    static const unsigned char digits[10][5] = {
        { 7, 5, 5, 5, 7 }, { 2, 6, 2, 2, 7 }, { 7, 1, 7, 4, 7 }, { 7, 1, 7, 1, 7 }, { 5, 5, 7, 1, 1 },
        { 7, 4, 7, 1, 7 }, { 7, 4, 7, 5, 7 }, { 7, 1, 1, 1, 1 }, { 7, 5, 7, 5, 7 }, { 7, 5, 7, 1, 7 },
    };
    static const unsigned char f[5] = { 7, 4, 6, 4, 4 }, p[5] = { 6, 5, 6, 4, 4 },
                               s[5] = { 7, 4, 7, 1, 7 }, dash[5] = { 0, 0, 7, 0, 0 },
                               blank[5] = { 0, 0, 0, 0, 0 };
    if (c >= '0' && c <= '9') return digits[c - '0'];
    switch (c) {
    case 'F': return f;
    case 'P': return p;
    case 'S': return s;
    case '-': return dash;
    default:  return blank;
    }
}

/* The rectangles for the frame rate at (x, y) of a picture h pixels high:
 * out[0] the dark box behind, then the text. Returns how many. */
static inline int fps_osd_rects(int fps, int x, int y, int h, FpsOsdRect *out)
{
    char text[16];
    int px = h / 144 > 2 ? h / 144 : 2;         /* 5 px a dot at 720p, 7 at 1080p */
    int n = 1, len, i, row, col;
    if (fps >= 0) snprintf(text, sizeof text, "%d FPS", fps > 999 ? 999 : fps);
    else          snprintf(text, sizeof text, "-- FPS");
    len = (int)strlen(text);
    x += px * 2;
    y += px * 2;
    out[0].x = x;
    out[0].y = y;
    out[0].w = (len * 4 + 1) * px;
    out[0].h = 7 * px;
    for (i = 0; i < len; i++) {
        const unsigned char *g = fps_osd_glyph(text[i]);
        for (row = 0; row < 5; row++)
            for (col = 0; col < 3; col++) {
                int run = 0;
                if (!(g[row] & (4 >> col))) continue;
                while (col + run < 3 && (g[row] & (4 >> (col + run)))) run++;
                if (n < FPS_OSD_MAX_RECTS) {
                    out[n].x = x + (1 + i * 4 + col) * px;
                    out[n].y = y + (1 + row) * px;
                    out[n].w = run * px;
                    out[n].h = px;
                    n++;
                }
                col += run - 1;
            }
    }
    return n;
}

#endif /* D3D8_FPS_OSD_H */
