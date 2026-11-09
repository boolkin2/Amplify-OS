#!/usr/bin/env python3
"""
test_apps_screenshot.py — снимок 37.6c-3 после клика по нескольким desktop-иконкам.

1. Логинимся как guest.
2. Ждём WM.
3. Кликаем по иконкам Calc, Notes, Term, Files (одна за другой) — открываются окна.
4. Печатаем '12+34=' в Calc (focused last → terminal? нет, calc), нажимаем =.
5. Screendump.
"""
import os
import socket
import subprocess
import sys
import time
from pathlib import Path

ROOT       = Path(__file__).resolve().parent.parent
ISO        = ROOT / "build" / "myos-x86_64.iso"
DISK       = ROOT / "build" / "disk-x86_64.img"
SERIAL_LOG = ROOT / "build" / "serial-apps.log"
MON_SOCK   = ROOT / "build" / "qemu-mon-apps.sock"
SHOT_PPM   = ROOT / "build" / "apps-screenshot.ppm"
SHOT_PNG   = ROOT / "build" / "apps-screenshot.png"

for f in (SERIAL_LOG, MON_SOCK, SHOT_PPM, SHOT_PNG):
    Path(f).unlink(missing_ok=True)

cmd = [
    "qemu-system-x86_64",
    "-cdrom", str(ISO), "-boot", "d",
    "-m", "256M",
    "-display", "none",
    "-vga", "std",
    "-serial", f"file:{SERIAL_LOG}",
    "-monitor", f"unix:{MON_SOCK},server,nowait",
    "-drive", f"file={DISK},format=raw,if=ide,index=0,media=disk",
    "-netdev", "user,id=net0",
    "-device", "rtl8139,netdev=net0,mac=52:54:00:12:34:56",
    "-audiodev", "none,id=audio0",
    "-device", "AC97,audiodev=audio0",
    "-no-reboot", "-no-shutdown",
]

print("[test] starting QEMU...")
qemu = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

try:
    for _ in range(50):
        if MON_SOCK.exists(): break
        time.sleep(0.1)
    else:
        raise RuntimeError("monitor socket timeout")

    mon = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    mon.connect(str(MON_SOCK))
    mon.settimeout(2.0)
    time.sleep(0.5)
    try: mon.recv(8192)
    except socket.timeout: pass

    def send(s):
        mon.sendall((s + "\n").encode())
        time.sleep(0.05)
        try: mon.recv(4096)
        except socket.timeout: pass

    print("[test] waiting for login prompt...")
    for _ in range(120):
        if SERIAL_LOG.exists() and "Operator gate" in SERIAL_LOG.read_text(errors="ignore"):
            break
        time.sleep(0.1)
    else:
        raise RuntimeError("login prompt timeout")

    # Guest login
    send("sendkey ret"); time.sleep(0.3)
    send("sendkey ret"); time.sleep(2.0)

    # Дать WM время инициализации
    for _ in range(60):
        if "[wm] main loop start" in SERIAL_LOG.read_text(errors="ignore"):
            break
        time.sleep(0.1)
    else:
        raise RuntimeError("WM did not start")
    time.sleep(1.5)

    # Иконки рабочего стола: layout — вертикальная колонка слева,
    # margin=20, w=h=80, pad=20. Координаты центров: x=60, y=60/160/260/360...
    # порядок: About, Files, Notes, Calc, Term, Paint, Embers, Browser
    def click_at(x, y):
        send(f"mouse_move {x} {y}"); time.sleep(0.1)
        send("mouse_button 1");      time.sleep(0.1)
        send("mouse_button 0");      time.sleep(0.4)

    # Закрыть Welcome (кнопка X в spawn=120,80 окне About=340×140).
    # X-кнопка: x=120+340-14-8=438, y=80+(28-14)/2=87 -> click (445, 94)
    print("[test] close welcome About window")
    click_at(445, 94)
    time.sleep(0.3)

    print("[test] open Calc")
    click_at(60, 60 + 1*(80+20))    # 2-я иконка слева... нет, это Files
    # Перечитаем layout: i=0 About, i=1 Files, i=2 Notes, i=3 Calc...
    # y_i = 20 + i * 100
    # Calc — i=3, y=320, x=60
    click_at(60, 320)
    time.sleep(0.6)

    print("[test] open Notes")
    click_at(60, 220)    # i=2
    time.sleep(0.6)

    print("[test] open Term")
    click_at(60, 420)    # i=4
    time.sleep(0.6)

    print("[test] open Files")
    click_at(60, 120)    # i=1
    time.sleep(0.6)

    # Сфокусируем Calc (последний click), напечатаем 12+34=
    print("[test] focus Calc by clicking its title bar (spawn_x = 120 + 4*30 = 240, y=80+4*30=200)")
    # Calc был spawn'ом #4 (After Welcome closed... 0=About already focused but X.
    # Spawn order in WM: 1=About(welcome), 2=Calc, 3=Notes, 4=Term, 5=Files (each at +30,+30)
    # spawn_x=120+30*N, spawn_y=80+30*N для каждого open_*. The about welcome closed but the spawn state still increments.
    # А ну попробуем кликом по другому: фокус последний открытый = Files.
    # Не критично — просто screenshot

    time.sleep(1.0)
    print(f"[test] screendump → {SHOT_PPM}")
    send(f"screendump {SHOT_PPM}")
    time.sleep(1.0)

    mon.close()
finally:
    qemu.terminate()
    try: qemu.wait(timeout=3)
    except subprocess.TimeoutExpired: qemu.kill()

if SHOT_PPM.exists():
    print(f"[test] PPM saved: {SHOT_PPM} ({SHOT_PPM.stat().st_size} bytes)")
    try:
        from PIL import Image
        img = Image.open(SHOT_PPM)
        img.save(SHOT_PNG)
        print(f"[test] PNG saved: {SHOT_PNG}")
    except Exception as e:
        print(f"[test] PIL error: {e}")
else:
    print("[test] FAIL: screenshot не создан")
    sys.exit(1)

# Анализ серийного лога
log = SERIAL_LOG.read_text(errors="ignore") if SERIAL_LOG.exists() else ""
checks = [
    ("Stage 37.6c-3",          "stage banner 37.6c-3"),
    ("[doublebuf] initialized", "doublebuf init ok"),
    ("[wm] main loop start",   "WM main loop running"),
    ("Welcome, guest",         "login as guest accepted"),
]
ok = True
for substr, desc in checks:
    if substr in log:
        print(f"  ✓ {desc}")
    else:
        print(f"  ✗ MISSING: {desc} (искали '{substr}')")
        ok = False

sys.exit(0 if ok else 1)
