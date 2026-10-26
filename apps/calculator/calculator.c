/* Calculator.RAK - key layout is read from assets/keys.txt */
#include "rakapi.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const RakHost *H;
#define MAXK 24
static struct { char l[8]; char k; int r, c, sp; } K[MAXK];
static int nk;
static char in[32] = "0"; static double acc_v = 0; static char op = 0; static int fresh = 0;

#define Y0 150.0
#define DY 43.0
#define KH 38.0

static void load_keys(void) {
    uint32_t n; const unsigned char *a = H->asset("assets/keys.txt", &n);
    char *t, *sv = NULL, *ln;
    nk = 0;
    if (!a) return;
    t = malloc(n + 1); if (!t) return;
    memcpy(t, a, n); t[n] = 0;
    for (ln = strtok_r(t, "\r\n", &sv); ln && nk < MAXK; ln = strtok_r(NULL, "\r\n", &sv)) {
        char l[8], k[4]; int r, c, sp;
        if (ln[0] == '#') continue;
        if (sscanf(ln, "%7s %3s %d %d %d", l, k, &r, &c, &sp) != 5) continue;
        snprintf(K[nk].l, sizeof K[nk].l, "%s", l); K[nk].k = k[0]; K[nk].r = r; K[nk].c = c; K[nk].sp = sp; nk++;
    }
    free(t);
}
static void rect(int i, double *x, double *y, double *w, double *h) {
    *x = 26 + 88.0 * K[i].c; *y = Y0 + DY * K[i].r; *w = 80 + 88.0 * (K[i].sp - 1); *h = KH;
}
static void fmt(double v) { snprintf(in, sizeof in, "%.10g", v); }
static void eval(void) {
    double b = atof(in), r = b;
    switch (op) {
    case '+': r = acc_v + b; break;
    case '-': r = acc_v - b; break;
    case '*': r = acc_v * b; break;
    case '/': if (b == 0) { snprintf(in, sizeof in, "Error"); acc_v = 0; op = 0; fresh = 1; return; } r = acc_v / b; break;
    }
    fmt(r); acc_v = r; fresh = 1;
}
static void press_key(char k) {
    int n = (int)strlen(in);
    if (k >= '0' && k <= '9') {
        if (fresh) { in[0] = 0; fresh = 0; n = 0; }
        if (!strcmp(in, "0")) { in[0] = 0; n = 0; }
        if (n < 12) { in[n] = k; in[n + 1] = 0; }
    } else if (k == '.') {
        if (fresh) { snprintf(in, sizeof in, "0"); fresh = 0; }
        if (!strchr(in, '.') && strlen(in) < 12) strcat(in, ".");
    } else if (k == 'C') {
        snprintf(in, sizeof in, "0"); acc_v = 0; op = 0; fresh = 0;
    } else if (k == '+' || k == '-' || k == '*' || k == '/') {
        if (op && !fresh) eval();
        acc_v = atof(in); op = k; fresh = 1;
    } else if (k == '=') {
        if (op) { if (!fresh) eval(); op = 0; fresh = 1; }
    }
}

static void on_open(void) { load_keys(); }
static void on_draw(void) {
    int len = (int)strlen(in);
    H->panel();
    H->color(1, 1, 1, 0.07); H->round_rect(16, 18, 368, 118, 22); H->fill();
    if (op) {
        char h[48]; snprintf(h, sizeof h, "%.10g %c", acc_v, op == '*' ? 'x' : op);
        H->color(1, 1, 1, 0.55); H->text(h, 366, 52, 16, 0, RAK_RIGHT);
    }
    H->color(1, 1, 1, 1); H->text(in, 366, 112, len <= 7 ? 58 : len <= 9 ? 46 : len <= 12 ? 36 : 28, 1, RAK_RIGHT);
    if (!nk) { H->color(1, 1, 1, 0.6); H->text("assets/keys.txt missing", 200, 250, 14, 0, RAK_CENTER); return; }
    for (int i = 0; i < nk; i++) {
        double x, y, w, h; char k = K[i].k; int isop = k == '+' || k == '-' || k == '*' || k == '/' || k == '=';
        int act = op && fresh && k == op;
        rect(i, &x, &y, &w, &h);
        if (act) H->color(1, 1, 1, 0.95); else if (isop) H->accent(1); else H->color(1, 1, 1, 0.10);
        H->round_rect(x, y, w, h, h / 2); H->fill();
        if (act) H->accent(1); else H->color(1, 1, 1, 1);
        H->text(K[i].l, x + w / 2, y + 26, 19, 1, RAK_CENTER);
    }
}
static int on_tap(double px, double py) {
    for (int i = 0; i < nk; i++) {
        double x, y, w, h; rect(i, &x, &y, &w, &h);
        if (px >= x - 4 && px <= x + w + 4 && py >= y - 2 && py <= y + h + 2) { press_key(K[i].k); H->redraw(); return 1; }
    }
    return 0;
}
static int on_key(unsigned long ks, const char *u) {
    char c = 0;
    if (ks >= 0xffb0 && ks <= 0xffb9) c = (char)('0' + (ks - 0xffb0));       /* keypad digits */
    else if (ks == 0xffab) c = '+'; else if (ks == 0xffad) c = '-'; else if (ks == 0xffaa) c = '*';
    else if (ks == 0xffaf) c = '/'; else if (ks == 0xffae) c = '.';
    else if (ks == 0xff0d || ks == 0xff8d) c = '=';                         /* Return / KP_Enter */
    else if (ks == 0xff08) c = 'C';                                         /* BackSpace */
    else if (u[0] && !u[1] && strchr("0123456789+-*/.=", u[0])) c = u[0];
    else if (u[0] == 'c' || u[0] == 'C') c = 'C';
    if (!c) return 0;
    press_key(c); H->redraw(); return 1;
}

static RakApp app = { .api = RAK_API, .size = sizeof(RakApp), .open = on_open, .draw = on_draw, .tap = on_tap, .key = on_key };
RAK_EXPORT const RakApp *rak_entry(const RakHost *h) { if (h->api < RAK_API) return 0; H = h; return &app; }
