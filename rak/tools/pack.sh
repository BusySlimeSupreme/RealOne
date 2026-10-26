#!/bin/bash
# pack.sh [outdir]  -  builds apps/<name>/<name>.c into <Name>.RAK (stored zip: manifest.txt + app.so + assets/)
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; cd "$ROOT"
OUT="${1:-out/apps}"; case "$OUT" in /*) ;; *) OUT="$ROOT/$OUT";; esac
CF="${CAIRO_CFLAGS:-$(pkg-config --cflags cairo 2>/dev/null )}"
CL="${CAIRO_LIBS:-$(pkg-config --libs cairo 2>/dev/null || true)}"
mkdir -p "$OUT"
for d in apps/*/; do
  a="$(basename "$d")"; N="$(tr '[:lower:]' '[:upper:]' <<<"${a:0:1}")${a:1}"
  ( cd "$d"
    gcc -shared -fPIC -O2 -Wall -I"$ROOT/rak/include" $CF -o app.so "$a.c" $CL -lm -lpthread
    rm -f "$OUT/$N.RAK"
    zip -0 -X -q "$OUT/$N.RAK" manifest.txt app.so $(find assets -type f | sort)
    rm -f app.so )
  echo "packed $OUT/$N.RAK"
done
