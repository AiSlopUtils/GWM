/* infinawm — an infinite-canvas compositing window manager for Xorg.
 *
 * Windows live at coordinates on an unbounded 2D canvas. A viewport
 * (pan offset + zoom factor) decides what you see. Zoom is pure display
 * scaling: the compositor renders window contents scaled like an image,
 * apps never reflow. Input still works at any zoom because the real
 * (server-side) window layout is continuously re-anchored at the pointer:
 * the point under the cursor always maps to the correct in-window pixel,
 * and input only ever happens at the cursor.
 *
 * Default keys (Mod = Super/Windows key):
 *   Mod+Arrows            pan the canvas
 *   Mod+= / Mod+-         zoom in / out (true visual zoom)
 *   Mod+scroll            zoom at cursor
 *   Mod+0                 reset zoom to 100%
 *   Mod+R                 launcher: type a command, Enter to run
 *   Mod+Return            spawn a terminal
 *   Mod+Shift+Left/Right  snap window to left/right half
 *   Mod+Shift+Up          fullscreen (toggle)
 *   Mod+Shift+Down        restore snapped window
 *   Mod+Tab               cycle focus (pans to the window)
 *   Mod+F                 fullscreen the focused window (toggle)
 *   Mod+Q                 close window
 *   Mod+Shift+E           quit WM
 *   Mod+LeftDrag          move window (drag to screen edge to snap L/R/full)
 *   Mod+RightDrag         resize window
 * Mouse on decorations:
 *   drag title bar        move (release at screen edge to snap L/R/full)
 *   title bar buttons     fullscreen toggle, close
 *   drag window border    resize (any edge or corner)
 *   drag empty canvas     pan
 * Zoom is permanent: windows are really scaled, so you can interact with
 * everything at any zoom level. Zoom only changes when you ask it to.
 */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/Xproto.h>
#include <X11/keysym.h>
#include <X11/cursorfont.h>
#include <X11/extensions/Xrender.h>
#include <X11/extensions/Xcomposite.h>
#ifndef HAVE_DAMAGE
#define HAVE_DAMAGE 0
#endif
#if HAVE_DAMAGE
#include <X11/extensions/Xdamage.h>
#endif
#ifndef HAVE_IMLIB2
#define HAVE_IMLIB2 0
#endif
#if HAVE_IMLIB2
#include <Imlib2.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <signal.h>
#include <math.h>
#include <dirent.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/stat.h>

#define MOD Mod4Mask
#define BORDER 2
#define TBAR 26                 /* title bar height, world units        */
#define BANDW 8                 /* resize band width, screen px         */
#define MAX_BINS 8192
#define LAUNCHER_W 640
#define LAUNCHER_H 44

/* resize edge bits */
#define EDGE_N 1
#define EDGE_S 2
#define EDGE_E 4
#define EDGE_W 8

typedef enum { HIT_NONE = 0, HIT_TITLE, HIT_CLOSE, HIT_MAX, HIT_RESIZE } Hit;

/* ---- colors (0xRRGGBB) ---- */
#define COL_BG        0x000000
#define COL_DOT       0x2a2f44
#define COL_BORDER    0xffffff  /* unfocused border outline             */
#define COL_FOCUS     0x4a90e2  /* focused border outline: subtle blue  */
#define COL_TBAR      0x000000  /* unfocused title bar background       */
#define COL_TBARF     0x000000  /* focused title bar background         */
#define COL_TTEXT     0xffffff  /* unfocused title text                 */
#define COL_TTEXTF    0xffffff  /* focused title text                   */
#define COL_BTN_CLOSE 0xff5f57  /* macOS red                            */
#define COL_BTN_MAX   0x28c840  /* macOS green                          */
#define COL_BTN_IDLE  0x9a9a9a  /* dots on unfocused windows            */
#define COL_GLYPH_CLO 0x7a1d1a  /* x glyph on hover                     */
#define COL_GLYPH_MAX 0x1d6b27  /* expand glyph on hover                */
#define COL_LBG       0x1f2335
#define COL_LFG       0xc0caf5
#define COL_LHINT     0x565f89
#define COL_LACCENT   0x7aa2f7

typedef enum { SNAP_NONE = 0, SNAP_L, SNAP_R, SNAP_FULL,
               SNAP_TL, SNAP_TR, SNAP_BL, SNAP_BR } Snap;

/* user-configurable (from ~/.config/infinawm/config.yml) */
static unsigned long col_bg     = COL_BG;
static unsigned long col_border = COL_BORDER;
static unsigned long col_focus  = COL_FOCUS;
static char autostart_cmds[1024];
static char bg_image[512];      /* wallpaper path; screen-fixed */
static char locker_cmd[256] = "i3lock -c 000000 || slock || xsecurelock";
static char battery_name[64];   /* /sys/class/power_supply entry; ""=auto */

typedef struct Client {
    Window win;
    Window frame;               /* InputOnly window catching decoration
                                 * clicks (title bar + resize bands)     */
    double x, y, w, h;          /* world-space geometry                  */
    double sx, sy, sw, sh;      /* saved geometry for snap restore       */
    Snap snapped;
    int fullscreen;             /* EWMH fullscreen (no decorations)      */
    double fsx, fsy, fsw, fsh;  /* saved geometry for fullscreen restore */
    int mapped;
    int is_or;                  /* override-redirect (menus, tooltips)   */
    int screenspace;            /* rendered in screen coords (launcher)  */
    Pixmap pm;
    Picture pict;
    double pict_zoom;           /* zoom the picture transform was set to */
    int lx, ly, lw, lh;         /* last applied real geometry            */
    int pending_cfg;            /* ConfigureNotifies caused by us        */
    char title[128];
    char cls[64];               /* WM_CLASS (program name)               */
    pid_t pid;                  /* from _NET_WM_PID, 0 if unknown        */
    unsigned long cpu_ticks;    /* utime+stime at last sample            */
    double cpu_t;               /* time of last sample                   */
    double cpu_pct;
    long rss_kb;
    Pixmap icon_pm;             /* _NET_WM_ICON, scaled ARGB             */
    Picture icon;
#if HAVE_DAMAGE
    Damage damage;
#endif
    struct Client *next;        /* list order == stacking, bottom->top   */
} Client;

static Display *dpy;
static int scr;
static Window root;
static int SW, SH;
static Visual *visual;
static int depth;
static Client *clients;         /* bottom -> top */
static Client *focused;

/* viewport: world coords of screen origin + zoom; t* are animation targets */
static double vx, vy, zoom = 1.0;
static double tvx, tvy, tzoom = 1.0;
static double pcx, pcy;         /* last known pointer position (screen) */

static Pixmap backpm;
static Picture backpict, rootpict;
static Pixmap bgpm;             /* wallpaper, pre-scaled to the screen   */
static Picture bgpict;
static XRenderPictFormat *fmt_rgb, *fmt_argb;
static GC dgc;                  /* decoration GC on the backbuffer       */

/* cursors */
static Cursor cur_norm, cur_move, cur_h, cur_v, cur_nw, cur_ne, cur_sw, cur_se;
static Cursor cur_current;

static int dirty = 1;
#if HAVE_DAMAGE
static int pdirty;                      /* partial repaint pending       */
static int dmgx1, dmgy1, dmgx2, dmgy2;  /* damaged bounding box, scr px  */
#endif
static int running = 1;
static int placement_n;

/* launcher */
static Window lwin;
static Client *lclient;
static GC lgc;
static XFontStruct *lfont;
static char ltext[256];
static int lopen;
static char *bins[MAX_BINS];
static int nbins = -1;

static Atom A_WM_PROTOCOLS, A_WM_DELETE, A_WM_STATE, A_WM_TAKE_FOCUS,
            A_NET_SUPPORTING, A_NET_WM_NAME, A_NET_ACTIVE, A_UTF8,
            A_NET_WM_PID, A_NET_WM_ICON, A_NET_SUPPORTED, A_NET_CLIENT_LIST,
            A_NET_WM_STATE, A_NET_WM_STATE_FS;

/* task manager (Super+M) */
#define TM_W 560
#define TM_ROWH 30
#define TM_HDR 40
#define TM_GRAPH 64             /* system cpu/ram graph height           */
#define TM_ROWTOP (TM_HDR + TM_GRAPH + 8)
#define TM_ICON 20
#define TM_MAXROWS 24
#define HIST_MAX 120            /* 2 minutes of 1s samples               */
static Window tmwin;
static Client *tmclient;
static GC tmgc;
static Picture tmpict;
static int tmopen;
static double tm_sampled;
static Client *tm_rows[TM_MAXROWS];
static int tm_nrows;
static Time tm_click_time;
static int tm_click_row = -1;

/* system-wide cpu/ram history for the panel graph */
static double hist_cpu[HIST_MAX], hist_ram[HIST_MAX];
static int hist_len;
static double sys_sampled;
static unsigned long long cpu_prev_total, cpu_prev_busy;

/* XEmbed system tray, docked at the bottom of the task manager panel */
#define TRAY_MAX 16
#define TRAY_PX 20
#define TRAY_ROW 34
static Window tray[TRAY_MAX];
static int ntray;
static Atom A_TRAY_SEL, A_TRAY_OPCODE, A_TRAY_ORIENT, A_MANAGER, A_XEMBED;

/* decoration hover state (for macOS-style button glyphs) */
static Client *hover_c;
static Hit hover_hit;

/* drag state */
static int drag_mode;           /* 0 none, 1 move, 2 resize, 3 pan */
static Client *drag_c;
static int drag_sx, drag_sy;    /* pointer at drag start (screen) */
static int drag_edges;          /* EDGE_* bitmask for resize drags */
static int ptr_grabbed;         /* we hold an active pointer grab  */
static double drag_wx, drag_wy, drag_ww, drag_wh, drag_vx, drag_vy;

#if HAVE_DAMAGE
static int damage_ev;
#endif

/* ------------------------------------------------------------------ */
static int xerror(Display *d, XErrorEvent *e) {
    (void)d;
    if (e->error_code == BadWindow || e->error_code == BadDrawable ||
        e->error_code == BadMatch || e->error_code == BadPixmap ||
        e->error_code == BadAccess)
        return 0; /* windows vanish at any time; ignore */
    fprintf(stderr, "infinawm: X error code=%d req=%d\n",
            e->error_code, e->request_code);
    return 0;
}
static int wm_running_err;
static int xerror_start(Display *d, XErrorEvent *e) {
    (void)d; (void)e;
    wm_running_err = 1;
    return 0;
}

static void spawn(const char *cmd) {
    if (!cmd || !*cmd) return;
    if (fork() == 0) {
        if (dpy) close(ConnectionNumber(dpy));
        setsid();
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
}

static double now_s(void) {
    struct timeval tv; gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

/* ---- config: ~/.config/infinawm/config.yml --------------------------- */
static unsigned long parse_color(const char *s, unsigned long def) {
    char hex[8];
    int n = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '#') s++;
    else if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    while (n < 7 && ((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'f') ||
                     (*s >= 'A' && *s <= 'F')))
        hex[n++] = *s++;
    hex[n] = 0;
    if (n == 3) {   /* #fff shorthand */
        char e[7] = { hex[0], hex[0], hex[1], hex[1], hex[2], hex[2], 0 };
        return strtoul(e, NULL, 16);
    }
    if (n == 6) return strtoul(hex, NULL, 16);
    return def;
}

static const char *config_default =
    "# infinawm configuration\n"
    "\n"
    "# programs launched once at startup, separated by spaces\n"
    "autostart: \"\"\n"
    "\n"
    "# colors, hex (#rgb / #rrggbb)\n"
    "background_color: \"#000000\"\n"
    "border_color: \"#ffffff\"\n"
    "focus_color: \"#4a90e2\"\n"
    "\n"
    "# wallpaper (png/jpg), scaled to cover the screen; stays fixed while\n"
    "# the canvas pans and zooms. Needs a build with libimlib2-dev.\n"
    "background_image: \"\"\n"
    "\n"
    "# screen locker command (Super+L). i3lock recommended:\n"
    "#   sudo apt install i3lock\n"
    "locker: \"i3lock -c 000000 || slock || xsecurelock\"\n"
    "\n"
    "# battery shown in the Super+M panel: a name from\n"
    "# /sys/class/power_supply (e.g. BAT0, BAT1).\n"
    "# Empty = auto-detect the first battery.\n"
    "battery: \"\"\n";

static void load_config(void) {
    char path[512], line[1280];
    const char *home = getenv("HOME");
    FILE *f;
    if (!home) return;
    snprintf(path, sizeof path, "%s/.config/infinawm/config.yml", home);
    f = fopen(path, "r");
    if (!f) {
        char alt[512];
        snprintf(alt, sizeof alt, "%s/.infinawm.yml", home);
        f = fopen(alt, "r");
    }
    if (!f) {
        /* first run: write a commented default config */
        char dir[512];
        snprintf(dir, sizeof dir, "%s/.config", home);
        mkdir(dir, 0755);
        snprintf(dir, sizeof dir, "%s/.config/infinawm", home);
        mkdir(dir, 0755);
        f = fopen(path, "w");
        if (f) { fputs(config_default, f); fclose(f); }
        return;
    }
    while (fgets(line, sizeof line, f)) {
        char *p = line, *colon, *key, *val, *end;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '#' || *p == '\n') continue;
        colon = strchr(p, ':');
        if (!colon) continue;
        *colon = 0;
        key = p;
        end = colon - 1;
        while (end > key && (*end == ' ' || *end == '\t')) *end-- = 0;
        val = colon + 1;
        while (*val == ' ' || *val == '\t') val++;
        end = val + strlen(val);
        while (end > val && (end[-1] == '\n' || end[-1] == '\r' ||
                             end[-1] == ' ' || end[-1] == '\t')) *--end = 0;
        if (*val == '"' || *val == '\'') {       /* quoted value */
            char q = *val++;
            char *cq = strchr(val, q);
            if (cq) *cq = 0;
        } else {                                  /* strip trailing comment */
            char *h = strstr(val, " #");
            if (h) { *h = 0;
                end = val + strlen(val);
                while (end > val && (end[-1] == ' ' || end[-1] == '\t'))
                    *--end = 0;
            }
        }
        if (!strcmp(key, "autostart"))
            snprintf(autostart_cmds, sizeof autostart_cmds, "%s", val);
        else if (!strcmp(key, "battery"))
            snprintf(battery_name, sizeof battery_name, "%s", val);
        else if (!strcmp(key, "locker") && *val)
            snprintf(locker_cmd, sizeof locker_cmd, "%s", val);
        else if (!strcmp(key, "background_image"))
            snprintf(bg_image, sizeof bg_image, "%s", val);
        else if (!strcmp(key, "background_color"))
            col_bg = parse_color(val, col_bg);
        else if (!strcmp(key, "border_color"))
            col_border = parse_color(val, col_border);
        else if (!strcmp(key, "focus_color"))
            col_focus = parse_color(val, col_focus);
    }
    fclose(f);
}

static XRenderColor rcol(unsigned long c) {
    XRenderColor r;
    r.red   = ((c >> 16) & 0xff) * 0x101;
    r.green = ((c >> 8)  & 0xff) * 0x101;
    r.blue  = (c & 0xff) * 0x101;
    r.alpha = 0xffff;
    return r;
}

/* ---- coordinate transforms ------------------------------------------ */
static double w2sx(double wx_) { return (wx_ - vx) * zoom; }
static double w2sy(double wy_) { return (wy_ - vy) * zoom; }
static double s2wx(double sx_) { return vx + sx_ / zoom; }
static double s2wy(double sy_) { return vy + sy_ / zoom; }

/* ---- client helpers -------------------------------------------------- */
static Client *find_client(Window w) {
    Client *c;
    for (c = clients; c; c = c->next) if (c->win == w) return c;
    return NULL;
}

static Client *find_by_frame(Window w) {
    Client *c;
    if (!w) return NULL;
    for (c = clients; c; c = c->next) if (c->frame == w) return c;
    return NULL;
}

static void restack_frame(Client *c) {
    XWindowChanges wc;
    if (!c->frame) return;
    wc.sibling = c->win;
    wc.stack_mode = Below;
    XConfigureWindow(dpy, c->frame, CWSibling | CWStackMode, &wc);
}

/* is this a normal, visible, user-facing window? */
static int eligible(Client *c) {
    return c->mapped && !c->is_or && !c->screenspace;
}

/* send an ICCCM WM_PROTOCOLS message if the client supports it */
static int send_proto(Client *c, Atom proto) {
    Atom *protos = NULL;
    int n = 0, i, found = 0;
    if (XGetWMProtocols(dpy, c->win, &protos, &n)) {
        for (i = 0; i < n; i++) if (protos[i] == proto) found = 1;
        if (protos) XFree(protos);
    }
    if (found) {
        XEvent ev;
        memset(&ev, 0, sizeof ev);
        ev.xclient.type = ClientMessage;
        ev.xclient.window = c->win;
        ev.xclient.message_type = A_WM_PROTOCOLS;
        ev.xclient.format = 32;
        ev.xclient.data.l[0] = (long)proto;
        ev.xclient.data.l[1] = CurrentTime;
        XSendEvent(dpy, c->win, False, NoEventMask, &ev);
    }
    return found;
}

/* EWMH _NET_CLIENT_LIST for taskbars / wmctrl / scripts */
static void update_client_list(void) {
    Window arr[512];
    int n = 0;
    Client *c;
    for (c = clients; c && n < 512; c = c->next)
        if (!c->is_or && !c->screenspace) arr[n++] = c->win;
    XChangeProperty(dpy, root, A_NET_CLIENT_LIST, XA_WINDOW, 32,
                    PropModeReplace, (unsigned char *)arr, n);
}

static void free_pict(Client *c) {
    if (c->pict) { XRenderFreePicture(dpy, c->pict); c->pict = 0; }
    if (c->pm)   { XFreePixmap(dpy, c->pm); c->pm = 0; }
}

static void detach(Client *c) {
    Client **p;
    for (p = &clients; *p && *p != c; p = &(*p)->next);
    if (*p) *p = c->next;
    c->next = NULL;
}

static void attach_top(Client *c) {
    Client **p;
    for (p = &clients; *p; p = &(*p)->next);
    *p = c;
    c->next = NULL;
}

/* keep an internal panel (launcher / task manager) above everything, in
 * both server stacking and compositor paint order */
static void raise_panel(Client *pc, Window pw) {
    if (!pc) return;
    detach(pc); attach_top(pc);
    XRaiseWindow(dpy, pw);
}

static void raise_client(Client *c) {
    detach(c); attach_top(c);
    XRaiseWindow(dpy, c->win);
    restack_frame(c);
    if (lopen && c != lclient) raise_panel(lclient, lwin);
    if (tmopen && c != tmclient) raise_panel(tmclient, tmwin);
    dirty = 1;
}

static void focus_client(Client *c) {
    if (c && (c->is_or || c->screenspace)) return;
    focused = c;
    if (c) {
        XSetInputFocus(dpy, c->win, RevertToPointerRoot, CurrentTime);
        send_proto(c, A_WM_TAKE_FOCUS);
        XChangeProperty(dpy, root, A_NET_ACTIVE, XA_WINDOW, 32,
                        PropModeReplace, (unsigned char *)&c->win, 1);
    } else {
        XSetInputFocus(dpy, root, RevertToPointerRoot, CurrentTime);
    }
    dirty = 1;
}

/* Real (server-side) geometry. Sizes are always the world size, so window
 * contents NEVER reflow when zooming — the compositor scales the pixels.
 * Positions use a pointer-anchored transform: the real layout is the
 * rendered layout scaled up by 1/zoom about the cursor, which makes the
 * real window point under the cursor coincide exactly with the rendered
 * point under the cursor. Input happens at the cursor, so the mapping is
 * always correct there; everything else may sit (far) offscreen. */
static double anchor_x(void) { return pcx * (1.0 - 1.0 / tzoom); }
static double anchor_y(void) { return pcy * (1.0 - 1.0 / tzoom); }

static void apply_geometry(Client *c) {
    int px, py, pw, ph;
    if (c->screenspace) return;
    px = (int)lround(c->x - tvx + anchor_x());
    py = (int)lround(c->y - tvy + anchor_y());
    pw = (int)fmax(1.0, lround(c->w));
    ph = (int)fmax(1.0, lround(c->h));
    if (px == c->lx && py == c->ly && pw == c->lw && ph == c->lh) return;
    if (pw != c->lw || ph != c->lh) free_pict(c);
    c->lx = px; c->ly = py; c->lw = pw; c->lh = ph;
    if (c->is_or) c->pending_cfg++;   /* see ConfigureNotify: menu moves */
    XMoveResizeWindow(dpy, c->win, px, py, (unsigned)pw, (unsigned)ph);
    if (c->frame) {
        /* decoration zones in real space are the rendered zones / zoom */
        int tb = (int)lround(TBAR);
        int m = (int)lround((BORDER + BANDW) / tzoom);
        XMoveResizeWindow(dpy, c->frame, px - m, py - tb - m,
                          (unsigned)(pw + 2 * m),
                          (unsigned)(ph + tb + 2 * m));
    }
}

static void apply_all_geometry(void) {
    Client *c;
    for (c = clients; c; c = c->next) apply_geometry(c);
}

/* ---- viewport -------------------------------------------------------- */
static void set_zoom_at(double nz, double ax, double ay) {
    double wx_, wy_;
    if (nz < 0.1) nz = 0.1;
    if (nz > 4.0) nz = 4.0;
    if (nz > 1.001) {
        /* past 100%, zoom moves in whole steps (100/200/300/400%) so the
         * magnification is exact pixel-doubling and text stays sharp */
        if (nz > tzoom)      nz = floor(tzoom + 1.0001);
        else if (nz < tzoom) nz = ceil(tzoom - 1.0001);
        if (nz < 1.0) nz = 1.0;
        if (nz > 4.0) nz = 4.0;
    } else if (fabs(nz - 1.0) < 0.07) {
        nz = 1.0;                          /* snap to 100% near 1 */
    }
    if (tzoom == 1.0 && nz != 1.0) {
        /* the pointer isn't polled while at 100%: refresh the anchor
         * before the real layout starts depending on it */
        Window qr, qw; int qx, qy, qwx, qwy; unsigned qm;
        if (XQueryPointer(dpy, root, &qr, &qw, &qx, &qy, &qwx, &qwy, &qm)) {
            pcx = qx; pcy = qy;
        }
    }
    /* keep world point under anchor (screen coords, at target view) fixed */
    wx_ = tvx + ax / tzoom;
    wy_ = tvy + ay / tzoom;
    tzoom = nz;
    tvx = wx_ - ax / tzoom;
    tvy = wy_ - ay / tzoom;
    dirty = 1;
}

static void pan(double dx, double dy) {       /* screen-px amounts */
    tvx += dx / tzoom;
    tvy += dy / tzoom;
    dirty = 1;
}

static void fly_to(Client *c) {
    /* pan (at the current zoom) so the window is centered */
    tvx = c->x + c->w / 2.0 - SW / (2.0 * tzoom);
    tvy = c->y + c->h / 2.0 - SH / (2.0 * tzoom);
    focus_client(c);
    raise_client(c);
}

static int step_animation(void) {
    double f = 0.35, eps = 0.5;
    int moving = fabs(vx - tvx) * zoom > eps || fabs(vy - tvy) * zoom > eps ||
                 fabs(zoom - tzoom) > 0.002;
    if (!moving) {
        if (vx != tvx || vy != tvy || zoom != tzoom) {
            vx = tvx; vy = tvy; zoom = tzoom;
            apply_all_geometry();
            dirty = 1;
        }
        return 0;
    }
    vx += (tvx - vx) * f;
    vy += (tvy - vy) * f;
    zoom += (tzoom - zoom) * f;
    apply_all_geometry();
    dirty = 1;
    return 1;
}

/* ---- snapping -------------------------------------------------------- */
static void set_fullscreen(Client *c, int on);

static void snap_client(Client *c, Snap mode) {
    double vw = SW / tzoom, vh = SH / tzoom;
    if (!c || c->is_or || c->screenspace) return;
    if (c->fullscreen) set_fullscreen(c, 0);
    if (mode == SNAP_FULL && c->snapped == SNAP_FULL) mode = SNAP_NONE;
    if (mode == SNAP_NONE) {
        if (c->snapped) {
            c->x = c->sx; c->y = c->sy; c->w = c->sw; c->h = c->sh;
            c->snapped = SNAP_NONE;
        }
    } else {
        if (!c->snapped) { c->sx = c->x; c->sy = c->y; c->sw = c->w; c->sh = c->h; }
        switch (mode) {   /* leave room for the title bar */
        case SNAP_L:    c->x = tvx;          c->y = tvy + TBAR; c->w = vw / 2; c->h = vh - TBAR; break;
        case SNAP_R:    c->x = tvx + vw / 2; c->y = tvy + TBAR; c->w = vw / 2; c->h = vh - TBAR; break;
        case SNAP_FULL: c->x = tvx;          c->y = tvy + TBAR; c->w = vw;     c->h = vh - TBAR; break;
        case SNAP_TL:   c->x = tvx;          c->y = tvy + TBAR;          c->w = vw / 2; c->h = vh / 2 - TBAR; break;
        case SNAP_TR:   c->x = tvx + vw / 2; c->y = tvy + TBAR;          c->w = vw / 2; c->h = vh / 2 - TBAR; break;
        case SNAP_BL:   c->x = tvx;          c->y = tvy + vh / 2 + TBAR; c->w = vw / 2; c->h = vh / 2 - TBAR; break;
        case SNAP_BR:   c->x = tvx + vw / 2; c->y = tvy + vh / 2 + TBAR; c->w = vw / 2; c->h = vh / 2 - TBAR; break;
        default: break;
        }
        c->snapped = mode;
    }
    apply_geometry(c);
    dirty = 1;
}

/* ---- window management ---------------------------------------------- */
static void send_configure(Client *c);

/* EWMH fullscreen: fill the viewport exactly, no decorations (what
 * browsers and video players request via _NET_WM_STATE_FULLSCREEN) */
static void set_fullscreen(Client *c, int on) {
    if (!c || c->is_or || c->screenspace || c->fullscreen == !!on) return;
    if (on) {
        c->fsx = c->x; c->fsy = c->y; c->fsw = c->w; c->fsh = c->h;
        c->x = tvx; c->y = tvy;
        c->w = SW / tzoom; c->h = SH / tzoom;
        c->fullscreen = 1;
        if (c->frame) XUnmapWindow(dpy, c->frame);
        XChangeProperty(dpy, c->win, A_NET_WM_STATE, XA_ATOM, 32,
                        PropModeReplace, (unsigned char *)&A_NET_WM_STATE_FS,
                        1);
        raise_client(c);
    } else {
        c->x = c->fsx; c->y = c->fsy; c->w = c->fsw; c->h = c->fsh;
        c->fullscreen = 0;
        if (c->frame && c->mapped) {
            XMapWindow(dpy, c->frame);
            restack_frame(c);
        }
        XDeleteProperty(dpy, c->win, A_NET_WM_STATE);
    }
    c->lx = -99999;
    apply_geometry(c);
    send_configure(c);
    dirty = 1;
}

static void update_title(Client *c) {
    XTextProperty tp;
    char *nm = NULL;
    c->title[0] = 0;
    /* prefer UTF-8 _NET_WM_NAME, fall back to legacy WM_NAME */
    if (XGetTextProperty(dpy, c->win, &tp, A_NET_WM_NAME) &&
        tp.value && tp.nitems) {
        int len = tp.nitems < sizeof c->title - 1 ? (int)tp.nitems
                                                  : (int)sizeof c->title - 1;
        memcpy(c->title, tp.value, (size_t)len);
        c->title[len] = 0;
        XFree(tp.value);
    }
    if (!c->title[0] && XFetchName(dpy, c->win, &nm) && nm) {
        snprintf(c->title, sizeof c->title, "%s", nm);
        XFree(nm);
    }
    if (!c->title[0]) snprintf(c->title, sizeof c->title, "window");
    dirty = 1;
}

static void manage(Window w, XWindowAttributes *wa) {
    Client *c;
    int want_fs = 0;
    if (find_client(w) || w == lwin || w == tmwin || w == backpm) return;
    c = calloc(1, sizeof(Client));
    c->win = w;
    c->is_or = wa->override_redirect;
    c->lx = c->ly = c->lw = c->lh = -1;
    if (c->is_or) {
        /* pin where it appeared: invert the pointer-anchored layout
         * (real -> world), using the settled target viewport */
        c->x = wa->x - anchor_x() + tvx;
        c->y = wa->y - anchor_y() + tvy;
        c->w = wa->width;   c->h = wa->height;
    } else {
        double vw = SW / tzoom, vh = SH / tzoom;
        long st = NormalState;
        /* app pixels == world units; content is never rescaled by zoom */
        c->w = wa->width  > 1 ? wa->width  : 640;
        c->h = wa->height > 1 ? wa->height : 480;
        if (c->w > vw) c->w = vw;
        if (c->h > vh - TBAR) c->h = vh - TBAR;
        c->x = tvx + (vw - c->w) / 2.0 + (placement_n % 7) * 36;
        c->y = tvy + TBAR + (vh - TBAR - c->h) / 2.0 + (placement_n % 7) * 28;
        placement_n++;
        {
            /* dialogs: center over the window they belong to */
            Window tr = None;
            Client *p;
            if (XGetTransientForHint(dpy, w, &tr) && tr &&
                (p = find_client(tr)) && !p->screenspace) {
                c->x = p->x + (p->w - c->w) / 2.0;
                c->y = p->y + (p->h - c->h) / 2.0;
                if (c->y < p->y + TBAR) c->y = p->y + TBAR;
            }
        }
        update_title(c);
        {
            /* program identity for the task manager */
            Atom rt; int fmt2; unsigned long n2, after2;
            unsigned char *d = NULL;
            XClassHint ch; memset(&ch, 0, sizeof ch);
            if (XGetWindowProperty(dpy, w, A_NET_WM_PID, 0, 1, False,
                                   XA_CARDINAL, &rt, &fmt2, &n2, &after2,
                                   &d) == Success && d) {
                if (n2) c->pid = (pid_t)*(unsigned long *)d;
                XFree(d);
            }
            if (XGetClassHint(dpy, w, &ch)) {
                if (ch.res_class) {
                    snprintf(c->cls, sizeof c->cls, "%s", ch.res_class);
                    XFree(ch.res_class);
                }
                if (ch.res_name) XFree(ch.res_name);
            }
        }
        {
            /* apps that set fullscreen before mapping (mpv --fs, games) */
            Atom rt; int fmt2; unsigned long n2, after2, k;
            unsigned char *d = NULL;
            if (XGetWindowProperty(dpy, w, A_NET_WM_STATE, 0, 8, False,
                                   XA_ATOM, &rt, &fmt2, &n2, &after2,
                                   &d) == Success && d) {
                Atom *as = (Atom *)d;
                for (k = 0; k < n2; k++)
                    if (as[k] == A_NET_WM_STATE_FS) want_fs = 1;
                XFree(d);
            }
        }
        {
            XSetWindowAttributes fa;
            fa.override_redirect = True;
            c->frame = XCreateWindow(dpy, root, 0, 0, 1, 1, 0, 0, InputOnly,
                                     CopyFromParent, CWOverrideRedirect, &fa);
            XSelectInput(dpy, c->frame, ButtonPressMask | ButtonReleaseMask |
                                        PointerMotionMask | EnterWindowMask |
                                        LeaveWindowMask);
        }
        XSelectInput(dpy, w, EnterWindowMask | StructureNotifyMask |
                             PropertyChangeMask);
        XGrabButton(dpy, Button1, AnyModifier, w, False, ButtonPressMask,
                    GrabModeSync, GrabModeAsync, None, None);
        XChangeProperty(dpy, w, A_WM_STATE, A_WM_STATE, 32, PropModeReplace,
                        (unsigned char *)&st, 2);
        apply_geometry(c);
        send_configure(c);
    }
#if HAVE_DAMAGE
    c->damage = XDamageCreate(dpy, w, XDamageReportNonEmpty);
#endif
    attach_top(c);
    if (!c->is_or) {
        update_client_list();
        if (want_fs) set_fullscreen(c, 1);
    }
    dirty = 1;
}

static void free_icon(Client *c);   /* fwd */
static void draw_tm(void);

static void unmanage(Client *c) {
    free_pict(c);
    free_icon(c);
    if (c->frame) { XDestroyWindow(dpy, c->frame); c->frame = 0; }
#if HAVE_DAMAGE
    if (c->damage) { XDamageDestroy(dpy, c->damage); c->damage = 0; }
#endif
    detach(c);
    if (lclient == c) lclient = NULL;
    if (hover_c == c) { hover_c = NULL; hover_hit = HIT_NONE; }
    if (drag_c == c) { drag_c = NULL; drag_mode = 0; }
    if (focused == c) {
        /* fall back to the topmost remaining window */
        Client *i, *t = NULL;
        focused = NULL;
        for (i = clients; i; i = i->next) if (eligible(i)) t = i;
        if (t) focus_client(t);
    }
    update_client_list();
    free(c);
    if (tmopen) draw_tm();
    dirty = 1;
}

static void close_client(Client *c) {
    if (!c) return;
    if (!send_proto(c, A_WM_DELETE))
        XKillClient(dpy, c->win);
}

/* ICCCM: tell the client where it ended up, even if nothing changed —
 * GTK apps wait for this after a ConfigureRequest. */
static void send_configure(Client *c) {
    XConfigureEvent ce;
    memset(&ce, 0, sizeof ce);
    ce.type = ConfigureNotify;
    ce.display = dpy;
    ce.event = c->win;
    ce.window = c->win;
    ce.x = c->lx; ce.y = c->ly;
    ce.width = c->lw > 0 ? c->lw : (int)c->w;
    ce.height = c->lh > 0 ? c->lh : (int)c->h;
    ce.border_width = 0;
    ce.above = None;
    ce.override_redirect = False;
    XSendEvent(dpy, c->win, False, StructureNotifyMask, (XEvent *)&ce);
}

static Client *client_at(double sx_, double sy_) {
    /* topmost client whose RENDERED rect (incl. title bar) contains point */
    Client *c, *hit = NULL;
    for (c = clients; c; c = c->next) {
        double rx, ry, rw, rh;
        if (!c->mapped || c->screenspace || c->is_or) continue;
        rx = w2sx(c->x); ry = w2sy(c->y) - TBAR * zoom;
        rw = c->w * zoom; rh = c->h * zoom + TBAR * zoom;
        if (sx_ >= rx && sx_ < rx + rw && sy_ >= ry && sy_ < ry + rh) hit = c;
    }
    return hit;
}

/* classify a screen point against one client's decorations. The InputOnly
 * frame window already resolved the stacking, so no scan is needed. */
static Hit decor_zone(Client *c, int sx_, int sy_, int *edges) {
    int rx = (int)lround(w2sx(c->x));
    int ry = (int)lround(w2sy(c->y));
    int rw = (int)lround(c->w * zoom);
    int rh = (int)lround(c->h * zoom);
    int tb = (int)fmax(2.0, lround(TBAR * zoom));
    int by = ry - tb;
    int e = 0;
    *edges = 0;
    /* title bar; buttons on the left like macOS: [close][fullscreen] */
    if (sx_ >= rx - BORDER && sx_ < rx + rw + BORDER && sy_ >= by && sy_ < ry) {
        if (rw > 3 * tb) {
            if (sx_ < rx + tb)     return HIT_CLOSE;
            if (sx_ < rx + 2 * tb) return HIT_MAX;
        }
        return HIT_TITLE;
    }
    /* otherwise: resize bands around the frame */
    if (sx_ <  rx)            e |= EDGE_W;
    if (sx_ >= rx + rw)       e |= EDGE_E;
    if (sy_ <  by)            e |= EDGE_N;
    if (sy_ >= ry + rh)       e |= EDGE_S;
    /* widen corner grips */
    if ((e & (EDGE_E | EDGE_W)) && !(e & (EDGE_N | EDGE_S))) {
        if (sy_ < by + 18) e |= EDGE_N;
        else if (sy_ >= ry + rh - 18) e |= EDGE_S;
    }
    if ((e & (EDGE_N | EDGE_S)) && !(e & (EDGE_E | EDGE_W))) {
        if (sx_ < rx + 18) e |= EDGE_W;
        else if (sx_ >= rx + rw - 18) e |= EDGE_E;
    }
    if (e) { *edges = e; return HIT_RESIZE; }
    return HIT_NONE;
}

static Cursor edge_cursor(int e) {
    int h = (e & (EDGE_E | EDGE_W)) != 0, v = (e & (EDGE_N | EDGE_S)) != 0;
    if (h && v) {
        if ((e & EDGE_N) && (e & EDGE_W)) return cur_nw;
        if ((e & EDGE_N) && (e & EDGE_E)) return cur_ne;
        if ((e & EDGE_S) && (e & EDGE_W)) return cur_sw;
        return cur_se;
    }
    return h ? cur_h : cur_v;
}

static Client *next_client(Client *c) {
    Client *i;
    for (i = c ? c->next : clients; i; i = i->next)
        if (eligible(i)) return i;
    for (i = clients; i && i != c; i = i->next)
        if (eligible(i)) return i;
    return (c && eligible(c)) ? c : NULL;
}

/* ---- compositor ------------------------------------------------------ */
static void make_backbuffer(void) {
    if (backpict) XRenderFreePicture(dpy, backpict);
    if (backpm) XFreePixmap(dpy, backpm);
    backpm = XCreatePixmap(dpy, root, (unsigned)SW, (unsigned)SH, (unsigned)depth);
    backpict = XRenderCreatePicture(dpy, backpm, fmt_rgb, 0, NULL);
}

/* load the wallpaper (if configured) and pre-scale it to cover the screen.
 * It is drawn in screen space, so it never moves with the canvas. */
static void load_wallpaper(void) {
    if (bgpict) { XRenderFreePicture(dpy, bgpict); bgpict = 0; }
    if (bgpm)   { XFreePixmap(dpy, bgpm); bgpm = 0; }
    if (!bg_image[0]) return;
#if HAVE_IMLIB2
    {
        char path[600];
        Imlib_Image im;
        int iw, ih, has_a;
        uint32_t *data, *buf;
        long npx, p;
        Pixmap spm;
        Picture spict;
        GC g32;
        XImage *img;
        XTransform t;
        double s;

        if (bg_image[0] == '~' && bg_image[1] == '/' && getenv("HOME"))
            snprintf(path, sizeof path, "%s/%s", getenv("HOME"), bg_image + 2);
        else
            snprintf(path, sizeof path, "%s", bg_image);
        im = imlib_load_image(path);
        if (!im) {
            fprintf(stderr, "infinawm: cannot load wallpaper %s\n", path);
            return;
        }
        imlib_context_set_image(im);
        iw = imlib_image_get_width();
        ih = imlib_image_get_height();
        has_a = imlib_image_has_alpha();
        data = (uint32_t *)imlib_image_get_data_for_reading_only();
        if (!data || iw < 1 || ih < 1) { imlib_free_image(); return; }
        npx = (long)iw * ih;
        buf = malloc((size_t)npx * 4);
        if (!buf) { imlib_free_image(); return; }
        for (p = 0; p < npx; p++) {
            uint32_t v = data[p];
            uint32_t a = has_a ? v >> 24 : 0xff;
            uint32_t r = (v >> 16) & 0xff, g = (v >> 8) & 0xff, b = v & 0xff;
            buf[p] = (a << 24) | ((r * a / 255) << 16) |
                     ((g * a / 255) << 8) | (b * a / 255);
        }
        imlib_free_image();

        spm = XCreatePixmap(dpy, root, (unsigned)iw, (unsigned)ih, 32);
        g32 = XCreateGC(dpy, spm, 0, NULL);
        img = XCreateImage(dpy, visual, 32, ZPixmap, 0, (char *)buf,
                           (unsigned)iw, (unsigned)ih, 32, 0);
        if (img) {
            XPutImage(dpy, spm, g32, img, 0, 0, 0, 0,
                      (unsigned)iw, (unsigned)ih);
            XDestroyImage(img);   /* frees buf */
        } else free(buf);
        XFreeGC(dpy, g32);
        spict = XRenderCreatePicture(dpy, spm, fmt_argb, 0, NULL);

        /* scale to cover, centered (crop the overflow axis) */
        s = (double)SW / iw;
        if ((double)SH / ih > s) s = (double)SH / ih;
        memset(&t, 0, sizeof t);
        t.matrix[0][0] = XDoubleToFixed(1.0 / s);
        t.matrix[1][1] = XDoubleToFixed(1.0 / s);
        t.matrix[0][2] = XDoubleToFixed((iw - SW / s) / 2.0);
        t.matrix[1][2] = XDoubleToFixed((ih - SH / s) / 2.0);
        t.matrix[2][2] = XDoubleToFixed(1.0);
        XRenderSetPictureTransform(dpy, spict, &t);
        XRenderSetPictureFilter(dpy, spict, FilterBilinear, NULL, 0);

        bgpm = XCreatePixmap(dpy, root, (unsigned)SW, (unsigned)SH,
                             (unsigned)depth);
        bgpict = XRenderCreatePicture(dpy, bgpm, fmt_rgb, 0, NULL);
        {
            XRenderColor bgc = rcol(col_bg);
            XRenderFillRectangle(dpy, PictOpSrc, bgpict, &bgc, 0, 0,
                                 (unsigned)SW, (unsigned)SH);
        }
        XRenderComposite(dpy, PictOpOver, spict, None, bgpict,
                         0, 0, 0, 0, 0, 0, (unsigned)SW, (unsigned)SH);
        XRenderFreePicture(dpy, spict);
        XFreePixmap(dpy, spm);
    }
#else
    fprintf(stderr, "infinawm: built without Imlib2; "
                    "background_image ignored (apt install libimlib2-dev)\n");
#endif
}

static void ensure_pict(Client *c) {
    XWindowAttributes wa;
    XRenderPictFormat *f;
    XRenderPictureAttributes pa;
    if (c->pict || !c->mapped) return;
    if (!XGetWindowAttributes(dpy, c->win, &wa)) return;
    if (wa.class == InputOnly || wa.map_state != IsViewable) return;
    f = XRenderFindVisualFormat(dpy, wa.visual);
    if (!f) f = fmt_rgb;
    c->pm = XCompositeNameWindowPixmap(dpy, c->win);
    if (!c->pm) return;
    pa.subwindow_mode = IncludeInferiors;
    c->pict = XRenderCreatePicture(dpy, c->pm, f, CPSubwindowMode, &pa);
    c->pict_zoom = -1;
}

static void set_pict_zoom(Client *c, double z) {
    XTransform t;
    if (!c->pict || c->pict_zoom == z) return;
    memset(&t, 0, sizeof t);
    t.matrix[0][0] = XDoubleToFixed(1.0 / z);
    t.matrix[1][1] = XDoubleToFixed(1.0 / z);
    t.matrix[2][2] = XDoubleToFixed(1.0);
    XRenderSetPictureTransform(dpy, c->pict, &t);
    /* zoom-out: bilinear (smooth shrink). Integer zoom-in: nearest, i.e.
     * exact pixel-doubling with sharp edges. Transients: bilinear. */
    XRenderSetPictureFilter(dpy, c->pict,
                            (z > 0.999 && fabs(z - nearbyint(z)) < 0.01)
                                ? FilterNearest : FilterBilinear,
                            NULL, 0);
    c->pict_zoom = z;
}

static void paint(void) {
    XRenderColor col;
    Client *c;
    double spacing;
    int cx1 = 0, cy1 = 0, cx2 = SW, cy2 = SH, partial = 0;

#if HAVE_DAMAGE
    /* app-content damage only (typing, scrolling): repaint just the
     * damaged box instead of the whole screen */
    if (!dirty && pdirty) {
        cx1 = dmgx1 > 0 ? dmgx1 : 0;
        cy1 = dmgy1 > 0 ? dmgy1 : 0;
        cx2 = dmgx2 < SW ? dmgx2 : SW;
        cy2 = dmgy2 < SH ? dmgy2 : SH;
        if (cx2 <= cx1 || cy2 <= cy1) { pdirty = 0; return; }
        partial = cx1 > 0 || cy1 > 0 || cx2 < SW || cy2 < SH;
    }
    pdirty = 0;
#endif
    if (partial) {
        XRectangle r;
        r.x = (short)cx1; r.y = (short)cy1;
        r.width  = (unsigned short)(cx2 - cx1);
        r.height = (unsigned short)(cy2 - cy1);
        XRenderSetPictureClipRectangles(dpy, backpict, 0, 0, &r, 1);
        if (dgc) XSetClipRectangles(dpy, dgc, 0, 0, &r, 1, Unsorted);
    }

    if (bgpict) {
        /* wallpaper: screen-fixed, the canvas moves over it */
        XRenderComposite(dpy, PictOpSrc, bgpict, None, backpict,
                         0, 0, 0, 0, 0, 0, (unsigned)SW, (unsigned)SH);
    } else {
        col = rcol(col_bg);
        XRenderFillRectangle(dpy, PictOpSrc, backpict, &col, 0, 0,
                             (unsigned)SW, (unsigned)SH);
    }

    /* canvas dot grid so panning/zoom is visible (color background only) */
    spacing = 96.0 * zoom;
    if (!bgpict && spacing >= 14.0) {
        double ox = fmod(-vx * zoom, spacing); if (ox < 0) ox += spacing;
        double oy = fmod(-vy * zoom, spacing); if (oy < 0) oy += spacing;
        double X, Y;
        col = rcol(COL_DOT);
        for (Y = oy; Y < cy2; Y += spacing) {
            if (Y + 2 < cy1) continue;
            for (X = ox; X < cx2; X += spacing) {
                if (X + 2 < cx1) continue;
                XRenderFillRectangle(dpy, PictOpSrc, backpict, &col,
                                     (int)X, (int)Y, 2, 2);
            }
        }
    }

    for (c = clients; c; c = c->next) {
        int rx, ry, rw, rh;
        if (!c->mapped) continue;
        if (c->screenspace) {
            rx = c->lx; ry = c->ly; rw = c->lw; rh = c->lh;
        } else {
            rx = (int)lround(w2sx(c->x));
            ry = (int)lround(w2sy(c->y));
            rw = (int)fmax(1.0, lround(c->w * zoom));
            rh = (int)fmax(1.0, lround(c->h * zoom));
        }
        if (rx + rw + BORDER < cx1 || ry + rh + BORDER < cy1 ||
            rx - BORDER > cx2 || ry - TBAR * zoom - BORDER > cy2)
            continue;
        ensure_pict(c);
        if (!c->pict) continue;
        /* window pixmaps are world-sized; scale them by the animated zoom */
        set_pict_zoom(c, c->screenspace ? 1.0 : zoom);
        if (!c->is_or && !c->screenspace && !c->fullscreen) {
            int tb = (int)fmax(2.0, lround(TBAR * zoom));
            int by = ry - tb;
            int focusedc = (c == focused);
            /* frame (border ring + title bar background) */
            col = rcol(focusedc ? col_focus : col_border);
            XRenderFillRectangle(dpy, PictOpSrc, backpict, &col,
                                 rx - BORDER, by - BORDER,
                                 (unsigned)(rw + 2 * BORDER),
                                 (unsigned)(rh + tb + 2 * BORDER));
            col = rcol(focusedc ? COL_TBARF : COL_TBAR);
            XRenderFillRectangle(dpy, PictOpSrc, backpict, &col,
                                 rx, by, (unsigned)rw, (unsigned)tb);
            /* separator under the title bar completes the outline square */
            col = rcol(focusedc ? col_focus : col_border);
            XRenderFillRectangle(dpy, PictOpSrc, backpict, &col,
                                 rx, ry - BORDER > by ? ry - BORDER : by,
                                 (unsigned)rw, (unsigned)BORDER);
            /* macOS-style traffic lights, left side: [close][fullscreen] */
            if (tb >= 8 && rw > 3 * tb && dgc) {
                int d = (int)fmax(6.0, tb * 0.5);
                int gap = (tb - d) / 2;
                int cx1 = rx + gap;          /* red close dot   */
                int cx2 = rx + tb + gap;     /* green fullscreen dot */
                int cy = by + gap;
                int hov = (c == hover_c &&
                           (hover_hit == HIT_CLOSE || hover_hit == HIT_MAX));
                XSetForeground(dpy, dgc, focusedc || hov ? COL_BTN_CLOSE
                                                         : COL_BTN_IDLE);
                XFillArc(dpy, backpm, dgc, cx1, cy, (unsigned)d, (unsigned)d,
                         0, 360 * 64);
                XSetForeground(dpy, dgc, focusedc || hov ? COL_BTN_MAX
                                                         : COL_BTN_IDLE);
                XFillArc(dpy, backpm, dgc, cx2, cy, (unsigned)d, (unsigned)d,
                         0, 360 * 64);
                if (hov && d >= 8) {
                    /* x glyph in the red dot */
                    int i = d * 3 / 10;
                    XSetForeground(dpy, dgc, COL_GLYPH_CLO);
                    XDrawLine(dpy, backpm, dgc, cx1 + i, cy + i,
                              cx1 + d - i, cy + d - i);
                    XDrawLine(dpy, backpm, dgc, cx1 + d - i, cy + i,
                              cx1 + i, cy + d - i);
                    /* expand glyph (two triangles) in the green dot */
                    int s = d - 2 * i;
                    XPoint t1[3] = {
                        { (short)(cx2 + i),     (short)(cy + i) },
                        { (short)(cx2 + i + s - 2), (short)(cy + i) },
                        { (short)(cx2 + i),     (short)(cy + i + s - 2) } };
                    XPoint t2[3] = {
                        { (short)(cx2 + d - i),     (short)(cy + d - i) },
                        { (short)(cx2 + d - i - s + 2), (short)(cy + d - i) },
                        { (short)(cx2 + d - i),     (short)(cy + d - i - s + 2) } };
                    XSetForeground(dpy, dgc, COL_GLYPH_MAX);
                    XFillPolygon(dpy, backpm, dgc, t1, 3, Convex,
                                 CoordModeOrigin);
                    XFillPolygon(dpy, backpm, dgc, t2, 3, Convex,
                                 CoordModeOrigin);
                }
            }
            /* title text, centered like macOS */
            if (tb >= 15 && lfont && dgc && c->title[0]) {
                int maxw = rw - 4 * tb - 12, len = (int)strlen(c->title);
                while (len > 0 && XTextWidth(lfont, c->title, len) > maxw)
                    len--;
                if (len > 0) {
                    int tw = XTextWidth(lfont, c->title, len);
                    int tx = rx + (rw - tw) / 2;
                    if (tx < rx + 2 * tb + 6) tx = rx + 2 * tb + 6;
                    XSetForeground(dpy, dgc,
                                   focusedc ? COL_TTEXTF : COL_TTEXT);
                    XDrawString(dpy, backpm, dgc, tx,
                                by + (tb + lfont->ascent) / 2 - 1,
                                c->title, len);
                }
            }
        }
        XRenderComposite(dpy, PictOpOver, c->pict, None, backpict,
                         0, 0, 0, 0, rx, ry, (unsigned)rw, (unsigned)rh);
    }

    XRenderComposite(dpy, PictOpSrc, backpict, None, rootpict,
                     cx1, cy1, 0, 0, cx1, cy1,
                     (unsigned)(cx2 - cx1), (unsigned)(cy2 - cy1));
    if (partial) {
        XRenderPictureAttributes pa;
        pa.clip_mask = None;
        XRenderChangePicture(dpy, backpict, CPClipMask, &pa);
        if (dgc) XSetClipMask(dpy, dgc, None);
    }
    dirty = 0;
}

/* ---- launcher -------------------------------------------------------- */
static int bin_cmp(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}
static void scan_path(void) {
    char *path, *p, *tok;
    if (nbins >= 0) return;
    nbins = 0;
    path = getenv("PATH");
    if (!path) return;
    p = strdup(path);
    for (tok = strtok(p, ":"); tok && nbins < MAX_BINS; tok = strtok(NULL, ":")) {
        DIR *d = opendir(tok);
        struct dirent *e;
        if (!d) continue;
        while ((e = readdir(d)) && nbins < MAX_BINS) {
            char full[1024];
            struct stat st;
            if (e->d_name[0] == '.') continue;
            snprintf(full, sizeof full, "%s/%s", tok, e->d_name);
            if (stat(full, &st) == 0 && S_ISREG(st.st_mode) && (st.st_mode & 0111))
                bins[nbins++] = strdup(e->d_name);
        }
        closedir(d);
    }
    free(p);
    qsort(bins, (size_t)nbins, sizeof(char *), bin_cmp);
}
static const char *completion(void) {
    int i, len = (int)strlen(ltext);
    if (!len) return NULL;
    for (i = 0; i < nbins; i++)
        if (!strncmp(bins[i], ltext, (size_t)len) && strcmp(bins[i], ltext))
            return bins[i];
    return NULL;
}

static void draw_launcher(void) {
    const char *hint = completion();
    int tx = 14, ty = LAUNCHER_H / 2 + (lfont ? lfont->ascent / 2 : 5);
    char buf[300];
    XSetForeground(dpy, lgc, COL_LBG);
    XFillRectangle(dpy, lwin, lgc, 0, 0, LAUNCHER_W, LAUNCHER_H);
    XSetForeground(dpy, lgc, COL_LACCENT);
    XFillRectangle(dpy, lwin, lgc, 0, LAUNCHER_H - 3, LAUNCHER_W, 3);
    XSetForeground(dpy, lgc, COL_LHINT);
    XDrawString(dpy, lwin, lgc, tx, ty, "run:", 4);
    snprintf(buf, sizeof buf, "%s_", ltext);
    XSetForeground(dpy, lgc, COL_LFG);
    XDrawString(dpy, lwin, lgc, tx + 50, ty, buf, (int)strlen(buf));
    if (hint && lfont) {
        int off = XTextWidth(lfont, ltext, (int)strlen(ltext));
        XSetForeground(dpy, lgc, COL_LHINT);
        XDrawString(dpy, lwin, lgc, tx + 50 + off, ty,
                    hint + strlen(ltext), (int)strlen(hint + strlen(ltext)));
    }
    dirty = 1;
}

static void open_launcher(void) {
    if (lopen) return;
    scan_path();
    ltext[0] = 0;
    lopen = 1;
    XMoveResizeWindow(dpy, lwin, (SW - LAUNCHER_W) / 2, 10, LAUNCHER_W, LAUNCHER_H);
    XMapRaised(dpy, lwin);
    raise_panel(lclient, lwin);
    XGrabKeyboard(dpy, root, False, GrabModeAsync, GrabModeAsync, CurrentTime);
    if (lclient) { lclient->mapped = 1; free_pict(lclient); }
    draw_launcher();
}
static void close_launcher(void) {
    if (!lopen) return;
    lopen = 0;
    XUngrabKeyboard(dpy, CurrentTime);
    XUnmapWindow(dpy, lwin);
    if (lclient) { lclient->mapped = 0; free_pict(lclient); }
    if (focused) XSetInputFocus(dpy, focused->win, RevertToPointerRoot, CurrentTime);
    dirty = 1;
}
static void launcher_key(XKeyEvent *ev) {
    char buf[32];
    KeySym ks;
    int n = XLookupString(ev, buf, sizeof buf - 1, &ks, NULL);
    size_t len = strlen(ltext);
    if (ks == XK_Escape) { close_launcher(); return; }
    if (ks == XK_Return) {
        char cmd[300];
        snprintf(cmd, sizeof cmd, "%s", ltext);
        close_launcher();
        spawn(cmd);
        return;
    }
    if (ks == XK_Tab) {
        const char *h = completion();
        if (h) snprintf(ltext, sizeof ltext, "%s", h);
    } else if (ks == XK_BackSpace) {
        if (len) ltext[len - 1] = 0;
    } else if (n > 0 && buf[0] >= 32 && buf[0] < 127) {
        if (len + (size_t)n < sizeof ltext - 1) {
            memcpy(ltext + len, buf, (size_t)n);
            ltext[len + (size_t)n] = 0;
        }
    }
    draw_launcher();
}

/* ---- task manager (Super+M) ------------------------------------------ */
static void free_icon(Client *c) {
    if (c->icon) { XRenderFreePicture(dpy, c->icon); c->icon = 0; }
    if (c->icon_pm) { XFreePixmap(dpy, c->icon_pm); c->icon_pm = 0; }
}

/* decode _NET_WM_ICON (ARGB cardinals) into a TM_ICON-sized picture */
static void ensure_icon(Client *c) {
    Atom rt; int fmt2;
    unsigned long n, after, *arr;
    unsigned char *d = NULL;
    unsigned long bw = 0, bh = 0, *bpix = NULL;
    unsigned long i;
    if (c->icon) return;
    if (XGetWindowProperty(dpy, c->win, A_NET_WM_ICON, 0, 1 << 20, False,
                           XA_CARDINAL, &rt, &fmt2, &n, &after, &d) != Success
        || !d) return;
    if (fmt2 != 32) { XFree(d); return; }
    arr = (unsigned long *)d;
    /* pick the icon closest to TM_ICON (prefer the smallest >= TM_ICON) */
    i = 0;
    while (i + 2 <= n) {
        unsigned long w_ = arr[i], h_ = arr[i + 1];
        if (!w_ || !h_ || w_ > 1024 || h_ > 1024 ||
            i + 2 + w_ * h_ > n) break;
        if (!bpix ||
            (bw < TM_ICON && w_ > bw) ||
            (w_ >= TM_ICON && (bw < TM_ICON || w_ < bw))) {
            bw = w_; bh = h_; bpix = arr + i + 2;
        }
        i += 2 + w_ * h_;
    }
    if (bpix && bw && bh && bw <= 512 && bh <= 512) {
        uint32_t *buf = malloc(bw * bh * 4);
        XImage *img;
        GC g32;
        unsigned long p;
        if (buf) {
            for (p = 0; p < bw * bh; p++) {
                uint32_t v = (uint32_t)bpix[p];
                uint32_t a = v >> 24, r = (v >> 16) & 0xff,
                         g = (v >> 8) & 0xff, b = v & 0xff;
                /* premultiply for XRender */
                buf[p] = (a << 24) | ((r * a / 255) << 16) |
                         ((g * a / 255) << 8) | (b * a / 255);
            }
            c->icon_pm = XCreatePixmap(dpy, root, (unsigned)bw, (unsigned)bh, 32);
            g32 = XCreateGC(dpy, c->icon_pm, 0, NULL);
            img = XCreateImage(dpy, visual, 32, ZPixmap, 0, (char *)buf,
                               (unsigned)bw, (unsigned)bh, 32, 0);
            if (img) {
                XPutImage(dpy, c->icon_pm, g32, img, 0, 0, 0, 0,
                          (unsigned)bw, (unsigned)bh);
                XDestroyImage(img);   /* frees buf */
            } else free(buf);
            XFreeGC(dpy, g32);
            c->icon = XRenderCreatePicture(dpy, c->icon_pm, fmt_argb, 0, NULL);
            if (c->icon) {
                XTransform t;
                memset(&t, 0, sizeof t);
                t.matrix[0][0] = XDoubleToFixed((double)bw / TM_ICON);
                t.matrix[1][1] = XDoubleToFixed((double)bh / TM_ICON);
                t.matrix[2][2] = XDoubleToFixed(1.0);
                XRenderSetPictureTransform(dpy, c->icon, &t);
                XRenderSetPictureFilter(dpy, c->icon, FilterBilinear, NULL, 0);
            }
        }
    }
    XFree(d);
}

static int tm_is_listed(Client *c) {
    return eligible(c);
}

/* total cpu%% (/proc/stat) and ram%% (/proc/meminfo), once per second */
/* battery: /sys/class/power_supply/<name>/{capacity,status}. The name
 * comes from the config ("battery:"), or the first type==Battery entry. */
static int bat_pct = -1;        /* -1: none found / unreadable */
static int bat_charging;
static char bat_auto[64];       /* cached auto-detected name */

static void sample_battery(void) {
    char path[160], buf[64];
    const char *name = battery_name[0] ? battery_name : bat_auto;
    FILE *f;
    bat_pct = -1;
    bat_charging = 0;
    if (!name[0]) {
        DIR *d = opendir("/sys/class/power_supply");
        struct dirent *e;
        if (d) {
            while ((e = readdir(d))) {
                if (e->d_name[0] == '.') continue;
                snprintf(path, sizeof path,
                         "/sys/class/power_supply/%s/type", e->d_name);
                f = fopen(path, "r");
                if (!f) continue;
                buf[0] = 0;
                if (!fgets(buf, sizeof buf, f)) buf[0] = 0;
                fclose(f);
                if (!strncmp(buf, "Battery", 7)) {
                    snprintf(bat_auto, sizeof bat_auto, "%s", e->d_name);
                    break;
                }
            }
            closedir(d);
        }
        name = bat_auto;
        if (!name[0]) return;
    }
    snprintf(path, sizeof path, "/sys/class/power_supply/%s/capacity", name);
    f = fopen(path, "r");
    if (f) {
        if (fgets(buf, sizeof buf, f)) bat_pct = atoi(buf);
        fclose(f);
    } else if (!battery_name[0]) {
        bat_auto[0] = 0;        /* battery vanished: re-detect next time */
        return;
    }
    if (bat_pct > 100) bat_pct = 100;
    snprintf(path, sizeof path, "/sys/class/power_supply/%s/status", name);
    f = fopen(path, "r");
    if (f) {
        if (fgets(buf, sizeof buf, f) &&
            (!strncmp(buf, "Charging", 8) || !strncmp(buf, "Full", 4)))
            bat_charging = 1;
        fclose(f);
    }
}

static void sample_system(void) {
    FILE *f;
    char buf[256];
    double cpu = hist_len ? hist_cpu[hist_len - 1] : 0.0, ram = 0.0;
    f = fopen("/proc/stat", "r");
    if (f) {
        unsigned long long u = 0, n = 0, s = 0, i = 0, io = 0,
                           irq = 0, sirq = 0, st = 0;
        if (fgets(buf, sizeof buf, f) &&
            sscanf(buf, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
                   &u, &n, &s, &i, &io, &irq, &sirq, &st) >= 4) {
            unsigned long long total = u + n + s + i + io + irq + sirq + st;
            unsigned long long busy = total - i - io;
            if (cpu_prev_total && total > cpu_prev_total)
                cpu = 100.0 * (double)(busy - cpu_prev_busy)
                      / (double)(total - cpu_prev_total);
            cpu_prev_total = total; cpu_prev_busy = busy;
        }
        fclose(f);
    }
    f = fopen("/proc/meminfo", "r");
    if (f) {
        long total = 0, avail = 0;
        while (fgets(buf, sizeof buf, f)) {
            if (!strncmp(buf, "MemTotal:", 9)) sscanf(buf + 9, "%ld", &total);
            if (!strncmp(buf, "MemAvailable:", 13))
                sscanf(buf + 13, "%ld", &avail);
        }
        fclose(f);
        if (total > 0) ram = 100.0 * (total - avail) / total;
    }
    if (cpu < 0) cpu = 0; if (cpu > 100) cpu = 100;
    if (ram < 0) ram = 0; if (ram > 100) ram = 100;
    if (hist_len == HIST_MAX) {
        memmove(hist_cpu, hist_cpu + 1, (HIST_MAX - 1) * sizeof(double));
        memmove(hist_ram, hist_ram + 1, (HIST_MAX - 1) * sizeof(double));
        hist_len--;
    }
    hist_cpu[hist_len] = cpu;
    hist_ram[hist_len] = ram;
    hist_len++;
    sample_battery();
    sys_sampled = now_s();
}

/* sample cpu (utime+stime from /proc/pid/stat) and rss for all clients */
static void tm_sample(void) {
    Client *c;
    double t = now_s();
    long hz = sysconf(_SC_CLK_TCK);
    if (hz <= 0) hz = 100;
    for (c = clients; c; c = c->next) {
        char path[64], buf[512], *p;
        FILE *f;
        unsigned long ut = 0, st = 0;
        if (!tm_is_listed(c) || c->pid <= 0) continue;
        snprintf(path, sizeof path, "/proc/%d/stat", (int)c->pid);
        f = fopen(path, "r");
        if (f) {
            if (fgets(buf, sizeof buf, f) && (p = strrchr(buf, ')'))) {
                /* after ')': state ppid pgrp sess tty tpgid flags minflt
                 * cminflt majflt cmajflt utime stime ... */
                sscanf(p + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u "
                              "%lu %lu", &ut, &st);
                if (c->cpu_t > 0 && t > c->cpu_t)
                    c->cpu_pct = 100.0 * (double)(ut + st - c->cpu_ticks)
                                 / hz / (t - c->cpu_t);
                if (c->cpu_pct < 0) c->cpu_pct = 0;
                c->cpu_ticks = ut + st;
                c->cpu_t = t;
            }
            fclose(f);
        }
        snprintf(path, sizeof path, "/proc/%d/status", (int)c->pid);
        f = fopen(path, "r");
        if (f) {
            while (fgets(buf, sizeof buf, f))
                if (!strncmp(buf, "VmRSS:", 6)) {
                    sscanf(buf + 6, "%ld", &c->rss_kb);
                    break;
                }
            fclose(f);
        }
    }
    tm_sampled = t;
}

static void draw_tm(void) {
    Client *c;
    int n = 0, i, h, y, traytop;
    char buf[160];
    for (c = clients; c && n < TM_MAXROWS; c = c->next)
        if (tm_is_listed(c)) tm_rows[n++] = c;
    tm_nrows = n;
    traytop = TM_ROWTOP + (n ? n * TM_ROWH : TM_ROWH) + 6;
    h = traytop + TRAY_ROW + 8;
    XMoveResizeWindow(dpy, tmwin, (SW - TM_W) / 2, (SH - h) / 2,
                      TM_W, (unsigned)h);
    if (tmclient) {
        if (tmclient->lh != h) free_pict(tmclient);
        tmclient->lx = (SW - TM_W) / 2; tmclient->ly = (SH - h) / 2;
        tmclient->lw = TM_W; tmclient->lh = h;
    }
    /* panel: black, white outline, like the window theme */
    XSetForeground(dpy, tmgc, 0x000000);
    XFillRectangle(dpy, tmwin, tmgc, 0, 0, TM_W, (unsigned)h);
    XSetForeground(dpy, tmgc, 0xffffff);
    XDrawRectangle(dpy, tmwin, tmgc, 0, 0, TM_W - 1, (unsigned)(h - 1));
    XDrawString(dpy, tmwin, tmgc, 14, 24, "Open Windows", 12);
    XSetForeground(dpy, tmgc, 0x8e8e8e);
    XDrawString(dpy, tmwin, tmgc, TM_W - 170, 24, "RAM", 3);
    XDrawString(dpy, tmwin, tmgc, TM_W - 70, 24, "CPU", 3);
    XSetForeground(dpy, tmgc, 0x333333);
    XDrawLine(dpy, tmwin, tmgc, 8, TM_HDR - 6, TM_W - 8, TM_HDR - 6);
    /* system graph: blue = total cpu, red = ram, 1 sample/second */
    {
        int gx = 12, gy = TM_HDR, gw = TM_W - 24, gh = TM_GRAPH - 12;
        double step = (double)gw / (HIST_MAX - 1);
        XPoint pc[HIST_MAX], pr[HIST_MAX];
        int k;
        XSetForeground(dpy, tmgc, 0x333333);
        XDrawRectangle(dpy, tmwin, tmgc, gx, gy, (unsigned)gw, (unsigned)gh);
        XSetForeground(dpy, tmgc, 0x1c1c1c);
        XDrawLine(dpy, tmwin, tmgc, gx + 1, gy + gh / 2,
                  gx + gw - 1, gy + gh / 2);
        for (k = 0; k < hist_len; k++) {
            int xk = gx + gw - (int)((hist_len - 1 - k) * step);
            if (xk <= gx) xk = gx + 1;
            pc[k].x = (short)xk;
            pc[k].y = (short)(gy + gh - 1 - (int)(hist_cpu[k] / 100.0 * (gh - 2)));
            pr[k].x = (short)xk;
            pr[k].y = (short)(gy + gh - 1 - (int)(hist_ram[k] / 100.0 * (gh - 2)));
        }
        if (hist_len > 1) {
            XSetForeground(dpy, tmgc, 0xff5f57);          /* ram: red  */
            XDrawLines(dpy, tmwin, tmgc, pr, hist_len, CoordModeOrigin);
            XSetForeground(dpy, tmgc, 0x4a90e2);          /* cpu: blue */
            XDrawLines(dpy, tmwin, tmgc, pc, hist_len, CoordModeOrigin);
        }
        if (hist_len) {
            snprintf(buf, sizeof buf, "CPU %.0f%%", hist_cpu[hist_len - 1]);
            XSetForeground(dpy, tmgc, 0x4a90e2);
            XDrawString(dpy, tmwin, tmgc, gx + 6, gy + 14, buf,
                        (int)strlen(buf));
            snprintf(buf, sizeof buf, "RAM %.0f%%", hist_ram[hist_len - 1]);
            XSetForeground(dpy, tmgc, 0xff5f57);
            XDrawString(dpy, tmwin, tmgc, gx + 96, gy + 14, buf,
                        (int)strlen(buf));
        }
        /* battery: green while charging/full, red when low, grey idle.
         * "BAT --" if the configured battery can't be read. */
        if (bat_pct >= 0 || battery_name[0]) {
            if (bat_pct >= 0)
                snprintf(buf, sizeof buf, "%s %d%%%s",
                         battery_name[0] ? battery_name : "BAT", bat_pct,
                         bat_charging ? "+" : "");
            else
                snprintf(buf, sizeof buf, "%s --", battery_name);
            XSetForeground(dpy, tmgc,
                           bat_charging                  ? 0x28c840 :
                           bat_pct >= 0 && bat_pct <= 15 ? 0xff5f57 :
                                                           0xc8c8c8);
            XDrawString(dpy, tmwin, tmgc, gx + 186, gy + 14, buf,
                        (int)strlen(buf));
        }
    }
    if (!n) {
        XSetForeground(dpy, tmgc, 0x8e8e8e);
        XDrawString(dpy, tmwin, tmgc, 14, TM_ROWTOP + 20,
                    "(no windows open)", 17);
    }
    for (i = 0; i < n; i++) {
        const char *name;
        int len, ty;
        c = tm_rows[i];
        y = TM_ROWTOP + i * TM_ROWH;
        ty = y + TM_ROWH / 2 + (lfont ? lfont->ascent / 2 : 5) - 2;
        /* icon, or a letter box fallback */
        ensure_icon(c);
        if (c->icon && tmpict) {
            XRenderComposite(dpy, PictOpOver, c->icon, None, tmpict,
                             0, 0, 0, 0, 12, y + (TM_ROWH - TM_ICON) / 2,
                             TM_ICON, TM_ICON);
        } else {
            unsigned long hcol[] = { 0x4a90e2, 0x28c840, 0xff5f57, 0xd9a521,
                                     0x9b59b6, 0x16a085 };
            const char *nm = c->cls[0] ? c->cls : c->title;
            char init[2] = { nm[0] ? nm[0] : '?', 0 };
            unsigned hv = 0; const char *q;
            for (q = nm; *q; q++) hv = hv * 31 + (unsigned char)*q;
            XSetForeground(dpy, tmgc, hcol[hv % 6]);
            XFillRectangle(dpy, tmwin, tmgc, 12,
                           y + (TM_ROWH - TM_ICON) / 2, TM_ICON, TM_ICON);
            XSetForeground(dpy, tmgc, 0xffffff);
            XDrawString(dpy, tmwin, tmgc, 12 + 6,
                        y + (TM_ROWH + TM_ICON) / 2 - 5, init, 1);
        }
        /* name */
        name = c->cls[0] ? c->cls : c->title;
        snprintf(buf, sizeof buf, "%s", name);
        len = (int)strlen(buf);
        while (len > 0 && lfont &&
               XTextWidth(lfont, buf, len) > TM_W - 200 - 44) len--;
        XSetForeground(dpy, tmgc, c == focused ? 0xffffff : 0xc8c8c8);
        XDrawString(dpy, tmwin, tmgc, 44, ty, buf, len);
        /* ram + cpu */
        XSetForeground(dpy, tmgc, 0xc8c8c8);
        if (c->pid > 0) {
            if (c->rss_kb >= 1024)
                snprintf(buf, sizeof buf, "%.1f MB", c->rss_kb / 1024.0);
            else
                snprintf(buf, sizeof buf, "%ld kB", c->rss_kb);
            XDrawString(dpy, tmwin, tmgc, TM_W - 170, ty, buf,
                        (int)strlen(buf));
            snprintf(buf, sizeof buf, "%.1f%%", c->cpu_pct);
            XDrawString(dpy, tmwin, tmgc, TM_W - 70, ty, buf,
                        (int)strlen(buf));
        } else {
            XDrawString(dpy, tmwin, tmgc, TM_W - 170, ty, "-", 1);
            XDrawString(dpy, tmwin, tmgc, TM_W - 70, ty, "-", 1);
        }
        if (i) {
            XSetForeground(dpy, tmgc, 0x1c1c1c);
            XDrawLine(dpy, tmwin, tmgc, 8, y, TM_W - 8, y);
        }
    }
    /* system tray row */
    XSetForeground(dpy, tmgc, 0x333333);
    XDrawLine(dpy, tmwin, tmgc, 8, traytop, TM_W - 8, traytop);
    XSetForeground(dpy, tmgc, 0x8e8e8e);
    XDrawString(dpy, tmwin, tmgc, 14,
                traytop + TRAY_ROW / 2 + (lfont ? lfont->ascent / 2 : 5) - 2,
                "tray:", 5);
    if (!ntray) {
        XSetForeground(dpy, tmgc, 0x555555);
        XDrawString(dpy, tmwin, tmgc, 64,
                    traytop + TRAY_ROW / 2 + (lfont ? lfont->ascent / 2 : 5) - 2,
                    "(empty)", 7);
    }
    for (i = 0; i < ntray; i++)
        XMoveWindow(dpy, tray[i], 64 + i * (TRAY_PX + 8),
                    traytop + (TRAY_ROW - TRAY_PX) / 2 + 1);
    dirty = 1;
}

/* dock an XEmbed tray icon (nm-applet, pavucontrol, ...) into the panel */
static void tray_dock(Window w) {
    int i;
    XEvent e;
    if (!w || ntray >= TRAY_MAX) return;
    for (i = 0; i < ntray; i++) if (tray[i] == w) return;
    XSelectInput(dpy, w, StructureNotifyMask);
    XReparentWindow(dpy, w, tmwin, 0, 0);
    XResizeWindow(dpy, w, TRAY_PX, TRAY_PX);
    XSetWindowBackgroundPixmap(dpy, w, ParentRelative); /* may BadMatch: ok */
    tray[ntray++] = w;
    memset(&e, 0, sizeof e);
    e.xclient.type = ClientMessage;
    e.xclient.window = w;
    e.xclient.message_type = A_XEMBED;
    e.xclient.format = 32;
    e.xclient.data.l[0] = CurrentTime;
    e.xclient.data.l[1] = 0;            /* XEMBED_EMBEDDED_NOTIFY */
    e.xclient.data.l[3] = (long)tmwin;
    XSendEvent(dpy, w, False, NoEventMask, &e);
    XMapWindow(dpy, w);
    if (tmopen) draw_tm();
    dirty = 1;
}

static void tray_remove(Window w) {
    int i, j;
    for (i = 0; i < ntray; i++) {
        if (tray[i] == w) {
            for (j = i; j < ntray - 1; j++) tray[j] = tray[j + 1];
            ntray--;
            if (tmopen) draw_tm();
            dirty = 1;
            return;
        }
    }
}

static void open_tm(void) {
    if (tmopen) return;
    if (lopen) close_launcher();
    tmopen = 1;
    tm_click_row = -1;
    tm_sample();
    XMapRaised(dpy, tmwin);
    raise_panel(tmclient, tmwin);
    if (tmclient) { tmclient->mapped = 1; free_pict(tmclient); }
    draw_tm();
}
static void close_tm(void) {
    if (!tmopen) return;
    tmopen = 0;
    XUnmapWindow(dpy, tmwin);
    if (tmclient) { tmclient->mapped = 0; free_pict(tmclient); }
    dirty = 1;
}

/* ---- key/button grabs ------------------------------------------------ */
static void grab_key(KeySym ks, unsigned mods) {
    KeyCode kc = XKeysymToKeycode(dpy, ks);
    unsigned extra[] = { 0, LockMask, Mod2Mask, LockMask | Mod2Mask };
    int i;
    if (!kc) return;
    for (i = 0; i < 4; i++)
        XGrabKey(dpy, kc, mods | extra[i], root, True,
                 GrabModeAsync, GrabModeAsync);
}
static void grab_button(unsigned btn, unsigned mods) {
    unsigned extra[] = { 0, LockMask, Mod2Mask, LockMask | Mod2Mask };
    int i;
    for (i = 0; i < 4; i++)
        XGrabButton(dpy, btn, mods | extra[i], root, False,
                    ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                    GrabModeAsync, GrabModeAsync, None, None);
}
static void setup_grabs(void) {
    KeySym keys[] = { XK_Left, XK_Right, XK_Up, XK_Down, XK_equal, XK_plus,
                      XK_minus, XK_0, XK_r, XK_q, XK_m, XK_l, XK_f, XK_Return,
                      XK_Tab, XK_e };
    unsigned i;
    for (i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        grab_key(keys[i], MOD);
        grab_key(keys[i], MOD | ShiftMask);
    }
    grab_button(Button1, MOD);
    grab_button(Button3, MOD);
    grab_button(Button4, MOD);
    grab_button(Button5, MOD);
}

/* ---- input handling -------------------------------------------------- */
static void key_normal(KeySym ks, unsigned state) {
    int shift = (state & ShiftMask) != 0;
    double step = 0.35 * SW;
    switch (ks) {
    case XK_Left:  shift ? snap_client(focused, SNAP_L) : pan(-step, 0); break;
    case XK_Right: shift ? snap_client(focused, SNAP_R) : pan(step, 0);  break;
    case XK_Up:    shift ? snap_client(focused, SNAP_FULL) : pan(0, -0.35 * SH); break;
    case XK_Down:  shift ? snap_client(focused, SNAP_NONE) : pan(0, 0.35 * SH);  break;
    case XK_equal: case XK_plus:
        set_zoom_at(tzoom * 1.3, SW / 2.0, SH / 2.0); break;
    case XK_minus:
        set_zoom_at(tzoom / 1.3, SW / 2.0, SH / 2.0); break;
    case XK_0:
        set_zoom_at(1.0, SW / 2.0, SH / 2.0); break;
    case XK_r:
        open_launcher(); break;
    case XK_m:
        if (tmopen) close_tm(); else open_tm();
        break;
    case XK_l:
        if (shift) running = 0;        /* log out (ends the X session)   */
        else spawn(locker_cmd);        /* lock screen (i3lock etc.)      */
        break;
    case XK_f:
        if (focused) set_fullscreen(focused, !focused->fullscreen);
        break;
    case XK_q:
        close_client(focused); break;
    case XK_Return:
        spawn("x-terminal-emulator || xterm"); break;
    case XK_Tab: {
        Client *c = next_client(focused);
        if (c) fly_to(c);
        break; }
    case XK_e:
        if (shift) running = 0;
        break;
    default: break;
    }
}

static void start_drag(Client *c, XButtonEvent *ev, int mode, int edges) {
    drag_c = c;
    drag_mode = mode;
    drag_edges = edges;
    drag_sx = ev->x_root; drag_sy = ev->y_root;
    if (c) {
        drag_wx = c->x; drag_wy = c->y; drag_ww = c->w; drag_wh = c->h;
    } else {
        drag_vx = tvx; drag_vy = tvy;
    }
}

static void unsnap_under_cursor(Client *c, XButtonEvent *ev) {
    if (!c->snapped) return;
    /* dragging a snapped window releases it at the cursor */
    double fx = (ev->x_root - w2sx(c->x)) / (c->w * zoom);
    c->w = c->sw; c->h = c->sh;
    c->x = s2wx(ev->x_root) - fx * c->w;
    c->snapped = SNAP_NONE;
    drag_wx = c->x; drag_wy = c->y; drag_ww = c->w; drag_wh = c->h;
    apply_geometry(c);
}

static void button_press(XButtonEvent *ev) {
    Client *c;
    if (lopen) close_launcher();   /* clicking away dismisses the launcher */
    if (ev->window == tmwin) {
        /* task manager: double-click a row to jump to that window */
        int row = (ev->y - TM_ROWTOP) / TM_ROWH;
        if (ev->button == Button1 && ev->y >= TM_ROWTOP &&
            row >= 0 && row < tm_nrows) {
            if (row == tm_click_row &&
                ev->time - tm_click_time < 450) {
                Client *t = tm_rows[row], *v;
                for (v = clients; v; v = v->next) if (v == t) break;
                if (v && tm_is_listed(v)) {
                    close_tm();
                    fly_to(v);
                    return;
                }
            }
            tm_click_row = row;
            tm_click_time = ev->time;
        }
        return;
    }
    if (tmopen) close_tm();        /* clicking away dismisses it */
    if (ev->state & MOD) {
        if (ev->button == Button4) { set_zoom_at(tzoom * 1.15, ev->x_root, ev->y_root); return; }
        if (ev->button == Button5) { set_zoom_at(tzoom / 1.15, ev->x_root, ev->y_root); return; }
        c = client_at(ev->x_root, ev->y_root);
        if (!c) return;
        focus_client(c);
        raise_client(c);
        start_drag(c, ev, ev->button == Button3 ? 2 : 1,
                   ev->button == Button3 ? (EDGE_E | EDGE_S) : 0);
        if (drag_mode == 1) unsnap_under_cursor(c, ev);
        return;
    }
    c = find_by_frame(ev->window);
    if (c) {
        /* click on decorations (title bar / resize bands) */
        Hit zone; int edges;
        if (ev->button != Button1) return;
        zone = decor_zone(c, ev->x_root, ev->y_root, &edges);
        focus_client(c);
        raise_client(c);
        switch (zone) {
        case HIT_CLOSE: close_client(c); return;
        case HIT_MAX:   snap_client(c, SNAP_FULL); return;
        case HIT_TITLE:
            start_drag(c, ev, 1, 0);
            unsnap_under_cursor(c, ev);
            XGrabPointer(dpy, root, False,
                         ButtonReleaseMask | PointerMotionMask,
                         GrabModeAsync, GrabModeAsync, None,
                         cur_move, ev->time);
            ptr_grabbed = 1;
            return;
        case HIT_RESIZE:
            start_drag(c, ev, 2, edges);
            c->snapped = SNAP_NONE;
            XGrabPointer(dpy, root, False,
                         ButtonReleaseMask | PointerMotionMask,
                         GrabModeAsync, GrabModeAsync, None,
                         edge_cursor(edges), ev->time);
            ptr_grabbed = 1;
            return;
        default: return;
        }
    }
    if (ev->window == root && ev->subwindow == None) {
        /* empty canvas: drag to pan */
        if (ev->button != Button1) return;
        start_drag(NULL, ev, 3, 0);
        XGrabPointer(dpy, root, False, ButtonReleaseMask | PointerMotionMask,
                     GrabModeAsync, GrabModeAsync, None, cur_move, ev->time);
        ptr_grabbed = 1;
        return;
    }
    /* sync click-to-focus grab on a client window */
    c = find_client(ev->window);
    if (c) { focus_client(c); raise_client(c); }
    XAllowEvents(dpy, ReplayPointer, ev->time);
}

static void set_cursor(Cursor cur) {
    if (cur == cur_current) return;
    cur_current = cur;
    XDefineCursor(dpy, root, cur);
}

static void motion(XMotionEvent *ev) {
    double dx = ev->x_root - drag_sx, dy = ev->y_root - drag_sy;
    if (drag_mode == 1 && drag_c) {
        drag_c->x = drag_wx + dx / zoom;
        drag_c->y = drag_wy + dy / zoom;
        apply_geometry(drag_c);
        dirty = 1;
    } else if (drag_mode == 2 && drag_c) {
        double nw = drag_ww, nh = drag_wh;
        if (drag_edges & EDGE_E) nw = fmax(60, drag_ww + dx / zoom);
        if (drag_edges & EDGE_S) nh = fmax(40, drag_wh + dy / zoom);
        if (drag_edges & EDGE_W) {
            nw = fmax(60, drag_ww - dx / zoom);
            drag_c->x = drag_wx + (drag_ww - nw);
        }
        if (drag_edges & EDGE_N) {
            nh = fmax(40, drag_wh - dy / zoom);
            drag_c->y = drag_wy + (drag_wh - nh);
        }
        drag_c->w = nw; drag_c->h = nh;
        drag_c->snapped = SNAP_NONE;
        apply_geometry(drag_c);
        dirty = 1;
    } else if (drag_mode == 3) {
        tvx = drag_vx - dx / zoom;
        tvy = drag_vy - dy / zoom;
        dirty = 1;
    } else {
        /* idle motion: cursor feedback + button hover state */
        Client *c = find_by_frame(ev->window);
        Client *nh = NULL;
        Hit nhit = HIT_NONE;
        if (c) {
            int edges;
            Hit zone = decor_zone(c, ev->x_root, ev->y_root, &edges);
            if (zone == HIT_RESIZE) set_cursor(edge_cursor(edges));
            else set_cursor(cur_norm);
            if (zone == HIT_CLOSE || zone == HIT_MAX) { nh = c; nhit = zone; }
        } else if (ev->window == root) {
            set_cursor(cur_norm);
        }
        if (nh != hover_c || nhit != hover_hit) {
            hover_c = nh; hover_hit = nhit;
            dirty = 1;
        }
    }
}

static void button_release(XButtonEvent *ev) {
    if (ptr_grabbed) {
        XUngrabPointer(dpy, ev->time);
        ptr_grabbed = 0;
    }
    if (drag_mode == 1 && drag_c) {
        /* corners snap to quarters (generous zones), edges to halves/full */
        int cz = 48;
        int L = ev->x_root <= cz,      R = ev->x_root >= SW - cz - 1;
        int T = ev->y_root <= cz,      B = ev->y_root >= SH - cz - 1;
        if (L && T)                     snap_client(drag_c, SNAP_TL);
        else if (R && T)                snap_client(drag_c, SNAP_TR);
        else if (L && B)                snap_client(drag_c, SNAP_BL);
        else if (R && B)                snap_client(drag_c, SNAP_BR);
        else if (ev->x_root <= 4)       snap_client(drag_c, SNAP_L);
        else if (ev->x_root >= SW - 5)  snap_client(drag_c, SNAP_R);
        else if (ev->y_root <= 4)       snap_client(drag_c, SNAP_FULL);
    }
    drag_mode = 0;
    drag_c = NULL;
}

/* ---- event dispatch -------------------------------------------------- */
static void handle_event(XEvent *ev) {
    switch (ev->type) {
    case MapRequest: {
        XWindowAttributes wa;
        if (!XGetWindowAttributes(dpy, ev->xmaprequest.window, &wa)) break;
        if (wa.override_redirect) break;
        manage(ev->xmaprequest.window, &wa);
        XMapWindow(dpy, ev->xmaprequest.window);
        break; }
    case MapNotify: {
        Client *c = find_client(ev->xmap.window);
        if (!c && ev->xmap.window != backpm) {
            XWindowAttributes wa;
            if (XGetWindowAttributes(dpy, ev->xmap.window, &wa) &&
                wa.override_redirect && wa.class != InputOnly)
                manage(ev->xmap.window, &wa);
            c = find_client(ev->xmap.window);
        }
        if (c) {
            c->mapped = 1;
            free_pict(c);
            if (c->is_or) {
                /* apps reuse menu/tooltip windows: on re-map, put the
                 * client back on top of the PAINT order too, or the menu
                 * renders behind windows raised since its first map */
                detach(c);
                attach_top(c);
            }
            if (c->frame && !c->fullscreen) {
                XMapWindow(dpy, c->frame);
                restack_frame(c);
            }
            if (!c->is_or && !c->screenspace) { focus_client(c); raise_client(c); }
            if (tmopen && c != tmclient) draw_tm();
            dirty = 1;
        }
        break; }
    case UnmapNotify: {
        Client *c = find_client(ev->xunmap.window);
        if (c) {
            c->mapped = 0;
            free_pict(c);
            if (c->frame) XUnmapWindow(dpy, c->frame);
            if (focused == c) {
                Client *i, *t = NULL;
                focused = NULL;
                for (i = clients; i; i = i->next) if (eligible(i)) t = i;
                if (t) focus_client(t);
            }
            if (tmopen && c != tmclient) draw_tm();
            dirty = 1;
        }
        break; }
    case DestroyNotify: {
        Client *c = find_client(ev->xdestroywindow.window);
        if (c) unmanage(c);
        else tray_remove(ev->xdestroywindow.window);
        break; }
    case ConfigureRequest: {
        XConfigureRequestEvent *cr = &ev->xconfigurerequest;
        Client *c = find_client(cr->window);
        if (c && !c->is_or) {
            /* app pixels == world units; positions honored once mapped
             * (apps placing dialogs), initial placement stays ours */
            if (c->mapped && (cr->value_mask & CWX))
                c->x = cr->x - anchor_x() + tvx;
            if (c->mapped && (cr->value_mask & CWY))
                c->y = cr->y - anchor_y() + tvy;
            if (cr->value_mask & CWWidth)  c->w = cr->width;
            if (cr->value_mask & CWHeight) c->h = cr->height;
            if (c->snapped) c->snapped = SNAP_NONE;
            c->lx = -99999; /* force reapply */
            apply_geometry(c);
            send_configure(c);
            dirty = 1;
        } else {
            XWindowChanges wc;
            wc.x = cr->x; wc.y = cr->y;
            wc.width = cr->width; wc.height = cr->height;
            wc.border_width = cr->border_width;
            wc.sibling = cr->above; wc.stack_mode = cr->detail;
            XConfigureWindow(dpy, cr->window, (unsigned)cr->value_mask, &wc);
        }
        break; }
    case ConfigureNotify: {
        XConfigureEvent *ce = &ev->xconfigure;
        if (ce->window == root) {
            SW = ce->width; SH = ce->height;
            make_backbuffer();
            load_wallpaper();   /* re-scale to the new resolution */
            dirty = 1;
            break;
        }
        Client *c = find_client(ce->window);
        if (c && c->is_or) {
            if (c->pending_cfg > 0) {
                c->pending_cfg--;        /* our own move: already tracked */
                break;
            }
            /* menus reposition themselves (flip to fit the screen,
             * submenus); track their real moves into world coords */
            c->x = ce->x - anchor_x() + tvx;
            c->y = ce->y - anchor_y() + tvy;
            c->lx = ce->x; c->ly = ce->y;
            if (ce->width != c->lw || ce->height != c->lh) {
                c->w = ce->width; c->h = ce->height;
                c->lw = ce->width; c->lh = ce->height;
                free_pict(c);
            }
            dirty = 1;
        } else if (c && (ce->width != c->lw || ce->height != c->lh)) {
            c->lw = ce->width; c->lh = ce->height;
            free_pict(c);
            dirty = 1;
        }
        break; }
    case KeyPress: {
        KeySym ks = XLookupKeysym(&ev->xkey, 0);
        if (lopen) launcher_key(&ev->xkey);
        else key_normal(ks, ev->xkey.state);
        break; }
    case PropertyNotify: {
        Client *c = find_client(ev->xproperty.window);
        if (c && (ev->xproperty.atom == XA_WM_NAME ||
                  ev->xproperty.atom == A_NET_WM_NAME))
            update_title(c);
        if (c && ev->xproperty.atom == A_NET_WM_ICON) {
            free_icon(c);
            if (tmopen) draw_tm();
        }
        break; }
    case ButtonPress:   button_press(&ev->xbutton); break;
    case ButtonRelease: button_release(&ev->xbutton); break;
    case MotionNotify:
        while (XCheckTypedEvent(dpy, MotionNotify, ev)) ;
        motion(&ev->xmotion);
        break;
    case EnterNotify: {
        Client *c;
        static int lex = -1, ley = -1;
        if (lopen || drag_mode) break;
        if (ev->xcrossing.mode != NotifyNormal) break;
        /* only focus-follow when the pointer actually moved (ignore
         * Enter events caused by windows moving under the pointer) */
        if (ev->xcrossing.x_root == lex && ev->xcrossing.y_root == ley) break;
        lex = ev->xcrossing.x_root; ley = ev->xcrossing.y_root;
        c = find_client(ev->xcrossing.window);
        if (!c) c = find_by_frame(ev->xcrossing.window);
        if (c && !c->is_or && !c->screenspace && c->mapped) focus_client(c);
        break; }
    case LeaveNotify: {
        Client *c = find_by_frame(ev->xcrossing.window);
        if (c && hover_c == c) {
            hover_c = NULL; hover_hit = HIT_NONE;
            dirty = 1;
        }
        break; }
    case Expose:
        if (ev->xexpose.window == lwin && lopen && ev->xexpose.count == 0)
            draw_launcher();
        if (ev->xexpose.window == tmwin && tmopen && ev->xexpose.count == 0)
            draw_tm();
        break;
    case ClientMessage: {
        XClientMessageEvent *cm = &ev->xclient;
        Client *c;
        if (cm->message_type == A_TRAY_OPCODE && cm->window == tmwin) {
            if (cm->data.l[1] == 0)   /* SYSTEM_TRAY_REQUEST_DOCK */
                tray_dock((Window)cm->data.l[2]);
            break;
        }
        c = find_client(cm->window);
        if (!c) break;
        if (cm->message_type == A_NET_WM_STATE) {
            /* 0 = remove, 1 = add, 2 = toggle */
            if ((Atom)cm->data.l[1] == A_NET_WM_STATE_FS ||
                (Atom)cm->data.l[2] == A_NET_WM_STATE_FS) {
                int on = cm->data.l[0] == 1 ||
                         (cm->data.l[0] == 2 && !c->fullscreen);
                set_fullscreen(c, on);
            }
        } else if (cm->message_type == A_NET_ACTIVE) {
            if (eligible(c)) {
                double rx = w2sx(c->x), ry = w2sy(c->y);
                double rw = c->w * zoom, rh = c->h * zoom;
                focus_client(c);
                raise_client(c);
                /* only travel the canvas when it's actually offscreen */
                if (rx + rw < 0 || ry + rh < 0 || rx > SW || ry > SH)
                    fly_to(c);
            }
        }
        break; }
    case MappingNotify: {
        XMappingEvent *me = &ev->xmapping;
        XRefreshKeyboardMapping(me);
        if (me->request == MappingKeyboard) {
            XUngrabKey(dpy, AnyKey, AnyModifier, root);
            setup_grabs();
        }
        break; }
    default:
#if HAVE_DAMAGE
        if (ev->type == damage_ev + XDamageNotify) {
            XDamageNotifyEvent *de = (XDamageNotifyEvent *)ev;
            Client *dc = find_client(de->drawable);
            XDamageSubtract(dpy, de->damage, None, None);
            if (dc && dc->mapped) {
                /* damaged box in screen px (window px == world units) */
                int x1, y1, x2, y2;
                if (dc->screenspace) {
                    x1 = dc->lx + de->area.x;
                    y1 = dc->ly + de->area.y;
                    x2 = x1 + de->area.width;
                    y2 = y1 + de->area.height;
                } else {
                    x1 = (int)floor(w2sx(dc->x + de->area.x));
                    y1 = (int)floor(w2sy(dc->y + de->area.y));
                    x2 = x1 + (int)ceil(de->area.width * zoom) + 1;
                    y2 = y1 + (int)ceil(de->area.height * zoom) + 1;
                }
                x1 -= 2; y1 -= 2; x2 += 2; y2 += 2;
                if (!pdirty) {
                    dmgx1 = x1; dmgy1 = y1; dmgx2 = x2; dmgy2 = y2;
                    pdirty = 1;
                } else {
                    if (x1 < dmgx1) dmgx1 = x1;
                    if (y1 < dmgy1) dmgy1 = y1;
                    if (x2 > dmgx2) dmgx2 = x2;
                    if (y2 > dmgy2) dmgy2 = y2;
                }
            } else
                dirty = 1;
        }
#endif
        break;
    }
}

/* ---- setup ----------------------------------------------------------- */
int main(void) {
    XSetWindowAttributes swa;
    XRenderPictureAttributes pa;
    XWindowAttributes rwa;
    int cev, cerr, maj, min;
    int xfd;

    load_config();

    dpy = XOpenDisplay(NULL);
    if (!dpy) { fprintf(stderr, "infinawm: cannot open display\n"); return 1; }
    scr = DefaultScreen(dpy);
    root = RootWindow(dpy, scr);
    SW = DisplayWidth(dpy, scr);
    SH = DisplayHeight(dpy, scr);
    visual = DefaultVisual(dpy, scr);
    depth = DefaultDepth(dpy, scr);

    signal(SIGCHLD, SIG_IGN);

    /* become the WM */
    wm_running_err = 0;
    XSetErrorHandler(xerror_start);
    XSelectInput(dpy, root, SubstructureRedirectMask | SubstructureNotifyMask |
                            StructureNotifyMask | ButtonPressMask |
                            ButtonReleaseMask | PointerMotionMask);
    XSync(dpy, False);
    if (wm_running_err) {
        fprintf(stderr, "infinawm: another window manager is running\n");
        return 1;
    }
    XSetErrorHandler(xerror);

    if (!XCompositeQueryExtension(dpy, &cev, &cerr) ||
        !XRenderQueryExtension(dpy, &cev, &cerr)) {
        fprintf(stderr, "infinawm: need Composite and Render extensions\n");
        return 1;
    }
    XCompositeQueryVersion(dpy, &maj, &min);
#if HAVE_DAMAGE
    {
        int derr;
        if (!XDamageQueryExtension(dpy, &damage_ev, &derr)) {
            fprintf(stderr, "infinawm: Damage extension missing\n");
            return 1;
        }
    }
#endif

    XCompositeRedirectSubwindows(dpy, root, CompositeRedirectManual);

    fmt_rgb  = XRenderFindVisualFormat(dpy, visual);
    fmt_argb = XRenderFindStandardFormat(dpy, PictStandardARGB32);
    XGetWindowAttributes(dpy, root, &rwa);
    pa.subwindow_mode = IncludeInferiors;
    rootpict = XRenderCreatePicture(dpy, root, fmt_rgb, CPSubwindowMode, &pa);
    make_backbuffer();
    load_wallpaper();

    A_WM_PROTOCOLS  = XInternAtom(dpy, "WM_PROTOCOLS", False);
    A_WM_DELETE     = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    A_WM_STATE      = XInternAtom(dpy, "WM_STATE", False);
    A_NET_SUPPORTING= XInternAtom(dpy, "_NET_SUPPORTING_WM_CHECK", False);
    A_NET_WM_NAME   = XInternAtom(dpy, "_NET_WM_NAME", False);
    A_NET_ACTIVE    = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
    A_UTF8          = XInternAtom(dpy, "UTF8_STRING", False);
    A_NET_WM_PID    = XInternAtom(dpy, "_NET_WM_PID", False);
    A_NET_WM_ICON   = XInternAtom(dpy, "_NET_WM_ICON", False);
    A_WM_TAKE_FOCUS = XInternAtom(dpy, "WM_TAKE_FOCUS", False);
    A_NET_SUPPORTED = XInternAtom(dpy, "_NET_SUPPORTED", False);
    A_NET_CLIENT_LIST = XInternAtom(dpy, "_NET_CLIENT_LIST", False);
    A_NET_WM_STATE  = XInternAtom(dpy, "_NET_WM_STATE", False);
    A_NET_WM_STATE_FS = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);

    {
        Atom sup[] = { A_NET_SUPPORTED, A_NET_SUPPORTING, A_NET_WM_NAME,
                       A_NET_ACTIVE, A_NET_CLIENT_LIST, A_NET_WM_STATE,
                       A_NET_WM_STATE_FS, A_NET_WM_PID, A_NET_WM_ICON };
        XChangeProperty(dpy, root, A_NET_SUPPORTED, XA_ATOM, 32,
                        PropModeReplace, (unsigned char *)sup,
                        (int)(sizeof sup / sizeof sup[0]));
    }

    /* EWMH check window */
    {
        Window chk = XCreateSimpleWindow(dpy, root, -100, -100, 1, 1, 0, 0, 0);
        XChangeProperty(dpy, chk, A_NET_SUPPORTING, XA_WINDOW, 32,
                        PropModeReplace, (unsigned char *)&chk, 1);
        XChangeProperty(dpy, chk, A_NET_WM_NAME, A_UTF8, 8, PropModeReplace,
                        (unsigned char *)"infinawm", 8);
        XChangeProperty(dpy, root, A_NET_SUPPORTING, XA_WINDOW, 32,
                        PropModeReplace, (unsigned char *)&chk, 1);
        XChangeProperty(dpy, root, A_NET_WM_NAME, A_UTF8, 8, PropModeReplace,
                        (unsigned char *)"infinawm", 8);
    }

    cur_norm = XCreateFontCursor(dpy, XC_left_ptr);
    cur_move = XCreateFontCursor(dpy, XC_fleur);
    cur_h    = XCreateFontCursor(dpy, XC_sb_h_double_arrow);
    cur_v    = XCreateFontCursor(dpy, XC_sb_v_double_arrow);
    cur_nw   = XCreateFontCursor(dpy, XC_top_left_corner);
    cur_ne   = XCreateFontCursor(dpy, XC_top_right_corner);
    cur_sw   = XCreateFontCursor(dpy, XC_bottom_left_corner);
    cur_se   = XCreateFontCursor(dpy, XC_bottom_right_corner);
    cur_current = cur_norm;
    XDefineCursor(dpy, root, cur_norm);

    /* launcher window */
    swa.override_redirect = True;
    swa.background_pixel = 0;
    swa.event_mask = ExposureMask;
    lwin = XCreateWindow(dpy, root, (SW - LAUNCHER_W) / 2, 10,
                         LAUNCHER_W, LAUNCHER_H, 0, (int)depth, InputOutput,
                         visual, CWOverrideRedirect | CWBackPixel | CWEventMask,
                         &swa);
    lgc = XCreateGC(dpy, lwin, 0, NULL);
    lfont = XLoadQueryFont(dpy, "9x15");
    if (!lfont) lfont = XLoadQueryFont(dpy, "fixed");
    if (lfont) XSetFont(dpy, lgc, lfont->fid);
    dgc = XCreateGC(dpy, backpm, 0, NULL);
    if (lfont) XSetFont(dpy, dgc, lfont->fid);
    lclient = calloc(1, sizeof(Client));
    lclient->win = lwin;
    lclient->screenspace = 1;
    lclient->lx = (SW - LAUNCHER_W) / 2; lclient->ly = 10;
    lclient->lw = LAUNCHER_W; lclient->lh = LAUNCHER_H;
    attach_top(lclient);

    /* task manager window (Super+M) */
    swa.event_mask = ExposureMask | ButtonPressMask | SubstructureNotifyMask;
    tmwin = XCreateWindow(dpy, root, (SW - TM_W) / 2, SH / 3, TM_W, 200, 0,
                          (int)depth, InputOutput, visual,
                          CWOverrideRedirect | CWBackPixel | CWEventMask,
                          &swa);
    /* become the XEmbed system tray (nm-applet, volume icons, ...) */
    {
        char selname[32];
        snprintf(selname, sizeof selname, "_NET_SYSTEM_TRAY_S%d", scr);
        A_TRAY_SEL    = XInternAtom(dpy, selname, False);
        A_TRAY_OPCODE = XInternAtom(dpy, "_NET_SYSTEM_TRAY_OPCODE", False);
        A_TRAY_ORIENT = XInternAtom(dpy, "_NET_SYSTEM_TRAY_ORIENTATION", False);
        A_MANAGER     = XInternAtom(dpy, "MANAGER", False);
        A_XEMBED      = XInternAtom(dpy, "_XEMBED", False);
        XSetSelectionOwner(dpy, A_TRAY_SEL, tmwin, CurrentTime);
        if (XGetSelectionOwner(dpy, A_TRAY_SEL) == tmwin) {
            long orient = 0;   /* horizontal */
            XEvent me;
            XChangeProperty(dpy, tmwin, A_TRAY_ORIENT, XA_CARDINAL, 32,
                            PropModeReplace, (unsigned char *)&orient, 1);
            memset(&me, 0, sizeof me);
            me.xclient.type = ClientMessage;
            me.xclient.window = root;
            me.xclient.message_type = A_MANAGER;
            me.xclient.format = 32;
            me.xclient.data.l[0] = CurrentTime;
            me.xclient.data.l[1] = (long)A_TRAY_SEL;
            me.xclient.data.l[2] = (long)tmwin;
            XSendEvent(dpy, root, False, StructureNotifyMask, &me);
        }
    }
    tmgc = XCreateGC(dpy, tmwin, 0, NULL);
    if (lfont) XSetFont(dpy, tmgc, lfont->fid);
    tmpict = XRenderCreatePicture(dpy, tmwin, fmt_rgb, 0, NULL);
    tmclient = calloc(1, sizeof(Client));
    tmclient->win = tmwin;
    tmclient->screenspace = 1;
    tmclient->lx = (SW - TM_W) / 2; tmclient->ly = SH / 3;
    tmclient->lw = TM_W; tmclient->lh = 200;
    attach_top(tmclient);

    setup_grabs();

    /* adopt pre-existing windows */
    {
        Window d1, d2, *wins = NULL; unsigned n, i;
        if (XQueryTree(dpy, root, &d1, &d2, &wins, &n)) {
            for (i = 0; i < n; i++) {
                XWindowAttributes wa;
                if (wins[i] == lwin) continue;
                if (!XGetWindowAttributes(dpy, wins[i], &wa)) continue;
                if (wa.class == InputOnly) continue;
                if (wa.map_state == IsViewable) {
                    manage(wins[i], &wa);
                    Client *c = find_client(wins[i]);
                    if (c) {
                        c->mapped = 1;
                        if (c->frame) {
                            XMapWindow(dpy, c->frame);
                            restack_frame(c);
                        }
                    }
                }
            }
            if (wins) XFree(wins);
        }
    }

    XSync(dpy, False);
    xfd = ConnectionNumber(dpy);

    /* autostart programs from the config (space separated) */
    if (autostart_cmds[0]) {
        char *copy = strdup(autostart_cmds), *tok;
        for (tok = strtok(copy, " \t"); tok; tok = strtok(NULL, " \t"))
            spawn(tok);
        free(copy);
    }

    /* initial pointer position for the anchored layout */
    {
        Window r_, w_; int rx_, ry_, wx_, wy_; unsigned m_;
        if (XQueryPointer(dpy, root, &r_, &w_, &rx_, &ry_, &wx_, &wy_, &m_)) {
            pcx = rx_; pcy = ry_;
        } else { pcx = SW / 2.0; pcy = SH / 2.0; }
    }

    while (running) {
        XEvent ev;
        int animating;
        fd_set fds;
        struct timeval tv;

        while (XPending(dpy)) {
            XNextEvent(dpy, &ev);
            handle_event(&ev);
        }
        /* keep the real layout anchored at the pointer while zoomed.
         * At 100% the anchor is identity — skip the synchronous
         * round-trip entirely (it costs latency on every wakeup). */
        if (tzoom != 1.0 || zoom != 1.0) {
            Window r_, w_; int rx_, ry_, wx_, wy_; unsigned m_;
            if (XQueryPointer(dpy, root, &r_, &w_, &rx_, &ry_, &wx_, &wy_, &m_)
                && ((int)pcx != rx_ || (int)pcy != ry_)) {
                pcx = rx_; pcy = ry_;
                if (tzoom != 1.0) apply_all_geometry();
            }
        }
        if (now_s() - sys_sampled > 1.0) {
            sample_system();   /* keep graph history even while closed */
            if (tmopen) {
                tm_sample();
                draw_tm();
            }
        }
        animating = step_animation();
#if HAVE_DAMAGE
        if (dirty || pdirty || animating) paint();
        tv.tv_sec = 0;
        tv.tv_usec = animating ? 16000
                     : ((tzoom != 1.0 || tmopen) ? 16000 : 250000);
#else
        paint();                       /* poll mode: repaint every tick */
        tv.tv_sec = 0; tv.tv_usec = animating ? 16000 : 33000;
#endif
        XFlush(dpy);
        FD_ZERO(&fds);
        FD_SET(xfd, &fds);
        select(xfd + 1, &fds, NULL, NULL, &tv);
    }
    XCloseDisplay(dpy);
    return 0;
}
