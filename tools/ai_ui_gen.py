#!/usr/bin/env python3
# ============================================================================
# ai_ui_gen.py — Neural Desktop Builder для MyOS
# ----------------------------------------------------------------------------
# Читает макеты рабочего стола из папки ui_mockups/ и генерирует два файла:
#
#   kernel/gui/ui_theme.h    — цвета темы (фон, панель задач, кнопка Пуск)
#   kernel/gui/ui_layout.inc — тело apps_register_all(): вызовы desktop_add_icon
#                              (имя, символ, цвет, размер окна, порядок иконок).
#
# Три режима работы, переключаются автоматически:
#
#   1) ANTHROPIC_API_KEY задан ⇒ посылаем макет в Claude Vision,
#      парсим JSON-ответ, применяем к шаблону.
#
#   2) API-ключа нет, но есть pillow ⇒ эвристика: доминирующий цвет фона,
#      попытка найти прямоугольники панели задач.
#
#   3) Ни то, ни другое ⇒ "safe default": layout совпадает с тем, что
#      был в apps.c до Этапа 23. Билд ОС НИКОГДА не ломается из-за AI.
#
# Запускается автоматически из Makefile (target `ui-layout`).
# Также можно запустить вручную: python3 tools/ai_ui_gen.py
# ============================================================================

import os
import sys
import json
import base64
import pathlib
import textwrap
import re

# ----------------------------------------------------------------------------
# Константы. Эти имена должны совпадать с кодом apps.c — AI-генератор
# НЕ создаёт новые приложения, он лишь меняет порядок, цвет, символ
# и размеры окон существующих. Это принципиально: callback'и в apps.c
# static, добавить новое приложение без ручного кода нельзя.
# ----------------------------------------------------------------------------
KNOWN_APPS = {
    # slug      callbacks_tuple
    "Files": ("explorer_create","NULL",        "explorer_click","NULL"),
    "Calc":  ("calc_draw",     "NULL",         "calc_click",  "NULL"),
    "Clock": ("clock_create",  "clock_update", "NULL",        "NULL"),
    "Paint": ("paint_create",  "NULL",         "paint_click", "NULL"),
    "Term":  ("term_create",   "term_update",  "NULL",        "term_key"),
    "Info":  ("sysinfo_create","sysinfo_update","NULL",       "NULL"),
    "About": ("about_create",  "NULL",         "NULL",        "NULL"),
    "Notes": ("notes_create",  "notes_update", "notes_click", "notes_key"),
    "Panel": ("cpanel_draw",   "cpanel_update","cpanel_click","NULL"),
    "Embers":("embers_create", "embers_update","NULL",        "NULL"),
}

# Дефолтный layout (safe fallback) — идентичен текущему apps.c.
DEFAULT_LAYOUT = {
    "theme": {
        "desktop_top":    "0xFF143264",
        "desktop_bottom": "0xFF050C19",
        "taskbar_top":    "0xFF1E2232",
        "taskbar_bottom": "0xFF0E1020",
        "taskbar_edge":   "0xFF3A4058",
        "startbtn_color": "0xFF8EB1FF",
    },
    "icons": [
        {"slug": "Files", "label": "Файлы",   "symbol": "F",  "color": "0xFF4477BB", "win_title": "Проводник",            "win_w": 640, "win_h": 440},
        {"slug": "Calc",  "label": "Кальк",   "symbol": "=",  "color": "0xFF228833", "win_title": "Калькулятор",          "win_w": 260, "win_h": 240},
        {"slug": "Clock", "label": "Часы",    "symbol": "@",  "color": "0xFF335599", "win_title": "Системные часы",       "win_w": 280, "win_h": 180},
        {"slug": "Paint", "label": "Рисунок", "symbol": "~",  "color": "0xFFBB8822", "win_title": "Графический редактор", "win_w": 350, "win_h": 280},
        {"slug": "Term",  "label": "Термин.", "symbol": ">_", "color": "0xFF444466", "win_title": "Терминал",             "win_w": 460, "win_h": 340},
        {"slug": "Info",  "label": "Сведен.", "symbol": "i",  "color": "0xFFAA3333", "win_title": "Сведения о системе",   "win_w": 300, "win_h": 280},
        {"slug": "About", "label": "О сист.", "symbol": "?",  "color": "0xFF773399", "win_title": "О Amplify-OS",         "win_w": 280, "win_h": 280},
        {"slug": "Notes", "label": "Заметки", "symbol": "T",  "color": "0xFF556677", "win_title": "Заметки",              "win_w": 500, "win_h": 360},
        {"slug": "Panel", "label": "Панель",  "symbol": "#",  "color": "0xFF996644", "win_title": "Панель управления",    "win_w": 420, "win_h": 420},
        {"slug": "Embers","label": "ЦПУ",     "symbol": "%",  "color": "0xFFCC4444", "win_title": "Загрузка ядер ЦП",     "win_w": 360, "win_h": 280},
    ],
}

# Системный промпт для Claude Vision.
CLAUDE_SYSTEM_PROMPT = textwrap.dedent("""
    Ты — UX-аналитик. Тебе покажут скриншот или нарисованный макет
    десктопа операционной системы. Твоя задача — вернуть СТРОГИЙ JSON
    (и ничего кроме JSON!), описывающий тему цветов и порядок иконок
    приложений.

    Формат ответа:
    {
      "theme": {
        "desktop_top":    "0xAARRGGBB",
        "desktop_bottom": "0xAARRGGBB",
        "taskbar_top":    "0xAARRGGBB",
        "taskbar_bottom": "0xAARRGGBB",
        "taskbar_edge":   "0xAARRGGBB",
        "startbtn_color": "0xAARRGGBB"
      },
      "icons": [
        {"slug": "Calc", "symbol": "=", "color": "0xAARRGGBB"},
        ...
      ]
    }

    Допустимые значения slug (ровно 9 приложений, все должны быть в ответе,
    порядок — как видишь их слева-направо / сверху-вниз на макете):
      Files, Calc, Clock, Paint, Term, Info, About, Notes, Panel.

    Symbol — 1-2 ASCII-символа для иконки. Color — цвет плитки иконки.
    Alpha-канал всегда 0xFF. Никаких комментариев, только JSON.
""").strip()


# ============================================================================
# Утилиты
# ============================================================================

def log(msg):
    sys.stderr.write(f"[ai_ui_gen] {msg}\n")


def find_mockups(ui_dir: pathlib.Path):
    """Находит все изображения в ui_mockups/."""
    if not ui_dir.is_dir():
        return []
    exts = {".png", ".jpg", ".jpeg", ".bmp", ".webp"}
    return sorted([p for p in ui_dir.iterdir() if p.suffix.lower() in exts])


def hex_color_ok(s):
    """Проверка 0xAARRGGBB."""
    if not isinstance(s, str):
        return False
    return bool(re.fullmatch(r"0x[0-9A-Fa-f]{8}", s))


def validate_layout(layout):
    """Дополняет недостающие поля из DEFAULT_LAYOUT, проверяет формат.
    Возвращает (layout, errors)."""
    errors = []
    out = json.loads(json.dumps(DEFAULT_LAYOUT))  # deep copy

    # --- theme ---
    th_in = (layout or {}).get("theme", {}) or {}
    for k, default_val in DEFAULT_LAYOUT["theme"].items():
        v = th_in.get(k)
        if hex_color_ok(v):
            out["theme"][k] = v.upper().replace("0X", "0x")
        elif v is not None:
            errors.append(f"theme.{k}={v!r} не hex — оставляю дефолт")

    # --- icons ---
    icons_in = (layout or {}).get("icons", []) or []
    # Индексируем входные иконки по slug. Порядок берём из input (если slug валиден).
    seen_slugs = set()
    ordered = []
    for ic in icons_in:
        slug = ic.get("slug") if isinstance(ic, dict) else None
        if slug in KNOWN_APPS and slug not in seen_slugs:
            seen_slugs.add(slug)
            # Берём дефолт для этого приложения и накладываем пользовательские поля.
            base = next((x for x in DEFAULT_LAYOUT["icons"] if x["slug"] == slug), None)
            merged = dict(base) if base else {"slug": slug}
            for fld in ("symbol", "color"):
                if ic.get(fld):
                    merged[fld] = ic[fld]
            if merged.get("symbol"):
                merged["symbol"] = str(merged["symbol"])[:3]
            if not hex_color_ok(merged.get("color", "")):
                merged["color"] = base["color"] if base else "0xFF808080"
            ordered.append(merged)

    # Добавляем пропущенные приложения в конец — порядок из DEFAULT_LAYOUT.
    for ic in DEFAULT_LAYOUT["icons"]:
        if ic["slug"] not in seen_slugs:
            ordered.append(dict(ic))

    out["icons"] = ordered
    return out, errors


# ============================================================================
# Режим 1: Claude Vision API
# ============================================================================

def try_claude_vision(images):
    """Пробуем получить layout через Claude Vision.
    Возвращает layout-dict или None при любой ошибке."""
    api_key = os.environ.get("ANTHROPIC_API_KEY")
    if not api_key:
        return None

    try:
        import urllib.request
        import urllib.error
    except Exception as e:
        log(f"urllib недоступен: {e}")
        return None

    # Сортируем картинки по размеру и берём максимум 4 (лимит Claude ≲ 5 img).
    imgs = sorted(images, key=lambda p: p.stat().st_size, reverse=True)[:4]
    content = []
    for path in imgs:
        try:
            data = path.read_bytes()
            b64 = base64.b64encode(data).decode("ascii")
            mime = {
                ".png": "image/png", ".jpg": "image/jpeg", ".jpeg": "image/jpeg",
                ".webp": "image/webp", ".bmp": "image/bmp",
            }.get(path.suffix.lower(), "image/png")
            content.append({
                "type": "image",
                "source": {"type": "base64", "media_type": mime, "data": b64},
            })
        except Exception as e:
            log(f"Не смог прочитать {path}: {e}")

    content.append({
        "type": "text",
        "text": "Проанализируй макет(ы) и верни JSON по описанной схеме.",
    })

    body = json.dumps({
        "model": "claude-sonnet-4-5",
        "max_tokens": 2000,
        "system": CLAUDE_SYSTEM_PROMPT,
        "messages": [{"role": "user", "content": content}],
    }).encode("utf-8")

    req = urllib.request.Request(
        "https://api.anthropic.com/v1/messages",
        data=body,
        headers={
            "x-api-key": api_key,
            "anthropic-version": "2023-06-01",
            "content-type": "application/json",
        },
        method="POST",
    )

    try:
        with urllib.request.urlopen(req, timeout=60) as resp:
            payload = json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        log(f"Claude HTTP {e.code}: {e.read()[:200]!r}")
        return None
    except Exception as e:
        log(f"Claude запрос упал: {e}")
        return None

    # Достаём JSON из text-части ответа.
    try:
        text = "".join(b.get("text", "") for b in payload.get("content", []) if b.get("type") == "text")
    except Exception:
        return None

    # Часто модель оборачивает JSON в ```json ... ```.
    m = re.search(r"\{.*\}", text, re.DOTALL)
    if not m:
        log(f"В ответе нет JSON: {text[:200]!r}")
        return None
    try:
        return json.loads(m.group(0))
    except Exception as e:
        log(f"JSON парс упал: {e}")
        return None


# ============================================================================
# Режим 2: эвристика Pillow
# ============================================================================

def try_pillow_heuristic(images):
    """Доминирующий цвет верхней и нижней полос макета — фон и панель задач."""
    if not images:
        return None
    try:
        from PIL import Image  # type: ignore
    except Exception:
        return None

    def dominant(img, box):
        region = img.crop(box).convert("RGB")
        region = region.resize((64, max(1, region.size[1] // 8)))
        # Самый частый цвет (квантованный до 32 уровней на канал).
        pixels = list(region.getdata())
        hist = {}
        for r, g, b in pixels:
            k = (r // 32, g // 32, b // 32)
            hist[k] = hist.get(k, 0) + 1
        (qr, qg, qb), _ = max(hist.items(), key=lambda kv: kv[1])
        r, g, b = qr * 32 + 16, qg * 32 + 16, qb * 32 + 16
        return f"0xFF{r:02X}{g:02X}{b:02X}"

    try:
        img = Image.open(images[0])
        w, h = img.size
        desktop_top = dominant(img, (0, 0, w, h // 3))
        desktop_bot = dominant(img, (0, h // 3, w, int(h * 0.85)))
        tb_top      = dominant(img, (0, int(h * 0.88), w, h))
        tb_bot      = tb_top

        return {
            "theme": {
                "desktop_top":    desktop_top,
                "desktop_bottom": desktop_bot,
                "taskbar_top":    tb_top,
                "taskbar_bottom": tb_bot,
                "taskbar_edge":   "0xFF3A4058",
                "startbtn_color": "0xFF8EB1FF",
            },
            # Эвристика не умеет читать иконки по картинке без AI —
            # оставляем дефолтный порядок.
            "icons": [dict(x) for x in DEFAULT_LAYOUT["icons"]],
        }
    except Exception as e:
        log(f"Pillow-эвристика упала: {e}")
        return None


# ============================================================================
# Генераторы C-кода
# ============================================================================

HEADER_BANNER = """// ============================================================================
// AUTO-GENERATED by tools/ai_ui_gen.py — НЕ РЕДАКТИРОВАТЬ ВРУЧНУЮ!
// Положите свои макеты в ui_mockups/ и запустите `make`.
// Источник генерации: {source}
// ============================================================================
"""


def emit_theme_h(layout, source_desc):
    th = layout["theme"]
    banner = HEADER_BANNER.format(source=source_desc)
    return banner + textwrap.dedent(f"""
        #ifndef UI_THEME_H
        #define UI_THEME_H

        #include <stdint.h>

        // Цвета рабочего стола (градиент сверху вниз).
        #define UI_THEME_DESKTOP_TOP      (uint32_t){th['desktop_top']}
        #define UI_THEME_DESKTOP_BOTTOM   (uint32_t){th['desktop_bottom']}

        // Панель задач.
        #define UI_THEME_TASKBAR_TOP      (uint32_t){th['taskbar_top']}
        #define UI_THEME_TASKBAR_BOTTOM   (uint32_t){th['taskbar_bottom']}
        #define UI_THEME_TASKBAR_EDGE     (uint32_t){th['taskbar_edge']}

        // Кнопка «Пуск».
        #define UI_THEME_STARTBTN_COLOR   (uint32_t){th['startbtn_color']}

        #endif // UI_THEME_H
    """).lstrip()


def emit_layout_inc(layout, source_desc):
    banner = HEADER_BANNER.format(source=source_desc)
    lines = [
        banner,
        "// Порядок вызовов desktop_add_icon — определяет порядок иконок",
        "// слева-направо, сверху-вниз (авто-размещение внутри desktop.c).",
        "// Имена callback-ов — static-функции из apps.c, поэтому этот",
        "// фрагмент включается ТОЛЬКО через #include внутри apps.c.",
        "",
    ]
    for ic in layout["icons"]:
        callbacks = KNOWN_APPS.get(ic["slug"])
        if not callbacks:
            lines.append(f"// WARN: unknown slug {ic['slug']!r} skipped")
            continue
        cb_create, cb_update, cb_click, cb_key = callbacks
        # Экранирование строк (label, symbol, win_title).
        def c_str(s):
            s = (s or "").replace("\\", "\\\\").replace('"', '\\"')
            return f"\"{s}\""
        lines.append(
            f"desktop_add_icon({c_str(ic['label'])}, {c_str(ic['symbol'])}, "
            f"(uint32_t){ic['color']},\n"
            f"    {c_str(ic['win_title'])}, {int(ic['win_w'])}, {int(ic['win_h'])},\n"
            f"    {cb_create}, {cb_update}, {cb_click}, {cb_key});"
        )
        lines.append("")
    return "\n".join(lines)


# ============================================================================
# Main
# ============================================================================

def main():
    root = pathlib.Path(__file__).resolve().parent.parent
    ui_dir = root / "ui_mockups"
    out_theme = root / "kernel" / "gui" / "ui_theme.h"
    out_inc   = root / "kernel" / "gui" / "ui_layout.inc"

    mockups = find_mockups(ui_dir)
    log(f"Найдено макетов: {len(mockups)} в {ui_dir}")

    layout = None
    source = "default (fallback)"

    if mockups:
        layout = try_claude_vision(mockups)
        if layout:
            source = f"Claude Vision API ({len(mockups)} img)"
            log("Режим: Claude Vision.")
        else:
            layout = try_pillow_heuristic(mockups)
            if layout:
                source = f"Pillow heuristic ({mockups[0].name})"
                log("Режим: Pillow эвристика.")

    if layout is None:
        layout = DEFAULT_LAYOUT
        log("Режим: safe default (AI недоступен или папка пуста).")

    layout, errors = validate_layout(layout)
    for e in errors:
        log(f"WARN: {e}")

    out_theme.parent.mkdir(parents=True, exist_ok=True)
    out_theme.write_text(emit_theme_h(layout, source), encoding="utf-8")
    out_inc.write_text(emit_layout_inc(layout, source), encoding="utf-8")

    log(f"Записано: {out_theme}")
    log(f"Записано: {out_inc}")
    log("OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
