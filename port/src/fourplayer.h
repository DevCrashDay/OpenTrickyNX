/*
 * fourplayer -- four-player split screen, a first test (OT_FOUR_PLAYER=1).
 *
 * The race already keeps four of most per-player things: the setup has four
 * player slots (0x1DBF50+0x3344, 0x44 each), the roster ("AI", 0x4E4 at
 * lvl+0x1C) four Controllers (+0x6C) and four Players (+0x8C, humans counted
 * at +0x7C), the level four view blocks (lvl+0x90 + i*0x80: x, y, w, h, the
 * Camera at +0x10, the rider it follows at +0x14), and the input manager
 * ([app+0x28]) reads all four pad ports. What stops at two:
 *
 *   - the view count: 0xAC240 maps the humans (0..3) to 1 or 2 views through
 *     a jump table (lvl+0x298, copy at +0x294). Hooked at 0xAC2AD, the part it
 *     falls into, where the cameras are made: 3 or 4 humans give 4 views. With
 *     3 humans the fourth view follows another rider (usually a computer one),
 *     as the title does for a view with no human.
 *   - the layout: 0xAB270 writes rectangles for 1 or 2 views only. With 3-4,
 *     a 2x2 grid with the title's 2-pixel gaps (P1 top left, P2 top right,
 *     P3 bottom left, P4 bottom right).
 *   - the camera mode: only views 0 and 1 get one (P1 [0x1DD864], P2
 *     [0x1DD868]); views 2 and 3 get P2's, through 0x7B450 after the cameras
 *     are made.
 *   - the race HUD layout mode [0x1E9418] (0xC3950: 0 full, 1 top, 2 bottom
 *     half): 0 for 3-4 views, a quarter has the shape of the whole screen.
 *   - joining: the frontend makes at most two humans. At level start (0xAC9B0,
 *     before anything counts them) each further connected pad turns a
 *     computer rider record (0x1DE900 + i*0x98, pad port at +0x7D, -1 for a
 *     computer) into a human, when the race already has two humans; undone
 *     when the level is deleted (0xAC960). Such a player has no setup slot:
 *     0x697A0 (port -> slot) answers a free slot number for it, so the views
 *     take it in order, and that slot gets a copy of the last player's slot
 *     (its port left at -1) for the code that reads a player's slot data;
 *     put back with the rider records.
 *
 * Two structures hold exactly two views and are kept to them:
 *   - LensFX (0x50E0, Draw 0xE2450): per view and frame buffer it keeps the
 *     viewport, matrices and occlusion lists at (view + buffer*2) and plays
 *     them back from a device callback (0xE13F0) that loops over the views
 *     recorded. Views 3 and 4 skip its Draw (no lens flare there), and views
 *     1 and 2 see a view count of 2 while it records, so nothing past its
 *     arrays is written or read.
 *   - Overlays (0x94, vtable 0x19C5D0): one 0x38-byte entry per view at
 *     +0x24; 0xD2C20 writes entry+0x58 of the current view, 0xCB470 arms
 *     the entry of a given view, 0xD2D80 (the finish banner) reads it. For
 *     views 3 and 4 the first two are skipped and the banner too
 *     (hud_anchor's hook asks fourplayer_skip_view()).
 * Left alone: 0xB56C0 adds one HUD part when there are exactly 2 views,
 * 0xC6F59 moves a HUD line for half-height views (a quarter is not one),
 * 0xD0E27 tests the rider count, not the views.
 *
 * Not checked: the view command lists (2048 records each, no bound) and the
 * frame's vertex pools take four views' worth; XBOX_DRAW_DISTANCE_LOG=1
 * prints the most used.
 *
 *   OT_FOUR_PLAYER=1   on; unset or 0, every hook passes straight through.
 */
#ifndef FORK_FOURPLAYER_H
#define FORK_FOURPLAYER_H

extern int g_fourplayer;

void fourplayer_init(void);          /* reads OT_FOUR_PLAYER */
int  fourplayer_skip_view(void);     /* 1: a 3rd / 4th view is being drawn */
void (*fourplayer_lookup(unsigned int xbox_va))(void);

#endif
