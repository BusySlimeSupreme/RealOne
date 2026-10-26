/* Settings.RAK - Display, Battery, Performance, About, Wallpaper and (hidden) Developer tools.
 * The developer test list is read from assets/devtests.txt. Needs cairo for glyph placement, clips and gradients. */
#define _GNU_SOURCE
#include <cairo.h>
#include "rakapi.h"
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <dirent.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <unistd.h>

static const RakHost *H;
#define PI 3.14159265358979323846
#define SL_X 40.0
#define SL_W 320.0
#define DSL_Y 138.0

enum { P_ROOT, P_DISP, P_BAT, P_PERF, P_ABOUT, P_WALL, P_DEV, P_TEST };
enum { T_PANIC, T_CRASH, T_FREEZE, T_COLORS, T_GRAD, T_TOUCH, T_FPS, T_CPU, T_MEM, T_DISK, T_LED, T_BL, T_RF, T_SENS, T_DMESG, T_INFO, T_OFF, NT_KNOWN };
static const char *TID[NT_KNOWN] = {"panic", "crash", "freeze", "colors", "grad", "touch", "fps", "cpu", "mem", "disk", "led", "bl", "rf", "sens", "dmesg", "info", "off"};
#define MAXT 24
static struct { int kind, danger; char name[40], desc[72]; } TS[MAXT];
static int nts;

static int page = P_ROOT;
static const char *SRN[5] = {"Display", "Battery", "Performance", "About", "Developer mode"};
static const int SRG[5] = {2, 8, 9, 10, 12};
static const double SRC[5][3] = {{0.30, 0.62, 0.95}, {0.20, 0.78, 0.35}, {0.62, 0.40, 0.95}, {0.49, 0.51, 0.57}, {0.95, 0.35, 0.30}};
static const int SRP[5] = {P_DISP, P_BAT, P_PERF, P_ABOUT, P_DEV};

/* slider */
static int br_drag; static double br_val; static long br_last;
/* about */
static int dev_taps; static long dev_tap_t;
/* wallpaper */
static int wall_pg, th_pg = -1, ren_k = -1;
static cairo_surface_t *th[9];
static char ren_buf[40];
/* developer */
#define DEV_PP 6
static int dev_page, tst_id, tst_kind, dev_arm = -1; static long dev_at;
static volatile int tst_busy, tst_gen;
static int stage, anim_on;                       /* stage: 1 = message queued, 2 = do it on the next draw */
static char tl[10][64]; static int tl_n;
static __thread int t_gen;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static int tcol, tpn, tg_n; static double tp[24][2], tg_dx, tg_dy;

static long long now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000000LL + t.tv_nsec / 1000; }
static cairo_t *CR(void) { return H->cairo(); }
static void fmt_mb(char *o, int n, double mb) { if (mb >= 1024) snprintf(o, n, "%.1f GB", mb / 1024); else snprintf(o, n, "%.0f MB", mb); }
static void kv(double y, const char *k, const char *v) {
    H->color(1, 1, 1, 0.55); H->text(k, 32, y, 14, 0, RAK_LEFT);
    H->color(1, 1, 1, 1); H->text(v, 368, y, 14, 1, RAK_RIGHT);
}
static void chevron(double x, double y) {
    cairo_t *cr = CR(); H->color(1, 1, 1, 0.4); cairo_set_line_width(cr, 2.5);
    cairo_move_to(cr, x, y - 7); cairo_line_to(cr, x + 8, y); cairo_line_to(cr, x, y + 7); cairo_stroke(cr);
}
static int writef(const char *path, const char *v) {
    int fd = open(path, O_WRONLY); ssize_t n; if (fd < 0) return 0;
    n = write(fd, v, strlen(v)); close(fd); return n > 0;
}
static int readf(const char *path, char *buf, int n) {
    FILE *f = fopen(path, "r"); int k; if (!f) return 0;
    k = (int)fread(buf, 1, n - 1, f); fclose(f); buf[k > 0 ? k : 0] = 0; return k > 0;
}
static void chomp(char *s) { s[strcspn(s, "\r\n")] = 0; }

/* ---------- developer test list (assets/devtests.txt) ---------- */
static void load_tests(void) {
    uint32_t n; const unsigned char *a = H->asset("assets/devtests.txt", &n);
    char *t, *sv = NULL, *ln;
    nts = 0;
    if (!a) return;
    t = malloc(n + 1); if (!t) return;
    memcpy(t, a, n); t[n] = 0;
    for (ln = strtok_r(t, "\r\n", &sv); ln && nts < MAXT; ln = strtok_r(NULL, "\r\n", &sv)) {
        char *f[4]; int nf = 0, k;
        if (ln[0] == '#') continue;
        f[nf++] = ln;
        for (char *p = ln; *p && nf < 4; p++) if (*p == '|') { *p = 0; f[nf++] = p + 1; }
        if (nf < 4) continue;
        for (k = 0; k < NT_KNOWN; k++) if (!strcmp(f[0], TID[k])) break;
        if (k == NT_KNOWN) continue;
        TS[nts].kind = k; TS[nts].danger = atoi(f[1]);
        snprintf(TS[nts].name, sizeof TS[nts].name, "%.39s", f[2]); snprintf(TS[nts].desc, sizeof TS[nts].desc, "%.71s", f[3]);
        nts++;
    }
    free(t);
}

/* ---------- wallpaper thumbnails ---------- */
static void wall_free(void) { for (int i = 0; i < 9; i++) { if (th[i]) H->surface_free(th[i]); th[i] = NULL; } th_pg = -1; }
static int wall_tile(double x, double y) {
    for (int i = 0; i < 9; i++) {
        double bx = 16 + 123.0 * (i % 3), by = 68 + 96.0 * (i / 3); int k = wall_pg * 9 + i;
        if (k < H->wall_count() && x >= bx && x <= bx + 114 && y >= by && y <= by + 86) return k;
    }
    return -1;
}
static void goto_page(int p) { page = p; H->animate(0); anim_on = 0; }

/* ---------- pages ---------- */
static void draw_root(void) {
    char t[16], v[16]; int nrows = H->get(RAK_DEVMODE) != 0 ? 5 : 4;
    double p = nrows == 5 ? 60.0 : 70.0, rh = nrows == 5 ? 54.0 : 62.0;
    cairo_t *cr = CR();
    snprintf(t, sizeof t, "%02d:%02d", H->now->tm_hour, H->now->tm_min);
    H->panel();
    H->color(1, 1, 1, 1); H->text("Settings", 24, 46, 22, 1, RAK_LEFT);
    H->color(1, 1, 1, 0.6); H->text(t, 376, 44, 16, 0, RAK_RIGHT);
    for (int i = 0; i < nrows; i++) {
        double y = 74 + p * i, mid = y + rh / 2;
        H->color(1, 1, 1, 0.07); H->round_rect(16, y, 368, rh, 18); H->fill();
        H->color(SRC[i][0], SRC[i][1], SRC[i][2], 1); H->round_rect(28, mid - 17, 34, 34, 10); H->fill();
        cairo_save(cr); cairo_translate(cr, 45, mid); cairo_scale(cr, 0.5, 0.5); H->color(1, 1, 1, 1); H->glyph(SRG[i], 1); cairo_restore(cr);
        H->color(1, 1, 1, 1); H->text(SRN[i], 76, mid + 7, 18, 1, RAK_LEFT);
        v[0] = 0;
        if (i == 0) snprintf(v, sizeof v, "%d%%", (int)(H->get(RAK_BRIGHTNESS) * 100 + 0.5));
        else if (i == 1 && H->sys->bat >= 0) snprintf(v, sizeof v, "%d%%", H->sys->bat);
        if (v[0]) { H->color(1, 1, 1, 0.6); H->text(v, 332, mid + 6, 15, 0, RAK_RIGHT); }
        chevron(358, mid);
    }
}
static void draw_disp(void) {
    char s[48]; cairo_t *cr = CR(); double f = br_drag ? br_val : H->get(RAK_BRIGHTNESS); int cur = H->wall_current();
    H->header("Display");
    H->color(1, 1, 1, 0.07); H->round_rect(16, 74, 368, 108, 18); H->fill();
    H->accent(1); H->text("BRIGHTNESS", 32, 104, 12, 1, RAK_LEFT);
    snprintf(s, sizeof s, "%d%%", (int)(f * 100 + 0.5)); H->color(1, 1, 1, 1); H->text(s, 368, 108, 24, 1, RAK_RIGHT);
    H->color(1, 1, 1, 0.12); H->round_rect(SL_X, DSL_Y - 20, SL_W, 40, 20); H->fill();
    cairo_save(cr); H->round_rect(SL_X, DSL_Y - 20, SL_W, 40, 20); cairo_clip(cr);
    H->accent(1); cairo_rectangle(cr, SL_X, DSL_Y - 20, SL_W * f, 40); cairo_fill(cr); cairo_restore(cr);
    cairo_save(cr); cairo_translate(cr, SL_X + 24, DSL_Y); cairo_scale(cr, 0.45, 0.45); H->color(1, 1, 1, 1); H->glyph(2, 1); cairo_restore(cr);
    H->color(1, 1, 1, 0.07); H->round_rect(16, 196, 368, 76, 18); H->fill();
    H->accent(1); H->text("WALLPAPER", 32, 226, 12, 1, RAK_LEFT);
    snprintf(s, sizeof s, "%.16s", cur >= 0 ? H->wall_name(cur) : "None");
    H->color(1, 1, 1, 1); H->text(s, 32, 256, 20, 1, RAK_LEFT);
    chevron(358, 234);
    H->color(1, 1, 1, 0.5); H->text("Change", 368, 256, 13, 0, RAK_RIGHT);
}
static void draw_bat(void) {
    char s[48]; const RakSys *S = H->sys;
    H->header("Battery");
    H->color(1, 1, 1, 0.07); H->round_rect(16, 74, 368, 140, 18); H->fill();
    H->accent(1); H->text("CHARGE", 32, 104, 12, 1, RAK_LEFT);
    if (S->bat >= 0) snprintf(s, sizeof s, "%d%%", S->bat); else snprintf(s, sizeof s, "--");
    H->color(1, 1, 1, 1); H->text(s, 32, 170, 64, 1, RAK_LEFT);
    H->color(1, 1, 1, 0.7); H->text(S->bat < 0 ? "No battery" : S->charging ? "Charging" : "On battery", 368, 104, 14, 0, RAK_RIGHT);
    H->color(1, 1, 1, 0.12); H->round_rect(32, 188, 336, 10, 5); H->fill();
    if (S->bat > 0) { H->accent(1); H->round_rect(32, 188, fmax(10, 336 * S->bat / 100.0), 10, 5); H->fill(); }
    H->color(1, 1, 1, 0.07); H->round_rect(16, 228, 368, 90, 18); H->fill();
    if (S->bat < 0) { H->color(1, 1, 1, 0.8); H->text("No battery detected.", 200, 270, 14, 1, RAK_CENTER); return; }
    if (S->bat_v > 0) snprintf(s, sizeof s, "%.2f V", S->bat_v); else snprintf(s, sizeof s, "n/a");
    kv(264, "Voltage", s);
    if (S->bat_temp > -900) snprintf(s, sizeof s, "%.1f C", S->bat_temp); else snprintf(s, sizeof s, "n/a");
    kv(298, "Temperature", s);
}
static void card3(double y, const char *title, const char *pct, double f, const char *detail) {
    H->color(1, 1, 1, 0.07); H->round_rect(16, y, 368, 94, 18); H->fill();
    H->accent(1); H->text(title, 32, y + 26, 12, 1, RAK_LEFT);
    H->color(1, 1, 1, 1); H->text(pct, 368, y + 32, 26, 1, RAK_RIGHT);
    if (f < 0) f = 0;
    if (f > 1) f = 1;
    H->color(1, 1, 1, 0.12); H->round_rect(32, y + 44, 336, 8, 4); H->fill();
    if (f > 0) { H->accent(1); H->round_rect(32, y + 44, fmax(8, 336 * f), 8, 4); H->fill(); }
    H->color(1, 1, 1, 0.65); H->text(detail, 32, y + 78, 13, 0, RAK_LEFT);
}
static void draw_perf(void) {
    char a[48], b[48], c[48], d[112]; const RakSys *S = H->sys;
    H->header("Performance");
    if (S->cpu_pct >= 0) snprintf(a, sizeof a, "%.0f%%", S->cpu_pct); else snprintf(a, sizeof a, "--");
    if (S->cpu_mhz > 0) snprintf(d, sizeof d, "%d cores, %.0f MHz", S->ncores, S->cpu_mhz); else snprintf(d, sizeof d, "%d cores", S->ncores);
    if (S->cpu_temp > 0) { size_t l = strlen(d); snprintf(d + l, sizeof d - l, ", %.0f C", S->cpu_temp); }
    card3(66, "PROCESSOR", a, S->cpu_pct >= 0 ? S->cpu_pct / 100 : 0, d);
    if (S->ram_tot > 0) {
        snprintf(a, sizeof a, "%.0f%%", 100.0 * S->ram_used / S->ram_tot); fmt_mb(b, sizeof b, S->ram_used); fmt_mb(c, sizeof c, S->ram_tot);
        snprintf(d, sizeof d, "%s of %s", b, c); card3(166, "MEMORY", a, S->ram_used / S->ram_tot, d);
    } else card3(166, "MEMORY", "--", 0, "n/a");
    if (S->st_tot > 0) {
        snprintf(a, sizeof a, "%.0f%%", 100.0 * S->st_used / S->st_tot); fmt_mb(b, sizeof b, S->st_used); fmt_mb(c, sizeof c, S->st_tot);
        snprintf(d, sizeof d, "%s of %s", b, c); card3(266, "STORAGE", a, S->st_used / S->st_tot, d);
    } else card3(266, "STORAGE", "--", 0, "n/a");
}
static void draw_about(void) {
    char s[64]; struct utsname un; struct sysinfo si;
    H->header("About");
    H->color(1, 1, 1, 0.07); H->round_rect(16, 70, 368, 304, 18); H->fill();
    kv(106, "Name", "RealOne OS");
    kv(144, "Version", "v33 Linux");
    if (!uname(&un)) { snprintf(s, sizeof s, "%.20s", un.release); kv(182, "Kernel", s); kv(220, "Arch", un.machine); }
    snprintf(s, sizeof s, "%d", H->sys->ncores); kv(258, "Cores", s);
    if (H->sys->ram_tot > 0) fmt_mb(s, sizeof s, H->sys->ram_tot); else snprintf(s, sizeof s, "n/a");
    kv(296, "Memory", s);
    if (!sysinfo(&si)) { long m = si.uptime / 60; snprintf(s, sizeof s, "%ldh %02ldm", m / 60, m % 60); } else snprintf(s, sizeof s, "n/a");
    kv(334, "Uptime", s);
}
static void draw_wall(void) {
    char s[48]; int cnt = H->wall_count(), pages = (cnt + 8) / 9, cur = H->wall_current();
    H->header("Wallpaper");
    if (pages < 1) pages = 1;
    if (wall_pg >= pages) wall_pg = pages - 1;
    if (!cnt) {
        H->color(1, 1, 1, 0.9); H->text("No wallpapers yet", 200, 190, 18, 1, RAK_CENTER);
        H->color(1, 1, 1, 0.55); H->text("Put .png / .jpg files in", 200, 224, 13, 0, RAK_CENTER); H->text("~/.realone/wallpapers", 200, 244, 13, 0, RAK_CENTER);
        return;
    }
    if (th_pg != wall_pg) {
        wall_free();
        for (int i = 0; i < 9; i++) { int k = wall_pg * 9 + i; if (k < cnt) th[i] = H->load_image(H->wall_path(k), 180); }
        th_pg = wall_pg;
    }
    for (int i = 0; i < 9; i++) {
        int k = wall_pg * 9 + i, sel; double x = 16 + 123.0 * (i % 3), y = 68 + 96.0 * (i / 3);
        if (k >= cnt) break;
        sel = k == cur;
        if (th[i]) H->image(th[i], x, y, 114, 64, 12);
        else { H->color(1, 1, 1, 0.1); H->round_rect(x, y, 114, 64, 12); H->fill(); }
        if (sel) { H->accent(1); H->round_rect(x, y, 114, 64, 12); H->stroke(3); }
        snprintf(s, sizeof s, "%.15s", H->wall_name(k));
        if (sel) H->accent(1); else H->color(1, 1, 1, 0.85);
        H->text(s, x + 57, y + 80, 12, sel, RAK_CENTER);
    }
    snprintf(s, sizeof s, pages > 1 ? "%d / %d   hold a photo to rename" : "hold a photo to rename", wall_pg + 1, pages);
    H->color(1, 1, 1, 0.5); H->text(s, 200, 380, 12, 0, RAK_CENTER);
    if (ren_k >= 0) {
        H->color(0, 0, 0, 0.75); H->round_rect(-1, -1, 402, 402, 0); H->fill();
        H->color(0.14, 0.14, 0.17, 1); H->round_rect(30, 130, 340, 130, 18); H->fill();
        H->accent(1); H->text("RENAME", 48, 160, 12, 1, RAK_LEFT);
        H->color(1, 1, 1, 0.12); H->round_rect(48, 174, 304, 40, 10); H->fill();
        snprintf(s, sizeof s, "%s_", ren_buf); H->color(1, 1, 1, 1); H->text(s, 60, 201, 18, 1, RAK_LEFT);
        H->color(1, 1, 1, 0.5); H->text("Type a name, Enter to save", 200, 242, 12, 0, RAK_CENTER);
    }
}
static void draw_dev(void) {
    char s[64]; int pages = (nts + DEV_PP - 1) / DEV_PP;
    H->header("Developer");
    if (pages < 1) pages = 1;
    if (dev_page >= pages) dev_page = pages - 1;
    if (!nts) { H->color(1, 1, 1, 0.6); H->text("assets/devtests.txt missing", 200, 200, 14, 0, RAK_CENTER); return; }
    for (int i = 0; i < DEV_PP; i++) {
        int k = dev_page * DEV_PP + i; double y = 64 + 54.0 * i;
        if (k >= nts) break;
        H->color(1, 1, 1, 0.07); H->round_rect(16, y, 368, 48, 16); H->fill();
        if (TS[k].danger) H->color(0.95, 0.27, 0.23, 1); else H->accent(1);
        H->round_rect(28, y + 12, 6, 24, 3); H->fill();
        H->color(1, 1, 1, 1); H->text(TS[k].name, 46, y + 21, 15, 1, RAK_LEFT);
        H->color(1, 1, 1, 0.55); H->text(TS[k].desc, 46, y + 38, 11, 0, RAK_LEFT);
        chevron(358, y + 24);
    }
    snprintf(s, sizeof s, "%d / %d   swipe up/down for more", dev_page + 1, pages);
    H->color(1, 1, 1, 0.5); H->text(s, 200, 396, 11, 0, RAK_CENTER);
}
static void draw_test(void) {
    int id = tst_kind; char s[80]; cairo_t *cr = CR();
    if (id == T_COLORS) {
        static const double C[5][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {1, 1, 1}, {0, 0, 0}};
        H->clear(C[tcol % 5][0], C[tcol % 5][1], C[tcol % 5][2]);
        if (tcol % 5 == 3) H->color(0, 0, 0, 0.6); else H->color(1, 1, 1, 0.6);
        H->text("tap: next color   swipe right: exit", 200, 390, 12, 0, RAK_CENTER);
        return;
    }
    if (id == T_GRAD) {
        static const double E[4][3] = {{1, 1, 1}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
        H->clear(0, 0, 0);
        for (int r = 0; r < 4; r++) {
            cairo_pattern_t *p = cairo_pattern_create_linear(20, 0, 380, 0);
            cairo_pattern_add_color_stop_rgb(p, 0, 0, 0, 0); cairo_pattern_add_color_stop_rgb(p, 1, E[r][0], E[r][1], E[r][2]);
            cairo_set_source(cr, p); cairo_rectangle(cr, 20, 30 + 84.0 * r, 360, 66); cairo_fill(cr); cairo_pattern_destroy(p);
        }
        H->color(1, 1, 1, 0.6); H->text("tap to exit", 200, 390, 12, 0, RAK_CENTER);
        return;
    }
    if (id == T_TOUCH) {
        H->clear(0.03, 0.03, 0.05);
        H->color(1, 1, 1, 0.06); cairo_set_line_width(cr, 1);
        for (int g = 50; g < 400; g += 50) { cairo_move_to(cr, g, 0); cairo_line_to(cr, g, 400); cairo_move_to(cr, 0, g); cairo_line_to(cr, 400, g); cairo_stroke(cr); }
        for (int i = 0; i < tpn; i++) { H->accent(0.3 + 0.7 * (i + 1) / tpn); H->circle(tp[i][0], tp[i][1], 8); H->fill(); }
        H->color(1, 1, 1, 0.9); H->text("Touch Test", 200, 28, 16, 1, RAK_CENTER);
        if (tpn) { snprintf(s, sizeof s, "Last tap: %d, %d", (int)tp[tpn - 1][0], (int)tp[tpn - 1][1]); H->color(1, 1, 1, 0.8); H->text(s, 200, 52, 13, 0, RAK_CENTER); }
        if (tg_n) { snprintf(s, sizeof s, "Last swipe: dx %+.0f  dy %+.0f", tg_dx, tg_dy); H->color(1, 1, 1, 0.8); H->text(s, 200, 72, 13, 0, RAK_CENTER); }
        H->color(1, 1, 1, 0.14); H->round_rect(120, 356, 160, 34, 17); H->fill();
        H->color(1, 1, 1, 1); H->text("Exit", 200, 378, 14, 1, RAK_CENTER);
        return;
    }
    H->header(TS[tst_id].name);
    H->color(1, 1, 1, 0.07); H->round_rect(16, 64, 368, 262, 18); H->fill();
    if (id == T_FPS) {
        double rm = H->get(RAK_RENDER_MS), fi = H->get(RAK_FRAME_MS), m = rm > fi ? rm : fi;
        snprintf(s, sizeof s, "Render time: %.1f ms", rm); H->color(1, 1, 1, 0.95); H->text(s, 30, 100, 14, 0, RAK_LEFT);
        snprintf(s, sizeof s, "Frame interval: %.0f ms", fi); H->text(s, 30, 128, 14, 0, RAK_LEFT);
        snprintf(s, sizeof s, "Max frame rate: %.0f fps", m > 0 ? 1000.0 / m : 0); H->text(s, 30, 156, 14, 0, RAK_LEFT);
        snprintf(s, sizeof s, "Window: %.0fx%.0f  scale %.2f", H->get(RAK_WIN_W), H->get(RAK_WIN_H), H->get(RAK_SCALE)); H->text(s, 30, 212, 14, 0, RAK_LEFT);
        H->color(1, 1, 1, 0.5); H->text("Updates live while this page is open.", 30, 300, 12, 0, RAK_LEFT);
        return;
    }
    if (!tl_n && !tst_busy) {
        H->color(1, 1, 1, 0.9); H->text(TS[tst_id].desc, 200, 185, 13, 1, RAK_CENTER);
        H->color(1, 1, 1, 0.5); H->text(TS[tst_id].danger ? "Needs two taps to confirm." : "Press Run to start.", 200, 215, 12, 0, RAK_CENTER);
    } else {
        for (int i = 0; i < 10; i++) {
            char ln[64];
            pthread_mutex_lock(&mu); snprintf(ln, sizeof ln, "%.63s", tl[i]); pthread_mutex_unlock(&mu);
            H->color(1, 1, 1, 0.92); H->text(ln, 30, 92 + 24.0 * i, 13, 0, RAK_LEFT);
        }
    }
    {
        int armed = dev_arm == tst_id && H->now_ms() - dev_at < 4000;
        if (tst_busy) H->color(1, 1, 1, 0.15); else if (TS[tst_id].danger) H->color(0.95, 0.27, 0.23, armed ? 1 : 0.85); else H->accent(1);
        H->round_rect(100, 340, 200, 44, 22); H->fill();
        H->color(1, 1, 1, 1); H->text(tst_busy ? "Running..." : armed ? "Tap again to confirm" : tl_n ? "Run again" : "Run", 200, 368, 14, 1, RAK_CENTER);
    }
}

/* ---------- developer tests (run on worker threads; they only touch tl[] and H->hw / H->get) ---------- */
static void tl_set(int i, const char *fmt, ...) {
    va_list ap;
    if (t_gen != tst_gen || i < 0 || i >= 10) return;
    pthread_mutex_lock(&mu);
    va_start(ap, fmt); vsnprintf(tl[i], sizeof tl[i], fmt, ap); va_end(ap);
    if (i >= tl_n) tl_n = i + 1;
    pthread_mutex_unlock(&mu);
}
static volatile long long stress_end; static unsigned long long stress_ops[16];
static void *burn(void *a) {
    int i = (int)(intptr_t)a; double x = 0.5; unsigned long long n = 0;
    while (now_us() < stress_end) { for (int k = 0; k < 20000; k++) x = sin(x) * 1.0001 + 0.5; n += 20000; }
    stress_ops[i] = n + (x > 1e300 ? 1 : 0);
    return 0;
}
static void t_cpu(void) {
    int nc = H->sys->ncores > 0 ? H->sys->ncores : 1, made = 0; pthread_t t[16]; double tot = 0; RakSys s;
    if (nc > 16) nc = 16;
    stress_end = now_us() + 4000000LL;
    for (int i = 0; i < nc; i++) { stress_ops[i] = 0; if (pthread_create(&t[i], 0, burn, (void *)(intptr_t)i)) break; made++; }
    if (!made) { tl_set(0, "Could not start worker threads"); return; }
    tl_set(0, "Running on %d core(s)...", made);
    while (now_us() < stress_end) usleep(200000);
    for (int i = 0; i < made; i++) { pthread_join(t[i], 0); tot += (double)stress_ops[i]; }
    tl_set(0, "Ran on %d core(s) for 4 s", made);
    tl_set(1, "Total: %.1f Mops/s", tot / 4e6);
    tl_set(2, "Per core: %.1f Mops/s", tot / 4e6 / made);
    s = *H->sys;
    if (s.cpu_temp > 0) tl_set(3, "CPU temp (last sample): %.0f C", s.cpu_temp);
    if (s.cpu_mhz > 0) tl_set(4, "CPU clock (last sample): %.0f MHz", s.cpu_mhz);
    tl_set(5, "Done.");
}
static void t_mem(void) {
    size_t n = 64u << 20; unsigned char *p = malloc(n); long long a, b; double dt; volatile unsigned char *q; size_t bad = 0;
    if (!p) { tl_set(0, "Allocating 64 MB FAILED"); return; }
    tl_set(0, "Allocated 64 MB");
    a = now_us(); memset(p, 0xA5, n); b = now_us();
    dt = (b - a) / 1e6; if (dt < 1e-6) dt = 1e-6;
    tl_set(1, "Fill: %.0f MB/s", 64.0 / dt);
    q = p;
    for (size_t i = 0; i < n; i++) if (q[i] != 0xA5) bad++;
    tl_set(2, "Verify fill: %s (%lu bad)", bad ? "FAIL" : "OK", (unsigned long)bad);
    for (size_t i = 0; i < n; i++) q[i] = (unsigned char)(i * 31u);
    bad = 0;
    for (size_t i = 0; i < n; i++) if (q[i] != (unsigned char)(i * 31u)) bad++;
    tl_set(3, "Verify pattern: %s (%lu bad)", bad ? "FAIL" : "OK", (unsigned long)bad);
    free(p);
    tl_set(4, "Freed. Result: %s", bad ? "PROBLEM" : "PASS");
}
static void t_disk(void) {
    char path[320]; const char *d = H->data_dir(); size_t cs = 1u << 20; char *buf = malloc(cs); int ok = 1, fd; long long a, b; double dt;
    snprintf(path, sizeof path, "%sspeedtest.tmp", d);
    if (!buf) { tl_set(0, "Out of memory"); return; }
    memset(buf, 0x5A, cs);
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) { tl_set(0, "Cannot create the test file"); free(buf); return; }
    tl_set(0, "Writing 16 MB...");
    a = now_us();
    for (int i = 0; i < 16; i++) if (write(fd, buf, cs) != (ssize_t)cs) { ok = 0; break; }
    fsync(fd); b = now_us(); close(fd);
    dt = (b - a) / 1e6; if (dt < 1e-6) dt = 1e-6;
    tl_set(0, "Write: %.1f MB/s%s", 16.0 / dt, ok ? "" : " (error)");
    fd = open(path, O_RDONLY);
    if (fd >= 0) {
        long tot = 0; ssize_t r;
        posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
        tl_set(1, "Reading back...");
        a = now_us();
        while ((r = read(fd, buf, cs)) > 0) tot += r;
        b = now_us(); close(fd);
        dt = (b - a) / 1e6; if (dt < 1e-6) dt = 1e-6;
        tl_set(1, "Read: %.1f MB/s", tot / 1048576.0 / dt);
        tl_set(2, "Read back %ld of 16777216 bytes", tot);
    } else tl_set(1, "Cannot reopen the test file");
    unlink(path); free(buf);
    tl_set(3, "Test file removed.");
}
static void t_led(void) {
    int was = H->get(RAK_TORCH) != 0;
    tl_set(0, "Blinking 6 times...");
    for (int i = 0; i < 6; i++) { H->hw("torch", "on"); usleep(250000); H->hw("torch", "off"); usleep(250000); }
    H->hw("torch", was ? "on" : "off");
    tl_set(0, "Done. Torch restored to %s.", was ? "on" : "off");
    tl_set(1, "Nothing lit up? No ACT/led0/led1 here.");
}
static void bl_set(int v) { char c[16]; snprintf(c, sizeof c, "%d", v); H->hw("brightness", c); }
static void t_bl(void) {
    int v;
    tl_set(0, "Ramping up...");
    for (v = 10; v <= 255; v += 15) { bl_set(v); usleep(80000); }
    tl_set(0, "Ramping down...");
    for (v = 255; v >= 10; v -= 15) { bl_set(v); usleep(80000); }
    bl_set((int)(H->get(RAK_BRIGHTNESS) * 255 + 0.5));
    tl_set(0, "Done. Restored to %d%%.", (int)(H->get(RAK_BRIGHTNESS) * 100 + 0.5));
    tl_set(1, "Nothing changed? No /sys/class/backlight.");
}
static void t_rf(void) {
    int k = 2; DIR *d; struct dirent *e;
    tl_set(0, "Wi-Fi radio: %s", H->get(RAK_WIFI) ? "on" : "off");
    tl_set(1, "Airplane (all blocked): %s", H->get(RAK_AIRPLANE) ? "yes" : "no");
    d = opendir("/sys/class/rfkill");
    if (!d) { tl_set(2, "No /sys/class/rfkill here"); return; }
    while (k < 10 && (e = readdir(d))) {
        char p[300], ty[48], so[16], hd[16];
        if (strncmp(e->d_name, "rfkill", 6)) continue;
        ty[0] = so[0] = hd[0] = 0;
        snprintf(p, sizeof p, "/sys/class/rfkill/%.60s/type", e->d_name); readf(p, ty, sizeof ty); chomp(ty);
        snprintf(p, sizeof p, "/sys/class/rfkill/%.60s/soft", e->d_name); readf(p, so, sizeof so); chomp(so);
        snprintf(p, sizeof p, "/sys/class/rfkill/%.60s/hard", e->d_name); readf(p, hd, sizeof hd); chomp(hd);
        tl_set(k++, "%s: %s soft=%s hard=%s", e->d_name, ty, so, hd);
    }
    closedir(d);
    if (k == 2) tl_set(2, "No rfkill devices found");
}
static void t_sens(void) {
    RakSys s = *H->sys; char a[32], b[32];
    if (s.bat >= 0) tl_set(0, "Battery: %d%% %s", s.bat, s.charging ? "charging" : "on battery"); else tl_set(0, "Battery: none found");
    if (s.bat_v > 0) tl_set(1, "Battery voltage: %.2f V", s.bat_v); else tl_set(1, "Battery voltage: n/a");
    if (s.bat_temp > -900) tl_set(2, "Battery temp: %.1f C", s.bat_temp); else tl_set(2, "Battery temp: n/a");
    if (s.cpu_pct >= 0) tl_set(3, "CPU load: %.0f%%", s.cpu_pct); else tl_set(3, "CPU load: n/a");
    if (s.cpu_temp > 0) tl_set(4, "CPU temp: %.0f C", s.cpu_temp); else tl_set(4, "CPU temp: n/a");
    if (s.cpu_mhz > 0) tl_set(5, "CPU clock: %.0f MHz (%d cores)", s.cpu_mhz, s.ncores); else tl_set(5, "CPU clock: n/a (%d cores)", s.ncores);
    if (s.ram_tot > 0) { fmt_mb(a, sizeof a, s.ram_used); fmt_mb(b, sizeof b, s.ram_tot); tl_set(6, "RAM: %s of %s", a, b); } else tl_set(6, "RAM: n/a");
    if (s.st_tot > 0) { fmt_mb(a, sizeof a, s.st_used); fmt_mb(b, sizeof b, s.st_tot); tl_set(7, "Storage: %s of %s", a, b); } else tl_set(7, "Storage: n/a");
    tl_set(8, "Values refresh every 3 s.");
}
static void t_dmesg(void) {
    FILE *f = popen("dmesg 2>&1 | tail -n 9", "r"); char ln[160]; int k = 0;
    if (!f) { tl_set(0, "Could not run dmesg"); return; }
    while (k < 9 && fgets(ln, sizeof ln, f)) {
        char *p = ln; chomp(ln);
        if (*p == '[') { char *q = strchr(p, ']'); if (q && q[1] == ' ') p = q + 2; }
        tl_set(k++, "%.44s", p);
    }
    pclose(f);
    if (!k) tl_set(0, "No log output (needs root)");
}
static void t_info(void) {
    struct utsname un; struct sysinfo si; struct stat st;
    if (!uname(&un)) { tl_set(0, "Kernel: %.40s", un.release); tl_set(1, "Arch: %s  Host: %.20s", un.machine, un.nodename); }
    if (!sysinfo(&si)) {
        long m = si.uptime / 60;
        tl_set(2, "Uptime: %ldh %02ldm", m / 60, m % 60);
        tl_set(3, "Load: %.2f %.2f %.2f", si.loads[0] / 65536.0, si.loads[1] / 65536.0, si.loads[2] / 65536.0);
        tl_set(4, "Processes: %d", (int)si.procs);
    }
    tl_set(5, "Window: %.0fx%.0f  scale %.2f", H->get(RAK_WIN_W), H->get(RAK_WIN_H), H->get(RAK_SCALE));
    tl_set(6, "Render: %.1f ms", H->get(RAK_RENDER_MS));
    tl_set(7, "/dev/dri: %s  /dev/fb0: %s", stat("/dev/dri", &st) ? "no" : "yes", stat("/dev/fb0", &st) ? "no" : "yes");
    tl_set(8, "DISPLAY: %s", getenv("DISPLAY") ? getenv("DISPLAY") : "?");
    tl_set(9, "Wallpapers found: %d", H->wall_count());
}
static void t_panic(void) {
    tl_set(1, "Kernel auto-reboots 10 s after the panic.");
    tl_set(0, "Kernel panic in 3..."); sleep(1);
    tl_set(0, "Kernel panic in 2..."); sleep(1);
    tl_set(0, "Kernel panic in 1..."); sleep(1);
    tl_set(0, "Crashing the kernel now");
    sync();
    writef("/proc/sys/kernel/panic", "10");
    writef("/proc/sys/kernel/sysrq", "1");
    if (!writef("/proc/sysrq-trigger", "c")) { tl_set(0, "Could not trigger the panic."); tl_set(1, "Needs root and CONFIG_MAGIC_SYSRQ."); }
}
static void *test_thread(void *a) {
    long v = (long)a; int id = (int)(v & 255);
    t_gen = (int)(v >> 8);
    switch (id) {
    case T_PANIC: t_panic(); break;
    case T_CPU: t_cpu(); break;
    case T_MEM: t_mem(); break;
    case T_DISK: t_disk(); break;
    case T_LED: t_led(); break;
    case T_BL: t_bl(); break;
    case T_RF: t_rf(); break;
    case T_SENS: t_sens(); break;
    case T_DMESG: t_dmesg(); break;
    case T_INFO: t_info(); break;
    }
    if (t_gen == tst_gen) tst_busy = 0;
    return 0;
}
static void open_test(int k) {
    tst_gen++; tst_busy = 0; tst_id = k; tst_kind = TS[k].kind; dev_arm = -1; stage = 0;
    memset(tl, 0, sizeof tl); tl_n = 0; tcol = 0; tpn = 0; tg_n = 0;
    page = P_TEST;
}
static void run_test(void) {
    int id = tst_kind; pthread_t t;
    if (tst_busy) return;
    if (TS[tst_id].danger) {
        if (!(dev_arm == tst_id && H->now_ms() - dev_at < 4000)) { dev_arm = tst_id; dev_at = H->now_ms(); return; }
        dev_arm = -1;
    }
    pthread_mutex_lock(&mu); memset(tl, 0, sizeof tl); tl_n = 0; pthread_mutex_unlock(&mu);
    tst_busy = 1; t_gen = tst_gen;
    if (id == T_FREEZE) { tl_set(0, "Freezing the UI for 5 seconds..."); stage = 1; return; }
    if (id == T_CRASH) { tl_set(0, "Crashing RealOneOS (SIGSEGV)..."); stage = 1; return; }
    if (pthread_create(&t, 0, test_thread, (void *)(long)(id | (tst_gen << 8)))) { tst_busy = 0; tl_set(0, "Could not start the test thread"); return; }
    pthread_detach(t);
}
/* freeze/crash run on the UI thread one frame later, so the message is on screen first */
static void run_staged(void) {
    if (stage == 2) {
        stage = 0; t_gen = tst_gen;
        if (tst_kind == T_FREEZE) { sleep(5); tl_set(1, "Back. The UI was blocked for 5 s."); tst_busy = 0; }
        else if (tst_kind == T_CRASH) { usleep(500000); signal(SIGSEGV, SIG_DFL); raise(SIGSEGV); tst_busy = 0; }
    }
}

/* ---------- callbacks ---------- */
static void up(void) {
    if (page == P_WALL) { ren_k = -1; wall_free(); goto_page(P_DISP); }
    else if (page == P_TEST) { stage = 0; tst_gen++; goto_page(P_DEV); }
    else if (page == P_DEV) goto_page(P_ROOT);
    else goto_page(P_ROOT);
}
static void on_open(void) {
    load_tests(); H->wall_rescan();
    page = P_ROOT; ren_k = -1; br_drag = 0; dev_taps = 0; dev_page = 0; stage = 0; anim_on = 0;
}
static void on_close(void) { wall_free(); ren_k = -1; stage = 0; tst_gen++; H->animate(0); anim_on = 0; }
static void on_draw(void) {
    int want;
    run_staged();
    switch (page) {
    case P_ROOT: draw_root(); break;
    case P_DISP: draw_disp(); break;
    case P_BAT: draw_bat(); break;
    case P_PERF: draw_perf(); break;
    case P_ABOUT: draw_about(); break;
    case P_WALL: draw_wall(); break;
    case P_DEV: draw_dev(); break;
    case P_TEST: draw_test(); break;
    }
    if (stage == 1) { stage = 2; H->redraw(); }
    want = page == P_TEST && (tst_busy || tst_kind == T_FPS);
    if (want != anim_on) { anim_on = want; H->animate(want); }
}
static int on_press(double x, double y) {
    if (page == P_DISP && x >= SL_X - 10 && x <= SL_X + SL_W + 10 && fabs(y - DSL_Y) <= 28) {
        br_drag = 1; br_val = (x - SL_X) / SL_W; if (br_val < 0.04) br_val = 0.04; if (br_val > 1) br_val = 1;
        H->set(RAK_BRIGHTNESS, br_val); br_last = H->now_ms(); H->redraw();
        return 1;
    }
    return 0;
}
static void on_move(double x, double y) {
    (void)y;
    if (!br_drag) return;
    br_val = (x - SL_X) / SL_W; if (br_val < 0.04) br_val = 0.04; if (br_val > 1) br_val = 1;
    if (H->now_ms() - br_last > 150) { H->set(RAK_BRIGHTNESS, br_val); br_last = H->now_ms(); }
    H->redraw();
}
static void on_release(double x, double y) {
    (void)x; (void)y;
    if (!br_drag) return;
    H->set(RAK_BRIGHTNESS, br_val); br_drag = 0; H->redraw();
}
static int on_hold(double x, double y) {
    int k;
    if (page != P_WALL || ren_k >= 0) return 0;
    k = wall_tile(x, y); if (k < 0) return 0;
    ren_k = k; snprintf(ren_buf, sizeof ren_buf, "%s", H->wall_name(k)); H->redraw();
    return 1;
}
static int on_tap(double x, double y) {
    H->redraw();
    switch (page) {
    case P_ROOT: {
        int nrows = H->get(RAK_DEVMODE) != 0 ? 5 : 4; double p = nrows == 5 ? 60.0 : 70.0, rh = nrows == 5 ? 54.0 : 62.0;
        if (y < 56) { H->back(); return 1; }
        for (int i = 0; i < nrows; i++) {
            double ry = 74 + p * i;
            if (y >= ry - 2 && y <= ry + rh + 2) {
                if (SRP[i] == P_DEV) dev_page = 0;
                goto_page(SRP[i]); return 1;
            }
        }
        return 0;
    }
    case P_DISP:
        if (y < 56) { up(); return 1; }
        if (y >= 196 && y <= 272) { H->wall_rescan(); wall_free(); wall_pg = 0; goto_page(P_WALL); return 1; }
        return 0;
    case P_ABOUT:
        if (y < 56) { up(); return 1; }
        if (y >= 124 && y <= 158) {
            char m[48];
            if (H->now_ms() - dev_tap_t > 2000) dev_taps = 0;
            dev_tap_t = H->now_ms();
            if (H->get(RAK_DEVMODE) != 0) { H->toast("Developer mode is already on"); return 1; }
            dev_taps++;
            if (dev_taps >= 7) { H->set(RAK_DEVMODE, 1); dev_taps = 0; H->toast("Developer mode enabled"); }
            else if (dev_taps >= 3) { snprintf(m, sizeof m, "%d taps to developer mode", 7 - dev_taps); H->toast(m); }
            return 1;
        }
        return 0;
    case P_BAT: case P_PERF:
        if (y < 56) { up(); return 1; }
        return 0;
    case P_WALL: {
        int k;
        if (ren_k >= 0) { ren_k = -1; return 0; }
        if (y < 56) { up(); return 1; }
        k = wall_tile(x, y);
        if (k >= 0) { H->wall_set(k == H->wall_current() ? -1 : k); wall_free(); goto_page(P_DISP); return 1; }
        return 0;
    }
    case P_DEV:
        if (y < 56) { up(); return 1; }
        for (int i = 0; i < DEV_PP; i++) {
            int k = dev_page * DEV_PP + i; double ry = 64 + 54.0 * i;
            if (k >= nts || y < ry || y > ry + 48) continue;
            if (TS[k].kind == T_OFF) { H->set(RAK_DEVMODE, 0); H->toast("Developer mode turned off"); goto_page(P_ROOT); }
            else open_test(k);
            return 1;
        }
        return 0;
    case P_TEST:
        if (tst_kind == T_GRAD) { up(); return 1; }
        if (tst_kind == T_COLORS) { tcol = (tcol + 1) % 5; return 1; }
        if (tst_kind == T_TOUCH) {
            if (y > 350) { up(); return 1; }
            if (tpn == 24) { memmove(tp[0], tp[1], 23 * sizeof tp[0]); tpn = 23; }
            tp[tpn][0] = x; tp[tpn][1] = y; tpn++;
            return 0;
        }
        if (y < 56) { up(); return 1; }
        if (tst_kind == T_FPS) return 0;
        if (x >= 100 && x <= 300 && y >= 336 && y <= 388) { run_test(); return 1; }
        return 0;
    }
    return 0;
}
static int on_gesture(double dx, double dy) {
    int horiz = fabs(dx) > fabs(dy);
    H->redraw();
    if (page == P_ROOT) return 0;                                   /* swipe right: the OS closes the app */
    if (page == P_WALL && ren_k >= 0) return 1;
    if (page == P_TEST && tst_kind == T_TOUCH) { tg_dx = dx; tg_dy = dy; tg_n = 1; return 1; }
    if (horiz) { if (dx > 0) up(); return 1; }
    if (page == P_DEV) { int pages = (nts + DEV_PP - 1) / DEV_PP, np = dev_page + (dy < 0 ? 1 : -1); if (np >= 0 && np < pages) dev_page = np; return 1; }
    if (page == P_WALL) { int pages = H->wall_count() ? (H->wall_count() + 8) / 9 : 1, np = wall_pg + (dy < 0 ? 1 : -1); if (np >= 0 && np < pages) wall_pg = np; return 1; }
    return 1;
}
static void rename_commit(void) {
    if (ren_k >= 0 && ren_k < H->wall_count()) { H->wall_rename(ren_k, ren_buf); wall_free(); }
    ren_k = -1;
}
static int on_key(unsigned long ks, const char *u) {
    if (page == P_WALL && ren_k >= 0) {
        size_t l = strlen(ren_buf);
        if (ks == 0xff1b) ren_k = -1;                                                /* Escape */
        else if (ks == 0xff0d || ks == 0xff8d) rename_commit();                      /* Return / KP_Enter */
        else if (ks == 0xff08) { if (l) ren_buf[l - 1] = 0; }                        /* BackSpace */
        else for (; *u; u++) { l = strlen(ren_buf); if ((unsigned char)*u >= 32 && (unsigned char)*u < 127 && l < 15) { ren_buf[l] = *u; ren_buf[l + 1] = 0; } }
        H->redraw();
        return 1;
    }
    if (page != P_ROOT && (ks == 0xff1b || ks == 0xff50 || ks == 0xff55 || ks == 0x1008FF13UL)) { up(); H->redraw(); return 1; }
    return 0;                                                                         /* root: the OS closes the app */
}

static RakApp app = { .api = RAK_API, .size = sizeof(RakApp), .open = on_open, .close = on_close, .draw = on_draw,
    .press = on_press, .move = on_move, .release = on_release, .tap = on_tap, .hold = on_hold, .gesture = on_gesture, .key = on_key };
RAK_EXPORT const RakApp *rak_entry(const RakHost *h) { if (h->api < RAK_API || h->size < sizeof(RakHost)) return 0; H = h; return &app; }
