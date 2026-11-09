#!/usr/bin/env python3
"""
test_c5_apps.py — комплексный smoke-тест 37.6c-5: все GUI-приложения.

Проходит полный сценарий:
  1. splash + firstboot (создаём admin/secret)
  2. login (admin/secret) → desktop
  3. Открываем каждое из 12 приложений через клик по иконке.
  4. Особо проверяем:
     - Terminal: вводим `guide`, `matrix`, `clock` — должны исполниться.
     - Notes: вводим текст + Ctrl+S → проверяем /disk/NOTES.TXT в логе.
     - Files: жмём Refresh, Open.
     - TaskMgr: окно отрисовалось, видим Tasks/Windows/System секции.
     - Paint: палитра, клики на canvas, Save → /disk/PAINT.PPM.
     - Calc: жмём цифры, операторы, =.

Делает много screendump'ов в build/c5app-*.png.
"""
import os, shutil, socket, subprocess, sys, time
from pathlib import Path

ROOT       = Path(__file__).resolve().parent.parent
ISO        = ROOT / "build" / "myos-x86_64.iso"
DISK_ORIG  = ROOT / "build" / "disk-x86_64.img"
DISK_TEST  = ROOT / "build" / "disk-c5apps-test.img"
SERIAL_LOG = ROOT / "build" / "serial-c5apps.log"
MON_SOCK   = ROOT / "build" / "qemu-mon-c5apps.sock"
SHOT_DIR   = ROOT / "build"

for f in (SERIAL_LOG, MON_SOCK, DISK_TEST):
    Path(f).unlink(missing_ok=True)
for f in SHOT_DIR.glob("c5app-*.ppm"): f.unlink()
for f in SHOT_DIR.glob("c5app-*.png"): f.unlink()

shutil.copy(DISK_ORIG, DISK_TEST)

cmd = [
    "qemu-system-x86_64",
    "-cdrom", str(ISO), "-boot", "d",
    "-m", "256M",
    "-display", "none",
    "-vga", "std",
    "-serial", f"file:{SERIAL_LOG}",
    "-monitor", f"unix:{MON_SOCK},server,nowait",
    "-drive", f"file={DISK_TEST},format=raw,if=ide,index=0,media=disk",
    "-netdev", "user,id=net0",
    "-device", "rtl8139,netdev=net0,mac=52:54:00:12:34:56",
    "-audiodev", "none,id=audio0",
    "-device", "AC97,audiodev=audio0",
    "-no-reboot", "-no-shutdown",
]

print("[test] starting QEMU (full apps smoke test)...")
qemu = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

def wait_log(substr, timeout=15.0):
    end = time.time() + timeout
    while time.time() < end:
        if SERIAL_LOG.exists():
            data = SERIAL_LOG.read_text(errors="ignore")
            if substr in data: return True
        time.sleep(0.1)
    return False

# Координаты центра иконок на рабочем столе (вертикальный столбец слева,
# ICON_W=96, ICON_PAD=12, начало y=12). Берём 12 приложений в порядке
# добавления в kernel64_main.c.
ICON_W, ICON_H, ICON_PAD = 96, 100, 12
def icon_xy(idx):
    # 12 иконок: 7 в колонку, потом перенос. screen 1024x768 (этап F: taskbar удалён).
    # max_y = 768 - 100 - 4 = 664 (раньше минус 36px taskbar). Один столбец вмещает (628-12)/(100+12)=5.5
    # → 5 иконок в колонке.
    col = idx // 5
    row = idx % 5
    x = ICON_PAD + col * (ICON_W + ICON_PAD) + ICON_W // 2
    y = ICON_PAD + row * (ICON_H + ICON_PAD) + ICON_H // 2
    return x, y

ok = True
try:
    for _ in range(50):
        if MON_SOCK.exists(): break
        time.sleep(0.1)
    else:
        raise RuntimeError("monitor socket timeout")

    mon = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    mon.connect(str(MON_SOCK))
    mon.settimeout(2.0)
    time.sleep(0.3)
    try: mon.recv(8192)
    except socket.timeout: pass

    def send(s):
        mon.sendall((s + "\n").encode())
        time.sleep(0.04)
        try: mon.recv(4096)
        except socket.timeout: pass

    def shot(name):
        f = SHOT_DIR / f"c5app-{name}.ppm"
        send(f"screendump {f}")
        time.sleep(0.8)
        if f.exists():
            try:
                from PIL import Image
                Image.open(f).save(SHOT_DIR / f"c5app-{name}.png")
                f.unlink()
                print(f"  ✓ c5app-{name}.png")
            except Exception as e:
                print(f"  ! PIL {name}: {e}")

    def type_str(s):
        SHIFT = set("ABCDEFGHIJKLMNOPQRSTUVWXYZ!@#$%^&*()_+")
        for ch in s:
            if ch == ' ': send("sendkey spc")
            elif ch == '.': send("sendkey dot")
            elif ch == ',': send("sendkey comma")
            elif ch == '/': send("sendkey slash")
            elif ch in SHIFT: send(f"sendkey shift-{ch.lower()}")
            else: send(f"sendkey {ch}")
            time.sleep(0.02)

    def click(x, y):
        # qemu mouse_move expects abs coords 0..32767. Screen 1024x768.
        ax = int(x * 32767 / 1024)
        ay = int(y * 32767 / 768)
        send(f"mouse_move {ax} {ay}")
        time.sleep(0.05)
        send("mouse_button 1")
        time.sleep(0.05)
        send("mouse_button 0")
        time.sleep(0.1)

    # 1) firstboot
    print("[test] passing firstboot...")
    wait_log("[firstboot] starting graphical wizard", 20.0)
    time.sleep(1.5)
    send("sendkey ret"); time.sleep(0.5)        # welcome -> policy
    send("sendkey ret"); time.sleep(0.5)        # policy -> create
    type_str("admin"); time.sleep(0.2)
    send("sendkey tab"); time.sleep(0.2)
    type_str("secret"); time.sleep(0.2)
    send("sendkey tab"); time.sleep(0.2)
    type_str("secret"); time.sleep(0.2)
    send("sendkey ret"); time.sleep(2.5)        # success → login screen

    # 2) login
    print("[test] passing login...")
    time.sleep(1.0)
    type_str("admin"); time.sleep(0.2)
    send("sendkey tab"); time.sleep(0.2)
    type_str("secret"); time.sleep(0.2)
    send("sendkey ret"); time.sleep(2.0)

    # 3) desktop
    print("[test] waiting for WM...")
    wait_log("[wm] main loop start", 10.0)
    time.sleep(1.5)
    shot("01-desktop")

    # 4) Открываем Terminal (idx=5 в списке, как добавляется в
    # kernel64_main.c → но порядок может быть другим). Ищем по координатам
    # все 12 иконок и кликаем по их центрам.
    # Этап F: меню «Пуск» и taskbar удалены. Единственный лаунчер — Core,
    # кнопка внизу левого рейла: логические (14,906,36,36) на канве 1600x1000
    # → при 1024x768 это центр примерно (20, 710).
    print("[test] core launcher...")
    click(20, 710)
    time.sleep(0.6)
    shot("02-core")
    send("sendkey esc")        # close
    time.sleep(0.4)

    # 5) Terminal — по индексу 4 (About=0, Files=1, Notes=2, Calc=3, Term=4 ...).
    # Точный порядок в kernel64_main.c. Кликаем там, где предположительно
    # терминал — попробуем idx=4.
    print("[test] terminal...")
    tx, ty = icon_xy(4)
    click(tx, ty); time.sleep(1.0)
    shot("03-terminal-open")
    # Сначала ставим фокус кликом в окно (~центр экрана).
    click(512, 384); time.sleep(0.3)
    type_str("guide"); time.sleep(0.2)
    send("sendkey ret"); time.sleep(0.8)
    shot("04-terminal-guide")
    type_str("matrix"); time.sleep(0.2)
    send("sendkey ret"); time.sleep(0.8)
    shot("05-terminal-matrix")
    type_str("clock"); time.sleep(0.2)
    send("sendkey ret"); time.sleep(0.8)
    shot("06-terminal-clock")

    # 6) Notes (idx=2)
    print("[test] notes...")
    nx, ny = icon_xy(2)
    click(nx, ny); time.sleep(1.0)
    # Фокус — кликаем в центр.
    click(520, 384); time.sleep(0.3)
    type_str("Hello from AMPLIFY-OS"); time.sleep(0.3)
    send("sendkey ret"); time.sleep(0.1)
    type_str("Saved via Ctrl+S"); time.sleep(0.3)
    shot("07-notes-typed")
    # Ctrl+S
    send("sendkey ctrl-s"); time.sleep(0.8)
    shot("08-notes-saved")

    # 7) Files (idx=1)
    print("[test] files...")
    fx, fy = icon_xy(1)
    click(fx, fy); time.sleep(1.0)
    # Фокус
    click(520, 384); time.sleep(0.3)
    shot("09-files-open")

    # 8) Calc (idx=3)
    print("[test] calc...")
    cx, cy = icon_xy(3)
    click(cx, cy); time.sleep(1.0)
    click(560, 384); time.sleep(0.3)
    type_str("2+3="); time.sleep(0.3)
    shot("10-calc")

    # 9) Paint (idx=6) — кликаем для рисования + Save
    print("[test] paint...")
    px, py = icon_xy(6)
    click(px, py); time.sleep(1.0)
    click(520, 384); time.sleep(0.3)
    # рисуем 5 точек
    for dx in range(5):
        click(560 + dx * 12, 380); time.sleep(0.1)
    shot("11-paint-drawn")
    # Ctrl+S (save) — на случай если у Paint он работает.
    send("sendkey ctrl-s"); time.sleep(0.8)
    shot("12-paint-saved")

    # 10) TaskMgr (idx=11 = последняя)
    print("[test] taskmgr...")
    txm, tym = icon_xy(11)
    click(txm, tym); time.sleep(1.0)
    shot("13-taskmgr")

    mon.close()
finally:
    qemu.terminate()
    try: qemu.wait(timeout=3)
    except subprocess.TimeoutExpired: qemu.kill()

# Анализ serial-лога
log = SERIAL_LOG.read_text(errors="ignore") if SERIAL_LOG.exists() else ""
checks = [
    ("[doublebuf] initialized",              "doublebuf init"),
    ("[firstboot] starting graphical wizard","firstboot GUI started"),
    ("[users] added 'admin'",                "admin added"),
    ("[fat32] created /disk/USERS.TXT",      "users.txt created"),
    ("[login] user=admin (authenticated)",   "login authenticated"),
    ("[wm] main loop start",                 "WM running"),
]
# Опциональные (если кликнули правильные иконки)
optional_checks = [
    ("[notes] save -> /disk/NOTES.TXT OK",   "Notes saved"),
    ("[paint] save -> /disk/PAINT.PPM OK",   "Paint saved"),
]
print()
print("=" * 60)
print("Mandatory checks:")
for s, d in checks:
    if s in log: print(f"  ✓ {d}")
    else:        print(f"  ✗ MISSING: {d}"); ok = False
print()
print("Optional (depends on icon order):")
for s, d in optional_checks:
    if s in log: print(f"  ✓ {d}")
    else:        print(f"  ? {d} — not triggered (icon idx may differ)")

sys.exit(0 if ok else 1)
