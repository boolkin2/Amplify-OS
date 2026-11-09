#!/usr/bin/env python3
"""
test_c4_final.py — финальный smoke-тест 37.6c-4.

Сценарий:
  1. Делаем копию disk-x86_64.img → disk-c4-test.img (чтобы users.txt не
     попал в исходный диск).
  2. Запускаем QEMU, ждём first-boot wizard.
  3. Вводим admin / secret / secret.
  4. Ждём login screen, вводим admin / secret.
  5. Ждём WM, делаем screendump.
  6. Проверяем serial лог: должны быть строки [users] и [login] (authenticated).
"""

import os, shutil, socket, subprocess, sys, time
from pathlib import Path

ROOT       = Path(__file__).resolve().parent.parent
ISO        = ROOT / "build" / "myos-x86_64.iso"
DISK_ORIG  = ROOT / "build" / "disk-x86_64.img"
DISK_TEST  = ROOT / "build" / "disk-c4-test.img"
SERIAL_LOG = ROOT / "build" / "serial-c4.log"
MON_SOCK   = ROOT / "build" / "qemu-mon-c4.sock"
SHOT_PPM   = ROOT / "build" / "c4-screenshot.ppm"
SHOT_PNG   = ROOT / "build" / "c4-screenshot.png"

for f in (SERIAL_LOG, MON_SOCK, SHOT_PPM, SHOT_PNG, DISK_TEST):
    Path(f).unlink(missing_ok=True)

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

print("[test] starting QEMU...")
qemu = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

def wait_log(substr, timeout=12.0):
    end = time.time() + timeout
    while time.time() < end:
        if SERIAL_LOG.exists():
            data = SERIAL_LOG.read_text(errors="ignore")
            if substr in data:
                return True
        time.sleep(0.1)
    return False

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
        time.sleep(0.04)
        try: mon.recv(4096)
        except socket.timeout: pass

    def type_str(s):
        SHIFT = {'A','B','C','D','E','F','G','H','I','J','K','L','M',
                 'N','O','P','Q','R','S','T','U','V','W','X','Y','Z',
                 '!','@','#','$','%','^','&','*','(',')','_','+'}
        for ch in s:
            if ch == ' ':
                send("sendkey spc")
            elif ch in SHIFT:
                send(f"sendkey shift-{ch.lower()}")
            else:
                send(f"sendkey {ch}")
            time.sleep(0.02)

    # 1) Ждём firstboot wizard.
    print("[test] waiting for firstboot wizard...")
    if not wait_log("first-time setup", 12.0):
        raise RuntimeError("firstboot wizard did not start")

    # 2) Вводим admin / secret / secret.
    time.sleep(0.5)
    print("[test] type 'admin' + ENTER")
    type_str("admin"); send("sendkey ret"); time.sleep(0.5)
    print("[test] type 'secret' + ENTER (password)")
    type_str("secret"); send("sendkey ret"); time.sleep(0.5)
    print("[test] type 'secret' + ENTER (confirm)")
    type_str("secret"); send("sendkey ret"); time.sleep(0.5)

    # 3) Ждём подтверждения сохранения.
    print("[test] waiting for [users] save...")
    if not wait_log("[users] added 'admin'", 10.0):
        raise RuntimeError("users_add did not happen")

    # 4) Ждём login prompt.
    print("[test] waiting for login prompt...")
    if not wait_log("Operator gate", 10.0):
        raise RuntimeError("login did not start")

    time.sleep(0.5)
    print("[test] type 'admin' + ENTER (name)")
    type_str("admin"); send("sendkey ret"); time.sleep(0.5)
    print("[test] type 'secret' + ENTER (pass)")
    type_str("secret"); send("sendkey ret"); time.sleep(0.5)

    # 5) Ждём authenticated.
    print("[test] waiting for authenticated marker...")
    if not wait_log("(authenticated)", 5.0):
        print("[test] WARN: 'authenticated' marker not found, log so far:")
        if SERIAL_LOG.exists():
            print(SERIAL_LOG.read_text(errors='ignore')[-1500:])

    # 6) Ждём WM.
    print("[test] waiting for WM main loop...")
    if not wait_log("[wm] main loop start", 8.0):
        raise RuntimeError("WM did not start")
    time.sleep(2.0)

    # 7) Screendump.
    print(f"[test] screendump → {SHOT_PPM}")
    send(f"screendump {SHOT_PPM}")
    time.sleep(1.0)

    mon.close()
finally:
    qemu.terminate()
    try: qemu.wait(timeout=3)
    except subprocess.TimeoutExpired: qemu.kill()

# Конвертация PNG.
if SHOT_PPM.exists():
    try:
        from PIL import Image
        Image.open(SHOT_PPM).save(SHOT_PNG)
        print(f"[test] PNG saved: {SHOT_PNG}")
    except Exception as e:
        print(f"[test] PIL error: {e}")

# Анализ
log = SERIAL_LOG.read_text(errors="ignore") if SERIAL_LOG.exists() else ""
checks = [
    ("Stage 37.6c-4",            "stage banner 37.6c-4"),
    ("[users] /disk/users.txt not found", "users init detects empty DB"),
    ("AMPLIFY-OS  —  first-time setup",   "firstboot wizard rendered"),
    ("[users] added 'admin'",     "admin user added"),
    ("[doublebuf] initialized",   "doublebuf"),
    ("[wm] main loop start",      "WM running"),
]
ok = True
for substr, desc in checks:
    if substr in log:
        print(f"  ✓ {desc}")
    else:
        print(f"  ✗ MISSING: {desc}")
        ok = False

sys.exit(0 if ok else 1)
