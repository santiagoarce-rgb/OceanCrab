#!/usr/bin/env python3
"""Automatic first-pass symbolization of the SpongeBob EBOOT (phase 2).

Input is the relocated guest image written by `dump_image` plus the analyzer's
function list. Everything is derived from the binary itself:

1. GCC RTTI: typeinfo objects {vptr, name, [base]} give every polymorphic
   class name and its single-inheritance parent.
2. Vtables {offset_to_top=0, &typeinfo, vfunc...} give each class's virtual
   methods (named Class::vfunc_NN, or inherited when the slot is shared).
3. Constructors/destructors: functions that build &vtable (lui/addiu) and store
   it into an object are named Class::ctor_or_dtor.
4. Debug strings "Class::Method" referenced from a function name it directly.

Outputs (in the output directory):
  symbols.csv  address,name,kind,confidence,evidence
  classes.csv  class,typeinfo,vtable,parent,virtuals
  summary.txt  counts

usage: symbolize.py <image.bin> <sections.csv> <functions_auto.csv> <outdir>
"""
import csv
import os
import re
import struct
import sys
from collections import Counter, defaultdict

BASE = 0x08804000
DEBUG_PRINTF = 0x089C9E34


def load(argv):
    image = open(argv[1], 'rb').read()
    sections = {}
    with open(argv[2]) as f:
        for row in csv.DictReader(f):
            sections[row['name']] = (int(row['address'], 16), int(row['size'], 16))
    functions = []
    with open(argv[3]) as f:
        for row in csv.DictReader(f):
            functions.append((int(row['address'], 16), int(row['instructions'])))
    functions.sort()
    return image, sections, functions


class Image:
    def __init__(self, data):
        self.data = data
        self.end = BASE + len(data)

    def word(self, address):
        if address < BASE or address + 4 > self.end or address & 3:
            return None
        return struct.unpack_from('<I', self.data, address - BASE)[0]

    def cstring(self, address, limit=256):
        if address < BASE or address >= self.end:
            return None
        off = address - BASE
        end = self.data.find(b'\0', off, off + limit)
        if end < 0:
            return None
        raw = self.data[off:end]
        if not raw or any((b < 0x20 and b not in (9, 10, 13)) or b > 0x7E for b in raw):
            return None
        text = raw.decode().strip()
        return text or None


def demangle_class(name):
    """'16CWStandardCamera' -> 'CWStandardCamera'; templates kept readable."""
    def parse(s, i):
        if i < len(s) and s[i] == 'N':  # nested name N...E
            parts = []
            i += 1
            while i < len(s) and s[i] != 'E':
                part, i = parse(s, i)
                parts.append(part)
            return '::'.join(parts), i + 1
        m = re.match(r'\d+', s[i:])
        if not m:
            return s[i:], len(s)
        n = int(m.group())
        i += len(m.group())
        ident = s[i:i + n]
        i += n
        if i < len(s) and s[i] == 'I':  # template args I...E
            args = []
            i += 1
            while i < len(s) and s[i] != 'E':
                if s[i].isdigit() or s[i] == 'N':
                    a, i = parse(s, i)
                else:
                    a = {'f': 'float', 'i': 'int', 'j': 'unsigned', 'b': 'bool', 'c': 'char',
                         'd': 'double', 'v': 'void'}.get(s[i], s[i])
                    i += 1
                args.append(a)
            ident += '<' + ', '.join(args) + '>'
            i += 1
        return ident, i
    try:
        out, _ = parse(name, 0)
        return out
    except Exception:
        return name


def main(argv):
    if len(argv) != 5:
        print(__doc__)
        return 2
    data, sections, functions = load(argv)
    img = Image(data)
    text_lo, text_size = sections['.text']
    text_hi = text_lo + text_size
    function_starts = {a for a, _ in functions}
    is_code_ptr = lambda v: v is not None and text_lo <= v < text_hi and (v & 3) == 0

    # Function extents: up to the next known start.
    extents = {}
    for i, (addr, count) in enumerate(functions):
        nxt = functions[i + 1][0] if i + 1 < len(functions) else text_hi
        extents[addr] = min(nxt, addr + max(count, 1) * 4) if count else nxt

    data_ranges = [(a, a + s) for n, (a, s) in sections.items()
                   if (n.startswith('.rodata') or n == '.data') and s]

    def data_words():
        for lo, hi in data_ranges:
            for a in range((lo + 3) & ~3, hi - 3, 4):
                yield a, img.word(a)

    # ---- 1. RTTI typeinfo objects -------------------------------------------------
    rtti_name = re.compile(r'(N?\d+[A-Za-z_]\w*)(E)?')
    typeinfos = {}  # typeinfo address -> (class, parent typeinfo or None)
    words = dict(data_words())
    for a, v in words.items():
        s = img.cstring(v, 128) if v is not None else None
        if not s or not s[0].isdigit() and not s.startswith('N'):
            continue
        if not rtti_name.fullmatch(s):
            continue
        ti = a - 4  # name is the 2nd word of std::type_info
        vptr = img.word(ti)
        if vptr is None or vptr == 0:
            continue
        typeinfos[ti] = [demangle_class(s), None]
    # Parents. __si_class_type_info: {vptr, name, base}. __vmi_class_type_info:
    # {vptr, name, flags, base_count, {base, offset_flags}...}; take the primary
    # (first) base. A parent is valid only if it is itself a typeinfo.
    for ti, entry in typeinfos.items():
        si_base = img.word(ti + 8)
        if si_base in typeinfos and si_base != ti:
            entry[1] = si_base
            continue
        count = img.word(ti + 12)
        vmi_base = img.word(ti + 16)
        if count is not None and 1 <= count <= 8 and vmi_base in typeinfos and vmi_base != ti:
            entry[1] = vmi_base

    # ---- 2. Vtables ----------------------------------------------------------------
    vtables = {}  # address of vfunc0 -> (class, [vfuncs])
    for a, v in words.items():
        if v in typeinfos and words.get(a - 4) == 0:
            slots = []
            p = a + 4
            while is_code_ptr(words.get(p)) or (words.get(p) == 0 and slots and
                                                  is_code_ptr(words.get(p + 4))):
                slots.append(words.get(p))
                p += 4
            if slots:
                vtables[a + 4] = (typeinfos[v][0], slots)

    symbols = {}  # address -> (name, kind, confidence, evidence)

    def propose(addr, name, kind, confidence, evidence):
        rank = {'high': 3, 'medium': 2, 'low': 1}
        old = symbols.get(addr)
        if old is None or rank[confidence] > rank[old[2]]:
            symbols[addr] = (name, kind, confidence, evidence)

    # Slot ownership: a function shared by many vtables belongs to the most
    # basic class that has it (inherited, not overridden).
    slot_classes = defaultdict(list)
    for vt, (cls, slots) in vtables.items():
        for i, f in enumerate(slots):
            if f:
                slot_classes[f].append((cls, i))
    depth_cache = {}
    class_to_ti = {c: ti for ti, (c, _) in typeinfos.items()}

    def depth(cls):
        if cls in depth_cache:
            return depth_cache[cls]
        d, ti, seen = 0, class_to_ti.get(cls), set()
        while ti is not None and ti not in seen:
            seen.add(ti)
            ti = typeinfos[ti][1]
            d += 1
        depth_cache[cls] = d
        return d

    for f, owners in slot_classes.items():
        cls, slot = min(owners, key=lambda o: (depth(o[0]), o[1]))
        conf = 'medium' if len({o[0] for o in owners}) == 1 else 'low'
        propose(f, f'{cls}::vfunc_{slot:02d}', 'virtual', conf,
                f'vtable slot {slot} ({len(owners)} vtable(s))')

    # ---- 3/4. Linear scan of each function: constant addresses + calls -------------
    vtable_set = set(vtables)
    calls = defaultdict(set)
    string_refs = defaultdict(list)
    constants = defaultdict(list)  # function -> lui/addiu constants, in order
    vtable_stores = defaultdict(set)
    for addr, _ in functions:
        hi_regs = {}  # reg -> value from lui / lui+addiu
        end = extents[addr]
        p = addr
        while p < end:
            w = img.word(p)
            if w is None:
                break
            op, rs, rt = w >> 26, (w >> 21) & 31, (w >> 16) & 31
            imm = w & 0xFFFF
            simm = imm - 0x10000 if imm & 0x8000 else imm
            if op == 0x0F:  # lui
                hi_regs[rt] = imm << 16
            elif op in (0x09, 0x0D) and rs in hi_regs:  # addiu / ori
                value = (hi_regs[rs] + simm) & 0xFFFFFFFF if op == 0x09 else hi_regs[rs] | imm
                constants[addr].append(value)
                s = img.cstring(value)
                if s:
                    string_refs[addr].append(s)
                if value in vtable_set:
                    hi_regs[rt] = value
                    vtable_stores[addr].add(value)
                else:
                    hi_regs.pop(rt, None) if rt != rs else None
                    if rt != rs:
                        hi_regs[rt] = value
            elif op == 0x03:  # jal
                calls[addr].add(((p + 4) & 0xF0000000) | ((w & 0x03FFFFFF) << 2))
            elif op == 0x2B and rt in hi_regs and hi_regs[rt] in vtable_set:  # sw vtable
                vtable_stores[addr].add(hi_regs[rt])
            p += 4

    for f, vts in vtable_stores.items():
        if len(vts) == 1:
            cls = vtables[next(iter(vts))][0]
            propose(f, f'{cls}::ctor_or_dtor', 'ctor', 'medium', 'stores the class vtable')

    method_re = re.compile(r'\b([A-Z]\w*(?:::[A-Z]\w*)*)::(~?[A-Za-z_]\w*)')
    for f, refs in string_refs.items():
        names = Counter()
        for s in refs:
            m = method_re.search(s)
            if m:
                names[f'{m.group(1)}::{m.group(2)}'] += 1
        if len(names) == 1:
            name = next(iter(names))
            conf = 'high' if DEBUG_PRINTF in calls[f] or 'Incorrect' in ''.join(refs) else 'medium'
            propose(f, name, 'debug-string', conf, 'references its own name in a message')

    # ---- 5. Lua binding tables: luaL_reg {const char *name, lua_CFunction} ---------
    # Consecutive pairs form one table; a table is named after the class whose
    # debug strings its functions use (e.g. CCharType), else lua_table_<addr>.
    ident = re.compile(r'[A-Za-z_]\w{2,}')
    tables, current, last = [], [], None  # each: (start address, [(name, fn)])
    for lo, hi in data_ranges:
        for a in range((lo + 3) & ~3, hi - 7, 4):
            name = img.cstring(img.word(a) or 0, 64)
            fn = img.word(a + 4)
            if name and ident.fullmatch(name) and is_code_ptr(fn):
                if last is None or a != last + 8:
                    if current:
                        tables.append((start, current))
                    current, start = [], a
                current.append((name, fn))
                last = a
    if current:
        tables.append((start, current))
    # Registration site: the function that loads the table address; the string
    # it loads right before is the Lua library name (luaL_register(L, name, t)).
    registered_as = {}
    for f, values in constants.items():
        for i, v in enumerate(values):
            if any(v == t for t, _ in tables):
                for prev in reversed(values[:i]):
                    lib = img.cstring(prev, 64)
                    if lib and ident.fullmatch(lib):
                        registered_as.setdefault(v, lib)
                        break
    lua_tables = []
    for start, table in tables:
        if len(table) < 2:
            continue
        owners = Counter()
        for _, fn in table:
            for s in string_refs.get(fn, []):
                m = method_re.search(s)
                if m:
                    owners[m.group(1)] += 1
        library = registered_as.get(start)
        prefix = owners.most_common(1)[0][0] if owners else (f'lua_{library}' if library else None)
        lua_tables.append((start, library, prefix, len(table)))
        for name, fn in table:
            full = f'{prefix}::lua_{name}' if prefix else f'lua_{name}'
            # The table entry is authoritative: debug messages get copy-pasted
            # (AddStrike reports itself as CCharType::AddAttack).
            symbols[fn] = (full, 'lua-binding', 'high',
                           f'luaL_reg table 0x{start:08X}' + (f' "{library}"' if library else ''))

    # ---- 6. Import stubs ----------------------------------------------------------
    imports_csv = os.path.join(os.path.dirname(argv[3]), 'ULUS10478_report_imports.csv')
    if os.path.exists(imports_csv):
        with open(imports_csv) as f:
            for row in csv.reader(f):
                if len(row) < 4 or not row[3].startswith('0x'):
                    continue
                library, nid, name, stub = row[0], row[1], row[2], int(row[3], 16)
                propose(stub, name or f'{library}_{nid}', 'import', 'high', f'{library} NID {nid}')

    # ---- Output ---------------------------------------------------------------------
    out = argv[4]
    os.makedirs(out, exist_ok=True)
    ti_vtable = {}
    for vt, (cls, slots) in vtables.items():
        ti_vtable.setdefault(cls, (vt, len(slots)))
    with open(os.path.join(out, 'classes.csv'), 'w', newline='') as f:
        w = csv.writer(f)
        w.writerow(['class', 'typeinfo', 'vtable', 'parent', 'virtuals'])
        for ti, (cls, parent) in sorted(typeinfos.items(), key=lambda kv: kv[1][0]):
            vt, n = ti_vtable.get(cls, (0, 0))
            w.writerow([cls, f'0x{ti:08X}', f'0x{vt:08X}' if vt else '',
                        typeinfos[parent][0] if parent else '', n])
    # ---- 7. Propagate method names through vtable slots ------------------------------
    # If slot N of class C's vtable is a function we named C'::Method (C' == C or
    # an ancestor), slot N of every subclass of C is its override of Method.
    def ancestors(cls):
        seen, ti = [], class_to_ti.get(cls)
        while ti is not None and ti not in [class_to_ti.get(c) for c in seen]:
            seen.append(typeinfos[ti][0])
            ti = typeinfos[ti][1]
        return seen
    slot_names = {}  # (class, slot) -> method
    for vt, (cls, slots) in vtables.items():
        for i, f in enumerate(slots):
            known = symbols.get(f)
            if known and known[1] in ('debug-string', 'lua-binding') and '::' in known[0]:
                owner, method = known[0].rsplit('::', 1)
                if owner in ancestors(cls):
                    slot_names[(owner, i)] = method
    propagated = 0
    for vt, (cls, slots) in vtables.items():
        chain = ancestors(cls)
        for i, f in enumerate(slots):
            if not f:
                continue
            method = next((slot_names[(c, i)] for c in chain if (c, i) in slot_names), None)
            old_entry = symbols.get(f)
            if method and (old_entry is None or old_entry[1] == 'virtual'):
                owner = old_entry[0].rsplit('::', 1)[0] if old_entry else cls
                symbols[f] = (f'{owner}::{method}', 'virtual-named', 'medium',
                              f'vtable slot {i} of {cls}, named from an ancestor')
                propagated += 1

    with open(os.path.join(out, 'symbols.csv'), 'w', newline='') as f:
        w = csv.writer(f)
        w.writerow(['address', 'name', 'kind', 'confidence', 'evidence'])
        for a in sorted(symbols):
            w.writerow([f'0x{a:08X}', *symbols[a]])
    with open(os.path.join(out, 'lua_tables.csv'), 'w', newline='') as f:
        w = csv.writer(f)
        w.writerow(['table', 'library', 'class', 'entries'])
        for start, library, prefix, n in lua_tables:
            w.writerow([f'0x{start:08X}', library or '', prefix or '', n])
    # Indented class hierarchy (single/primary inheritance), with vtable sizes.
    children = defaultdict(list)
    for ti, (cls, parent) in typeinfos.items():
        children[typeinfos[parent][0] if parent else None].append(cls)
    with open(os.path.join(out, 'class_tree.txt'), 'w') as f:
        def walk(cls, level):
            n = ti_vtable.get(cls, (0, 0))[1]
            f.write('  ' * level + cls + (f'  [{n} virtuals]' if n else '') + '\n')
            for child in sorted(children.get(cls, [])):
                walk(child, level + 1)
        for root in sorted(children.get(None, [])):
            walk(root, 0)
    kinds = Counter(k for _, k, _, _ in symbols.values())
    with open(os.path.join(out, 'summary.txt'), 'w') as f:
        lines = [f'functions in corpus: {len(functions)}',
                 f'classes (RTTI): {len(typeinfos)}',
                 f'classes with parent: {sum(1 for _, p in typeinfos.values() if p)}',
                 f'vtables: {len(vtables)}',
                 f'named functions: {len(symbols)}'] + \
                [f'  {k}: {n}' for k, n in kinds.most_common()]
        f.write('\n'.join(lines) + '\n')
        print('\n'.join(lines))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
