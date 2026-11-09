#!/usr/bin/env python3
"""
test_c5_splash.py — тест на графические splash + firstboot + login.

Делает 3 скриншота в разные моменты:
  1. splash (середина анимации)
  2. firstboot wizard (welcome screen)
  3. login (после создания admin)
"""
import os, shutil, socket, subprocess, sys, time
from pathlib import Path

ROOT       = Path(__file__).resolve().parent.parent
ISO        = ROOT / "build" / "myos-x86_64.iso"
DISK_ORIG  = ROOT / "build" / "disk-x86_64.img"
DISK_TEST  = ROOT / "build" / "disk-c5-test.img"
SERIAL_LOG = ROOT / "build" / "serial-c5.log"
MON_SOCK   = ROOT / "build" / "qemu-mon-c5.sock"
SHOT_DIR   = ROOT / "build"

for f in (SERIAL_LOG, MON_SOCK, DISK_TEST):
    Path(f).unlink(missing_ok=True)
for f in SHOT_DIR.glob("c5-*.ppm"): f.unlink()
for f in SHOT_DIR.glob("c5-*.png"): f.unlink()

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

def wait_log(substr, timeout=15.0):
    end = time.time() + timeout
    while time.time() < end:
        if SERIAL_LOG.exists():
            data = SERIAL_LOG.read_text(errors="ignore")
            if substr in data: return True
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
    time.sleep(0.3)
    try: mon.recv(8192)
    except socket.timeout: pass

    def send(s):
        mon.sendall((s + "\n").encode())
        time.sleep(0.04)
        try: mon.recv(4096)
        except socket.timeout: pass

    def shot(name):
        f = SHOT_DIR / f"c5-{name}.ppm"
        send(f"screendump {f}")
        time.sleep(0.8)
        if f.exists():
            try:
                from PIL import Image
                Image.open(f).save(SHOT_DIR / f"c5-{name}.png")
                print(f"  ✓ c5-{name}.png")
            except Exception as e:
                print(f"  ! PIL {name}: {e}")

    def type_str(s):
        SHIFT = set("ABCDEFGHIJKLMNOPQRSTUVWXYZ!@#$%^&*()_+")
        for ch in s:
            if ch == ' ': send("sendkey spc")
            elif ch in SHIFT: send(f"sendkey shift-{ch.lower()}")
            else: send(f"sendkey {ch}")
            time.sleep(0.02)

    # 1) Ждём splash
    print("[test] waiting for splash...")
    wait_log("[splash]", 5.0)  # может и не быть, но ждём doublebuf
    wait_log("[doublebuf] initialized", 5.0)
    time.sleep(1.5)
    shot("01-splash")

    # 2) Ждём firstboot Welcome
    print("[test] waiting for firstboot...")
    wait_log("[firstboot] starting graphical wizard", 15.0)
    time.sleep(1.5)
    shot("02-welcome")

    # Enter -> policy
    send("sendkey ret"); time.sleep(1.0)
    shot("03-policy")

    # Enter -> create
    send("sendkey ret"); time.sleep(1.0)
    shot("04-create-empty")

    # Ввод admin
    type_str("admin"); time.sleep(0.3)
    send("sendkey tab"); time.sleep(0.3)
    type_str("secret"); time.sleep(0.3)
    send("sendkey tab"); time.sleep(0.3)
    type_str("secret"); time.sleep(0.3)
    shot("05-create-filled")

    # Создать
    send("sendkey ret"); time.sleep(1.5)
    shot("06-success")

    # Ждём login screen
    print("[test] waiting for login...")
    time.sleep(3.0)
    shot("07-login")

    # Вводим admin/secret
    type_str("admin"); time.sleep(0.3)
    send("sendkey tab"); time.sleep(0.3)
    type_str("secret"); time.sleep(0.3)
    shot("08-login-filled")
    send("sendkey ret"); time.sleep(2.0)

    # Ждём WM
    print("[test] waiting for WM...")
    wait_log("[wm] main loop start", 10.0)
    time.sleep(2.0)
    shot("09-desktop")

    mon.close()
finally:
    qemu.terminate()
    try: qemu.wait(timeout=3)
    except subprocess.TimeoutExpired: qemu.kill()

# Анализ
log = SERIAL_LOG.read_text(errors="ignore") if SERIAL_LOG.exists() else ""
checks = [
    ("[doublebuf] initialized",              "doublebuf init"),
    ("[firstboot] starting graphical wizard","firstboot GUI started"),
    ("[users] added 'admin'",                "admin added"),
    ("[fat32] created /disk/USERS.TXT",      "users.txt file created on FAT32"),
    ("[login] user=admin (authenticated)",   "login authenticated"),
    ("[wm] main loop start",                 "WM running"),
]
ok = True
for s, d in checks:
    if s in log: print(f"  ✓ {d}")
    else:        print(f"  ✗ MISSING: {d} — искали '{s}'"); ok = False

sys.exit(0 if ok else 1)
