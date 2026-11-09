#!/usr/bin/env python3
"""
test_wm_screenshot.py — снимает screenshot после login → WM в AMPLIFY-OS x86_64.

Делает то же что test_keyboard.py, но дополнительно вызывает
`screendump` через QEMU monitor → получаем PPM с реальным состоянием
графического WM (рабочий стол + окно About + левый рейл + cursor).
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
SERIAL_LOG = ROOT / "build" / "serial-wmtest.log"
MON_SOCK   = ROOT / "build" / "qemu-mon-wm.sock"
SHOT_PPM   = ROOT / "build" / "wm-screenshot.ppm"
SHOT_PNG   = ROOT / "build" / "wm-screenshot.png"

if not ISO.exists():
    print(f"FAIL: {ISO} не найден", file=sys.stderr); sys.exit(2)

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

    print("[test] guest login...")
    send("sendkey ret"); time.sleep(0.3)
    send("sendkey ret"); time.sleep(2.0)

    print("[test] waiting for [wm] main loop...")
    for _ in range(80):
        if "[wm] main loop start" in SERIAL_LOG.read_text(errors="ignore"):
            break
        time.sleep(0.1)
    else:
        raise RuntimeError("WM did not start")

    # Дать WM нарисовать первый кадр.
    time.sleep(2.0)

    # Двинем мышь чтобы курсор был не в (0,0) и нарисовался.
    print("[test] moving mouse to (320, 240)...")
    send("mouse_move 320 240"); time.sleep(0.5)

    # Этап F: кнопки «Пуск» больше нет. Открываем Core кнопкой внизу
    # левого рейла: логические (14,906,36,36) на канве 1600x1000
    # → при 1024x768 центр примерно (20, 710).
    print("[test] click on Core launcher...")
    send("mouse_move 20 710"); time.sleep(0.3)
    send("mouse_button 1");    time.sleep(0.2)
    send("mouse_button 0");    time.sleep(0.6)

    # Финальный screendump.
    print(f"[test] screendump → {SHOT_PPM}")
    send(f"screendump {SHOT_PPM}")
    time.sleep(1.0)

    mon.close()
finally:
    qemu.terminate()
    try: qemu.wait(timeout=3)
    except subprocess.TimeoutExpired: qemu.kill()

# Конвертируем PPM → PNG (если ImageMagick есть).
if SHOT_PPM.exists():
    print(f"[test] PPM saved: {SHOT_PPM} ({SHOT_PPM.stat().st_size} bytes)")
    if subprocess.run(["which", "convert"], capture_output=True).returncode == 0:
        subprocess.run(["convert", str(SHOT_PPM), str(SHOT_PNG)])
        print(f"[test] PNG saved: {SHOT_PNG}")
    else:
        print("[test] (ImageMagick `convert` не установлен — оставляю PPM)")
else:
    print("[test] FAIL: screenshot не создан")
    sys.exit(1)

print("[test] done.")
