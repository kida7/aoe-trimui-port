/* xmirror: copy the biggest viewable X11 window of an (Xwayland) server to the real screen via SDL2,
 * draw a normal arrow mouse cursor, and provide an on-screen (virtual) keyboard driven by the gamepad.
 * Talks raw X11 protocol over the unix socket (no Xlib), loads SDL2 with dlopen.
 *
 * usage: xmirror [display-number=0]
 * env:   XMIRROR_SDL (path to libSDL2), XMIRROR_FPS (default 20), XMIRROR_CURSOR_SCALE (default 2),
 *        XMIRROR_PAD (evdev path of the pad, default: device named "TRIMUI Player1")
 * Controls come from an INI file (env XMIRROR_CONFIG, default ./controls.ini), section [osk]:
 *   toggle/type/backspace/space/shift/enter/close = button[+button...][, alternative...]
 *   buttons: a b x y l1 r1 l2 r2 l3 r3 select(back) start menu. Defaults below.
 * Virtual keyboard: R3 (right stick click) toggles it. While open: d-pad / left stick = move, A = type key,
 *   B = backspace, X = space, Y = shift (one shot), Start = enter, Select / L3 = close.
 *   gptokeyb is paused (SIGSTOP) while the keyboard is open so the pad does not also move the mouse.
 * Keys are typed through a uinput keyboard created at startup (start xmirror before the compositor so it is seen).
 * Cursor: draws the real X cursor (XFixes) so a game that hides it and draws its own gets one cursor; [display] cursor = real|arrow|none.
 * Test FIFO /tmp/xmirror.cmd: "toggle", "cursor", "type TEXT", "warp X Y", "up|down|left|right|ok|back|space|shift|enter|close".
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>

static int fd = -1, bad = 0;

static int rd(void *b, size_t n) {
    uint8_t *p = b;
    while (n) { ssize_t r = read(fd, p, n); if (r <= 0) { if (r < 0 && errno == EINTR) continue; bad = 1; return -1; } p += r; n -= r; }
    return 0;
}
static int wr(const void *b, size_t n) {
    const uint8_t *p = b;
    while (n) { ssize_t r = write(fd, p, n); if (r <= 0) { if (r < 0 && errno == EINTR) continue; bad = 1; return -1; } p += r; n -= r; }
    return 0;
}
static uint16_t g16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t g32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static void p16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void p32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

/* read one reply (skipping events); returns malloc'ed buffer (32 + extra), NULL on error reply/IO error */
static uint8_t *reply(size_t *len_out) {
    for (;;) {
        uint8_t h[32];
        if (rd(h, 32)) return NULL;
        if (h[0] == 0) return NULL;          /* X error */
        if (h[0] != 1) continue;             /* event */
        size_t extra = (size_t)g32(h + 4) * 4;
        uint8_t *b = malloc(32 + extra + 1);
        memcpy(b, h, 32);
        if (extra && rd(b + 32, extra)) { free(b); return NULL; }
        if (len_out) *len_out = 32 + extra;
        return b;
    }
}
static uint8_t *req(const uint8_t *r, size_t n, size_t *l) { if (wr(r, n)) return NULL; return reply(l); }

static uint8_t *simple(uint8_t op, uint8_t data, uint32_t win, size_t *l) {
    uint8_t r[8] = { op, data, 2, 0 }; p32(r + 4, win); return req(r, 8, l);
}

static int connect_x(int dpy) {
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    snprintf(a.sun_path, sizeof a.sun_path, "/tmp/.X11-unix/X%d", dpy);
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof a)) { if (fd >= 0) close(fd); fd = -1; return -1; }
    uint8_t h[12] = { 'l', 0, 11, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    if (wr(h, 12)) return -1;
    uint8_t s[8];
    if (rd(s, 8)) return -1;
    size_t n = (size_t)g16(s + 6) * 4;
    uint8_t *d = malloc(n);
    if (rd(d, n)) return -1;
    if (s[0] != 1) { fprintf(stderr, "xmirror: X refused: %.*s\n", s[1], d); return -1; }
    uint16_t vlen = g16(d + 16); uint8_t nfmt = d[21];
    size_t off = 32 + ((vlen + 3) & ~3) + (size_t)nfmt * 8;
    uint32_t root = g32(d + off);
    free(d);
    return (int)root;   /* root window id */
}

struct geo { int x, y, w, h, depth; };
static int getgeo(uint32_t win, struct geo *g) {
    uint8_t *r = simple(14, 0, win, NULL);
    if (!r) return -1;
    g->depth = r[1]; g->x = (int16_t)g16(r + 12); g->y = (int16_t)g16(r + 14); g->w = g16(r + 16); g->h = g16(r + 18);
    free(r); return 0;
}
static int viewable(uint32_t win) {
    uint8_t *r = simple(3, 0, win, NULL);
    if (!r) return 0;
    int v = r[26] == 2; free(r); return v;
}
/* biggest viewable child of root */
static uint32_t pick_window(uint32_t root, struct geo *best) {
    size_t l; uint8_t *r = simple(15, 0, root, &l);
    if (!r) return 0;
    uint32_t nch = g16(r + 16), bw = 0; long ba = 0;
    for (uint32_t i = 0; i < nch; i++) {
        uint32_t w = g32(r + 32 + i * 4); struct geo g;
        if (!viewable(w) || getgeo(w, &g)) continue;
        long a = (long)g.w * g.h;
        if (a > ba && g.w >= 160 && g.h >= 120) { ba = a; bw = w; *best = g; }
    }
    free(r); return bw;
}
static int pointer(uint32_t root, int *px, int *py) {
    uint8_t *r = simple(38, 0, root, NULL);
    if (!r) return -1;
    *px = (int16_t)g16(r + 16); *py = (int16_t)g16(r + 18); free(r); return 0;
}

/* ---------------- SDL (dlopen) ---------------- */
static void *S;
static int (*SDL_Init)(uint32_t);
static void *(*SDL_CreateWindow)(const char *, int, int, int, int, uint32_t);
static void *(*SDL_CreateRenderer)(void *, int, uint32_t);
static void *(*SDL_CreateTexture)(void *, uint32_t, int, int, int);
static int (*SDL_UpdateTexture)(void *, const void *, const void *, int);
static int (*SDL_RenderCopy)(void *, void *, const void *, const void *);
static void (*SDL_RenderPresent)(void *);
static int (*SDL_RenderClear)(void *);
static const char *(*SDL_GetError)(void);
static int (*SDL_PollEvent)(void *);
static int (*SDL_SetHint)(const char *, const char *);
static int (*SDL_ShowCursor)(int);

static int load_sdl(void) {
    const char *p = getenv("XMIRROR_SDL"); if (!p) p = "/usr/lib64/libSDL2.so";
    S = dlopen(p, RTLD_NOW | RTLD_GLOBAL);
    if (!S) { fprintf(stderr, "xmirror: dlopen %s: %s\n", p, dlerror()); return -1; }
#define L(n) do { *(void **)&n = dlsym(S, #n); if (!n) { fprintf(stderr, "xmirror: missing %s\n", #n); return -1; } } while (0)
    L(SDL_Init); L(SDL_CreateWindow); L(SDL_CreateRenderer); L(SDL_CreateTexture); L(SDL_UpdateTexture);
    L(SDL_RenderCopy); L(SDL_RenderPresent); L(SDL_RenderClear); L(SDL_GetError); L(SDL_PollEvent); L(SDL_SetHint); L(SDL_ShowCursor);
    return 0;
}

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

/* ---------------- drawing helpers (frame is ARGB8888 / BGRX bytes, w*h uint32) ---------------- */
static uint32_t *F; static int FW, FH;
static void blend(int x, int y, uint32_t rgb, int a) {            /* a: 0..255 */
    if (x < 0 || y < 0 || x >= FW || y >= FH) return;
    uint32_t *p = &F[y * FW + x], d = *p;
    uint32_t r = (((rgb >> 16) & 255) * a + ((d >> 16) & 255) * (255 - a)) / 255;
    uint32_t g = (((rgb >> 8) & 255) * a + ((d >> 8) & 255) * (255 - a)) / 255;
    uint32_t b = ((rgb & 255) * a + (d & 255) * (255 - a)) / 255;
    *p = 0xff000000u | (r << 16) | (g << 8) | b;
}
static void rect(int x, int y, int w, int h, uint32_t rgb, int a) {
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) blend(x + i, y + j, rgb, a);
}
static void frame_rect(int x, int y, int w, int h, int t, uint32_t rgb) {
    rect(x, y, w, t, rgb, 255); rect(x, y + h - t, w, t, rgb, 255); rect(x, y, t, h, rgb, 255); rect(x + w - t, y, t, h, rgb, 255);
}

/* classic 5x7 font, ASCII 0x20..0x5A (column-major, LSB = top row) */
static const uint8_t font5x7[][5] = {
 {0,0,0,0,0},{0,0,0x5F,0,0},{0,7,0,7,0},{0x14,0x7F,0x14,0x7F,0x14},{0x24,0x2A,0x7F,0x2A,0x12},{0x23,0x13,8,0x64,0x62},{0x36,0x49,0x56,0x20,0x50},{0,8,7,3,0},
 {0,0x1C,0x22,0x41,0},{0,0x41,0x22,0x1C,0},{0x2A,0x1C,0x7F,0x1C,0x2A},{8,8,0x3E,8,8},{0,0x80,0x70,0x30,0},{8,8,8,8,8},{0,0,0x60,0x60,0},{0x20,0x10,8,4,2},
 {0x3E,0x51,0x49,0x45,0x3E},{0,0x42,0x7F,0x40,0},{0x72,0x49,0x49,0x49,0x46},{0x21,0x41,0x49,0x4D,0x33},{0x18,0x14,0x12,0x7F,0x10},{0x27,0x45,0x45,0x45,0x39},{0x3C,0x4A,0x49,0x49,0x31},{0x41,0x21,0x11,9,7},
 {0x36,0x49,0x49,0x49,0x36},{0x46,0x49,0x49,0x29,0x1E},{0,0,0x14,0,0},{0,0x40,0x34,0,0},{0,8,0x14,0x22,0x41},{0x14,0x14,0x14,0x14,0x14},{0,0x41,0x22,0x14,8},{2,1,0x59,9,6},
 {0x3E,0x41,0x5D,0x59,0x4E},{0x7C,0x12,0x11,0x12,0x7C},{0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22},{0x7F,0x41,0x41,0x41,0x3E},{0x7F,0x49,0x49,0x49,0x41},{0x7F,9,9,9,1},{0x3E,0x41,0x41,0x51,0x73},
 {0x7F,8,8,8,0x7F},{0,0x41,0x7F,0x41,0},{0x20,0x40,0x41,0x3F,1},{0x7F,8,0x14,0x22,0x41},{0x7F,0x40,0x40,0x40,0x40},{0x7F,2,0x1C,2,0x7F},{0x7F,4,8,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},
 {0x7F,9,9,9,6},{0x3E,0x41,0x51,0x21,0x5E},{0x7F,9,0x19,0x29,0x46},{0x26,0x49,0x49,0x49,0x32},{3,1,0x7F,1,3},{0x3F,0x40,0x40,0x40,0x3F},{0x1F,0x20,0x40,0x20,0x1F},{0x3F,0x40,0x38,0x40,0x3F},
 {0x63,0x14,8,0x14,0x63},{3,4,0x78,4,3},{0x61,0x59,0x49,0x4D,0x43}
};
static void glyph(int x, int y, int ch, int sc, uint32_t rgb) {
    if (ch >= 'a' && ch <= 'z') ch -= 32;
    const uint8_t *g;
    static const uint8_t under[5] = {0x40,0x40,0x40,0x40,0x40};
    if (ch == '_') g = under;
    else if (ch >= 0x20 && ch <= 0x5A) g = font5x7[ch - 0x20]; else return;
    for (int c = 0; c < 5; c++) for (int r = 0; r < 7; r++) if (g[c] & (1 << r)) rect(x + c * sc, y + r * sc, sc, sc, rgb, 255);
}
static int text_w(const char *s, int sc) { return (int)strlen(s) * 6 * sc - sc; }
static void text(int x, int y, const char *s, int sc, uint32_t rgb) { for (; *s; s++, x += 6 * sc) glyph(x, y, *s, sc, rgb); }

/* ---------------- real X cursor via XFixes ---------------- */
static int xfixes_op = 0;      /* major opcode of XFIXES, 0 = not available */
static int cursor_mode = 0;    /* 0 = real X cursor (XFixes), 1 = built-in arrow, 2 = none */
static int xfixes_init(void) {
    xfixes_op = 0;
    const char nm[] = "XFIXES"; size_t nl = 6;
    uint8_t q[16] = { 98, 0, 4, 0 }; p16(q + 4, (uint16_t)nl); memcpy(q + 8, nm, nl);
    uint8_t *r = req(q, 16, NULL);
    if (!r) return -1;
    int present = r[8], op = r[9]; free(r);
    if (!present) return -1;
    uint8_t v[12] = { (uint8_t)op, 0, 3, 0 }; p32(v + 4, 4); p32(v + 8, 0);     /* QueryVersion 4.0 */
    r = req(v, 12, NULL); if (!r) return -1; free(r);
    xfixes_op = op;
    return 0;
}
/* returns the cursor image reply (32-byte header + w*h ARGB pixels) or NULL */
static uint8_t *get_cursor_image(void) {
    if (!xfixes_op) return NULL;
    uint8_t q[4] = { (uint8_t)xfixes_op, 4, 1, 0 };
    return req(q, 4, NULL);
}
/* draw the X cursor at the pointer; returns 1 if the cursor was handled (even when invisible), 0 to fall back */
static int draw_real_cursor(int px, int py, int sc) {
    uint8_t *r = get_cursor_image();
    if (!r) return 0;
    int w = g16(r + 12), h = g16(r + 14), xh = g16(r + 16), yh = g16(r + 18);
    const uint8_t *pix = r + 32;
    int any = 0;
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) {
        uint32_t c = g32(pix + (size_t)(j * w + i) * 4); int a = c >> 24;
        if (!a) continue;
        any = 1;
        /* XFixes pixels are premultiplied ARGB: un-premultiply for blending */
        int rr = (c >> 16) & 255, gg = (c >> 8) & 255, bb = c & 255;
        if (a < 255) { rr = rr * 255 / a; gg = gg * 255 / a; bb = bb * 255 / a; if (rr > 255) rr = 255; if (gg > 255) gg = 255; if (bb > 255) bb = 255; }
        uint32_t rgb = ((uint32_t)rr << 16) | ((uint32_t)gg << 8) | (uint32_t)bb;
        for (int dy = 0; dy < sc; dy++) for (int dx = 0; dx < sc; dx++)
            blend(px + (i - xh) * sc + dx, py + (j - yh) * sc + dy, rgb, a);
    }
    free(r);
    (void)any;
    return 1;
}

/* classic arrow, X = black outline, . = white fill; hotspot = top-left */
static const char *ARROW[] = {
 "X           ", "XX          ", "X.X         ", "X..X        ", "X...X       ", "X....X      ", "X.....X     ", "X......X    ",
 "X.......X   ", "X........X  ", "X.....XXXXX ", "X..X..X     ", "X.X X..X    ", "XX  X..X    ", "X    X..X   ", "     X..X   ",
 "      X..X  ", "      X..X  ", "       XX   " };
static void draw_cursor(int px, int py, int sc) {
    for (int j = 0; j < 19; j++) for (int i = 0; ARROW[j][i]; i++) {
        char c = ARROW[j][i]; if (c == ' ') continue;
        rect(px + i * sc, py + j * sc, sc, sc, c == 'X' ? 0x000000 : 0xffffff, 255);
    }
}

/* ---------------- uinput keyboard ---------------- */
static int ufd = -1;
static void uemit(int type, int code, int val) {
    struct input_event e; memset(&e, 0, sizeof e); e.type = type; e.code = code; e.value = val;
    if (write(ufd, &e, sizeof e) < 0) { /* ignore */ }
}
static int mfd = -1;
static void mouse_nudge(void) {   /* 1 px right and back: makes the game redraw (and present) one frame */
    if (mfd < 0) return;
    struct input_event e[3]; memset(e, 0, sizeof e);
    e[0].type = EV_REL; e[0].code = REL_X; e[0].value = 1; e[1].type = EV_SYN;
    write(mfd, e, 2 * sizeof e[0]); usleep(30000);
    e[0].value = -1; write(mfd, e, 2 * sizeof e[0]);
}
static int uinput_init(void) {
    ufd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (ufd < 0) { fprintf(stderr, "xmirror: /dev/uinput: %s (virtual keyboard cannot type)\n", strerror(errno)); return -1; }
    ioctl(ufd, UI_SET_EVBIT, EV_KEY);
    for (int k = 1; k < 120; k++) ioctl(ufd, UI_SET_KEYBIT, k);
    struct uinput_setup us; memset(&us, 0, sizeof us);
    us.id.bustype = BUS_VIRTUAL; us.id.vendor = 0x1234; us.id.product = 0x5678; us.id.version = 1;
    snprintf(us.name, sizeof us.name, "xmirror virtual keyboard");
    if (ioctl(ufd, UI_DEV_SETUP, &us) || ioctl(ufd, UI_DEV_CREATE)) { fprintf(stderr, "xmirror: uinput setup: %s\n", strerror(errno)); close(ufd); ufd = -1; return -1; }
    fprintf(stderr, "xmirror: uinput keyboard created\n");
    mfd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (mfd >= 0) {
        ioctl(mfd, UI_SET_EVBIT, EV_KEY); ioctl(mfd, UI_SET_KEYBIT, BTN_LEFT);
        ioctl(mfd, UI_SET_EVBIT, EV_REL); ioctl(mfd, UI_SET_RELBIT, REL_X); ioctl(mfd, UI_SET_RELBIT, REL_Y);
        struct uinput_setup ms; memset(&ms, 0, sizeof ms);
        ms.id.bustype = BUS_VIRTUAL; ms.id.vendor = 0x1; ms.id.product = 0x3; snprintf(ms.name, sizeof ms.name, "xmirror virtual mouse");
        if (ioctl(mfd, UI_DEV_SETUP, &ms) || ioctl(mfd, UI_DEV_CREATE)) { close(mfd); mfd = -1; }
        else fprintf(stderr, "xmirror: uinput mouse created\n");
    }
    return 0;
}
static void key_tap(int code, int shift) {
    if (ufd < 0) return;
    if (shift) { uemit(EV_KEY, KEY_LEFTSHIFT, 1); uemit(EV_SYN, 0, 0); usleep(15000); }
    uemit(EV_KEY, code, 1); uemit(EV_SYN, 0, 0); usleep(25000);
    uemit(EV_KEY, code, 0); uemit(EV_SYN, 0, 0); usleep(15000);
    if (shift) { uemit(EV_KEY, KEY_LEFTSHIFT, 0); uemit(EV_SYN, 0, 0); usleep(15000); }
}

/* ---------------- on-screen keyboard ---------------- */
struct key { const char *label; int code; int w; int force_shift; int special; };   /* special: 1 shift, 2 close */
#define K(l, c) { l, c, 2, 0, 0 }
static const struct key ROW0[] = { K("1",2),K("2",3),K("3",4),K("4",5),K("5",6),K("6",7),K("7",8),K("8",9),K("9",10),K("0",11),K("-",12),{"BKSP",14,4,0,0},{0} };
static const struct key ROW1[] = { K("Q",16),K("W",17),K("E",18),K("R",19),K("T",20),K("Y",21),K("U",22),K("I",23),K("O",24),K("P",25),K("'",40),{"ENTER",28,4,0,0},{0} };
static const struct key ROW2[] = { K("A",30),K("S",31),K("D",32),K("F",33),K("G",34),K("H",35),K("J",36),K("K",37),K("L",38),K(";",39),K(",",51),K(".",52),K("?",53),{0} };
static const struct key ROW3[] = { {"SHIFT",42,3,0,1},K("Z",44),K("X",45),K("C",46),K("V",47),K("B",48),K("N",49),K("M",50),{"_",12,2,1,0},{"ESC",1,3,0,0},{"CLOSE",0,4,0,2},{0} };
static const struct key ROW4[] = { {"SPACE",57,26,0,0},{0} };
static const struct key *ROWS[5] = { ROW0, ROW1, ROW2, ROW3, ROW4 };
static int osk_open = 0, osk_row = 1, osk_col = 0, osk_shift = 0;
static int row_len(int r) { int n = 0; while (ROWS[r][n].label) n++; return n; }

static pid_t find_pid(const char *name) {
    DIR *d = opendir("/proc"); struct dirent *e; pid_t found = 0;
    if (!d) return 0;
    while ((e = readdir(d))) {
        if (e->d_name[0] < '1' || e->d_name[0] > '9') continue;
        char p[300], c[64]; snprintf(p, sizeof p, "/proc/%.200s/comm", e->d_name);
        FILE *f = fopen(p, "r"); if (!f) continue;
        if (fgets(c, sizeof c, f)) { c[strcspn(c, "\n")] = 0; if (!strcmp(c, name)) found = atoi(e->d_name); }
        fclose(f);
    }
    closedir(d); return found;
}
static void osk_set(int open) {
    if (open == osk_open) return;
    osk_open = open;
    pid_t g = find_pid("gptokeyb");
    if (g) kill(g, open ? SIGSTOP : SIGCONT);
    if (!open) osk_shift = 0;
    fprintf(stderr, "xmirror: keyboard %s (gptokeyb pid %d %s)\n", open ? "open" : "closed", (int)g, open ? "paused" : "resumed");
}
static void osk_move(int dx, int dy) {
    osk_row = (osk_row + dy + 5) % 5;
    int n = row_len(osk_row);
    if (dx) osk_col = (osk_col + dx + n) % n;
    if (osk_col >= n) osk_col = n - 1;
}
static void osk_press_key(const struct key *k) {
    if (k->special == 1) { osk_shift = !osk_shift; return; }
    if (k->special == 2) { osk_set(0); return; }
    key_tap(k->code, k->force_shift || osk_shift);
    if (osk_shift && !k->force_shift) osk_shift = 0;
}
static int g_root = 0;
static void warp_pointer(int x, int y) {
    if (fd < 0 || !g_root) return;
    uint8_t r[24] = { 41, 0, 6, 0 };
    p32(r + 8, g_root); p16(r + 20, (uint16_t)x); p16(r + 22, (uint16_t)y);
    wr(r, 24);
}
static void osk_cmd(const char *c) {
    if (!strcmp(c, "toggle")) osk_set(!osk_open);
    else if (!strcmp(c, "close")) osk_set(0);
    else if (!osk_open) return;
    else if (!strcmp(c, "up")) osk_move(0, -1);
    else if (!strcmp(c, "down")) osk_move(0, 1);
    else if (!strcmp(c, "left")) osk_move(-1, 0);
    else if (!strcmp(c, "right")) osk_move(1, 0);
    else if (!strcmp(c, "ok")) osk_press_key(&ROWS[osk_row][osk_col]);
    else if (!strcmp(c, "back")) key_tap(KEY_BACKSPACE, 0);
    else if (!strcmp(c, "space")) key_tap(KEY_SPACE, 0);
    else if (!strcmp(c, "shift")) osk_shift = !osk_shift;
    else if (!strcmp(c, "enter")) key_tap(KEY_ENTER, 0);
}
static void type_text(const char *s) {
    static const int LET[] = {30,48,46,32,18,33,34,35,23,36,37,38,50,49,24,25,16,19,31,20,22,47,17,45,21,44};
    for (; *s; s++) {
        int ch = *s, sh = 0, code = 0;
        if (ch >= 'a' && ch <= 'z') code = LET[ch - 'a'];
        else if (ch >= 'A' && ch <= 'Z') { code = LET[ch - 'A']; sh = 1; }
        else if (ch >= '1' && ch <= '9') code = 2 + (ch - '1');
        else if (ch == '0') code = 11; else if (ch == ' ') code = 57; else if (ch == '-') code = 12; else if (ch == '.') code = 52; else if (ch == ',') code = 51;
        else if (ch == '\'') code = 40; else if (ch == ';') code = 39; else if (ch == '?') { code = 53; sh = 1; } else if (ch == '_') { code = 12; sh = 1; }
        if (code) key_tap(code, sh);
    }
}
static void osk_draw(void) {
    int sc = FW >= 1000 ? 3 : 2;                          /* font scale */
    int unit = FW * 3 / 4 / 26;                           /* width of 1 unit; row 0 = 26 units */
    int kh = unit * 2 + 6, gap = unit / 4;
    int pw = unit * 26 + gap * 2, ph = kh * 5 + gap * 7 + 8 * sc + 4;
    int px = (FW - pw) / 2, py = FH - ph - 10;
    rect(px, py, pw, ph, 0x000000, 190); frame_rect(px, py, pw, ph, 2, 0xc0a040);
    int y = py + gap;
    for (int r = 0; r < 5; r++) {
        int x = px + gap, n = row_len(r);
        for (int c = 0; c < n; c++) {
            const struct key *k = &ROWS[r][c];
            int w = unit * k->w - gap;
            int sel = (r == osk_row && c == osk_col);
            int on = (k->special == 1 && osk_shift);
            rect(x, y, w, kh, sel ? 0xe0c040 : on ? 0x3070b0 : 0x303840, 235);
            frame_rect(x, y, w, kh, 1, 0x808890);
            char lab[8]; snprintf(lab, sizeof lab, "%s", k->label);
            int tw = text_w(lab, sc);
            text(x + (w - tw) / 2, y + (kh - 7 * sc) / 2, lab, sc, sel ? 0x000000 : 0xffffff);
            x += unit * k->w;
        }
        y += kh + gap;
    }
    const char *hint = "A TYPE  B BKSP  X SPACE  Y SHIFT  START ENTER  SELECT CLOSE";
    int hs = sc > 2 ? 2 : 1;
    text(px + (pw - text_w(hint, hs)) / 2, y + 2, hint, hs, 0xc0c0c0);
}

/* ---------------- pad / command input ---------------- */

/* ---------------- INI controls ---------------- */
static const struct { const char *name; int code; int bit; } BTN[] = {
    {"a",0x131,0},{"b",0x130,1},{"x",0x134,2},{"y",0x133,3},{"l1",0x136,4},{"r1",0x137,5},
    {"select",0x13a,6},{"back",0x13a,6},{"start",0x13b,7},{"menu",0x13c,8},{"l3",0x13d,9},{"r3",0x13e,10},
    {"l2",-2,11},{"r2",-5,12},{0,0,0} };
enum { A_TOGGLE, A_TYPE, A_BACKSPACE, A_SPACE, A_SHIFT, A_ENTER, A_CLOSE, A_N };
static const char *ACT_NAME[A_N] = { "toggle", "type", "backspace", "space", "shift", "enter", "close" };
static const char *ACT_DEFAULT[A_N] = { "r3", "a", "b", "x", "y", "start", "select, l3" };
#define MAXALT 6
struct act { unsigned mask[MAXALT]; int n; };
static struct act ACT[A_N];
static unsigned pressed_mask = 0;
static int cursor_scale_ini = 0;

static void trim(char *s) {
    char *a = s; while (isspace((unsigned char)*a)) a++;
    memmove(s, a, strlen(a) + 1);
    size_t n = strlen(s); while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
}
static int btn_bit(const char *n) { for (int i = 0; BTN[i].name; i++) if (!strcmp(BTN[i].name, n)) return BTN[i].bit; return -1; }
static void parse_action(int a, const char *val) {
    char buf[160]; snprintf(buf, sizeof buf, "%s", val);
    ACT[a].n = 0;
    char *sp1 = NULL;
    for (char *alt = strtok_r(buf, ",", &sp1); alt && ACT[a].n < MAXALT; alt = strtok_r(NULL, ",", &sp1)) {
        unsigned m = 0; int ok = 1;
        char tmp[80]; snprintf(tmp, sizeof tmp, "%s", alt);
        char *sp2 = NULL;
        for (char *b = strtok_r(tmp, "+", &sp2); b; b = strtok_r(NULL, "+", &sp2)) {
            trim(b); for (char *c = b; *c; c++) *c = tolower((unsigned char)*c);
            int bit = btn_bit(b);
            if (bit < 0) { fprintf(stderr, "xmirror: controls: unknown button '%s' for %s\n", b, ACT_NAME[a]); ok = 0; break; }
            m |= 1u << bit;
        }
        if (ok && m) ACT[a].mask[ACT[a].n++] = m;
    }
}
static void load_controls(void) {
    for (int a = 0; a < A_N; a++) parse_action(a, ACT_DEFAULT[a]);
    const char *path = getenv("XMIRROR_CONFIG"); if (!path) path = "controls.ini";
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "xmirror: controls: %s not found, using defaults\n", path); return; }
    char line[256], sec[32] = "";
    while (fgets(line, sizeof line, f)) {
        char *h = strpbrk(line, "#;"); if (h) *h = 0;
        trim(line);
        if (!line[0]) continue;
        if (line[0] == '[') { char *e = strchr(line, ']'); if (e) *e = 0; snprintf(sec, sizeof sec, "%s", line + 1); for (char *c = sec; *c; c++) *c = tolower((unsigned char)*c); continue; }
        if (!strcmp(sec, "display")) {
            char *e2 = strchr(line, '='); if (!e2) continue;
            *e2 = 0; char *k2 = line, *v2 = e2 + 1; trim(k2); trim(v2);
            for (char *c = k2; *c; c++) *c = tolower((unsigned char)*c);
            for (char *c = v2; *c; c++) *c = tolower((unsigned char)*c);
            if (!strcmp(k2, "cursor")) cursor_mode = !strcmp(v2, "arrow") ? 1 : (!strcmp(v2, "none") || !strcmp(v2, "off")) ? 2 : 0;
            else if (!strcmp(k2, "cursor_scale") && atoi(v2) > 0 && !getenv("XMIRROR_CURSOR_SCALE")) cursor_scale_ini = atoi(v2);
            continue;
        }
        if (strcmp(sec, "osk")) continue;
        char *eq = strchr(line, '='); if (!eq) continue;
        *eq = 0; char *k = line, *v = eq + 1; trim(k); trim(v);
        for (char *c = k; *c; c++) *c = tolower((unsigned char)*c);
        int found = 0;
        for (int a = 0; a < A_N; a++) if (!strcmp(k, ACT_NAME[a])) { parse_action(a, v); found = 1; }
        if (!found) fprintf(stderr, "xmirror: controls: unknown key '%s' in [osk]\n", k);
    }
    fclose(f);
    fprintf(stderr, "xmirror: controls loaded from %s\n", path);
}
static void fire(int a) {
    if (a == A_TOGGLE) { osk_set(!osk_open); return; }
    if (!osk_open) return;
    switch (a) {
        case A_TYPE: osk_cmd("ok"); break;
        case A_BACKSPACE: osk_cmd("back"); break;
        case A_SPACE: osk_cmd("space"); break;
        case A_SHIFT: osk_cmd("shift"); break;
        case A_ENTER: osk_cmd("enter"); break;
        case A_CLOSE: osk_set(0); break;
    }
}
/* select+start: quit the game. launch.sh sees /tmp/aoe.quit and does not restart; its cleanup then runs. */
static void quit_game(void) {
    static int done = 0; if (done) return; done = 1;
    fprintf(stderr, "xmirror: select+start -> quit\n");
    FILE *f = fopen("/tmp/aoe.quit", "w"); if (f) fclose(f);
    if (fork() == 0) {
        execl("/bin/sh", "sh", "-c", "killall -9 box86 wineserver 2>/dev/null; sleep 1; killall -9 wp_weston Xwayland seatd gptokeyb 2>/dev/null", (char *)NULL);
        _exit(0);
    }
}
static void button_event(int bit, int down) {
    if (down) pressed_mask |= 1u << bit; else { pressed_mask &= ~(1u << bit); return; }
    if ((pressed_mask & ((1u << 6) | (1u << 7))) == ((1u << 6) | (1u << 7))) quit_game();   /* select + start */
    for (int a = 0; a < A_N; a++)
        for (int i = 0; i < ACT[a].n; i++) {
            unsigned m = ACT[a].mask[i];
            if ((m & (1u << bit)) && (pressed_mask & m) == m) { fire(a); break; }
        }
}

static int padfd = -1, cmdfd = -1;
static int hatx = 0, haty = 0, stickx = 0, sticky = 0; static double rep_t = 0; static int rep_dx = 0, rep_dy = 0;
static int find_pad(void) {
    const char *forced = getenv("XMIRROR_PAD");
    if (forced) return open(forced, O_RDONLY | O_NONBLOCK);
    for (int i = 0; i < 16; i++) {
        char p[64], nm[96] = ""; snprintf(p, sizeof p, "/sys/class/input/event%d/device/name", i);
        FILE *f = fopen(p, "r"); if (!f) continue;
        if (fgets(nm, sizeof nm, f)) { nm[strcspn(nm, "\n")] = 0; }
        fclose(f);
        if (!strcmp(nm, "TRIMUI Player1")) { snprintf(p, sizeof p, "/dev/input/event%d", i); return open(p, O_RDONLY | O_NONBLOCK); }
    }
    return -1;
}
static void dir_event(int dx, int dy) { osk_move(dx, dy); rep_dx = dx; rep_dy = dy; rep_t = now() + 0.35; }
static void idle_check(void) { if (!hatx && !haty && !stickx && !sticky) rep_dx = rep_dy = 0; }
/* After a suspend/resume the firmware (trimui_inputd) may recreate the pad's input device, leaving our fd dead.  Detect a resume
 * (CLOCK_BOOTTIME jumps ahead of CLOCK_MONOTONIC) or a dead fd and open the pad again. */
static double boot_minus_mono(void) {
    struct timespec a, b; clock_gettime(CLOCK_BOOTTIME, &a); clock_gettime(CLOCK_MONOTONIC, &b);
    return (a.tv_sec - b.tv_sec) + (a.tv_nsec - b.tv_nsec) / 1e9;
}
static void pad_reopen_check(int dead) {
    static double off = -1, retry = 0;
    double o = boot_minus_mono(), t = now();
    int resumed = off >= 0 && o - off > 0.5;
    if (off < 0 || resumed) off = o;
    if (resumed || (dead && t >= retry) || (padfd < 0 && t >= retry)) {
        retry = t + 1.0;
        if (padfd >= 0) { close(padfd); padfd = -1; }
        padfd = find_pad();
        fprintf(stderr, "xmirror: pad %s (%s)\n", padfd >= 0 ? "reopened" : "not found", resumed ? "after resume" : "device lost");
        pressed_mask = 0;
    }
}
static void pad_poll(void) {
    pad_reopen_check(0);
    if (padfd < 0) return;
    struct input_event e;
    ssize_t rr;
    while ((rr = read(padfd, &e, sizeof e)) == (ssize_t)sizeof e) {
        if (e.type == EV_KEY && e.value != 2) {
            for (int i = 0; BTN[i].name; i++) if (BTN[i].code == e.code) { button_event(BTN[i].bit, e.value); break; }
        } else if (e.type == EV_ABS && (e.code == 2 || e.code == 5)) {
            int bit = e.code == 2 ? 11 : 12, down = e.value > 100;
            if (down != !!(pressed_mask & (1u << bit))) button_event(bit, down);
        } else if (e.type == EV_ABS && osk_open) {
            if (e.code == 16) { hatx = e.value; if (hatx) dir_event(hatx, 0); else idle_check(); }
            else if (e.code == 17) { haty = e.value; if (haty) dir_event(0, haty); else idle_check(); }
            else if (e.code == 0 || e.code == 1) {
                int v = e.value > 16000 ? 1 : e.value < -16000 ? -1 : 0;
                if (e.code == 0 && v != stickx) { stickx = v; if (v) dir_event(v, 0); else idle_check(); }
                if (e.code == 1 && v != sticky) { sticky = v; if (v) dir_event(0, v); else idle_check(); }
            }
        }
    }
    if (rr < 0 && errno != EAGAIN && errno != EINTR) pad_reopen_check(1);
    if (osk_open && (rep_dx || rep_dy) && now() >= rep_t) { osk_move(rep_dx, rep_dy); rep_t = now() + 0.12; }
}
static void cmd_poll(void) {
    if (cmdfd < 0) return;
    static char buf[256]; static size_t n = 0;
    ssize_t r = read(cmdfd, buf + n, sizeof buf - 1 - n);
    if (r > 0) n += r;
    char *nl;
    buf[n] = 0;
    while ((nl = memchr(buf, '\n', n))) {
        *nl = 0;
        if (!strncmp(buf, "type ", 5)) type_text(buf + 5);
        else if (!strncmp(buf, "warp ", 5)) { int wx = 0, wy = 0; if (sscanf(buf + 5, "%d %d", &wx, &wy) == 2) warp_pointer(wx, wy); }
        else if (!strncmp(buf, "tapkey ", 7)) key_tap(atoi(buf + 7), 0);
        else if (!strcmp(buf, "quit")) quit_game();
        else if (!strcmp(buf, "cursor")) { uint8_t *ci = get_cursor_image(); if (!ci) fprintf(stderr, "xmirror: cursor: no xfixes\n"); else { int cw = g16(ci + 12), ch = g16(ci + 14), op = 0; for (int q = 0; q < cw * ch; q++) if (g32(ci + 32 + q * 4) >> 24) op++; fprintf(stderr, "xmirror: cursor %dx%d hot %d,%d opaque_px=%d serial=%u\n", cw, ch, g16(ci + 16), g16(ci + 18), op, g32(ci + 20)); free(ci); } }
        else if (!strcmp(buf, "where")) { int wx = 0, wy = 0; if (g_root && !pointer(g_root, &wx, &wy)) fprintf(stderr, "xmirror: pointer %d %d\n", wx, wy); }
        else osk_cmd(buf);
        size_t used = (nl - buf) + 1; memmove(buf, buf + used, n - used); n -= used; buf[n] = 0;
    }
    if (n >= sizeof buf - 1) n = 0;
}

int main(int argc, char **argv) {
    int dpy = argc > 1 ? atoi(argv[1]) : 0;
    int fps = getenv("XMIRROR_FPS") ? atoi(getenv("XMIRROR_FPS")) : 20;
    int cs = getenv("XMIRROR_CURSOR_SCALE") ? atoi(getenv("XMIRROR_CURSOR_SCALE")) : 2;
    if (fps < 1) fps = 20;
    if (cs < 1) cs = 2;
    load_controls();
    if (cursor_scale_ini > 0 && !getenv("XMIRROR_CURSOR_SCALE")) cs = cursor_scale_ini;
    signal(SIGPIPE, SIG_IGN);
    setvbuf(stderr, NULL, _IONBF, 0);
    uinput_init();
    padfd = find_pad();
    fprintf(stderr, "xmirror: pad %s\n", padfd >= 0 ? "opened" : "not found");
    mkfifo("/tmp/xmirror.cmd", 0666);
    cmdfd = open("/tmp/xmirror.cmd", O_RDWR | O_NONBLOCK);
    if (getenv("XMIRROR_NOSDL")) {   /* no-display mode: only serve the FIFO (tapkey/type/warp...) and the pad */
        fprintf(stderr, "xmirror: no-display mode (FIFO/uinput only)\n");
        /* export the on-screen keyboard as an RGBA image (for an external overlay) */
        FW = 1280; FH = 720; F = calloc((size_t)FW * FH, 4);
        int last_open = -1, last_row = -1, last_col = -1, last_shift = -1;
        for (;;) {
            pad_poll(); cmd_poll();
            if (fd < 0) { static double lt = 0; if (now() - lt > 1.0) { lt = now(); int rr = connect_x(dpy); if (rr > 0) g_root = rr; else { if (fd >= 0) close(fd); fd = -1; } } }
            if (osk_open != last_open || osk_row != last_row || osk_col != last_col || osk_shift != last_shift) {
                last_open = osk_open; last_row = osk_row; last_col = osk_col; last_shift = osk_shift;
                int x0 = FW, y0 = FH, x1 = -1, y1 = -1;
                if (osk_open) {
                    memset(F, 0, (size_t)FW * FH * 4);
                    osk_draw();
                    for (int y = 0; y < FH; y++) for (int x = 0; x < FW; x++)
                        if (F[y * FW + x] >> 24) { if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y; }
                }
                FILE *f = fopen("/tmp/aoe_osk.bin.tmp", "wb");
                if (f) {
                    int vis = osk_open && x1 >= x0, w = vis ? x1 - x0 + 1 : 0, h = vis ? y1 - y0 + 1 : 0;
                    uint32_t hd[8] = { 0x314b534f, (uint32_t)vis, (uint32_t)(vis ? x0 : 0), (uint32_t)(vis ? y0 : 0), (uint32_t)w, (uint32_t)h, 1280, 720 };
                    fwrite(hd, 4, 8, f);
                    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
                        uint32_t c = F[(y0 + y) * FW + x0 + x]; uint8_t px[4] = { (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c, (uint8_t)(c >> 24) };
                        fwrite(px, 1, 4, f);
                    }
                    fclose(f); rename("/tmp/aoe_osk.bin.tmp", "/tmp/aoe_osk.bin");
                }
                /* cnc-ddraw only presents when the game draws; nudge the pointer so the game redraws (its cursor) and the overlay shows */
                mouse_nudge();
            }
            usleep(20000);
        }
    }
    if (load_sdl()) return 2;
    if (SDL_Init(0x20)) { fprintf(stderr, "xmirror: SDL_Init: %s\n", SDL_GetError()); return 2; }
    SDL_SetHint("SDL_RENDER_SCALE_QUALITY", "0");
    void *win = SDL_CreateWindow("xmirror", 0, 0, 1280, 720, 0x1001 /* FULLSCREEN_DESKTOP */);
    if (!win) { fprintf(stderr, "xmirror: window: %s\n", SDL_GetError()); return 2; }
    void *ren = SDL_CreateRenderer(win, -1, 0);
    if (!ren) { fprintf(stderr, "xmirror: renderer: %s\n", SDL_GetError()); return 2; }
    SDL_ShowCursor(0);
    fprintf(stderr, "xmirror: SDL ready\n");
    void *tex = NULL; int tw = 0, th = 0;
    uint8_t *frame = NULL; size_t fcap = 0;
    long frames = 0, lastframes = 0; double t0 = now(), tl = t0;
    for (;;) {
        int root = connect_x(dpy);
        if (root < 0) { if (fd >= 0) close(fd); fd = -1; pad_poll(); cmd_poll(); usleep(100000); continue; }
        fprintf(stderr, "xmirror: connected to :%d root=0x%x\n", dpy, root);
        g_root = root;
        xfixes_init();
        uint32_t win_id = 0; struct geo g = {0}; double lastpick = 0;
        for (;;) {
            double t = now();
            uint8_t ev[256]; while (SDL_PollEvent(ev)) { if (g32(ev) == 0x100) { osk_set(0); return 0; } }
            pad_poll(); cmd_poll();
            if (!win_id || t - lastpick > 1.0) {
                struct geo ng; uint32_t w = pick_window(root, &ng);
                lastpick = t;
                if (bad) break;
                if (w) { if (w != win_id) fprintf(stderr, "xmirror: window 0x%x %dx%d+%d+%d depth %d\n", w, ng.w, ng.h, ng.x, ng.y, ng.depth); win_id = w; g = ng; }
                else win_id = 0;
            }
            if (win_id) {
                struct geo cg; if (getgeo(win_id, &cg)) { win_id = 0; if (bad) break; continue; } g = cg;
                uint8_t r[20] = { 73, 2, 5, 0 };
                p32(r + 4, win_id); p16(r + 8, 0); p16(r + 10, 0); p16(r + 12, g.w); p16(r + 14, g.h); p32(r + 16, 0xffffffff);
                size_t l; uint8_t *rep = req(r, 20, &l);
                if (!rep) { win_id = 0; lastpick = 0; if (bad) break; usleep(100000); continue; }
                size_t dl = l - 32;
                if (dl >= (size_t)g.w * g.h * 4) {
                    if (g.w != tw || g.h != th || !tex) { tex = SDL_CreateTexture(ren, 0x16362004 /* ARGB8888 */, 1, g.w, g.h); tw = g.w; th = g.h; if (!tex) fprintf(stderr, "xmirror: texture: %s\n", SDL_GetError()); }
                    if (tex) {
                        if (fcap < dl) { frame = realloc(frame, dl); fcap = dl; }
                        memcpy(frame, rep + 32, dl);
                        F = (uint32_t *)frame; FW = g.w; FH = g.h;
                        int px, py;
                        if (osk_open) osk_draw();
                        if (cursor_mode != 2 && !pointer(root, &px, &py)) {
                            if (cursor_mode == 1 || !draw_real_cursor(px - g.x, py - g.y, cs)) draw_cursor(px - g.x, py - g.y, cs);
                        }
                        SDL_UpdateTexture(tex, NULL, frame, g.w * 4);
                        SDL_RenderClear(ren);
                        SDL_RenderCopy(ren, tex, NULL, NULL);
                        SDL_RenderPresent(ren);
                        frames++;
                    }
                } else fprintf(stderr, "xmirror: short image %zu for %dx%d depth %d\n", dl, g.w, g.h, g.depth);
                free(rep);
            } else usleep(50000);
            if (t - tl > 10) { fprintf(stderr, "xmirror: %.1f fps (win=0x%x %dx%d)\n", (frames - lastframes) / (t - tl), win_id, g.w, g.h); tl = t; lastframes = frames; }
            double el = now() - t, per = 1.0 / fps; if (el < per) usleep((useconds_t)((per - el) * 1e6));
        }
        close(fd); fd = -1; bad = 0; g_root = 0;
    }
}
