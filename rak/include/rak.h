/* rak.h - RAK package loader (header-only). Used by RealOneOS and by mock_host.c.
 *
 * A .RAK file is a plain ZIP, STORED (no compression):   zip -0 -X -r Name.RAK manifest.txt app.so assets
 *   manifest.txt   key=value lines:  id=  name=  glyph=<OS glyph id>  color=<RRGGBB>  order=<sort key>  api=1
 *   app.so         the app (exports rak_entry, see rakapi.h)
 *   assets/...     anything the app wants (png, txt, ...), read with host->asset()/host->png()
 * app.so is never written to disk: it is copied into a memfd and dlopen()ed from there.
 */
#ifndef RAK_H
#define RAK_H
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <dlfcn.h>
#include <unistd.h>
#include <sys/mman.h>
#include "rakapi.h"

#define RAK_MAXAPPS 16

typedef struct { char name[96]; uint32_t off, size; } RakEntry;

typedef struct Rak {
    char path[320];
    char id[40], name[40];
    int glyph, order;
    double color[3];
    unsigned char *blob; size_t blen;       /* whole .RAK file in memory */
    RakEntry *ent; int nent;
    void *lib; int fd; const RakApp *app;    /* filled by rak_dlopen() and the host */
} Rak;

static uint32_t rak_u16(const unsigned char *p) { return p[0] | (p[1] << 8); }
static uint32_t rak_u32(const unsigned char *p) { return p[0] | (p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

static const unsigned char *rak_get(Rak *r, const char *name, uint32_t *size) {
    if (size) *size = 0;
    for (int i = 0; i < r->nent; i++)
        if (!strcmp(r->ent[i].name, name)) { if (size) *size = r->ent[i].size; return r->blob + r->ent[i].off; }
    return NULL;
}

static void rak_free(Rak *r) {
    if (r->lib) dlclose(r->lib);
    if (r->fd > 0) close(r->fd);
    free(r->blob); free(r->ent);
    memset(r, 0, sizeof *r);
}

/* reads the zip central directory; only STORED entries are accepted */
static int rak_index(Rak *r) {
    const unsigned char *b = r->blob; size_t n = r->blen, e = 0;
    if (n < 22) return 0;
    for (size_t i = n - 22; ; i--) { if (rak_u32(b + i) == 0x06054b50) { e = i; break; } if (i == 0 || n - i > 65557) return 0; }
    int cnt = (int)rak_u16(b + e + 10); size_t cd = rak_u32(b + e + 16);
    if (cnt < 1 || cnt > 512 || cd >= n) return 0;
    r->ent = calloc((size_t)cnt, sizeof(RakEntry));
    if (!r->ent) return 0;
    for (int i = 0; i < cnt; i++) {
        if (cd + 46 > n || rak_u32(b + cd) != 0x02014b50) return 0;
        uint32_t method = rak_u16(b + cd + 10), csz = rak_u32(b + cd + 20), usz = rak_u32(b + cd + 24);
        uint32_t nl = rak_u16(b + cd + 28), xl = rak_u16(b + cd + 30), cl = rak_u16(b + cd + 32), lo = rak_u32(b + cd + 42);
        if (cd + 46 + nl > n || nl >= sizeof r->ent[0].name) return 0;
        if ((size_t)lo + 30 > n || rak_u32(b + lo) != 0x04034b50) return 0;
        size_t data = (size_t)lo + 30 + rak_u16(b + lo + 26) + rak_u16(b + lo + 28);
        if (nl && b[cd + 46 + nl - 1] != '/') {               /* skip directory entries */
            if (method != 0 || csz != usz) return 0;           /* compressed: not supported, repack with zip -0 */
            if (data + usz > n) return 0;
            RakEntry *x = &r->ent[r->nent++];
            memcpy(x->name, b + cd + 46, nl); x->name[nl] = 0; x->off = (uint32_t)data; x->size = usz;
        }
        cd += 46 + nl + xl + cl;
    }
    return 1;
}

static void rak_manifest(Rak *r, const char *fallback) {
    uint32_t n; const unsigned char *m = rak_get(r, "manifest.txt", &n);
    char *t = m ? malloc(n + 1) : NULL, *sv = NULL, *ln;
    snprintf(r->id, sizeof r->id, "%.39s", fallback); snprintf(r->name, sizeof r->name, "%.39s", fallback);
    r->glyph = 5; r->order = 1000; r->color[0] = r->color[1] = r->color[2] = 0.5;
    if (!t) return;
    memcpy(t, m, n); t[n] = 0;
    for (ln = strtok_r(t, "\r\n", &sv); ln; ln = strtok_r(NULL, "\r\n", &sv)) {
        char *v = strchr(ln, '='); if (!v || ln[0] == '#') continue;
        *v++ = 0;
        if (!strcmp(ln, "id")) { snprintf(r->id, sizeof r->id, "%.39s", v); for (char *c = r->id; *c; c++) if (*c == '/' || *c == '.') *c = '_'; }
        else if (!strcmp(ln, "name")) snprintf(r->name, sizeof r->name, "%.39s", v);
        else if (!strcmp(ln, "glyph")) r->glyph = atoi(v);
        else if (!strcmp(ln, "order")) r->order = atoi(v);
        else if (!strcmp(ln, "color")) { unsigned c = 0; if (sscanf(v, "%x", &c) == 1) { r->color[0] = ((c >> 16) & 255) / 255.0; r->color[1] = ((c >> 8) & 255) / 255.0; r->color[2] = (c & 255) / 255.0; } }
    }
    free(t);
}

static int rak_load_file(const char *path, const char *base, Rak *r) {
    FILE *f = fopen(path, "rb"); long n;
    memset(r, 0, sizeof *r);
    if (!f) return 0;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n < 22 || n > 32L * 1024 * 1024) { fclose(f); return 0; }
    r->blob = malloc((size_t)n);
    if (!r->blob || fread(r->blob, 1, (size_t)n, f) != (size_t)n) { fclose(f); rak_free(r); return 0; }
    fclose(f); r->blen = (size_t)n;
    snprintf(r->path, sizeof r->path, "%s", path);
    if (!rak_index(r) || !rak_get(r, "app.so", NULL)) { rak_free(r); return 0; }
    rak_manifest(r, base);
    return 1;
}

static int rak_cmp(const void *a, const void *b) {
    const Rak *x = a, *y = b;
    return x->order != y->order ? x->order - y->order : strcasecmp(x->name, y->name);
}

/* every *.RAK in dir; returns how many were added to out[] (bad packages are skipped). Sorted by order, then name. */
static int rak_scan(const char *dir, Rak *out, int max) {
    DIR *d = opendir(dir); struct dirent *e; int n = 0;
    if (!d) return 0;
    while (n < max && (e = readdir(d))) {
        const char *x = strrchr(e->d_name, '.'); char p[320], base[48];
        if (e->d_name[0] == '.' || !x || strcasecmp(x, ".RAK")) continue;
        if (snprintf(p, sizeof p, "%s/%s", dir, e->d_name) >= (int)sizeof p) continue;
        snprintf(base, sizeof base, "%.*s", (int)(x - e->d_name) > 39 ? 39 : (int)(x - e->d_name), e->d_name);
        if (rak_load_file(p, base, &out[n])) n++;
    }
    closedir(d);
    qsort(out, (size_t)n, sizeof out[0], rak_cmp);
    return n;
}

/* copies app.so into a memfd and dlopen()s it; nonzero on success */
static int rak_dlopen(Rak *r) {
    uint32_t n; const unsigned char *so = rak_get(r, "app.so", &n); char p[64]; int fd; size_t off = 0;
    if (r->lib) return 1;
    if (!so) return 0;
    fd = memfd_create("rak-app", 0);
    if (fd < 0) return 0;
    while (off < n) { ssize_t k = write(fd, so + off, n - off); if (k <= 0) { close(fd); return 0; } off += (size_t)k; }
    /* the fd stays open until rak_free(): dlopen() identifies libraries by path, so a reused fd number would return the wrong app */
    snprintf(p, sizeof p, "/proc/self/fd/%d", fd);
    r->lib = dlopen(p, RTLD_NOW | RTLD_LOCAL);
    if (!r->lib) { close(fd); return 0; }
    r->fd = fd;
    return 1;
}

static void *rak_sym(Rak *r, const char *name) { return r->lib ? dlsym(r->lib, name) : NULL; }
#endif
