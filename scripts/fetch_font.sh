#!/usr/bin/env bash
# ============================================================================
# fetch_font.sh - Поставляет TTF-шрифт для MyOS (Этап 21)
# ============================================================================
# Порядок попыток:
#   1. Если $1 уже существует — ничего не делать.
#   2. Попытка скопировать DejaVu Sans из системных путей Linux.
#   3. Попытка скачать Liberation Sans с GitHub.
# ============================================================================

set -e

OUT="${1:-initrd_content/default.ttf}"

if [[ -s "$OUT" ]]; then
    echo "[fetch_font] $OUT уже присутствует — пропускаем"
    exit 0
fi

mkdir -p "$(dirname "$OUT")"

SYSTEM_CANDIDATES=(
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
    "/usr/share/fonts/TTF/DejaVuSans.ttf"
    "/usr/share/fonts/dejavu/DejaVuSans.ttf"
    "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf"
    "/usr/share/fonts/liberation/LiberationSans-Regular.ttf"
    "/usr/share/fonts/noto/NotoSans-Regular.ttf"
    "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf"
)

for f in "${SYSTEM_CANDIDATES[@]}"; do
    if [[ -s "$f" ]]; then
        cp "$f" "$OUT"
        echo "[fetch_font] Скопировано из $f -> $OUT"
        exit 0
    fi
done

# Фолбэк — скачиваем открытый шрифт (Liberation Sans)
URLS=(
    "https://github.com/liberationfonts/liberation-fonts/raw/main/src/LiberationSans-Regular.ttf"
    "https://raw.githubusercontent.com/googlefonts/roboto/main/src/hinted/Roboto-Regular.ttf"
)

for url in "${URLS[@]}"; do
    echo "[fetch_font] Скачиваем $url ..."
    if command -v wget >/dev/null 2>&1; then
        wget -q -O "$OUT" "$url" && [[ -s "$OUT" ]] && {
            echo "[fetch_font] Готово: $OUT"
            exit 0
        }
    elif command -v curl >/dev/null 2>&1; then
        curl -sL -o "$OUT" "$url" && [[ -s "$OUT" ]] && {
            echo "[fetch_font] Готово: $OUT"
            exit 0
        }
    fi
done

echo "[fetch_font] ВНИМАНИЕ: не удалось достать TTF-шрифт." >&2
echo "[fetch_font] Положите любой .ttf в $OUT вручную." >&2
exit 1
