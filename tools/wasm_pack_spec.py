#!/usr/bin/env python3
# ============================================================================
# wasm_pack_spec.py - официальные тесты WebAssembly -> один файл-пакет (этап 51).
# ----------------------------------------------------------------------------
# Вход: каталог, где `wast2json` (wabt) разложил каждый .wast на x.json +
# x.N.wasm / x.N.wat. Выход: пакет, который читает wasm_spec.c - в ядре
# (`rune spec <файл>`) и на хосте (build/wasm_host spec <файл>).
# Формат описан в шапке wasm_spec.c; менять - только вместе с ним.
#
#   python3 tools/wasm_pack_spec.py <пакет> <каталог с json> [имя.json ...]
# Без списка берутся все *.json каталога по алфавиту.
# ============================================================================
import json
import os
import struct
import sys

REC_FILE, REC_MODULE, REC_MALFORMED, REC_INVALID, REC_UNLINKABLE, REC_UNINST, \
    REC_REGISTER, REC_ACTION, REC_RETURN, REC_TRAP, REC_EXHAUSTION = range(1, 12)

TYPES = {'i32': 0x7F, 'i64': 0x7E, 'f32': 0x7D, 'f64': 0x7C, 'v128': 0x7B,
         'funcref': 0x70, 'externref': 0x6F}
# Как сравнивать значение: точно по битам, «любой канонический NaN»,
# «любой арифметический NaN», пустая ссылка.
EXACT, NAN_CANON, NAN_ARITH, REF_NULL = 0, 1, 2, 3


def s16(text):
    b = text.encode('utf-8')[:65535]
    return struct.pack('<H', len(b)) + b


def value(v):
    t = TYPES[v['type']]
    raw = v.get('value', '0')
    if isinstance(raw, list):                   # v128 по дорожкам - нам не нужен
        return struct.pack('<BBQ', t, EXACT, 0)
    if raw == 'nan:canonical':
        return struct.pack('<BBQ', t, NAN_CANON, 0)
    if raw == 'nan:arithmetic':
        return struct.pack('<BBQ', t, NAN_ARITH, 0)
    if raw == 'null':
        return struct.pack('<BBQ', t, REF_NULL, 0)
    return struct.pack('<BBQ', t, EXACT, int(raw) & 0xFFFFFFFFFFFFFFFF)


def action(a):
    kind = 0 if a['type'] == 'invoke' else 1
    args = a.get('args', [])
    out = struct.pack('<B', kind) + s16(a.get('module', '')) + s16(a['field'])
    out += struct.pack('<H', len(args))
    for v in args:
        out += value(v)
    return out


def values(vs):
    out = struct.pack('<H', len(vs))
    for v in vs:
        out += value(v)
    return out


def module_assert(directory, c):
    is_text = c.get('module_type') != 'binary'
    data = b''
    if not is_text:
        with open(os.path.join(directory, c['filename']), 'rb') as f:
            data = f.read()
    return struct.pack('<B', 1 if is_text else 0) + s16(c.get('text', '')) + struct.pack('<I', len(data)) + data


def pack_file(directory, name, records):
    with open(os.path.join(directory, name), encoding='utf-8') as f:
        j = json.load(f)
    src = os.path.basename(j.get('source_filename', name))
    records.append((REC_FILE, 0, src.encode('utf-8')))
    counts = {}
    for c in j['commands']:
        t, line = c['type'], c.get('line', 0)
        counts[t] = counts.get(t, 0) + 1
        if t == 'module':
            with open(os.path.join(directory, c['filename']), 'rb') as f:
                data = f.read()
            records.append((REC_MODULE, line, s16(c.get('name', '')) + struct.pack('<I', len(data)) + data))
        elif t == 'assert_malformed':
            records.append((REC_MALFORMED, line, module_assert(directory, c)))
        elif t == 'assert_invalid':
            records.append((REC_INVALID, line, module_assert(directory, c)))
        elif t == 'assert_unlinkable':
            records.append((REC_UNLINKABLE, line, module_assert(directory, c)))
        elif t == 'assert_uninstantiable':
            records.append((REC_UNINST, line, module_assert(directory, c)))
        elif t == 'register':
            records.append((REC_REGISTER, line, s16(c['as']) + s16(c.get('name', ''))))
        elif t == 'action':
            records.append((REC_ACTION, line, action(c['action'])))
        elif t == 'assert_return':
            records.append((REC_RETURN, line, action(c['action']) + values(c.get('expected', []))))
        elif t == 'assert_trap':
            records.append((REC_TRAP, line, action(c['action']) + s16(c.get('text', ''))))
        elif t == 'assert_exhaustion':
            records.append((REC_EXHAUSTION, line, action(c['action']) + s16(c.get('text', ''))))
        else:
            raise SystemExit('%s: unknown command type %r at line %d' % (name, t, line))
    return counts


def main():
    if len(sys.argv) < 3:
        print('usage: wasm_pack_spec.py <out.pack> <dir with wast2json output> [name.json ...]')
        return 2
    out, directory = sys.argv[1], sys.argv[2]
    names = sys.argv[3:] or sorted(n for n in os.listdir(directory) if n.endswith('.json'))
    records, total = [], {}
    for n in names:
        for k, v in pack_file(directory, n, records).items():
            total[k] = total.get(k, 0) + v
    with open(out, 'wb') as f:
        f.write(b'AWSP' + struct.pack('<II', 1, len(records)))
        for t, line, body in records:
            f.write(struct.pack('<BII', t, line, len(body)) + body)
    print('packed %d files, %d records, %d bytes -> %s' % (len(names), len(records), os.path.getsize(out), out))
    print('  ' + ' '.join('%s=%d' % kv for kv in sorted(total.items())))
    return 0


if __name__ == '__main__':
    sys.exit(main())
