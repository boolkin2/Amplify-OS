#!/usr/bin/env python3
# ============================================================================
# pcap_tcp.py — разбор записи трафика гостя (QEMU `-object filter-dump`).
# Для каждого TCP-соединения, в котором была пауза дольше порога, печатает
# пакеты вокруг паузы: время, направление, флаги, относительные seq/ack,
# длину данных и окно. Помогает понять, чей сегмент потерялся.
#   python3 tools/pcap_tcp.py <файл.pcap> [порог_сек=1.5] [пакетов_вокруг=14]
# Гость — 10.0.2.15 (slirp). tcpdump на узле нет, поэтому свой разбор.
# ============================================================================
import struct
import sys

path = sys.argv[1]
gap_thr = float(sys.argv[2]) if len(sys.argv) > 2 else 1.5
around = int(sys.argv[3]) if len(sys.argv) > 3 else 14
GUEST = bytes([10, 0, 2, 15])

data = open(path, 'rb').read()
magic = struct.unpack('<I', data[:4])[0]
endian = '<' if magic in (0xa1b2c3d4, 0xa1b23c4d) else '>'
pos = 24
flows = {}
order = []
while pos + 16 <= len(data):
    ts, tus, incl, orig = struct.unpack(endian + 'IIII', data[pos:pos + 16])
    pos += 16
    pkt = data[pos:pos + incl]
    pos += incl
    if len(pkt) < 54 or pkt[12:14] != b'\x08\x00':
        continue
    ip = pkt[14:]
    ihl = (ip[0] & 15) * 4
    if ip[9] != 6:
        continue
    tot = struct.unpack('>H', ip[2:4])[0]
    src, dst = ip[12:16], ip[16:20]
    tcp = ip[ihl:tot]
    if len(tcp) < 20:
        continue
    sp, dp, seq, ack, off, fl, win = struct.unpack('>HHIIBBH', tcp[:16])
    dlen = len(tcp) - (off >> 4) * 4
    out = (src == GUEST)
    key = (dst, dp, sp) if out else (src, sp, dp)
    if key not in flows:
        flows[key] = []
        order.append(key)
    flows[key].append((ts + tus / 1e6, out, fl, seq, ack, dlen, win))


def flags(f):
    return ''.join(c for c, b in (('S', 2), ('F', 1), ('R', 4), ('P', 8), ('A', 16)) if f & b)


for key in order:
    pk = flows[key]
    t0 = pk[0][0]
    iss_out = next((p[3] for p in pk if p[1]), 0)
    iss_in = next((p[3] for p in pk if not p[1]), 0)
    gaps = [i for i in range(1, len(pk)) if pk[i][0] - pk[i - 1][0] > gap_thr]
    ip = '.'.join(str(b) for b in key[0])
    total_in = sum(p[5] for p in pk if not p[1])
    print('== %s:%d  packets=%d  bytes_in=%d  duration=%.2fs  gaps>%.1fs: %d'
          % (ip, key[1], len(pk), total_in, pk[-1][0] - t0, gap_thr, len(gaps)))
    for g in gaps[:2]:
        lo, hi = max(0, g - around), min(len(pk), g + 6)
        for i in range(lo, hi):
            t, out, fl, seq, ack, dlen, win = pk[i]
            rs = (seq - (iss_out if out else iss_in)) & 0xffffffff
            ra = (ack - (iss_in if out else iss_out)) & 0xffffffff
            mark = '   <<< pause %.2fs before' % (t - pk[i - 1][0]) if i == g else ''
            print('  %8.3f %s %-4s seq=%-7d ack=%-7d len=%-5d win=%d%s'
                  % (t - t0, '->' if out else '<-', flags(fl), rs, ra if fl & 16 else 0, dlen, win, mark))
