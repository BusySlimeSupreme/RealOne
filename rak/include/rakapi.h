/* rakapi.h - the contract between RealOneOS and a RAK app (API v1).
 *
 * A RAK app is a shared library (app.so inside the .RAK) that exports ONE symbol:
 *     RAK_EXPORT const RakApp *rak_entry(const RakHost *host);
 * The OS calls it once after loading. The app keeps the host pointer, fills in
 * a static RakApp with its callbacks and returns it. Everything else is a callback.
 *
 * Drawing: the OS has already scaled the canvas, so apps draw in a fixed
 * 400 x 400 space (0,0 = top-left). Touch/mouse coordinates use the same space.
 * Threads: all callbacks run on the OS UI thread. Do not block.
 * Build an app: gcc -shared -fPIC -O2 -o app.so app.c   (no cairo needed unless
 * you call cairo yourself via host->cairo())
 */
#ifndef RAKAPI_H
#define RAKAPI_H
#include <stdint.h>
#include <time.h>
#ifndef CAIRO_H
typedef struct _cairo cairo_t;
typedef struct _cairo_surface cairo_surface_t;
#endif

#define RAK_API 1
#define RAK_EXPORT __attribute__((visibility("default")))

/* live system numbers, refreshed by the OS every ~3 s. Unknown = -1 (bat_temp: -999). */
typedef struct {
    int bat, charging, ncores;
    double cpu_temp, bat_temp, bat_v, cpu_pct, cpu_mhz;
    double ram_used, ram_tot, st_used, st_tot;   /* MB */
} RakSys;

/* ids for host->get / host->set */
enum {
    RAK_BRIGHTNESS = 0,   /* 0.04 .. 1.0, set() applies to hardware and saves */
    RAK_VOLUME     = 1,   /* 0 .. 1,      set() applies to hardware and saves */
    RAK_DEVMODE    = 2,   /* 0 or 1, saved */
    /* read-only, appended in API v1.1 */
    RAK_TORCH      = 3,   /* 0 or 1 */
    RAK_WIFI       = 4,   /* 0 or 1 */
    RAK_AIRPLANE   = 5,   /* 0 or 1 */
    RAK_RENDER_MS  = 6,   /* average time to draw one frame */
    RAK_FRAME_MS   = 7,   /* current frame interval */
    RAK_WIN_W      = 8,   /* window size in real pixels */
    RAK_WIN_H      = 9,
    RAK_SCALE      = 10   /* real pixels per canvas unit */
};

/* text alignment for host->text */
enum { RAK_LEFT = -1, RAK_CENTER = 0, RAK_RIGHT = 1 };

typedef struct RakHost {
    int api;                 /* RAK_API the OS implements */
    unsigned size;           /* sizeof(RakHost) in the OS: new fields are only ever appended */

    /* ---- drawing (paths are built, then fill()/stroke() paints them, like cairo) ---- */
    cairo_t *(*cairo)(void);                                   /* raw cairo context, valid only inside draw() */
    void (*color)(double r, double g, double b, double a);     /* set source color */
    void (*accent)(double a);                                  /* set source to the OS accent color */
    void (*round_rect)(double x, double y, double w, double h, double r);
    void (*circle)(double x, double y, double r);
    void (*fill)(void);                                        /* fill current path */
    void (*stroke)(double width);                              /* stroke current path */
    void (*text)(const char *s, double x, double y, double size, int bold, int align);
    void (*clear)(double r, double g, double b);               /* fill whole canvas */
    void (*panel)(void);                                       /* standard dark app background */
    void (*header)(const char *title);                         /* panel + back chevron + title (what Settings sub-pages use) */
    void (*glyph)(int id, int on);                             /* built-in OS icon glyph, drawn at origin (translate first) */

    /* ---- state ---- */
    const RakSys *sys;       /* read-only snapshot */
    const struct tm *now;    /* local time, updated every loop */
    long (*now_ms)(void);    /* monotonic milliseconds, for animation */

    /* ---- services ---- */
    void (*back)(void);                      /* leave the app (return to the Apps screen) */
    void (*toast)(const char *msg);          /* short message at the bottom */
    void (*redraw)(void);                    /* ask for one more draw() */
    void (*animate)(int on);                 /* 1 = draw() every frame until set to 0 */
    void (*click)(void);                     /* play the click sound */
    double (*get)(int id);                   /* RAK_BRIGHTNESS ... */
    void (*set)(int id, double v);
    void (*hw)(const char *name, const char *arg);   /* "torch","wifi","airplane","power" (arg "off"/"reboot") ... */

    /* ---- this app's own files (always refers to the app being called) ---- */
    const unsigned char *(*asset)(const char *name, uint32_t *size);   /* file inside the .RAK, NULL if missing */
    cairo_surface_t *(*png)(const char *name);                         /* PNG asset -> surface, caller destroys */
    const char *(*data_dir)(void);                                     /* writable folder ~/.realone/data/<id>/ (exists) */
    cairo_surface_t *(*load_image)(const char *path, int target);      /* png/jpg from disk, caller destroys */

    /* ---- wallpapers ---- */
    int  (*wall_count)(void);
    const char *(*wall_name)(int i);
    const char *(*wall_path)(int i);
    int  (*wall_current)(void);              /* index or -1 for none */
    void (*wall_set)(int i);                 /* -1 = none */
    void (*wall_rename)(int i, const char *name);   /* "" resets to file name */
    void (*wall_rescan)(void);

    /* ---- appended in API v1.1 (always present in RealOneOS) ---- */
    void (*image)(cairo_surface_t *s, double x, double y, double w, double h, double r);  /* cover-fit s into a rounded rect */
    void (*surface_free)(cairo_surface_t *s);                                              /* destroy a png()/load_image() surface */
} RakHost;

/* ---- what the app provides. Every callback is optional (NULL = ignored). ---- */
typedef struct RakApp {
    int api;                 /* set to RAK_API */
    unsigned size;           /* sizeof(RakApp) */

    void (*open)(void);      /* the app was launched */
    void (*close)(void);     /* the app is being left */
    void (*draw)(void);

    /* touch. press() may return 1 to CAPTURE the drag (sliders): then the OS sends
     * move()/release() and no tap/gesture/hold for this touch. */
    int  (*press)(double x, double y);
    void (*move)(double x, double y);
    void (*release)(double x, double y);
    int  (*tap)(double x, double y);                 /* return 1 if handled (OS plays the click) */
    int  (*hold)(double x, double y);                /* still pressed after 600 ms, not moved; 1 = handled */
    int  (*gesture)(double dx, double dy);           /* swipe > 40 px; return 1 if consumed.
                                                        Not consumed + swipe right => the OS closes the app */
    /* keyboard. keysym is an X11 KeySym; utf8 is the typed text (may be ""). Return 1 if consumed.
     * Escape / right-click / Home arrive here first; not consumed => the OS closes the app. */
    int  (*key)(unsigned long keysym, const char *utf8);

    int redraw_ms;           /* >0: OS redraws every N ms while the app is open (live numbers) */
} RakApp;

typedef const RakApp *(*rak_entry_fn)(const RakHost *);

#endif
