#define _GNU_SOURCE
#include <stdio.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdarg.h>
#include <jpeglib.h>
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/cursorfont.h>
#include <cairo.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/statvfs.h>
#include <sys/sysinfo.h>
#include <sys/time.h>
#include <sys/utsname.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <sys/reboot.h>
#include <linux/rfkill.h>
#if defined(__has_include)
#if __has_include(<alsa/asoundlib.h>)
#include <alsa/asoundlib.h>
#define HAVE_ALSA 1
#endif
#endif
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include "rak.h"

#define CW 400.0
#define MAXS 3000
#define WPS 400
#define TR_MS 260
#define HOLD_MS 350
#define FRAME_US 50000
static long frame_us(void);
#define PI 3.14159265358979323846
#define KS_VOLUP 0x1008FF13UL

enum { S_DIG, S_APPS, S_CC, S_POWER, S_APP, S_WIFI, S_WNET, S_WJOIN, S_SPLASH };

typedef struct {
    int bat, charging, ncores, bat_src;
    double cpu_temp;
    double bat_temp, bat_v;
    double cpu_pct, cpu_mhz, ram_used, ram_tot, st_used, st_tot;
} Sys;
static Sys G = { .bat = -1, .bat_temp = -999, .bat_v = -1, .cpu_temp = -1, .cpu_pct = -1, .cpu_mhz = -1, .ram_tot = -1, .st_tot = -1 }, S;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
#define SET(fld, v) do { pthread_mutex_lock(&mu); G.fld = (v); pthread_mutex_unlock(&mu); } while (0)

static Display *dpy; static Window win; static GC gc;
static int ww = 480, wh = 480;
static cairo_surface_t *surf; static cairo_t *cr; static XImage *ximg; static Pixmap backbuf;
static double sc = 1, ox = 0, oy = 0;
static struct tm tmv; static long g_now, last_input, tr_t0;
static int cur = S_SPLASH, running = 1, prev_scr = -1, tr_kind = 0;
static double bg_dark = 0.5, rend_ms = 99;
static const double ACC[3] = {0.30, 0.62, 0.95};
#define PANEL 0.07, 0.07, 0.09

static cairo_surface_t *wp_surf; static int last_wall = -1, wp_gen = 0, wp_c_gen = -1, wp_c_side = 0;
static char wp_path[320]; static double wp_avg[3] = {0.03, 0.03, 0.04};

static int torch_on = 0; static double br_f = 0.5, vol_f = 0.5;
static volatile int wifi_on = 0, airplane_on = 0;
static int wifi_before = 0;
static int cc_drag = -1; static long last_apply = 0;
static int dev_on = 0; static long toast_t = -100000; static char toast_buf[48];
static void set_toast(const char *m) { snprintf(toast_buf, sizeof toast_buf, "%s", m); toast_t = g_now; }
static long scr_t0 = -100000, cc_pop_t = -100000; static int cc_pop_i = -1;

#define NCELL 9
#define napp napp_n
typedef struct { Rak *rak; char id[40], name[40]; int glyph; double color[3]; cairo_surface_t *icon; } AppEnt;   /* rak == NULL: built-in (Power) */
static AppEnt apl[NCELL]; static int napp_n = 0;
static Rak raks[RAK_MAXAPPS]; static int nraks = 0;
static int app_cell[NCELL];
static double apx[NCELL], apy[NCELL];
static RakSys RS; static Rak *cur_rak; static int act = -1, want_close = 0, want_redraw = 0, app_anim = 0, app_cap = 0, app_hold = 0;
static int app_press = -1, app_drag = -1, app_from = 0; static long app_t0 = 0;

static long long now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000000LL + t.tv_nsec / 1000; }
static long long base_us = 0;
static long now_ms(void) { long long u = now_us(); if (!base_us) base_us = u - 1000000LL; return (long)((u - base_us) / 1000); }
static void col(double r, double g, double b, double a) { cairo_set_source_rgba(cr, r, g, b, a); }
static void acc(double a) { col(ACC[0], ACC[1], ACC[2], a); }
static void rr(double x, double y, double w, double h, double r) {
    r *= 0.35;
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -PI / 2, 0); cairo_arc(cr, x + w - r, y + h - r, r, 0, PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, PI / 2, PI); cairo_arc(cr, x + r, y + r, r, PI, 1.5 * PI);
    cairo_close_path(cr);
}
static void circ(double x, double y, double r) { cairo_new_sub_path(cr); cairo_arc(cr, x, y, r, 0, 2 * PI); }
static cairo_font_face_t *ffc[2];
static void T(const char *s, double x, double y, double sz, int bold, int al_) {
    if (!ffc[0]) {
        ffc[0] = cairo_toy_font_face_create("monospace", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        ffc[1] = cairo_toy_font_face_create("monospace", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    }
    cairo_set_font_face(cr, ffc[bold ? 1 : 0]);
    cairo_set_font_size(cr, sz);
    cairo_text_extents_t e; cairo_text_extents(cr, s, &e);
    double px = al_ == 0 ? x - e.x_advance / 2 : (al_ > 0 ? x - e.x_advance : x);
    cairo_move_to(cr, px, y); cairo_show_text(cr, s);
}
static void fillbg(double r, double g, double b) { col(r, g, b, 1); cairo_rectangle(cr, -1, -1, 402, 402); cairo_fill(cr); }
static cairo_surface_t *wp_cc[2];
static void bg(void) {
    cairo_surface_t *w = wp_cc[bg_dark > 0.3 ? 1 : 0];
    if (w) {
        cairo_save(cr); cairo_scale(cr, 1.0 / sc, 1.0 / sc);
        cairo_set_source_surface(cr, w, 0, 0); cairo_paint(cr);
        cairo_restore(cr);
    } else fillbg(0.02, 0.02, 0.03);
}
static void fmt_time(char *b, int n) { snprintf(b, n, "%02d:%02d", tmv.tm_hour, tmv.tm_min); }
static int hit(double x, double y, double cx, double cy, double r) { return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r; }
static double ease(double t) { if (t < 0) t = 0; if (t > 1) t = 1; return 1 - (1 - t) * (1 - t); }

static int writef(const char *path, const char *v) {
    int fd = open(path, O_WRONLY); if (fd < 0) return 0;
    ssize_t n = write(fd, v, strlen(v)); close(fd); return n > 0;
}
static int readf(const char *path, char *buf, int n);
/* The jailed user cannot write to /sys or /dev/rfkill. It drops a small request file into /run/realone-hw; the root-owned
   realone-hw.path unit validates it against a strict grammar (brightness N | led 0/1 | rfkill T 0/1) and performs it. */
static void hw_req(const char *fmt, int a, int b) {
    static unsigned seq; char p[96], t[48]; snprintf(p, sizeof p, "/run/realone-hw/req.%d.%u", (int)getpid(), seq++);
    snprintf(t, sizeof t, fmt, a, b);
    int fd = open(p, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd >= 0) { if (write(fd, t, strlen(t)) < 0) {} close(fd); }
}
static void hw_brightness(int v) {
    if (geteuid() != 0) { hw_req("brightness %d%.0d\n", v < 0 ? 0 : v > 255 ? 255 : v, 0); return; }
    DIR *d = opendir("/sys/class/backlight"); struct dirent *e; if (!d) return;
    while ((e = readdir(d))) {
        char p[300], t[32]; if (e->d_name[0] == '.') continue;
        snprintf(p, sizeof p, "/sys/class/backlight/%.80s/max_brightness", e->d_name);
        if (!readf(p, t, sizeof t)) continue;
        int mx = atoi(t), val = v * mx / 255; if (v > 0 && val < 1) val = 1;
        snprintf(p, sizeof p, "/sys/class/backlight/%.80s/brightness", e->d_name); snprintf(t, sizeof t, "%d", val); writef(p, t);
    }
    closedir(d);
}
static void hw_led(int on) {
    static char led[200];
    if (geteuid() != 0) { hw_req("led %d%.0d\n", on ? 1 : 0, 0); return; }
    if (!led[0]) {
        static const char *C[3] = {"/sys/class/leds/ACT", "/sys/class/leds/led0", "/sys/class/leds/led1"};
        for (int i = 0; i < 3 && !led[0]; i++) { char p[220]; snprintf(p, sizeof p, "%s/brightness", C[i]); if (!access(p, W_OK)) snprintf(led, sizeof led, "%s", C[i]); }
        if (!led[0]) return;
        char p[220]; snprintf(p, sizeof p, "%s/trigger", led); writef(p, "none");
    }
    char p[220], t[32]; snprintf(p, sizeof p, "%s/max_brightness", led);
    int mx = readf(p, t, sizeof t) ? atoi(t) : 1; snprintf(t, sizeof t, "%d", on ? (mx > 0 ? mx : 1) : 0);
    snprintf(p, sizeof p, "%s/brightness", led); writef(p, t);
}
static void hw_rfkill(int type, int block) {
    if (geteuid() != 0) { hw_req("rfkill %d %d\n", type & 15, block ? 1 : 0); return; }
    int fd = open("/dev/rfkill", O_WRONLY); if (fd < 0) return;
    struct rfkill_event ev; memset(&ev, 0, sizeof ev); ev.op = RFKILL_OP_CHANGE_ALL; ev.type = (unsigned char)type; ev.soft = (unsigned char)block;
    if (write(fd, &ev, sizeof ev) < 0) {}
    close(fd);
}
static void hw_rfkill_state(int *wifi, int *plane) {
    DIR *d = opendir("/sys/class/rfkill"); struct dirent *e; int n = 0, blocked = 0; *wifi = 0; if (!d) { *plane = 0; return; }
    while ((e = readdir(d))) {
        char p[300], t[32], ty[32]; if (strncmp(e->d_name, "rfkill", 6)) continue;
        snprintf(p, sizeof p, "/sys/class/rfkill/%.60s/type", e->d_name); if (!readf(p, ty, sizeof ty)) continue;
        snprintf(p, sizeof p, "/sys/class/rfkill/%.60s/soft", e->d_name); if (!readf(p, t, sizeof t)) continue;
        n++; if (atoi(t)) blocked++; else if (!strncmp(ty, "wlan", 4)) *wifi = 1;
    }
    closedir(d); *plane = n > 0 && blocked == n;
}
static void hw_volume(int v) {
#ifdef HAVE_ALSA
    snd_mixer_t *m; if (snd_mixer_open(&m, 0) < 0) return;
    if (snd_mixer_attach(m, "default") < 0 || snd_mixer_selem_register(m, NULL, NULL) < 0 || snd_mixer_load(m) < 0) { snd_mixer_close(m); return; }
    for (snd_mixer_elem_t *el = snd_mixer_first_elem(m); el; el = snd_mixer_elem_next(el)) {
        long lo, hi;
        if (!snd_mixer_selem_is_active(el) || !snd_mixer_selem_has_playback_volume(el)) continue;
        snd_mixer_selem_get_playback_volume_range(el, &lo, &hi);
        snd_mixer_selem_set_playback_volume_all(el, lo + (hi - lo) * v / 15);
        if (snd_mixer_selem_has_playback_switch(el)) snd_mixer_selem_set_playback_switch_all(el, v > 0);
        break;
    }
    snd_mixer_close(m);
#else
    (void)v;
#endif
}
/* Storage jail: the OS runs as the unprivileged user "realuser" (RealUser) and can only write inside Storage/ (User, Data/Apps, Downloads, Temps). */
static const char *storage_root(void) { const char *r = getenv("REALONE_STORAGE"); return r && *r ? r : "/Root/Storage"; }
static void hw_power(int reboot_it) {
    sync();
    if (geteuid() == 0) {   /* debug sessions running as root */
        if (kill(1, reboot_it ? SIGRTMIN + 5 : SIGRTMIN + 4) == 0) return;
        reboot(reboot_it ? RB_AUTOBOOT : RB_POWER_OFF);
        return;
    }
    /* unprivileged: drop a request for the root-owned realone-power.path unit, which only accepts "poweroff" or "reboot" */
    int fd = open("/run/realone-power/request", O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) { const char *w = reboot_it ? "reboot\n" : "poweroff\n"; if (write(fd, w, strlen(w)) < 0) {} close(fd); }
}
static void hook(const char *name, const char *arg) {
    int on = !strcmp(arg, "on");
    if (!strcmp(name, "brightness")) hw_brightness(atoi(arg));
    else if (!strcmp(name, "volume")) hw_volume(atoi(arg));
    else if (!strcmp(name, "torch")) hw_led(on);
    else if (!strcmp(name, "wifi")) hw_rfkill(RFKILL_TYPE_WLAN, !on);
    else if (!strcmp(name, "airplane")) hw_rfkill(RFKILL_TYPE_ALL, on);
    else if (!strcmp(name, "power")) hw_power(!strcmp(arg, "reboot"));
}
static int readf(const char *path, char *buf, int n) {
    FILE *f = fopen(path, "r"); if (!f) return 0;
    int k = (int)fread(buf, 1, n - 1, f); fclose(f);
    buf[k > 0 ? k : 0] = 0; return k > 0;
}
static void apply_br(void) { char c[16]; snprintf(c, sizeof c, "%d", (int)(br_f * 255 + 0.5)); hook("brightness", c); }
static void apply_vol(void) { char c[16]; snprintf(c, sizeof c, "%d", (int)(vol_f * 15 + 0.5)); hook("volume", c); }

static void cell_xy(int c, double *x, double *y) { *x = 90 + 110 * (c % 3); *y = 128 + 92 * (c / 3); }
static int cell_near(double x, double y) {
    int best = 0; double bd = 1e9;
    for (int c = 0; c < NCELL; c++) { double cx, cy; cell_xy(c, &cx, &cy); double d = (x - cx) * (x - cx) + (y - cy) * (y - cy); if (d < bd) { bd = d; best = c; } }
    return best;
}
static void apps_place(void) { for (int k = 0; k < napp; k++) cell_xy(app_cell[k], &apx[k], &apy[k]); }

static void cfg_path(char *p, int n) { const char *h = getenv("HOME"); snprintf(p, n, "%s/.realoneos5.conf", h ? h : "."); }
static void lay_path(char *p, int n) { const char *h = getenv("HOME"); snprintf(p, n, "%s/.realone/layout", h ? h : "."); }
static void save_cfg(void) {
    char p[320]; FILE *f; cfg_path(p, sizeof p);
    f = fopen(p, "w");
    if (f) { fprintf(f, "%.3f %.3f %d\n", br_f, vol_f, dev_on); fclose(f); }
    lay_path(p, sizeof p); f = fopen(p, "w");
    if (f) { for (int k = 0; k < napp; k++) fprintf(f, "%s %d\n", apl[k].id, app_cell[k]); fclose(f); }
}
static void load_cfg(void) {
    char p[320]; double b, v; int dv; cfg_path(p, sizeof p);
    FILE *f = fopen(p, "r"); if (!f) return;
    if (fscanf(f, "%lf %lf", &b, &v) == 2) {
        br_f = b < 0.04 ? 0.04 : b > 1 ? 1 : b; vol_f = v < 0 ? 0 : v > 1 ? 1 : v;
        if (fscanf(f, " %d", &dv) == 1) dev_on = dv != 0;
    }
    fclose(f);
}

struct jerr { struct jpeg_error_mgr pub; jmp_buf jb; };
static void jerr_exit(j_common_ptr c) { longjmp(((struct jerr *)c->err)->jb, 1); }
static unsigned rd16(const unsigned char *t, unsigned tn, unsigned p, int le) { if (p + 2 > tn) return 0; return le ? (unsigned)(t[p] | (t[p + 1] << 8)) : (unsigned)((t[p] << 8) | t[p + 1]); }
static unsigned rd32(const unsigned char *t, unsigned tn, unsigned p, int le) { return le ? rd16(t, tn, p, 1) | (rd16(t, tn, p + 2, 1) << 16) : (rd16(t, tn, p, 0) << 16) | rd16(t, tn, p + 2, 0); }
static int exif_orient(const unsigned char *d, unsigned n) {
    if (n < 14 || memcmp(d, "Exif\0\0", 6)) return 1;
    const unsigned char *t = d + 6; unsigned tn = n - 6; int le = t[0] == 'I';
    unsigned off = rd32(t, tn, 4, le), cnt = rd16(t, tn, off, le);
    for (unsigned i = 0; i < cnt && i < 64; i++) {
        unsigned e = off + 2 + 12 * i;
        if (rd16(t, tn, e, le) == 0x0112) { unsigned v = rd16(t, tn, e + 8, le); return v >= 1 && v <= 8 ? (int)v : 1; }
    }
    return 1;
}
static cairo_surface_t *orient(cairo_surface_t *src, int o) {
    if (o <= 1 || o > 8) return src;
    double w = cairo_image_surface_get_width(src), h = cairo_image_surface_get_height(src);
    int sw = o >= 5;
    cairo_surface_t *d = cairo_image_surface_create(CAIRO_FORMAT_RGB24, (int)(sw ? h : w), (int)(sw ? w : h));
    cairo_t *c = cairo_create(d); cairo_matrix_t m;
    switch (o) {
    case 2: cairo_matrix_init(&m, -1, 0, 0, 1, w, 0); break;
    case 3: cairo_matrix_init(&m, -1, 0, 0, -1, w, h); break;
    case 4: cairo_matrix_init(&m, 1, 0, 0, -1, 0, h); break;
    case 5: cairo_matrix_init(&m, 0, 1, 1, 0, 0, 0); break;
    case 6: cairo_matrix_init(&m, 0, 1, -1, 0, h, 0); break;
    case 7: cairo_matrix_init(&m, 0, -1, -1, 0, h, w); break;
    default: cairo_matrix_init(&m, 0, -1, 1, 0, 0, w); break;
    }
    cairo_set_matrix(c, &m); cairo_set_source_surface(c, src, 0, 0); cairo_paint(c); cairo_destroy(c);
    cairo_surface_destroy(src); cairo_surface_flush(d);
    return d;
}
static cairo_surface_t *load_jpeg(const char *path, int target) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    struct jpeg_decompress_struct ci; struct jerr je;
    cairo_surface_t *volatile sf = NULL; unsigned char *volatile row = NULL;
    ci.err = jpeg_std_error(&je.pub); je.pub.error_exit = jerr_exit;
    if (setjmp(je.jb)) { jpeg_destroy_decompress(&ci); fclose(f); free(row); if (sf) cairo_surface_destroy(sf); return NULL; }
    jpeg_create_decompress(&ci); jpeg_stdio_src(&ci, f);
    jpeg_save_markers(&ci, JPEG_APP0 + 1, 0xFFFF);
    jpeg_read_header(&ci, TRUE);
    int o = 1;
    for (jpeg_saved_marker_ptr m = ci.marker_list; m; m = m->next) if (m->marker == JPEG_APP0 + 1) { o = exif_orient(m->data, m->data_length); break; }
    int big = (int)(ci.image_width > ci.image_height ? ci.image_width : ci.image_height), d = 1;
    while (d < 8 && big / (d * 2) >= target) d *= 2;
    ci.scale_num = 1; ci.scale_denom = d; ci.out_color_space = JCS_RGB; ci.dct_method = JDCT_IFAST;
    jpeg_start_decompress(&ci);
    int w = (int)ci.output_width, h = (int)ci.output_height;
    sf = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
    if (cairo_surface_status(sf) != CAIRO_STATUS_SUCCESS || ci.output_components != 3) longjmp(je.jb, 1);
    unsigned char *px = cairo_image_surface_get_data(sf); int st = cairo_image_surface_get_stride(sf);
    row = malloc((size_t)w * 3);
    while (ci.output_scanline < ci.output_height) {
        unsigned char *rp = row; int y = (int)ci.output_scanline;
        if (jpeg_read_scanlines(&ci, &rp, 1) != 1) break;
        uint32_t *dst = (uint32_t *)(px + (size_t)y * st);
        for (int x = 0; x < w; x++) dst[x] = ((uint32_t)row[3 * x] << 16) | ((uint32_t)row[3 * x + 1] << 8) | row[3 * x + 2];
    }
    cairo_surface_mark_dirty(sf);
    jpeg_finish_decompress(&ci); jpeg_destroy_decompress(&ci); fclose(f); free(row);
    return orient(sf, o);
}
static cairo_surface_t *load_image(const char *path, int target) {
    const char *x = strrchr(path, '.');
    if (x && !strcasecmp(x, ".png")) {
        cairo_surface_t *s = cairo_image_surface_create_from_png(path);
        if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) { cairo_surface_destroy(s); return NULL; }
        return s;
    }
    return load_jpeg(path, target);
}

#define WLMAX 200
static struct wf { char p[240]; char n[40]; } wl[WLMAX];
static int nwl = 0;
static struct { char f[96]; char n[40]; } nm[200];
static int nnm = 0;
static const char *file_of(const char *path) { const char *b = strrchr(path, '/'); return b ? b + 1 : path; }
static void names_path(char *p, int n) { const char *h = getenv("HOME"); snprintf(p, n, "%s/.realone/wallpaper.names", h ? h : "."); }
static void load_names(void) {
    char p[320], ln[200]; nnm = 0; names_path(p, sizeof p);
    FILE *f = fopen(p, "r"); if (!f) return;
    while (nnm < 200 && fgets(ln, sizeof ln, f)) {
        char *t = strchr(ln, '\t'); if (!t) continue;
        *t++ = 0; t[strcspn(t, "\r\n")] = 0; if (!t[0]) continue;
        snprintf(nm[nnm].f, sizeof nm[nnm].f, "%s", ln); snprintf(nm[nnm].n, sizeof nm[nnm].n, "%s", t); nnm++;
    }
    fclose(f);
}
static void save_names(void) {
    char p[320]; names_path(p, sizeof p);
    FILE *f = fopen(p, "w"); if (!f) return;
    for (int i = 0; i < nnm; i++) fprintf(f, "%s\t%s\n", nm[i].f, nm[i].n);
    fclose(f);
}
static int cmp_n(const void *a, const void *b) { return strcasecmp(((const struct wf *)a)->n, ((const struct wf *)b)->n); }
static void add_dir(const char *dir) {
    DIR *d = opendir(dir); if (!d) return;
    struct dirent *e;
    while (nwl < WLMAX && (e = readdir(d))) {
        const char *n = e->d_name, *x = strrchr(n, '.');
        if (n[0] == '.' || !x || (strcasecmp(x, ".jpg") && strcasecmp(x, ".jpeg") && strcasecmp(x, ".png"))) continue;
        if (snprintf(wl[nwl].p, sizeof wl[nwl].p, "%s/%s", dir, n) >= (int)sizeof wl[nwl].p) continue;
        snprintf(wl[nwl].n, sizeof wl[nwl].n, "%.39s", n);
        char *dot = strrchr(wl[nwl].n, '.'); if (dot) *dot = 0;
        for (int q = 0; q < nnm; q++) if (!strcmp(nm[q].f, n)) { snprintf(wl[nwl].n, sizeof wl[nwl].n, "%s", nm[q].n); break; }
        nwl++;
    }
    closedir(d);
}
static void scan_photos(void) {
    char hw[300]; const char *h = getenv("HOME"); nwl = 0;
    load_names();
    add_dir("/usr/local/share/realone/wallpapers");   /* built-in wallpapers: part of the OS, read-only, outside Storage */
    snprintf(hw, sizeof hw, "%s/.realone/wallpapers", h ? h : "."); add_dir(hw);   /* the user's own pictures (data only) */
    qsort(wl, (size_t)nwl, sizeof wl[0], cmp_n);
}

static void scan_walls(void) {
    char p[320]; const char *h = getenv("HOME"); if (!h) h = "."; wp_path[0] = 0;
    snprintf(p, sizeof p, "%s/.realone/wallpaper.path", h);
    FILE *pf = fopen(p, "r");
    if (pf) {
        char b[320] = ""; if (fgets(b, sizeof b, pf)) b[strcspn(b, "\r\n")] = 0;
        fclose(pf);
        { char st[200]; snprintf(st, sizeof st, "%s/", storage_root());   /* only pictures from the OS or from Storage are accepted */
          if (strstr(b, "..") || (strncmp(b, "/usr/local/share/realone/wallpapers/", 36) && strncmp(b, st, strlen(st)))) b[0] = 0; }
        snprintf(wp_path, sizeof wp_path, "%s", b); return;
    }
    snprintf(wp_path, sizeof wp_path, "/usr/local/share/realone/wallpapers/Aurora.png");
    if (access(wp_path, R_OK)) wp_path[0] = 0;
}
static void load_wall(void) {
    last_wall = -1; wp_gen++;
    if (wp_surf) { cairo_surface_destroy(wp_surf); wp_surf = NULL; }
    wp_surf = cairo_image_surface_create(CAIRO_FORMAT_RGB24, WPS, WPS);
    cairo_t *c = cairo_create(wp_surf), *keep = cr; cr = c;
    cairo_scale(c, WPS / 400.0, WPS / 400.0);
    int ok = 0;
    if (wp_path[0]) {
        cairo_surface_t *img = load_image(wp_path, 700);
        if (img && cairo_image_surface_get_width(img) > 0) {
            double w = cairo_image_surface_get_width(img), h = cairo_image_surface_get_height(img), k = fmax(400.0 / w, 400.0 / h);
            cairo_translate(c, 200, 200); cairo_scale(c, k, k); cairo_translate(c, -w / 2, -h / 2);
            cairo_set_source_surface(c, img, 0, 0); cairo_paint(c);
            ok = 1;
        }
        if (img) cairo_surface_destroy(img);
    }
    if (!ok) fillbg(0.02, 0.02, 0.03);
    cr = keep; cairo_destroy(c); cairo_surface_flush(wp_surf);
    unsigned char *d = cairo_image_surface_get_data(wp_surf); int st = cairo_image_surface_get_stride(wp_surf);
    double r = 0, g = 0, b = 0; int n = 0;
    for (int y = 6; y < WPS; y += 30) for (int x = 6; x < WPS; x += 30) { unsigned v = *(unsigned *)(d + y * st + x * 4); r += (v >> 16) & 255; g += (v >> 8) & 255; b += v & 255; n++; }
    if (n) { wp_avg[0] = r / n / 255; wp_avg[1] = g / n / 255; wp_avg[2] = b / n / 255; }
}
static void set_wallpaper(const char *path) {
    char pf[320]; const char *h = getenv("HOME");
    snprintf(pf, sizeof pf, "%s/.realone/wallpaper.path", h ? h : ".");
    FILE *f = fopen(pf, "w"); if (f) { fputs(path, f); fputc('\n', f); fclose(f); }
    snprintf(wp_path, sizeof wp_path, "%s", path); load_wall();
}
static void build_cache(int side) {
    static const double dk[2] = {0.12, 0.5};
    if (wp_cc[0] && wp_c_side == side && wp_c_gen == wp_gen) return;
    for (int i = 0; i < 2; i++) { if (wp_cc[i]) cairo_surface_destroy(wp_cc[i]); wp_cc[i] = NULL; }
    wp_c_side = side; wp_c_gen = wp_gen;
    if (!wp_surf) return;
    for (int i = 0; i < 2; i++) {
        wp_cc[i] = cairo_image_surface_create(CAIRO_FORMAT_RGB24, side, side);
        cairo_t *c = cairo_create(wp_cc[i]);
        cairo_scale(c, side / (double)WPS, side / (double)WPS);
        cairo_set_source_surface(c, wp_surf, 0, 0); cairo_paint(c);
        cairo_identity_matrix(c); cairo_set_source_rgba(c, 0, 0, 0, dk[i]); cairo_paint(c);
        cairo_destroy(c); cairo_surface_flush(wp_cc[i]);
    }
}

static const char *BP[3] = {"/sys/class/power_supply/battery/", "/sys/class/power_supply/BAT0/", "/sys/class/power_supply/bms/"};
static void *poller(void *arg) {
    static char b[4096]; char t[96];
    unsigned long long pt = 0, pidl = 0; int nc = (int)sysconf(_SC_NPROCESSORS_CONF); struct sysinfo si; (void)arg;
    if (nc < 1) nc = 1;
    if (nc > 16) nc = 16;
    SET(ncores, nc);
    for (long tick = 0;; tick++, sleep(3)) {
        if (tick == 0) {
            { int w, a; hw_rfkill_state(&w, &a); wifi_on = w; airplane_on = a; }
        }
        {
            char base[200] = "", p[260];
            for (int i = 0; i < 3 && !base[0]; i++) { snprintf(p, sizeof p, "%scapacity", BP[i]); if (readf(p, t, sizeof t)) snprintf(base, sizeof base, "%s", BP[i]); }
            if (!base[0]) {
                DIR *pd = opendir("/sys/class/power_supply"); struct dirent *pe;
                while (pd && (pe = readdir(pd))) {
                    if (pe->d_name[0] == '.') continue;
                    snprintf(p, sizeof p, "/sys/class/power_supply/%.60s/capacity", pe->d_name);
                    if (readf(p, t, sizeof t)) { snprintf(base, sizeof base, "/sys/class/power_supply/%.60s/", pe->d_name); break; }
                }
                if (pd) closedir(pd);
            }
            if (base[0]) {
                snprintf(p, sizeof p, "%scapacity", base);
                if (readf(p, t, sizeof t)) { SET(bat, atoi(t)); SET(bat_src, 1); }
                snprintf(p, sizeof p, "%sstatus", base);
                if (readf(p, t, sizeof t)) SET(charging, strncmp(t, "Charging", 8) == 0);
                snprintf(p, sizeof p, "%svoltage_now", base);
                if (readf(p, t, sizeof t)) { double v = atof(t); if (v > 100000) v /= 1e6; else if (v > 100) v /= 1000; if (v > 0) SET(bat_v, v); }
                snprintf(p, sizeof p, "%stemp", base);
                if (readf(p, t, sizeof t)) SET(bat_temp, atof(t) / 10.0);
            }
        }
        if (readf("/proc/stat", b, sizeof b)) {
            unsigned long long u, n, s, i, io, ir, so, st;
            if (sscanf(b, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &u, &n, &s, &i, &io, &ir, &so, &st) == 8) {
                unsigned long long tot = u + n + s + i + io + ir + so + st, idl = i + io;
                if (pt && tot > pt) SET(cpu_pct, 100.0 * (double)((tot - pt) - (idl - pidl)) / (double)(tot - pt));
                pt = tot; pidl = idl;
            }
        }
        double mx = 0;
        for (int i = 0; i < nc; i++) {
            char p[96]; snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", i);
            if (readf(p, t, sizeof t) && atof(t) > mx) mx = atof(t);
        }
        if (mx > 0) SET(cpu_mhz, mx / 1000.0);
        if (readf("/sys/class/thermal/thermal_zone0/temp", t, sizeof t)) SET(cpu_temp, atof(t) / 1000.0);
        double tot = 0, av = 0; int ok = 0; const char *p;
        if (readf("/proc/meminfo", b, sizeof b)) {
            if ((p = strstr(b, "MemTotal:"))) sscanf(p, "MemTotal: %lf", &tot);
            if ((p = strstr(b, "MemAvailable:"))) sscanf(p, "MemAvailable: %lf", &av);
            if (tot > 0) { tot /= 1024; av /= 1024; ok = 1; }
        }
        if (!ok && !sysinfo(&si)) {
            tot = (double)si.totalram * si.mem_unit / 1048576.0;
            av = ((double)si.freeram + si.bufferram) * si.mem_unit / 1048576.0; ok = tot > 0;
        }
        if (ok) { pthread_mutex_lock(&mu); G.ram_tot = tot; G.ram_used = tot - av; pthread_mutex_unlock(&mu); }
        struct statvfs sv; const char *h = getenv("HOME");
        if (!statvfs(h ? h : "/", &sv) && sv.f_blocks) {
            double stt = (double)sv.f_blocks * sv.f_frsize / 1048576.0, fr = (double)sv.f_bavail * sv.f_frsize / 1048576.0;
            pthread_mutex_lock(&mu); G.st_tot = stt; G.st_used = stt - fr; pthread_mutex_unlock(&mu);
        }
    }
    return 0;
}

static void gear_path(double ro, double rt, int teeth) {
    cairo_new_path(cr);
    for (int i = 0; i < teeth; i++) {
        double a = i * 2 * PI / teeth, w = PI / teeth;
        double p[4][2] = {{a - w * 0.62, ro}, {a - w * 0.34, rt}, {a + w * 0.34, rt}, {a + w * 0.62, ro}};
        for (int j = 0; j < 4; j++) {
            double px = p[j][1] * sin(p[j][0]), py = -p[j][1] * cos(p[j][0]);
            if (i == 0 && j == 0) cairo_move_to(cr, px, py); else cairo_line_to(cr, px, py);
        }
    }
    cairo_close_path(cr);
}
static void glyph(int id, int on) {
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND); cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    switch (id) {
    case 0:
        cairo_save(cr); cairo_translate(cr, 0, 6);
        col(1, 1, 1, 1);
        cairo_move_to(cr, -12, -14); cairo_line_to(cr, 12, -14); cairo_line_to(cr, 7, -3); cairo_line_to(cr, -7, -3); cairo_close_path(cr); cairo_fill(cr);
        rr(-7, -4, 14, 28, 4.5); cairo_fill(cr);
        if (on) acc(1); else col(0.19, 0.19, 0.21, 1);
        rr(-2.2, 2, 4.4, 9, 2.2); cairo_fill(cr);
        if (on) {
            col(1, 1, 1, 1); cairo_set_line_width(cr, 3.4);
            cairo_move_to(cr, 0, -21); cairo_line_to(cr, 0, -29); cairo_stroke(cr);
            cairo_move_to(cr, -11, -19); cairo_line_to(cr, -16, -25); cairo_stroke(cr);
            cairo_move_to(cr, 11, -19); cairo_line_to(cr, 16, -25); cairo_stroke(cr);
        }
        cairo_restore(cr);
        break;
    case 1:
        cairo_set_line_width(cr, 3.5);
        for (int i = 0; i < 3; i++) { cairo_new_sub_path(cr); cairo_arc(cr, 0, 12, 10 + i * 9, -PI * 0.75, -PI * 0.25); cairo_stroke(cr); }
        circ(0, 12, 3.2); cairo_fill(cr);
        break;
    case 2:
        circ(0, 0, 8); cairo_fill(cr); cairo_set_line_width(cr, 3);
        for (int i = 0; i < 8; i++) { double a = i * PI / 4; cairo_move_to(cr, 14 * cos(a), 14 * sin(a)); cairo_line_to(cr, 21 * cos(a), 21 * sin(a)); cairo_stroke(cr); }
        break;
    case 3:
        cairo_move_to(cr, -18, -6); cairo_line_to(cr, -9, -6); cairo_line_to(cr, 2, -16); cairo_line_to(cr, 2, 16); cairo_line_to(cr, -9, 6); cairo_line_to(cr, -18, 6); cairo_close_path(cr); cairo_fill(cr);
        cairo_set_line_width(cr, 3.2);
        cairo_new_sub_path(cr); cairo_arc(cr, 2, 0, 9, -PI / 4, PI / 4); cairo_stroke(cr);
        cairo_new_sub_path(cr); cairo_arc(cr, 2, 0, 16, -PI / 4, PI / 4); cairo_stroke(cr);
        break;
    case 6:
        cairo_move_to(cr, 0, -22); cairo_line_to(cr, 4, -10); cairo_line_to(cr, 22, 4); cairo_line_to(cr, 22, 9); cairo_line_to(cr, 4, 3);
        cairo_line_to(cr, 3, 14); cairo_line_to(cr, 9, 19); cairo_line_to(cr, 9, 22); cairo_line_to(cr, 0, 20); cairo_line_to(cr, -9, 22);
        cairo_line_to(cr, -9, 19); cairo_line_to(cr, -3, 14); cairo_line_to(cr, -4, 3); cairo_line_to(cr, -22, 9); cairo_line_to(cr, -22, 4);
        cairo_line_to(cr, -4, -10); cairo_close_path(cr); cairo_fill(cr);
        break;
    case 7:
        cairo_set_line_width(cr, 3); rr(-14, -20, 28, 40, 5); cairo_stroke(cr);
        rr(-9, -15, 18, 8, 1.5); cairo_fill(cr);
        for (int r = 0; r < 2; r++) for (int c = -1; c <= 1; c++) { circ(c * 6.5, 3 + r * 9, 2.2); cairo_fill(cr); }
        break;
    case 8:
        cairo_set_line_width(cr, 3); rr(-19, -10, 32, 20, 4); cairo_stroke(cr);
        rr(15, -4, 4, 8, 1.5); cairo_fill(cr); rr(-14, -5.5, 20, 11, 2); cairo_fill(cr);
        break;
    case 9:
        rr(-16, 2, 8, 14, 2); cairo_fill(cr); rr(-4, -8, 8, 24, 2); cairo_fill(cr); rr(8, -18, 8, 34, 2); cairo_fill(cr);
        break;
    case 10:
        cairo_set_line_width(cr, 3); circ(0, 0, 18); cairo_stroke(cr);
        circ(0, -8, 2.6); cairo_fill(cr); rr(-2.2, -2, 4.4, 15, 2.2); cairo_fill(cr);
        break;
    case 11:
        cairo_set_line_width(cr, 3.8);
        cairo_new_sub_path(cr); cairo_arc(cr, 0, 3, 15, -PI / 2 + 0.65, 1.5 * PI - 0.65); cairo_stroke(cr);
        cairo_move_to(cr, 0, -20); cairo_line_to(cr, 0, 1); cairo_stroke(cr);
        break;
    case 12:
        cairo_set_line_width(cr, 3.4);
        cairo_move_to(cr, -8, -12); cairo_line_to(cr, -20, 0); cairo_line_to(cr, -8, 12); cairo_stroke(cr);
        cairo_move_to(cr, 8, -12); cairo_line_to(cr, 20, 0); cairo_line_to(cr, 8, 12); cairo_stroke(cr);
        cairo_move_to(cr, 3, -17); cairo_line_to(cr, -3, 17); cairo_stroke(cr);
        break;
    case 5:
        cairo_save(cr); cairo_scale(cr, 0.9, 0.9);
        gear_path(16, 22, 8); circ(0, 0, 7.5);
        cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD); cairo_fill(cr); cairo_set_fill_rule(cr, CAIRO_FILL_RULE_WINDING);
        cairo_restore(cr);
        break;
    }
}

#define SPL_IN 700
#define SPL_HOLD 2600
#define SPL_TOT 3400
static long splash_t0 = 0, last_click = -100000; static int tap_hit = 0;
static void play_snd(const char *name) {
    char p[160]; snprintf(p, sizeof p, "/usr/local/share/realone/%s", name);
    if (access(p, R_OK)) return;
    pid_t a = fork();
    if (a < 0) return;
    if (a == 0) {
        if (fork() != 0) _exit(0);
        int nl = open("/dev/null", O_RDWR);
        if (nl >= 0) { dup2(nl, 0); dup2(nl, 1); dup2(nl, 2); }
        execlp("mpg123", "mpg123", "-q", p, (char *)0);
        _exit(0);
    }
    waitpid(a, NULL, 0);
}
static void draw_splash(void) {
    fillbg(0, 0, 0);
    double e = (double)(g_now - splash_t0);
    double a = e < SPL_IN ? e / SPL_IN : e < SPL_HOLD ? 1 : 1 - (e - SPL_HOLD) / (double)(SPL_TOT - SPL_HOLD);
    if (a < 0) a = 0; if (a > 1) a = 1;
    double r = ease(e / 1200.0);
    col(1, 1, 1, a); T("RealOne OS", 200, 206 - 8 * (1 - r), 52, 1, 0);
    col(1, 1, 1, 0.6 * a); T("Made by NotBusySlime", 200, 336, 13, 0, 0);
    T("Secured by Aegis", 200, 356, 13, 0, 0);
}
static void draw_dig(void) {
    bg();
    char t[16], d[48]; fmt_time(t, sizeof t); strftime(d, sizeof d, "%A, %d %B", &tmv);
    col(1, 1, 1, 1); T(t, 200, 218, 100, 1, 0);
    col(1, 1, 1, 0.85); T(d, 200, 262, 20, 0, 0);
}

static void draw_apps(void) {
    bg();
    char t[16]; fmt_time(t, sizeof t);
    col(1, 1, 1, 0.97); T("Apps", 28, 52, 26, 1, -1);
    col(1, 1, 1, 0.7); T(t, 372, 50, 18, 0, 1);
    if (app_drag >= 0) {
        int nc = cell_near(apx[app_drag], apy[app_drag]);
        for (int c = 0; c < NCELL; c++) {
            double cx, cy; cell_xy(c, &cx, &cy);
            if (c == nc) { acc(0.9); cairo_set_line_width(cr, 2.5); rr(cx - 36, cy - 36, 72, 72, 22); cairo_stroke(cr); }
            else { col(1, 1, 1, 0.22); circ(cx, cy, 3); cairo_fill(cr); }
        }
    }
    for (int k = 0; k < napp; k++) {
        double sx = apx[k], sy = apy[k];
        int grab = app_drag == k;
        double pe = ease((g_now - scr_t0 - 45.0 * k) / 200.0), z = 0.7 + 0.3 * pe;
        if (app_press == k && app_drag < 0) z *= 0.92;
        cairo_save(cr); cairo_translate(cr, sx, sy); cairo_scale(cr, z, z); if (grab) cairo_scale(cr, 1.12, 1.12);
        col(apl[k].color[0], apl[k].color[1], apl[k].color[2], 1); rr(-32, -32, 64, 64, 20); cairo_fill(cr);
        if (apl[k].icon) {
            double iw = cairo_image_surface_get_width(apl[k].icon), ih = cairo_image_surface_get_height(apl[k].icon);
            cairo_save(cr); rr(-32, -32, 64, 64, 20); cairo_clip(cr);
            cairo_translate(cr, -32, -32); cairo_scale(cr, 64.0 / iw, 64.0 / ih); cairo_set_source_surface(cr, apl[k].icon, 0, 0); cairo_paint(cr);
            cairo_restore(cr);
        } else { col(1, 1, 1, 1); cairo_save(cr); cairo_scale(cr, 0.95, 0.95); glyph(apl[k].glyph, 1); cairo_restore(cr); }
        cairo_restore(cr);
        col(1, 1, 1, 0.95 * pe); T(apl[k].name, sx, sy + (grab ? 56 : 52), 14, 1, 0);
    }
}
static void sub_hdr(const char *title);

#define SL_X 40.0
#define SL_W 320.0
#define SL_H 48.0
static const double SLY[2] = {252, 322};
static void cc_btn(int i, double *x, double *y) { *x = 80 + 120 * i; *y = 120; }
static void cc_slide(int i, double x) {
    double f = (x - SL_X) / SL_W;
    if (f < 0) f = 0; if (f > 1) f = 1;
    if (i == 0) br_f = f < 0.04 ? 0.04 : f; else vol_f = f;
}
static void cc_apply(int i) { if (i == 0) apply_br(); else apply_vol(); last_apply = g_now; }
static void draw_cc(void) {
    fillbg(PANEL);
    char t[16], hdr[48], sub[16]; fmt_time(t, sizeof t);
    if (S.bat >= 0) snprintf(hdr, sizeof hdr, "%s   %d%%%s", t, S.bat, S.charging ? "+" : ""); else snprintf(hdr, sizeof hdr, "%s", t);
    col(1, 1, 1, 0.9); T(hdr, 200, 46, 16, 1, 0);
    static const int CG[3] = {0, 1, 6};
    static const char *CN[3] = {"Torch", "Wi-Fi", "Plane"};
    for (int i = 0; i < 3; i++) {
        double cx, cy; cc_btn(i, &cx, &cy);
        int on = i == 0 ? torch_on : i == 1 ? wifi_on : airplane_on;
        double pp = cc_pop_i == i ? (g_now - cc_pop_t) / 220.0 : 1, ps = pp < 1 ? 1 + 0.12 * sin(PI * pp) : 1;
        cairo_save(cr); cairo_translate(cr, cx, cy); cairo_scale(cr, ps, ps);
        if (on) acc(1); else col(1, 1, 1, 0.12);
        rr(-32, -32, 64, 64, 20); cairo_fill(cr);
        cairo_save(cr); cairo_scale(cr, 0.7, 0.7); col(1, 1, 1, 1); glyph(CG[i], on); cairo_restore(cr);
        cairo_restore(cr);
        col(1, 1, 1, 1); T(CN[i], cx, cy + 56, 12, 1, 0);
    }
    for (int i = 0; i < 2; i++) {
        double f = i == 0 ? br_f : vol_f, y = SLY[i];
        col(1, 1, 1, 0.12); rr(SL_X, y - SL_H / 2, SL_W, SL_H, SL_H / 2); cairo_fill(cr);
        cairo_save(cr);
        rr(SL_X, y - SL_H / 2, SL_W, SL_H, SL_H / 2); cairo_clip(cr);
        acc(1); cairo_rectangle(cr, SL_X, y - SL_H / 2, SL_W * f, SL_H); cairo_fill(cr);
        cairo_restore(cr);
        cairo_save(cr); cairo_translate(cr, SL_X + 28, y); cairo_scale(cr, 0.55, 0.55); col(1, 1, 1, 1); glyph(i == 0 ? 2 : 3, 1); cairo_restore(cr);
        snprintf(sub, sizeof sub, "%d%%", (int)(f * 100 + 0.5));
        col(1, 1, 1, 1); T(sub, SL_X + SL_W - 20, y + 5, 14, 1, 1);
    }
}

static void sub_hdr(const char *title) {
    fillbg(PANEL);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND); cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    col(1, 1, 1, 0.8); cairo_set_line_width(cr, 3);
    cairo_move_to(cr, 32, 30); cairo_line_to(cr, 24, 38); cairo_line_to(cr, 32, 46); cairo_stroke(cr);
    col(1, 1, 1, 1); T(title, 46, 46, 22, 1, -1);
}
static void kv(double y, const char *k, const char *v) {
    col(1, 1, 1, 0.55); T(k, 32, y, 14, 0, -1);
    col(1, 1, 1, 1); T(v, 368, y, 14, 1, 1);
}
static int pw_arm = -1; static long pw_t = 0;
static const char *PWN[2] = {"Shut down", "Restart"};
static const double PWC[2][3] = {{0.95, 0.27, 0.23}, {1.0, 0.62, 0.04}};
static double pw_y(int i) { return 84 + 120.0 * i; }
static void draw_power(void) {
    sub_hdr("Power");
    for (int i = 0; i < 2; i++) {
        double y = pw_y(i); int arm = pw_arm == i;
        col(1, 1, 1, 0.07); rr(16, y, 368, 104, 20); cairo_fill(cr);
        if (arm) col(PWC[i][0], PWC[i][1], PWC[i][2], 1); else col(PWC[i][0], PWC[i][1], PWC[i][2], 0.9);
        rr(30, y + 26, 52, 52, 15); cairo_fill(cr);
        cairo_save(cr); cairo_translate(cr, 56, y + 52); cairo_scale(cr, 0.6, 0.6); col(1, 1, 1, 1); glyph(11, 1); cairo_restore(cr);
        col(1, 1, 1, 1); T(PWN[i], 98, y + 52, 22, 1, -1);
        col(1, 1, 1, arm ? 0.95 : 0.5); T(arm ? "Tap again to confirm" : "Tap", 98, y + 76, 13, 0, -1);
    }
}
/* ---------------- Wi-Fi (hold the Wi-Fi icon in the control center) ---------------- */
#define WN_MAX 40
#define WF_Y0 112.0
#define WF_P 52.0
#define WF_PP 5
#define KB_Y 206.0
enum { SEC_OPEN, SEC_WEP, SEC_WPA, SEC_WPA3, SEC_EAP };
enum { WO_SCAN, WO_STATUS, WO_CONN, WO_SAVED, WO_DISC, WO_FORGET };
enum { K_SHIFT = -1, K_BKSP = -2, K_SYM = -3, K_JOIN = -4 };
static const char *SECN[5] = {"Open", "WEP", "WPA2", "WPA3", "WPA2-Ent"};
static const char *SECL[5] = {"Open (no encryption)", "WEP (insecure)", "WPA2-Personal", "WPA3-Personal", "WPA2/3-Enterprise"};
typedef struct { char ssid[40]; int sig, sec, saved, cur, id, trans; } WNet;
static WNet wfn[WN_MAX]; static int wfn_n = 0;
static pthread_mutex_t wfmu = PTHREAD_MUTEX_INITIALIZER;
static char wf_if[32], wf_msg[64], wf_cur[40], wf_ip[24];
static int wf_none = 0, wf_err = 0, wf_page = 0, wf_hold = 0, wf_press = -1;
static volatile int wf_busy = 0, wf_dirty = 0, wf_spin = 0;
static long wf_scan_t = -100000, wf_stat_t = -100000;
static struct { int op, sec, hidden, eap, id, trans; char ssid[40], user[64], pass[72]; } wa;
static char wnet_ssid[40];
static char wj_ssid[40], wj_user[64], wj_pass[72];
static int wj_sec, wj_hidden, wj_trans, wj_focus, wj_show, wj_layer, wj_eap;

static int isdigit_(int c) { return c >= '0' && c <= '9'; }
static void msleep(int ms) { struct timespec t; t.tv_sec = ms / 1000; t.tv_nsec = (long)(ms % 1000) * 1000000L; nanosleep(&t, 0); }
static int is_hex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

/* Runs a program WITHOUT a shell (no injection from SSIDs/passwords), 8 s timeout, output captured. */
static int run_argv(char *const av[], char *out, int n) {
    int pf[2]; if (out && n > 0) out[0] = 0;
    if (pipe(pf)) return -1;
    pid_t pid = fork();
    if (pid < 0) { close(pf[0]); close(pf[1]); return -1; }
    if (pid == 0) {
        int dn = open("/dev/null", O_RDWR);
        dup2(dn, 0); dup2(pf[1], 1); dup2(dn, 2);
        for (int f = 3; f < 64; f++) close(f);
        setenv("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", 1);
        execvp(av[0], av); _exit(127);
    }
    close(pf[1]);
    int len = 0; long long t0 = now_us();
    for (;;) {
        fd_set fs; FD_ZERO(&fs); FD_SET(pf[0], &fs); struct timeval tv = {0, 200000};
        int r = select(pf[0] + 1, &fs, 0, 0, &tv);
        if (r > 0) { char b[512]; int k = (int)read(pf[0], b, sizeof b); if (k <= 0) break; if (out && len + k < n) { memcpy(out + len, b, k); len += k; } }
        if (now_us() - t0 > 8000000LL) { kill(pid, SIGKILL); break; }
    }
    close(pf[0]);
    int st = 0; waitpid(pid, &st, 0);
    if (out && n > 0) out[len < n ? len : n - 1] = 0;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}
static int wcli(char *out, int n, ...) {
    char *av[20]; int ac = 0; char *s; va_list ap;
    static char tdir[160]; snprintf(tdir, sizeof tdir, "%s/Temps", storage_root());
    av[ac++] = "wpa_cli"; av[ac++] = "-s"; av[ac++] = tdir; av[ac++] = "-i"; av[ac++] = wf_if;
    va_start(ap, n);
    while (ac < 19 && (s = va_arg(ap, char *))) av[ac++] = s;
    va_end(ap); av[ac] = NULL;
    return run_argv(av, out, n);
}
static int wset(const char *id, const char *var, const char *val) { char o[64]; wcli(o, sizeof o, "set_network", id, var, val, NULL); return !strncmp(o, "OK", 2); }
static int wset_q(const char *id, const char *var, const char *v) { char q[160]; snprintf(q, sizeof q, "\"%s\"", v); return wset(id, var, q); }

static void wf_find_if(void) {
    DIR *d = opendir("/sys/class/net"); struct dirent *e; wf_if[0] = 0;
    while (d && (e = readdir(d))) {
        char p[300]; if (e->d_name[0] == '.') continue;
        snprintf(p, sizeof p, "/sys/class/net/%.60s/wireless", e->d_name);
        if (access(p, F_OK)) snprintf(p, sizeof p, "/sys/class/net/%.60s/phy80211", e->d_name);
        if (!access(p, F_OK)) { snprintf(wf_if, sizeof wf_if, "%.30s", e->d_name); break; }
    }
    if (d) closedir(d);
    wf_none = wf_if[0] == 0;
}
/* wpa_cli prints SSIDs with \xNN escapes; turn them back into real bytes */
static void ssid_unesc(char *o, int n, const char *s) {
    int k = 0;
    for (; *s && k < n - 1; s++) {
        if (*s == '\\' && s[1]) {
            s++;
            if (*s == 'x' && s[1] && s[2]) { int v = 0; if (sscanf(s + 1, "%2x", &v) == 1) { if (v) o[k++] = (char)v; s += 2; continue; } }
            if (*s == 'n' || *s == 'r' || *s == 't' || *s == 'e') o[k++] = '?'; else o[k++] = *s;
        } else o[k++] = *s;
    }
    o[k] = 0;
}
static void trunc_u8(char *o, int n, const char *s, int mx) {
    int l = (int)strlen(s);
    if (l > mx) { l = mx; while (l > 0 && ((unsigned char)s[l] & 0xC0) == 0x80) l--; snprintf(o, n, "%.*s..", l, s); }
    else snprintf(o, n, "%s", s);
}
static int sec_of(const char *f) {
    int eap = strstr(f, "EAP") != 0, sae = strstr(f, "SAE") != 0, psk = strstr(f, "PSK") != 0, wpa = strstr(f, "WPA") != 0, wep = strstr(f, "WEP") != 0;
    if (eap) return SEC_EAP;
    if (sae && !psk) return SEC_WPA3;
    if (psk || wpa) return SEC_WPA;
    if (wep) return SEC_WEP;
    return SEC_OPEN;
}
static int q_bars(int q) { return q > 75 ? 4 : q > 50 ? 3 : q > 25 ? 2 : q > 0 ? 1 : 0; }
static void wf_parse_scan(const char *buf, WNet *t, int *tn) {
    char *cp = strdup(buf), *sv = 0, *ln; if (!cp) return;
    for (ln = strtok_r(cp, "\n", &sv); ln; ln = strtok_r(0, "\n", &sv)) {
        char *f[5] = {0}; int nf = 0; char *p = ln;
        if (!strncmp(ln, "bssid", 5)) continue;
        f[nf++] = p;
        while (*p && nf < 5) { if (*p == '\t') { *p = 0; f[nf++] = p + 1; } p++; }
        if (nf < 5 || !f[4][0]) continue;
        WNet w; memset(&w, 0, sizeof w); int d = atoi(f[2]), q = d < 0 ? 2 * (d + 100) : d, j;
        ssid_unesc(w.ssid, sizeof w.ssid, f[4]);
        w.sec = sec_of(f[3]); w.trans = strstr(f[3], "SAE") && strstr(f[3], "PSK"); w.id = -1;
        w.sig = q < 1 ? 1 : q > 100 ? 100 : q;
        for (j = 0; j < *tn; j++) if (!strcmp(t[j].ssid, w.ssid)) break;
        if (j < *tn) { if (w.sig > t[j].sig) t[j].sig = w.sig; continue; }
        if (*tn < WN_MAX) t[(*tn)++] = w;
    }
    free(cp);
}
static void wf_mark_saved(const char *buf, WNet *t, int *tn) {
    char *cp = strdup(buf), *sv = 0, *ln; if (!cp) return;
    for (ln = strtok_r(cp, "\n", &sv); ln; ln = strtok_r(0, "\n", &sv)) {
        char *f[4] = {0}; int nf = 0, j; char *p = ln, ss[40];
        if (!strncmp(ln, "network", 7)) continue;
        f[nf++] = p;
        while (*p && nf < 4) { if (*p == '\t') { *p = 0; f[nf++] = p + 1; } p++; }
        if (nf < 2 || !f[1][0]) continue;
        ssid_unesc(ss, sizeof ss, f[1]);
        int id = atoi(f[0]), cu = nf >= 4 && strstr(f[3], "CURRENT") != 0;
        for (j = 0; j < *tn; j++) if (!strcmp(t[j].ssid, ss)) break;
        if (j < *tn) { t[j].saved = 1; t[j].id = id; t[j].cur = cu; }
        else if (*tn < WN_MAX) { WNet w; memset(&w, 0, sizeof w); snprintf(w.ssid, sizeof w.ssid, "%s", ss); w.sec = SEC_WPA; w.saved = 1; w.id = id; w.cur = cu; t[(*tn)++] = w; }
    }
    free(cp);
}
static int wf_rank(const WNet *w) { return (w->cur ? 1000000 : 0) + (w->sig > 0 ? 10000 : 0) + w->sig * 10 + (w->saved ? 1 : 0); }
static void wf_publish(WNet *t, int n) {
    for (int i = 1; i < n; i++) { WNet x = t[i]; int j = i - 1; while (j >= 0 && wf_rank(&t[j]) < wf_rank(&x)) { t[j + 1] = t[j]; j--; } t[j + 1] = x; }
    pthread_mutex_lock(&wfmu);
    if (!wifi_on) n = 0;
    memcpy(wfn, t, sizeof(WNet) * n); wfn_n = n;
    pthread_mutex_unlock(&wfmu); wf_dirty = 1;
}
static int wf_cnt(void) { int r; pthread_mutex_lock(&wfmu); r = wfn_n; pthread_mutex_unlock(&wfmu); return r; }
static int wf_get(int k, WNet *o) { int r = 0; pthread_mutex_lock(&wfmu); if (k >= 0 && k < wfn_n) { *o = wfn[k]; r = 1; } pthread_mutex_unlock(&wfmu); return r; }
static int wf_idx(const char *ssid) { int r = -1; pthread_mutex_lock(&wfmu); for (int i = 0; i < wfn_n; i++) if (!strcmp(wfn[i].ssid, ssid)) { r = i; break; } pthread_mutex_unlock(&wfmu); return r; }
static int wf_pages(void) { int it = wf_cnt() + 1; return (it + WF_PP - 1) / WF_PP; }

static void wf_status(void) {
    char b[1024], *sv = 0, *ln, ss[40] = "", ip[24] = ""; int comp = 0;
    wcli(b, sizeof b, "status", NULL);
    for (ln = strtok_r(b, "\n", &sv); ln; ln = strtok_r(0, "\n", &sv)) {
        if (!strncmp(ln, "wpa_state=", 10)) comp = !strcmp(ln + 10, "COMPLETED");
        else if (!strncmp(ln, "ssid=", 5)) ssid_unesc(ss, sizeof ss, ln + 5);
        else if (!strncmp(ln, "ip_address=", 11)) snprintf(ip, sizeof ip, "%s", ln + 11);
    }
    snprintf(wf_cur, sizeof wf_cur, "%s", comp ? ss : ""); snprintf(wf_ip, sizeof wf_ip, "%s", comp ? ip : "");
}
static int wf_net_has(const char *idb, const char *flag) {
    char b[2048], *sv = 0, *ln; size_t il = strlen(idb);
    wcli(b, sizeof b, "list_networks", NULL);
    for (ln = strtok_r(b, "\n", &sv); ln; ln = strtok_r(0, "\n", &sv)) if (!strncmp(ln, idb, il) && ln[il] == '\t' && strstr(ln, flag)) return 1;
    return 0;
}

static void wf_do_scan(void) {
    static char sr[16384], ln[2048]; WNet t[WN_MAX]; int n = 0, rc;
    if (wf_none) { snprintf(wf_msg, sizeof wf_msg, "No Wi-Fi adapter found"); wf_err = 1; wf_publish(t, 0); return; }
    wcli(ln, sizeof ln, "scan", NULL);
    msleep(2500);
    rc = wcli(sr, sizeof sr, "scan_results", NULL);
    if (rc != 0 || strstr(sr, "Failed")) { snprintf(wf_msg, sizeof wf_msg, "Wi-Fi service not ready"); wf_err = 1; return; }
    if (wf_err == 1) { wf_msg[0] = 0; wf_err = 0; }
    wf_parse_scan(sr, t, &n);
    wcli(ln, sizeof ln, "list_networks", NULL);
    wf_mark_saved(ln, t, &n);
    wf_publish(t, n);
    wf_status();
}
static int wf_wait_up(const char *idb, const char *ssid) {
    for (int i = 0; i < 50; i++) {
        msleep(500); wf_status();
        if (!strcmp(wf_cur, ssid) && wf_ip[0]) return 1;
        if (i % 2 == 1 && wf_net_has(idb, "TEMP-DISABLED")) return -1;
    }
    return !strcmp(wf_cur, ssid) ? 2 : 0;
}
static void wf_fail(const char *m) { snprintf(wf_msg, sizeof wf_msg, "%s", m); wf_err = 2; }
static void wf_do_conn(int saved) {
    char idb[16] = "", hx[100], o[2048]; int i, ok = 1, r;
    wf_msg[0] = 0; wf_err = 0;
    snprintf(wf_msg, sizeof wf_msg, "Connecting to %.30s...", wa.ssid);
    if (wf_none) { wf_fail("No Wi-Fi adapter found"); return; }
    if (wa.sec == SEC_WEP) { wf_fail("WEP is not supported"); return; }
    if (saved) snprintf(idb, sizeof idb, "%d", wa.id);
    else {
        char *sv = 0, *ln;
        wcli(o, sizeof o, "list_networks", NULL);                 /* replace an old entry with the same name */
        for (ln = strtok_r(o, "\n", &sv); ln; ln = strtok_r(0, "\n", &sv)) {
            char *tb = strchr(ln, '\t'); char ss[40], *te; if (!tb || !isdigit_(ln[0])) continue;
            te = strchr(tb + 1, '\t'); if (te) *te = 0;
            ssid_unesc(ss, sizeof ss, tb + 1);
            if (!strcmp(ss, wa.ssid)) { *tb = 0; wcli(hx, sizeof hx, "remove_network", ln, NULL); }
        }
        wcli(o, sizeof o, "add_network", NULL);
        if (!isdigit_(o[0])) { wf_fail("Wi-Fi service not ready"); return; }
        snprintf(idb, sizeof idb, "%d", atoi(o));
        for (i = 0; wa.ssid[i] && i < 32; i++) snprintf(hx + 2 * i, 3, "%02x", (unsigned char)wa.ssid[i]);
        ok = wset(idb, "ssid", hx);
        if (wa.hidden) ok = ok && wset(idb, "scan_ssid", "1");
        if (wa.sec == SEC_OPEN) ok = ok && wset(idb, "key_mgmt", "NONE");
        else if (wa.sec == SEC_WPA) {
            int pl = (int)strlen(wa.pass);
            ok = ok && wset(idb, "key_mgmt", wa.trans || wa.hidden ? "WPA-PSK SAE" : "WPA-PSK");
            ok = ok && (pl == 64 ? wset(idb, "psk", wa.pass) : wset_q(idb, "psk", wa.pass));
            ok = ok && wset(idb, "ieee80211w", "1");
        } else if (wa.sec == SEC_WPA3) {
            ok = ok && wset(idb, "key_mgmt", "SAE") && wset_q(idb, "sae_password", wa.pass) && wset(idb, "ieee80211w", "2");
        } else if (wa.sec == SEC_EAP) {
            ok = ok && wset(idb, "key_mgmt", "WPA-EAP") && wset(idb, "eap", wa.eap ? "TTLS" : "PEAP");
            ok = ok && wset_q(idb, "identity", wa.user) && wset_q(idb, "password", wa.pass) && wset_q(idb, "phase2", "auth=MSCHAPV2");
            if (!access("/etc/ssl/certs/ca-certificates.crt", R_OK)) ok = ok && wset_q(idb, "ca_cert", "/etc/ssl/certs/ca-certificates.crt");
            ok = ok && wset(idb, "ieee80211w", "1");
        }
        if (!ok) { wcli(o, sizeof o, "remove_network", idb, NULL); wf_fail("Could not apply settings"); return; }
    }
    wcli(o, sizeof o, "select_network", idb, NULL);
    r = wf_wait_up(idb, wa.ssid);
    if (r == 1 || r == 2) { wcli(o, sizeof o, "save_config", NULL); wf_msg[0] = 0; wf_err = 0; if (r == 2) snprintf(wf_msg, sizeof wf_msg, "Connected, waiting for IP"); }
    else { if (!saved) wcli(o, sizeof o, "remove_network", idb, NULL); wf_fail(r < 0 ? "Wrong password" : "Could not connect"); }
    wf_do_scan();
}
static void *wf_thread(void *arg) {
    int op = wa.op; (void)arg;
    if (op == WO_SCAN) { wf_spin = 1; wf_do_scan(); }
    else if (op == WO_STATUS) { if (!wf_none) wf_status(); }
    else if (op == WO_CONN || op == WO_SAVED) { wf_spin = 2; wf_do_conn(op == WO_SAVED); }
    else if (op == WO_DISC) {
        wf_spin = 1;
        if (!wf_none) { char o[128]; wcli(o, sizeof o, "disconnect", NULL); msleep(400); wf_do_scan(); }
    } else if (op == WO_FORGET) {
        wf_spin = 1;
        if (!wf_none) { char o[128], idb[16]; snprintf(idb, sizeof idb, "%d", wa.id); wcli(o, sizeof o, "remove_network", idb, NULL); wcli(o, sizeof o, "save_config", NULL); wf_do_scan(); }
    }
    memset(wa.pass, 0, sizeof wa.pass);
    wf_spin = 0; wf_dirty = 1; wf_busy = 0;
    return 0;
}
static void wf_op(int op) {
    pthread_t t;
    if (wf_busy) return;
    wa.op = op; wf_busy = 1; wf_stat_t = g_now; if (op == WO_SCAN) wf_scan_t = g_now;
    if (pthread_create(&t, 0, wf_thread, 0)) { wf_busy = 0; return; }
    pthread_detach(t);
}
static void wf_tick(void) {
    if (wf_busy || !wifi_on) return;
    if (g_now - wf_scan_t > 30000) wf_op(WO_SCAN);
    else if (g_now - wf_stat_t > 5000) wf_op(WO_STATUS);
}

/* ---- on-screen keyboard ---- */
typedef struct { double x, y, w, h; int code; char lab[8]; } WK;
static WK wk[64]; static int wk_n;
static void wk_add(double x, double y, double w, int code, const char *lab) {
    if (wk_n >= 64) return;
    WK *k = &wk[wk_n++]; k->x = x; k->y = y; k->w = w; k->h = 32; k->code = code; snprintf(k->lab, sizeof k->lab, "%s", lab);
}
static void wk_build(void) {
    static const char *R0 = "1234567890";
    static const char *R1[4] = {"qwertyuiop", "QWERTYUIOP", "!@#$%^&*()", "<>|~`"};
    static const char *R2[4] = {"asdfghjkl", "ASDFGHJKL", "-_=+[]{};:", ""};
    static const char *R3[4] = {"zxcvbnm", "ZXCVBNM", "'\",.?/\\", ""};
    static const char *SHL[4] = {"Aa", "AA", "1/2", "2/2"};
    const char *rows[4]; wk_n = 0;
    rows[0] = R0; rows[1] = R1[wj_layer]; rows[2] = R2[wj_layer]; rows[3] = R3[wj_layer];
    for (int r = 0; r < 4; r++) {
        int n = (int)strlen(rows[r]); double y = KB_Y + 36.0 * r;
        for (int i = 0; i < n; i++) { char l[2] = {rows[r][i], 0}; wk_add(200 - n * 18.4 + i * 36.8 + 2, y, 32.8, rows[r][i], l); }
    }
    wk_add(16, KB_Y + 108, 48, K_SHIFT, SHL[wj_layer]);
    wk_add(336, KB_Y + 108, 48, K_BKSP, "DEL");
    wk_add(16, KB_Y + 144, 70, K_SYM, wj_layer < 2 ? "123" : "abc");
    wk_add(92, KB_Y + 144, 200, ' ', "space");
    wk_add(298, KB_Y + 144, 86, K_JOIN, "Join");
}
static char *wj_buf(void) { return wj_focus == 0 ? wj_ssid : wj_focus == 1 ? wj_user : wj_pass; }
static void wj_type(int c) {
    char *b = wj_buf(); size_t l = strlen(b); int cap = wj_focus == 0 ? 32 : wj_focus == 1 ? 63 : 64;
    if ((int)l < cap && c >= 32 && c < 127) { b[l] = (char)c; b[l + 1] = 0; }
}
static void wj_bksp(void) { char *b = wj_buf(); size_t l = strlen(b); if (l) b[l - 1] = 0; }
static int wj_fields(int *ids) { int n = 0; if (wj_hidden) ids[n++] = 0; if (wj_sec == SEC_EAP) ids[n++] = 1; if (wj_sec != SEC_OPEN) ids[n++] = 2; return n; }
static int wj_chips(int *ids) { int n = 0; if (wj_hidden) ids[n++] = 0; if (wj_sec == SEC_EAP) ids[n++] = 1; if (wj_sec != SEC_OPEN) ids[n++] = 2; return n; }

/* ---- drawing ---- */
static void sig_bars(double x, double y, int bars, double a) {
    for (int i = 0; i < 4; i++) { double h = 6 + 5 * i; col(1, 1, 1, i < bars ? a : 0.25 * a); rr(x + i * 7, y - h, 4.5, h, 4); cairo_fill(cr); }
}
static void lock_icon(double x, double y, double a) {
    col(1, 1, 1, a); rr(x - 6, y - 1, 12, 9, 4); cairo_fill(cr);
    cairo_set_line_width(cr, 2); cairo_new_sub_path(cr); cairo_arc(cr, x, y - 1, 4, PI, 2 * PI); cairo_stroke(cr);
}
static void draw_wifi(void) {
    char a[64], b[64]; WNet w; const char *l1, *l2; int cnt, items, pages;
    sub_hdr("Wi-Fi");
    if (wifi_on) acc(1); else col(1, 1, 1, 0.2);
    rr(318, 24, 48, 24, 24); cairo_fill(cr);
    col(1, 1, 1, 1); circ(wifi_on ? 354 : 330, 36, 9); cairo_fill(cr);
    if (!wifi_on) {
        col(1, 1, 1, 0.5); T("Wi-Fi is off", 200, 190, 20, 1, 0);
        col(1, 1, 1, 0.35); T("Turn it on to see networks", 200, 218, 13, 0, 0);
        return;
    }
    col(1, 1, 1, 0.07); rr(16, 58, 368, 46, 16); cairo_fill(cr);
    if (wf_spin == 2) { l1 = wf_msg[0] ? wf_msg : "Connecting..."; l2 = "Please wait"; }
    else if (wf_msg[0]) { l1 = wf_cur[0] ? wf_cur : "Not connected"; l2 = wf_msg; }
    else if (wf_cur[0]) {
        snprintf(b, sizeof b, "Connected%s", wf_ip[0] ? " - " : ""); if (wf_ip[0]) { size_t L = strlen(b); snprintf(b + L, sizeof b - L, "%s", wf_ip); }
        l1 = wf_cur; l2 = b;
    } else { l1 = "Not connected"; l2 = wf_none ? "No Wi-Fi adapter found" : "Choose a network"; }
    trunc_u8(a, sizeof a, l1, 24);
    col(1, 1, 1, 1); T(a, 30, 79, 15, 1, -1);
    if (wf_spin != 2 && wf_msg[0]) col(1.0, 0.45, 0.4, 1); else if (wf_cur[0] && wf_spin != 2) acc(1); else col(1, 1, 1, 0.55);
    T(l2, 30, 96, 11, 0, -1);
    {
        double ang = wf_spin ? (g_now % 1000) / 1000.0 * 2 * PI : 0;
        cairo_save(cr); cairo_translate(cr, 360, 81); cairo_rotate(cr, ang);
        col(1, 1, 1, 0.85); cairo_set_line_width(cr, 2.4); cairo_new_sub_path(cr); cairo_arc(cr, 0, 0, 9, 0.5, 5.3); cairo_stroke(cr);
        cairo_move_to(cr, 7.2, -10.8); cairo_line_to(cr, 10, -4.2); cairo_line_to(cr, 2.8, -4.2); cairo_close_path(cr); cairo_fill(cr);
        cairo_restore(cr);
    }
    cnt = wf_cnt(); items = cnt + 1; pages = (items + WF_PP - 1) / WF_PP;
    if (wf_page >= pages) wf_page = pages - 1;
    if (wf_page < 0) wf_page = 0;
    if (cnt == 0 && wf_spin == 1) { col(1, 1, 1, 0.5); T("Scanning...", 200, 200, 16, 1, 0); }
    for (int i = 0; i < WF_PP; i++) {
        int k = wf_page * WF_PP + i; double y = WF_Y0 + WF_P * i;
        if (k >= items) break;
        if (k == cnt) {
            col(1, 1, 1, 0.07); rr(16, y, 368, 46, 16); cairo_fill(cr);
            col(1, 1, 1, 0.9); cairo_set_line_width(cr, 2.6); cairo_move_to(cr, 38, y + 23); cairo_line_to(cr, 56, y + 23); cairo_move_to(cr, 47, y + 14); cairo_line_to(cr, 47, y + 32); cairo_stroke(cr);
            T("Add network", 76, y + 22, 15, 1, -1); col(1, 1, 1, 0.5); T("Hidden or manual", 76, y + 38, 11, 0, -1);
            continue;
        }
        if (!wf_get(k, &w)) continue;
        if (w.cur) acc(0.22); else col(1, 1, 1, 0.07);
        rr(16, y, 368, 46, 16); cairo_fill(cr);
        sig_bars(34, y + 34, q_bars(w.sig), w.sig > 0 ? 1.0 : 0.35);
        trunc_u8(a, sizeof a, w.ssid, 24);
        col(1, 1, 1, w.sig > 0 || w.cur ? 1 : 0.6); T(a, 76, y + 21, 15, 1, -1);
        if (w.cur) { acc(1); snprintf(b, sizeof b, "Connected"); }
        else if (w.sec == SEC_WEP) { col(1, 1, 1, 0.5); snprintf(b, sizeof b, "WEP - unsupported"); }
        else if (w.sec == SEC_OPEN) { col(1, 0.75, 0.3, 0.9); snprintf(b, sizeof b, "%sOpen - not secure", w.saved ? "Saved - " : ""); }
        else { col(1, 1, 1, 0.5); if (w.saved) snprintf(b, sizeof b, "Saved - %s%s", w.sig > 0 ? "" : "not in range - ", SECN[w.sec]); else snprintf(b, sizeof b, "%s", SECN[w.sec]); }
        T(b, 76, y + 38, 11, 0, -1);
        if (w.sec != SEC_OPEN) lock_icon(362, y + 20, 0.8);
    }
    if (pages > 1) for (int p = 0; p < pages; p++) { col(1, 1, 1, p == wf_page ? 0.9 : 0.3); circ(200 + (p - (pages - 1) / 2.0) * 12, 389, 3); cairo_fill(cr); }
}
static void draw_wnet(void) {
    WNet w; char t[40], b[64]; int k = wf_idx(wnet_ssid);
    if (k < 0 || !wf_get(k, &w)) { memset(&w, 0, sizeof w); snprintf(w.ssid, sizeof w.ssid, "%s", wnet_ssid); w.id = -1; w.sec = SEC_WPA; }
    trunc_u8(t, sizeof t, w.ssid, 16); sub_hdr(t);
    kv(96, "Status", w.cur ? "Connected" : (w.saved ? "Saved" : "Not saved"));
    kv(126, "Security", SECL[w.sec]);
    if (w.sig > 0) snprintf(b, sizeof b, "%d%% %s", w.sig, w.sig > 75 ? "Excellent" : w.sig > 50 ? "Good" : w.sig > 25 ? "Fair" : "Weak"); else snprintf(b, sizeof b, "Out of range");
    kv(156, "Signal", b);
    if (w.cur && wf_ip[0]) kv(186, "IP address", wf_ip);
    if (w.cur || (w.saved && w.sig > 0)) { acc(0.9); rr(40, 262, 320, 44, 22); cairo_fill(cr); col(1, 1, 1, 1); T(w.cur ? "Disconnect" : "Connect", 200, 290, 15, 1, 0); }
    if (w.saved) { col(0.95, 0.27, 0.23, 0.9); rr(40, 316, 320, 44, 22); cairo_fill(cr); col(1, 1, 1, 1); T("Forget network", 200, 344, 15, 1, 0); }
}
static void draw_wjoin(void) {
    char t[40], s[96]; int ids[3], nf = wj_fields(ids), ch[3], nc = wj_chips(ch);
    static const char *PH[3] = {"Network name", "Identity", "Password"};
    trunc_u8(t, sizeof t, wj_hidden ? "Add network" : wj_ssid, 18); sub_hdr(t);
    for (int i = 0; i < nf; i++) {
        int f = ids[i], l, o, m = 0; double y = 58 + 34.0 * i; const char *v = f == 0 ? wj_ssid : f == 1 ? wj_user : wj_pass;
        col(1, 1, 1, 0.10); rr(16, y, 368, 30, 10); cairo_fill(cr);
        if (wj_focus == f) { acc(1); cairo_set_line_width(cr, 1.6); rr(16, y, 368, 30, 10); cairo_stroke(cr); }
        if (!v[0]) { col(1, 1, 1, 0.4); T(PH[f], 28, y + 20, 14, 0, -1); continue; }
        l = (int)strlen(v); o = l > 28 ? l - 28 : 0;
        for (int j = o; j < l && m < 60; j++) s[m++] = (f == 2 && !wj_show) ? '*' : v[j];
        if (wj_focus == f && m < 60) s[m++] = '_';
        s[m] = 0; col(1, 1, 1, 1); T(s, 28, y + 20, 14, 1, -1);
    }
    for (int i = 0; i < nc; i++) {
        double x = 16 + 124.0 * i, y = 58 + 34.0 * nf;
        col(1, 1, 1, 0.14); rr(x, y, 116, 28, 14); cairo_fill(cr);
        if (ch[i] == 0) snprintf(s, sizeof s, "Sec: %s", SECN[wj_sec]); else if (ch[i] == 1) snprintf(s, sizeof s, "%s", wj_eap ? "TTLS" : "PEAP"); else snprintf(s, sizeof s, "%s", wj_show ? "Hide" : "Show");
        col(1, 1, 1, 1); T(s, x + 58, y + 19, 13, 1, 0);
    }
    wk_build();
    for (int i = 0; i < wk_n; i++) {
        WK *k = &wk[i]; int sp = k->code < 0 || k->code == ' ';
        if (k->code == K_JOIN) acc(1); else col(1, 1, 1, sp ? 0.20 : 0.12);
        rr(k->x, k->y, k->w, k->h, 9); cairo_fill(cr);
        col(1, 1, 1, 1); T(k->lab, k->x + k->w / 2, k->y + 21, sp ? 12 : 15, 1, 0);
    }
}

static int is_face(int s) { return s == S_DIG; }
static void go(int s, int kind) {
    if (s == cur) return;
    if (cur == S_WJOIN) memset(wj_pass, 0, sizeof wj_pass);
    scr_t0 = g_now;
    prev_scr = (kind == 0) ? -1 : cur; cur = s; tr_kind = kind; tr_t0 = g_now; last_input = g_now;
    app_press = -1; app_drag = -1; cc_drag = -1; pw_arm = -1;
}
static void draw_screen(int s);

/* ---------------- RAK host: what apps in .RAK packages can call (see rakapi.h) ---------------- */
static const RakApp *A(void) { if (act < 0 || !apl[act].rak) return NULL; cur_rak = apl[act].rak; return cur_rak->app; }
static cairo_t *H_cairo(void) { return cr; }
static void H_color(double r, double g, double b, double a) { col(r, g, b, a); }
static void H_accent(double a) { acc(a); }
static void H_rr(double x, double y, double w, double h, double r) { rr(x, y, w, h, r); }
static void H_circ(double x, double y, double r) { circ(x, y, r); }
static void H_fill(void) { cairo_fill(cr); }
static void H_stroke(double w) { cairo_set_line_width(cr, w); cairo_stroke(cr); }
static void H_text(const char *s, double x, double y, double sz, int b, int al) { T(s, x, y, sz, b, al); }
static void H_clear(double r, double g, double b) { fillbg(r, g, b); }
static void H_panel(void) { fillbg(PANEL); }
static void H_header(const char *t) { sub_hdr(t); }
static void H_glyph(int id, int on) { glyph(id, on); }
static long H_now_ms(void) { return now_ms(); }
static void H_back(void) { want_close = 1; }
static void H_toast(const char *m) { set_toast(m); }
static void H_redraw(void) { want_redraw = 1; }
static void H_animate(int on) { app_anim = on != 0; }
static void H_click(void) { play_snd("click.mp3"); }
static double H_get(int id) {
    switch (id) {
    case RAK_BRIGHTNESS: return br_f; case RAK_VOLUME: return vol_f; case RAK_DEVMODE: return dev_on;
    case RAK_TORCH: return torch_on; case RAK_WIFI: return wifi_on; case RAK_AIRPLANE: return airplane_on;
    case RAK_RENDER_MS: return rend_ms; case RAK_FRAME_MS: return frame_us() / 1000.0;
    case RAK_WIN_W: return ww; case RAK_WIN_H: return wh; case RAK_SCALE: return sc;
    }
    return 0;
}
static void H_set(int id, double v) {
    if (id == RAK_BRIGHTNESS) { br_f = v < 0.04 ? 0.04 : v > 1 ? 1 : v; apply_br(); save_cfg(); }
    else if (id == RAK_VOLUME) { vol_f = v < 0 ? 0 : v > 1 ? 1 : v; apply_vol(); save_cfg(); }
    else if (id == RAK_DEVMODE) { dev_on = v != 0; save_cfg(); }
}
static void H_hw(const char *n, const char *a) { hook(n, a); }
static const unsigned char *H_asset(const char *name, uint32_t *size) { return cur_rak ? rak_get(cur_rak, name, size) : NULL; }
struct pngrd { const unsigned char *d; uint32_t n, p; };
static cairo_status_t png_read(void *c, unsigned char *buf, unsigned len) {
    struct pngrd *r = c;
    if (r->p + len > r->n) return CAIRO_STATUS_READ_ERROR;
    memcpy(buf, r->d + r->p, len); r->p += len; return CAIRO_STATUS_SUCCESS;
}
static cairo_surface_t *H_png(const char *name) {
    uint32_t n; const unsigned char *d = cur_rak ? rak_get(cur_rak, name, &n) : NULL; struct pngrd r; cairo_surface_t *sf;
    if (!d) return NULL;
    r.d = d; r.n = n; r.p = 0;
    sf = cairo_image_surface_create_from_png_stream(png_read, &r);
    if (cairo_surface_status(sf) != CAIRO_STATUS_SUCCESS) { cairo_surface_destroy(sf); return NULL; }
    return sf;
}
static const char *H_data_dir(void) {
    static char d[400]; char t[400];
    snprintf(t, sizeof t, "%s/Data/Apps", storage_root()); mkdir(t, 0700);
    snprintf(d, sizeof d, "%s/%s/", t, cur_rak ? cur_rak->id : "app"); mkdir(d, 0700);
    return d;
}
static cairo_surface_t *H_load_image(const char *p, int t) { return load_image(p, t); }
static int H_wcount(void) { return nwl; }
static const char *H_wname(int i) { return i >= 0 && i < nwl ? wl[i].n : ""; }
static const char *H_wpath(int i) { return i >= 0 && i < nwl ? wl[i].p : ""; }
static int H_wcur(void) { for (int i = 0; i < nwl; i++) if (wp_path[0] && !strcmp(wl[i].p, wp_path)) return i; return -1; }
static void H_wset(int i) { char np[240]; snprintf(np, sizeof np, "%s", i >= 0 && i < nwl ? wl[i].p : ""); set_wallpaper(np); }
static void H_wscan(void) { scan_photos(); }
static void H_wren(int i, const char *name) {
    const char *fn; int q;
    if (i < 0 || i >= nwl) return;
    fn = file_of(wl[i].p);
    for (q = 0; q < nnm; q++) if (!strcmp(nm[q].f, fn)) break;
    if (name[0]) {
        if (q == nnm && nnm < 200) nnm++;
        if (q < nnm) { snprintf(nm[q].f, sizeof nm[q].f, "%s", fn); snprintf(nm[q].n, sizeof nm[q].n, "%s", name); }
    } else if (q < nnm) { nm[q] = nm[nnm - 1]; nnm--; }
    save_names(); scan_photos();
}
static void H_image(cairo_surface_t *im, double x, double y, double w, double h, double r) {
    double iw, ih, k;
    if (!im) return;
    iw = cairo_image_surface_get_width(im); ih = cairo_image_surface_get_height(im);
    if (iw < 1 || ih < 1) return;
    k = fmax(w / iw, h / ih);
    cairo_save(cr); rr(x, y, w, h, r); cairo_clip(cr);
    cairo_translate(cr, x + w / 2, y + h / 2); cairo_scale(cr, k, k); cairo_translate(cr, -iw / 2, -ih / 2);
    cairo_set_source_surface(cr, im, 0, 0); cairo_paint(cr); cairo_restore(cr);
}
static void H_sfree(cairo_surface_t *sf) { if (sf) cairo_surface_destroy(sf); }
static const RakHost HOST = {
    .api = RAK_API, .size = sizeof(RakHost),
    .cairo = H_cairo, .color = H_color, .accent = H_accent, .round_rect = H_rr, .circle = H_circ, .fill = H_fill, .stroke = H_stroke,
    .text = H_text, .clear = H_clear, .panel = H_panel, .header = H_header, .glyph = H_glyph,
    .sys = &RS, .now = &tmv, .now_ms = H_now_ms,
    .back = H_back, .toast = H_toast, .redraw = H_redraw, .animate = H_animate, .click = H_click, .get = H_get, .set = H_set, .hw = H_hw,
    .asset = H_asset, .png = H_png, .data_dir = H_data_dir, .load_image = H_load_image,
    .wall_count = H_wcount, .wall_name = H_wname, .wall_path = H_wpath, .wall_current = H_wcur, .wall_set = H_wset, .wall_rename = H_wren, .wall_rescan = H_wscan,
    .image = H_image, .surface_free = H_sfree
};
/* the OS keeps its own Sys; apps get a RakSys copy */
static void sync_rs(void) {
    RS.bat = S.bat; RS.charging = S.charging; RS.ncores = S.ncores; RS.cpu_temp = S.cpu_temp; RS.bat_temp = S.bat_temp; RS.bat_v = S.bat_v;
    RS.cpu_pct = S.cpu_pct; RS.cpu_mhz = S.cpu_mhz; RS.ram_used = S.ram_used; RS.ram_tot = S.ram_tot; RS.st_used = S.st_used; RS.st_tot = S.st_tot;
}

/* ---- trusted apps: a .RAK is only ever loaded if its SHA-256 is in apps.sha256 (root-owned, immutable, installed with `aegis install-app`) ---- */
static const uint32_t K256[64] = {0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
#define ROR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
static void sha256_hex(const unsigned char *d, size_t n, char out[65]) {
    uint32_t h[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    size_t total = n + 1 + 8, padn = (64 - total % 64) % 64, len = total + padn;
    for (size_t off = 0; off < len; off += 64) {
        unsigned char blk[64]; uint32_t w[64];
        for (int i = 0; i < 64; i++) {
            size_t pos = off + i; unsigned char c;
            if (pos < n) c = d[pos]; else if (pos == n) c = 0x80;
            else if (pos >= len - 8) c = (unsigned char)(((unsigned long long)n * 8) >> (8 * (len - 1 - pos))); else c = 0;
            blk[i] = c;
        }
        for (int i = 0; i < 16; i++) w[i] = ((uint32_t)blk[4*i] << 24) | ((uint32_t)blk[4*i+1] << 16) | ((uint32_t)blk[4*i+2] << 8) | blk[4*i+3];
        for (int i = 16; i < 64; i++) { uint32_t s0 = ROR32(w[i-15], 7) ^ ROR32(w[i-15], 18) ^ (w[i-15] >> 3), s1 = ROR32(w[i-2], 17) ^ ROR32(w[i-2], 19) ^ (w[i-2] >> 10); w[i] = w[i-16] + s0 + w[i-7] + s1; }
        uint32_t a = h[0], b = h[1], c = h[2], dd = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) {
            uint32_t S1 = ROR32(e, 6) ^ ROR32(e, 11) ^ ROR32(e, 25), ch = (e & f) ^ (~e & g), t1 = hh + S1 + ch + K256[i] + w[i];
            uint32_t S0 = ROR32(a, 2) ^ ROR32(a, 13) ^ ROR32(a, 22), mj = (a & b) ^ (a & c) ^ (b & c), t2 = S0 + mj;
            hh = g; g = f; f = e; e = dd + t1; dd = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += dd; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    for (int i = 0; i < 8; i++) snprintf(out + 8 * i, 9, "%08x", h[i]);
}
static int rak_trusted(const Rak *r) {
    char hex[65], ln[200]; FILE *f = fopen("/usr/local/share/realone/apps.sha256", "r"); int ok = 0;
    if (!f) return 0;                      /* no list: nothing is trusted */
    sha256_hex(r->blob, r->blen, hex);
    while (!ok && fgets(ln, sizeof ln, f)) ok = strlen(ln) >= 64 && !strncmp(ln, hex, 64) && (ln[64] == ' ' || ln[64] == '\n' || !ln[64]);
    fclose(f); return ok;
}
static void apps_init(void) {
    const char *h = getenv("HOME"); char d[320], p[320], ln[120]; FILE *f; int used[NCELL] = {0}, saved[NCELL];
    nraks = rak_scan("/usr/local/share/realone/apps", raks, RAK_MAXAPPS);
    (void)d;   /* no code is ever loaded from Storage: apps are installed by root outside it */
    for (int i = 0; i < nraks; ) {      /* drop (and never load) every app that is not on the trusted list */
        if (rak_trusted(&raks[i])) { i++; continue; }
        rak_free(&raks[i]); memmove(&raks[i], &raks[i + 1], (size_t)(nraks - i - 1) * sizeof raks[0]); nraks--;
    }
    qsort(raks, (size_t)nraks, sizeof raks[0], rak_cmp);
    if (nraks > NCELL - 1) { for (int i = NCELL - 1; i < nraks; i++) rak_free(&raks[i]); nraks = NCELL - 1; }
    napp_n = 0;
    for (int i = 0; i < nraks; i++) {
        AppEnt *a = &apl[napp_n++]; a->rak = &raks[i];
        snprintf(a->id, sizeof a->id, "%s", raks[i].id); snprintf(a->name, sizeof a->name, "%s", raks[i].name); a->glyph = raks[i].glyph;
        memcpy(a->color, raks[i].color, sizeof a->color);
        cur_rak = &raks[i]; a->icon = H_png("assets/icon.png");
    }
    cur_rak = NULL;
    { AppEnt *a = &apl[napp_n++]; a->rak = NULL; snprintf(a->id, sizeof a->id, "power"); snprintf(a->name, sizeof a->name, "Power"); a->glyph = 11; a->color[0] = 0.95; a->color[1] = 0.27; a->color[2] = 0.23; }
    for (int k = 0; k < napp_n; k++) saved[k] = -1;
    lay_path(p, sizeof p); f = fopen(p, "r");
    if (f) {
        while (fgets(ln, sizeof ln, f)) {
            char id[40]; int c;
            if (sscanf(ln, "%39s %d", id, &c) != 2 || c < 0 || c >= NCELL) continue;
            for (int k = 0; k < napp_n; k++) if (!strcmp(apl[k].id, id) && !used[c]) { saved[k] = c; used[c] = 1; }
        }
        fclose(f);
    }
    for (int k = 0; k < napp_n; k++) {
        if (saved[k] < 0) { int c = 0; while (used[c]) c++; saved[k] = c; used[c] = 1; }
        app_cell[k] = saved[k];
    }
}
static void launch_app(int k) {
    AppEnt *a = &apl[k]; Rak *r = a->rak;
    if (!r) { go(S_POWER, 0); return; }
    if (!r->app) {
        rak_entry_fn e = rak_dlopen(r) ? (rak_entry_fn)rak_sym(r, "rak_entry") : NULL;
        const RakApp *ap = e ? e(&HOST) : NULL;
        if (!ap || ap->api != RAK_API) { set_toast("App failed to load"); return; }
        r->app = ap;
    }
    act = k; want_close = 0; app_anim = 0;
    if (A()->open) A()->open();
    go(S_APP, 0);
}
static void close_app(void) {
    const RakApp *a = A();
    if (a && a->close) a->close();
    act = -1; app_anim = 0; go(S_APPS, 0);
}
static void draw_app(void) {
    const RakApp *a = A();
    if (a && a->draw) { cairo_save(cr); a->draw(); cairo_restore(cr); } else fillbg(PANEL);
}
static void go_back(void) {
    if (cur == S_WIFI) go(S_CC, 0);
    else if (cur == S_WNET || cur == S_WJOIN) go(S_WIFI, 0);
    else if (cur == S_APP) close_app();
    else if (cur == S_POWER) go(S_APPS, 0);
    else if (cur == S_APPS) go(S_DIG, -6);
    else if (cur == S_CC) go(S_DIG, -4);
}

static void draw_screen(int s) {
    bg_dark = s == S_DIG ? 0.12 : 0.5;
    switch (s) {
    case S_DIG: draw_dig(); break;
    case S_APPS: draw_apps(); break;
    case S_CC: draw_cc(); break;
    case S_POWER: draw_power(); break;
    case S_APP: draw_app(); break;
    case S_WIFI: draw_wifi(); break;
    case S_WNET: draw_wnet(); break;
    case S_WJOIN: draw_wjoin(); break;
    case S_SPLASH: draw_splash(); break;
    }
}

static double layer_ty = 0;
static void layer(int scr, double tx) {
    cairo_save(cr); cairo_translate(cr, tx, layer_ty); draw_screen(scr); cairo_restore(cr);
}
static int side_px(void) { int m = ww < wh ? ww : wh; return m > MAXS ? MAXS : (m < 100 ? 100 : m); }
static int xerr(Display *d, XErrorEvent *e) { (void)d; (void)e; return 0; }
static void set_wall(int side) {
    if (last_wall == 1) return;
    last_wall = 1;
    double f = 0.45;
    XSetWindowBackground(dpy, win, ((unsigned long)(wp_avg[0] * f * 255) << 16) | ((unsigned long)(wp_avg[1] * f * 255) << 8) | (unsigned long)(wp_avg[2] * f * 255));
    int mx = (ww - side) / 2, my = (wh - side) / 2;
    if (mx > 0) { XClearArea(dpy, win, 0, 0, mx, wh, False); XClearArea(dpy, win, mx + side, 0, 0, wh, False); }
    if (my > 0) { XClearArea(dpy, win, 0, 0, ww, my, False); XClearArea(dpy, win, 0, my + side, ww, 0, False); }
}
static long frame_us(void) { long v = (long)(rend_ms * 1500); return v < FRAME_US ? FRAME_US : v > 150000 ? 150000 : v; }
static void render(void) {
    long long t_start = now_us();
    int side = side_px(); set_wall(side); build_cache(side);
    sc = side / CW; ox = (ww - side) / 2; oy = (wh - side) / 2;
    cairo_save(cr); cairo_scale(cr, sc, sc);
    double p = prev_scr >= 0 ? (g_now - tr_t0) / (double)TR_MS : 1;
    if (prev_scr >= 0 && p >= 1) prev_scr = -1;
    if (prev_scr < 0) {
        double fe = tr_kind == 0 ? ease((g_now - scr_t0) / 160.0) : 1;
        if (fe < 1) { cairo_push_group(cr); layer_ty = 10 * (1 - fe); layer(cur, 0); layer_ty = 0; cairo_pop_group_to_source(cr); cairo_paint_with_alpha(cr, fe); }
        else layer(cur, 0);
    } else {
        double e = ease(p);
        if (tr_kind == 6) { layer(prev_scr, 0); layer(cur, -400 * (1 - e)); }
        else if (tr_kind == -6) { layer(cur, 0); layer(prev_scr, -400 * e); }
        else if (tr_kind == 4) { layer(prev_scr, 0); layer_ty = -400 * (1 - e); layer(cur, 0); layer_ty = 0; }
        else if (tr_kind == -4) { layer(cur, 0); layer_ty = -400 * e; layer(prev_scr, 0); layer_ty = 0; }
        else layer(cur, 0);
    }
    if (g_now - toast_t < 1800) {
        double ta = g_now - toast_t > 1400 ? (1800 - (g_now - toast_t)) / 400.0 : 1;
        col(0, 0, 0, 0.85 * ta); rr(40, 342, 320, 36, 18); cairo_fill(cr);
        col(1, 1, 1, ta); T(toast_buf, 200, 365, 13, 1, 0);
    }
    cairo_restore(cr);
    cairo_surface_flush(surf);
    XPutImage(dpy, backbuf, gc, ximg, 0, 0, 0, 0, side, side);                       /* upload off-screen: never visible half-drawn */
    XCopyArea(dpy, backbuf, win, gc, 0, 0, side, side, (int)ox, (int)oy); XFlush(dpy); /* one fast server-side copy to the screen */
    rend_ms = 0.7 * rend_ms + 0.3 * ((now_us() - t_start) / 1000.0);
}
static void make_surface(void) {
    if (cr) cairo_destroy(cr);
    if (surf) cairo_surface_destroy(surf);
    if (ximg) { ximg->data = NULL; XDestroyImage(ximg); ximg = NULL; }
    if (backbuf) { XFreePixmap(dpy, backbuf); backbuf = 0; }
    int side = side_px();
    surf = cairo_image_surface_create(CAIRO_FORMAT_RGB24, side, side); cr = cairo_create(surf);
    cairo_set_antialias(cr, CAIRO_ANTIALIAS_FAST);
    int s = DefaultScreen(dpy);
    ximg = XCreateImage(dpy, DefaultVisual(dpy, s), DefaultDepth(dpy, s), ZPixmap, 0,
        (char *)cairo_image_surface_get_data(surf), side, side, 32, cairo_image_surface_get_stride(surf));
    backbuf = XCreatePixmap(dpy, DefaultRootWindow(dpy), side, side, DefaultDepth(dpy, s));
    last_wall = -1;
}

static void app_drop(void) {
    int k = app_drag, c = cell_near(apx[k], apy[k]);
    for (int j = 0; j < napp; j++) if (j != k && app_cell[j] == c) app_cell[j] = app_from;
    app_cell[k] = c; app_drag = -1; app_press = -1; apps_place(); save_cfg();
}
static void cc_toggle(int i) {
    cc_pop_i = i; cc_pop_t = g_now;
    switch (i) {
    case 0: torch_on = !torch_on; hook("torch", torch_on ? "on" : "off"); break;
    case 1: wifi_on = !wifi_on; hook("wifi", wifi_on ? "on" : "off"); break;
    case 2:
        airplane_on = !airplane_on;
        if (airplane_on) { wifi_before = wifi_on; wifi_on = 0; }
        hook("airplane", airplane_on ? "on" : "off");
        if (!airplane_on && wifi_before) { wifi_on = 1; hook("wifi", "on"); }
        break;
    }
}
/* ---------------- Wi-Fi input ---------------- */
static void gesture(double dx, double dy) {
    int horiz = fabs(dx) > fabs(dy);
    if (is_face(cur)) { if (horiz) { if (dx > 0) go(S_APPS, 6); } else if (dy > 0) go(S_CC, 4); }
    else if (cur == S_CC) { if (!horiz && dy < 0) go(S_DIG, -4); }
    else if (cur == S_APPS) { if (horiz && dx < 0) go(S_DIG, -6); }
    else if (cur == S_WIFI) {
        if (horiz) { if (dx > 0) go_back(); }
        else { int np = wf_page + (dy < 0 ? 1 : -1); if (np >= 0 && np < wf_pages()) wf_page = np; }
    }
    else if (cur == S_APP) { const RakApp *a = A(); int used = a && a->gesture && a->gesture(dx, dy); if (!used && horiz && dx > 0) go_back(); }
    else if (horiz && dx > 0) go_back();
}
static void wifi_open(void) {
    wf_find_if(); wf_page = 0; wf_press = -1;
    wf_scan_t = -100000; wf_stat_t = -100000;
    go(S_WIFI, 0);
}
static void wj_reset(void) { memset(wj_pass, 0, sizeof wj_pass); wj_user[0] = 0; wj_layer = 0; wj_show = 0; wj_eap = 0; }
static void wj_open(const WNet *w) {
    wj_reset(); snprintf(wj_ssid, sizeof wj_ssid, "%s", w->ssid);
    wj_sec = w->sec; wj_trans = w->trans; wj_hidden = 0; wj_focus = wj_sec == SEC_EAP ? 1 : 2;
    go(S_WJOIN, 0);
}
static void wj_open_hidden(void) {
    wj_reset(); wj_ssid[0] = 0; wj_sec = SEC_WPA; wj_trans = 1; wj_hidden = 1; wj_focus = 0;
    go(S_WJOIN, 0);
}
static void wj_fix_focus(void) {
    int ids[3], n = wj_fields(ids), ok = 0;
    for (int i = 0; i < n; i++) if (ids[i] == wj_focus) ok = 1;
    if (!ok) wj_focus = n ? ids[n - 1] : 0;
}
static void wj_go(void) {
    int pl = (int)strlen(wj_pass), hex = pl == 64;
    if (wf_busy) { set_toast("Busy, try again"); return; }
    for (int i = 0; hex && i < pl; i++) if (!is_hex(wj_pass[i])) hex = 0;
    if (wj_hidden && !wj_ssid[0]) { set_toast("Enter the network name"); return; }
    if (wj_sec == SEC_WPA && !(pl >= 8 && pl <= 63) && !hex) { set_toast("Password: 8 to 63 characters"); return; }
    if (wj_sec == SEC_WPA3 && !(pl >= 8 && pl <= 63)) { set_toast("Password: 8 to 63 characters"); return; }
    if (wj_sec == SEC_EAP && (!wj_user[0] || !pl)) { set_toast("Enter identity and password"); return; }
    wa.sec = wj_sec; wa.hidden = wj_hidden; wa.eap = wj_eap; wa.trans = wj_trans; wa.id = -1;
    snprintf(wa.ssid, sizeof wa.ssid, "%s", wj_ssid); snprintf(wa.user, sizeof wa.user, "%s", wj_user); snprintf(wa.pass, sizeof wa.pass, "%s", wj_pass);
    wf_op(WO_CONN);
    go(S_WIFI, 0);
}
static void wj_key(XKeyEvent *ke) {
    char b[16]; KeySym k = 0; int n = XLookupString(ke, b, sizeof b, &k, NULL);
    if (k == XK_Escape || k == KS_VOLUP || k == XK_Prior) go_back();
    else if (k == XK_Return || k == XK_KP_Enter) wj_go();
    else if (k == XK_BackSpace) wj_bksp();
    else if (k == XK_Tab) {
        int ids[3], nf = wj_fields(ids), at = 0;
        for (int i = 0; i < nf; i++) if (ids[i] == wj_focus) at = i;
        if (nf) wj_focus = ids[(at + 1) % nf];
    }
    else for (int i = 0; i < n; i++) wj_type((unsigned char)b[i]);
}
static void wj_tap(double x, double y) {
    int ids[3], nf = wj_fields(ids), ch[3], nc = wj_chips(ch);
    if (y < 56) { tap_hit = 1; go_back(); return; }
    for (int i = 0; i < nf; i++) { double fy = 58 + 34.0 * i; if (y >= fy && y <= fy + 30) { tap_hit = 1; wj_focus = ids[i]; return; } }
    {
        double cy = 58 + 34.0 * nf;
        if (nc && y >= cy - 2 && y <= cy + 30 && x >= 14) {
            int i = (int)((x - 14) / 124);
            if (i < nc) {
                tap_hit = 1;
                if (ch[i] == 0) { wj_sec = wj_sec == SEC_OPEN ? SEC_WPA : wj_sec == SEC_WPA ? SEC_WPA3 : wj_sec == SEC_WPA3 ? SEC_EAP : SEC_OPEN; wj_trans = wj_sec == SEC_WPA; wj_fix_focus(); }
                else if (ch[i] == 1) wj_eap = !wj_eap;
                else wj_show = !wj_show;
            }
            return;
        }
    }
    wk_build();
    for (int i = 0; i < wk_n; i++) {
        WK *k = &wk[i];
        if (x < k->x - 2 || x > k->x + k->w + 2 || y < k->y - 2 || y > k->y + k->h + 2) continue;
        tap_hit = 1;
        if (k->code == K_SHIFT) wj_layer = wj_layer == 0 ? 1 : wj_layer == 1 ? 0 : wj_layer == 2 ? 3 : 2;
        else if (k->code == K_BKSP) wj_bksp();
        else if (k->code == K_SYM) wj_layer = wj_layer < 2 ? 2 : 0;
        else if (k->code == K_JOIN) wj_go();
        else { wj_type(k->code); if (wj_layer == 1) wj_layer = 0; }
        return;
    }
}
static void wf_press_set(double x, double y) {
    int cnt = wf_cnt();
    for (int i = 0; i < WF_PP; i++) {
        int k = wf_page * WF_PP + i; double ry = WF_Y0 + WF_P * i;
        if (k < cnt && y >= ry - 3 && y <= ry + 49 && x > 16 && x < 384) wf_press = k;
    }
}
static void wf_hold_open(void) {
    WNet w;
    if (wf_press >= 0 && wf_get(wf_press, &w) && (w.cur || w.saved)) { snprintf(wnet_ssid, sizeof wnet_ssid, "%s", w.ssid); go(S_WNET, 0); }
    wf_press = -1;
}
static void wf_tap(double x, double y) {
    WNet w; int cnt;
    if (hit(x, y, 342, 36, 34)) {
        tap_hit = 1; wifi_on = !wifi_on; hook("wifi", wifi_on ? "on" : "off");
        if (!wifi_on) { pthread_mutex_lock(&wfmu); wfn_n = 0; pthread_mutex_unlock(&wfmu); wf_cur[0] = 0; wf_ip[0] = 0; wf_msg[0] = 0; wf_err = 0; }
        wf_scan_t = g_now - 28500; wf_stat_t = g_now; wf_page = 0;
        return;
    }
    if (y < 56) { tap_hit = 1; go_back(); return; }
    if (!wifi_on) return;
    if (hit(x, y, 360, 81, 26)) { tap_hit = 1; if (!wf_busy) { wf_msg[0] = 0; wf_err = 0; wf_op(WO_SCAN); } return; }
    cnt = wf_cnt();
    for (int i = 0; i < WF_PP; i++) {
        int k = wf_page * WF_PP + i; double ry = WF_Y0 + WF_P * i;
        if (k > cnt || y < ry - 3 || y > ry + 49) continue;
        tap_hit = 1;
        if (k == cnt) { wj_open_hidden(); return; }
        if (!wf_get(k, &w)) return;
        if (w.cur) { snprintf(wnet_ssid, sizeof wnet_ssid, "%s", w.ssid); go(S_WNET, 0); return; }
        if (w.sec == SEC_WEP) { set_toast("WEP is insecure, not supported"); return; }
        if (w.saved && w.id >= 0) {
            if (wf_busy) { set_toast("Busy, try again"); return; }
            snprintf(wa.ssid, sizeof wa.ssid, "%s", w.ssid); wa.id = w.id; wa.sec = w.sec; wa.hidden = 0; wa.pass[0] = 0;
            wf_op(WO_SAVED); return;
        }
        if (w.sec == SEC_OPEN) {
            if (wf_busy) { set_toast("Busy, try again"); return; }
            snprintf(wa.ssid, sizeof wa.ssid, "%s", w.ssid); wa.id = -1; wa.sec = SEC_OPEN; wa.hidden = 0; wa.trans = 0; wa.pass[0] = 0;
            wf_op(WO_CONN); return;
        }
        wj_open(&w); return;
    }
}
static void wnet_tap(double x, double y) {
    WNet w; int k = wf_idx(wnet_ssid); (void)x;
    if (y < 56) { tap_hit = 1; go_back(); return; }
    if (k < 0 || !wf_get(k, &w)) return;
    if (y >= 262 && y <= 306 && (w.cur || (w.saved && w.sig > 0))) {
        tap_hit = 1;
        if (wf_busy) { set_toast("Busy, try again"); return; }
        snprintf(wa.ssid, sizeof wa.ssid, "%s", w.ssid); wa.id = w.id; wa.sec = w.sec; wa.hidden = 0; wa.pass[0] = 0;
        wf_op(w.cur ? WO_DISC : WO_SAVED); go(S_WIFI, 0); return;
    }
    if (y >= 316 && y <= 360 && w.saved) {
        tap_hit = 1;
        if (wf_busy) { set_toast("Busy, try again"); return; }
        wa.id = w.id; wf_op(WO_FORGET); go(S_WIFI, 0); return;
    }
}
static void tap(double x, double y) {
    if (cur == S_WIFI) { wf_tap(x, y); return; }
    if (cur == S_WNET) { wnet_tap(x, y); return; }
    if (cur == S_WJOIN) { wj_tap(x, y); return; }
    if (cur == S_APP) { const RakApp *ap = A(); if (ap && ap->tap && ap->tap(x, y)) tap_hit = 1; return; }
    if (cur == S_POWER) {
        if (y < 56) { tap_hit = 1; go_back(); return; }
        for (int i = 0; i < 2; i++) {
            double ry = pw_y(i);
            if (y < ry || y > ry + 104) continue;
            tap_hit = 1;
            if (pw_arm == i && g_now - pw_t < 4000) {
                pw_arm = -1;
                hook("power", i == 0 ? "off" : "reboot");
            } else { pw_arm = i; pw_t = g_now; }
            return;
        }
        return;
    }
    if (cur == S_CC) {
        for (int i = 0; i < 3; i++) { double cx, cy; cc_btn(i, &cx, &cy); if (hit(x, y, cx, cy, 40)) { tap_hit = 1; cc_toggle(i); return; } }
        if (y > 372) go(S_DIG, -4);
        return;
    }
    if (cur == S_APPS) {
        if (y < 60) { tap_hit = 1; go(S_DIG, -6); return; }
        for (int k = 0; k < napp; k++) if (hit(x, y, apx[k], apy[k] + 6, 44)) { tap_hit = 1; launch_app(k); break; }
    }
}

static void onsig(int sg) { (void)sg; running = 0; }
int main(int argc, char **argv) {
    int argsize = 0;
    now_ms();
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, onsig); signal(SIGTERM, onsig); signal(SIGHUP, onsig);
    load_cfg(); apps_init(); apps_place(); scan_walls(); load_names();
    for (int i = 1; i < argc; i++) { int v = atoi(argv[i]); if (v >= 300 && v <= 2000) { ww = wh = v; argsize = 1; } }
    if (!getenv("DISPLAY")) setenv("DISPLAY", ":0", 1);
    for (int i = 0; i < 60 && !(dpy = XOpenDisplay(NULL)); i++) usleep(100000);
    if (!dpy) { fprintf(stderr, "Could not open the X11 display.\n"); return 1; }
    XSetErrorHandler(xerr);
    load_wall();
    int s = DefaultScreen(dpy), scrw = DisplayWidth(dpy, s), scrh = DisplayHeight(dpy, s);
    if (!argsize) { ww = scrw; wh = scrh; }
    win = XCreateSimpleWindow(dpy, RootWindow(dpy, s), (scrw - ww) / 2 > 0 ? (scrw - ww) / 2 : 0, (scrh - wh) / 2 > 0 ? (scrh - wh) / 2 : 0, ww, wh, 0, 0, BlackPixel(dpy, s));
    XStoreName(dpy, win, "RealOneOS");
    XSelectInput(dpy, win, ExposureMask | ButtonPressMask | ButtonReleaseMask | KeyPressMask | StructureNotifyMask | ButtonMotionMask);
    Atom wmdel = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &wmdel, 1);
    gc = XCreateGC(dpy, win, 0, NULL); XSetGraphicsExposures(dpy, gc, False);
    XDefineCursor(dpy, win, XCreateFontCursor(dpy, XC_left_ptr));
    XMapWindow(dpy, win); make_surface();
    { time_t t0 = time(NULL); localtime_r(&t0, &tmv); }
    g_now = now_ms(); splash_t0 = g_now; S = G; sync_rs(); render(); apply_vol(); play_snd("boot.mp3");
    pthread_t th;
    pthread_create(&th, 0, poller, 0); pthread_detach(th);
    int down = 0, done = 0, redraw = 1, last_min = tmv.tm_min; double px = 0, py = 0;
    long long next_us = 0; long last_draw = g_now;
    int fd = ConnectionNumber(dpy);
    last_input = now_ms();
    while (running) {
        g_now = now_ms();
        { time_t t0 = time(NULL); localtime_r(&t0, &tmv); }
        pthread_mutex_lock(&mu); S = G; pthread_mutex_unlock(&mu); sync_rs();
        while (XPending(dpy)) {
            XEvent ev; XNextEvent(dpy, &ev); redraw = 1;
            if (ev.type == KeyPress && cur == S_WJOIN) wj_key(&ev.xkey);
            else if (ev.type == KeyPress) {
                char ub[16]; KeySym k = 0; int kn = XLookupString(&ev.xkey, ub, (int)sizeof ub - 1, &k, NULL);
                const RakApp *ap = cur == S_APP && prev_scr < 0 ? A() : NULL;
                int back = k == KS_VOLUP || k == XK_Prior || k == XK_Escape || k == XK_Home;
                ub[kn > 0 ? kn : 0] = 0;
                if (ap && ap->key && ap->key(k, ub)) {}
                else if (back) go_back();
                else if (k == XK_q) running = 0;
            } else if (ev.type == ButtonPress && ev.xbutton.button == 3) {
                const RakApp *ap = cur == S_APP && prev_scr < 0 ? A() : NULL;
                if (!(ap && ap->key && ap->key(XK_Escape, ""))) go_back();
            } else if (ev.type == ButtonPress && ev.xbutton.button == 1) {
                down = 1; done = 0; px = (ev.xbutton.x - ox) / sc; py = (ev.xbutton.y - oy) / sc;
                app_press = -1; app_drag = -1; cc_drag = -1; app_t0 = g_now;
                app_cap = 0; app_hold = 0;
                if (cur == S_APP && prev_scr < 0) { const RakApp *ap = A(); if (ap && ap->press) app_cap = ap->press(px, py) != 0; app_hold = !app_cap; }
                wf_hold = 0; wf_press = -1;
                if (cur == S_CC && prev_scr < 0) { double wx, wy; cc_btn(1, &wx, &wy); wf_hold = hit(px, py, wx, wy, 40); }
                if (cur == S_WIFI && prev_scr < 0 && wifi_on) wf_press_set(px, py);
                if (cur == S_APPS && prev_scr < 0) for (int k = 0; k < napp; k++) if (hit(px, py, apx[k], apy[k] + 6, 44)) app_press = k;
                if (cur == S_CC && prev_scr < 0)
                    for (int i = 0; i < 2; i++)
                        if (px >= SL_X - 10 && px <= SL_X + SL_W + 10 && fabs(py - SLY[i]) <= SL_H / 2 + 6) { cc_drag = i; cc_slide(i, px); }
            } else if (ev.type == ButtonRelease && ev.xbutton.button == 1 && down) {
                double lx = (ev.xbutton.x - ox) / sc, ly = (ev.xbutton.y - oy) / sc; down = 0;
                if (app_cap) { const RakApp *ap = A(); app_cap = 0; if (ap && ap->release) ap->release(lx, ly); }
                else if (app_drag >= 0) app_drop();
                else if (cc_drag >= 0) { cc_slide(cc_drag, lx); cc_apply(cc_drag); save_cfg(); cc_drag = -1; }
                else if (!done) {
                    if (fabs(lx - px) > 40 || fabs(ly - py) > 40) gesture(lx - px, ly - py);
                    else { int was = cur; tap_hit = 0; tap(lx, ly); if (tap_hit && was != S_SPLASH && g_now - last_click > 120) { last_click = g_now; play_snd("click.mp3"); } }
                }
                app_press = -1;
            } else if (ev.type == MotionNotify && down) {
                double lx = (ev.xmotion.x - ox) / sc, ly = (ev.xmotion.y - oy) / sc;
                if (app_cap) { const RakApp *ap = A(); if (ap && ap->move) ap->move(lx, ly); }
                else if (app_drag >= 0) {
                    apx[app_drag] = lx < 40 ? 40 : lx > 360 ? 360 : lx;
                    apy[app_drag] = ly < 90 ? 90 : ly > 340 ? 340 : ly;
                }
                else if (cc_drag >= 0) { cc_slide(cc_drag, lx); if (g_now - last_apply > 400) cc_apply(cc_drag); }
                else if (!done && (fabs(lx - px) > 40 || fabs(ly - py) > 40)) { gesture(lx - px, ly - py); done = 1; }
            } else if (ev.type == ConfigureNotify) {
                if (ev.xconfigure.width != ww || ev.xconfigure.height != wh) { ww = ev.xconfigure.width; wh = ev.xconfigure.height; make_surface(); }
            } else if (ev.type == ClientMessage && (Atom)ev.xclient.data.l[0] == wmdel) running = 0;
            if (ev.type == ButtonPress || ev.type == ButtonRelease || ev.type == KeyPress) last_input = g_now;
        }
        if (down && cur == S_CC && wf_hold && !done && g_now - app_t0 > 450) { wf_hold = 0; done = 1; redraw = 1; cc_pop_i = 1; cc_pop_t = g_now; play_snd("click.mp3"); wifi_open(); }
        if (down && cur == S_WIFI && wf_press >= 0 && !done && g_now - app_t0 > 600) { done = 1; redraw = 1; wf_hold_open(); }
        if (down && cur == S_APPS && app_press >= 0 && app_drag < 0 && !done && g_now - app_t0 > HOLD_MS) { app_drag = app_press; app_from = app_cell[app_drag]; }
        if (down && cur == S_APP && app_hold && !done && g_now - app_t0 > 600) {
            const RakApp *ap = A(); app_hold = 0; redraw = 1;
            if (ap && ap->hold && ap->hold(px, py)) done = 1;
        }
        if (want_close) { want_close = 0; if (cur == S_APP) close_app(); redraw = 1; }
        if (want_redraw) { want_redraw = 0; redraw = 1; }
        if (cur == S_SPLASH && g_now - splash_t0 > SPL_TOT) { go(S_DIG, 0); redraw = 1; }
        int active = prev_scr >= 0 || app_drag >= 0 || cc_drag >= 0 || g_now - scr_t0 < 360 || g_now - cc_pop_t < 240 || g_now - toast_t < 1900 || (cur == S_APP && app_anim) || cur == S_SPLASH || (cur == S_WIFI && wf_spin);
        if (wf_dirty) { wf_dirty = 0; redraw = 1; }
        if (cur == S_WIFI || cur == S_WNET) wf_tick();
        if (tmv.tm_min != last_min) { last_min = tmv.tm_min; redraw = 1; }
        if (cur == S_POWER && pw_arm >= 0 && g_now - pw_t > 4000) { pw_arm = -1; redraw = 1; }
        if (cur == S_APP) { const RakApp *ap = A(); if (ap && ap->redraw_ms > 0 && g_now - last_draw >= ap->redraw_ms) redraw = 1; }
        if (cur == S_CC && g_now - last_draw > 10000) redraw = 1;
        if (redraw || active) {
            render(); last_draw = g_now; redraw = 0;
        }
        if (active || down || want_redraw) {
            long long t = now_us(); next_us += frame_us();
            if (next_us < t) next_us = t;
            if (next_us - t > 0) usleep((useconds_t)(next_us - t));
        } else {
            long idle = (60 - tmv.tm_sec) * 1000L + 30;
            if (cur == S_APP) { const RakApp *ap = A(); if (ap && ap->redraw_ms > 0 && ap->redraw_ms < idle) idle = ap->redraw_ms; }
            else if (cur == S_CC && idle > 10000) idle = 10000;
            if (cur == S_WIFI || cur == S_WNET) idle = 700;
            struct timeval tv = {idle / 1000, (idle % 1000) * 1000};
            fd_set fs; FD_ZERO(&fs); FD_SET(fd, &fs);
            select(fd + 1, &fs, 0, 0, &tv);
            next_us = 0;
        }
    }
    XCloseDisplay(dpy);
    return 0;
}
