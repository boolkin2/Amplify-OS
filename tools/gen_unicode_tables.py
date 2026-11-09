#!/usr/bin/env python3
# ============================================================================
# tools/gen_unicode_tables.py — таблицы юникод-категорий для токенизатора MIND
# (этап 49.1). Генерирует kernel/arch/x86_64/mind_unicode.h: отсортированные
# диапазоны кодовых точек категорий L* (\p{L}) и N* (\p{N}) по данным
# unicodedata текущего Python. Раньше ядро приближало \p{L} эвристикой
# («всё >= U+0370, кроме пунктуации»), и для редких символов (кириллические
# титла U+0483.., знаки U+0482 и т.п.) претокенизация расходилась с HF.
#   python tools/gen_unicode_tables.py
# ============================================================================
import sys
import unicodedata


def ranges(pred):
    out = []
    start = None
    for cp in range(0x110000):
        ok = pred(chr(cp))
        if ok and start is None:
            start = cp
        elif not ok and start is not None:
            out.append((start, cp - 1))
            start = None
    if start is not None:
        out.append((start, 0x10FFFF))
    return out


def main():
    L = ranges(lambda c: unicodedata.category(c)[0] == 'L')
    N = ranges(lambda c: unicodedata.category(c)[0] == 'N')
    lines = []
    lines.append('// Сгенерировано tools/gen_unicode_tables.py (Unicode %s) — не править руками.' %
                 unicodedata.unidata_version)
    lines.append('// Диапазоны [lo, hi] категорий L* и N* для претокенизации BPE (mind_tok.c).')
    lines.append('#ifndef AOS_X86_64_MIND_UNICODE_H')
    lines.append('#define AOS_X86_64_MIND_UNICODE_H')
    lines.append('#include <stdint.h>')
    for name, rs in (('k_uni_L', L), ('k_uni_N', N)):
        lines.append('static const uint32_t %s[%d][2] = {' % (name, len(rs)))
        row = []
        for a, b in rs:
            row.append('{0x%X,0x%X}' % (a, b))
            if len(row) == 6:
                lines.append('    ' + ','.join(row) + ',')
                row = []
        if row:
            lines.append('    ' + ','.join(row) + ',')
        lines.append('};')
    lines.append('#endif')
    path = 'kernel/arch/x86_64/mind_unicode.h'
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')
    print('%s: L=%d ranges, N=%d ranges' % (path, len(L), len(N)))


if __name__ == '__main__':
    sys.exit(main())
