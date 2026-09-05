#!/usr/bin/env python3
# Rashid - run Intel (x86_64) macOS applications on Apple Silicon
# Copyright 2026 Toritan123
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
"""dsc.py - read the x86_64 dyld shared cache directly.

Apple's dsc_extractor.bundle un-splits the cache into individual Mach-O files,
but it leaves pointers slide-encoded AND it cannot recover selector names: in a
modern cache a method's selector is an offset into one cache-wide selector
pool, which no single extracted dylib contains. This reads the cache itself, so
selectors, type encodings and exports all resolve.

Every ObjC method's type encoding is a ready-made thunk signature, which is
what makes rashid's M4 (auto-generated objc_msgSend thunks) tractable.

  ./dsc.py info    <cache>              header, mappings, selector pool
  ./dsc.py list    <cache> [pattern]    images in the cache
  ./dsc.py objc    <cache> <pattern>    classes, methods, type encodings
  ./dsc.py symbols <cache> <pattern>    exported symbols
  ./dsc.py plan    <cache> [pattern]    thunk workload summary

<cache> is the main dyld_shared_cache_x86_64 file; subcaches (.01 ...) are
picked up automatically from the same directory.
"""

import glob, json, mmap, os, re, struct, sys

# Pointers in the cache keep a slide encoding: bits 40..55 hold the step to the
# next rebase, the rest is an offset from the shared region base.
DELTA_MASK = 0x00FFFF0000000000
VALUE_ADD  = 0x00007FF800000000

# objc's cache-wide selector pool opens with this sentinel; every relative
# method selector is an offset from it. (objc4 emits a deliberately unlikely
# emoji as entry zero.)
SEL_SENTINEL = b"\xf0\x9f\xa4\xaf\x00"

LC_SEGMENT_64        = 0x19
LC_DYLD_EXPORTS_TRIE = 0x80000033
LC_DYLD_INFO_ONLY    = 0x80000022

SMALL_METHODS   = 0x80000000   # 12-byte relative entries
DIRECT_SELECTOR = 0x40000000   # selectors are offsets from the pool base


class Cache:
    def __init__(self, path):
        base = path
        paths = [base] + sorted(glob.glob(base + ".[0-9][0-9]"))
        self.mm, self.maps = {}, []
        for p in paths:
            if p.endswith((".map", ".atlas")):
                continue
            f = open(p, "rb")
            m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
            if m[:7] != b"dyld_v1":
                m.close(); continue
            self.mm[p] = m
            mo, mc = struct.unpack_from("<2I", m, 16)
            for i in range(mc):
                a, s, fo, _, _ = struct.unpack_from("<3Q2I", m, mo + i * 32)
                self.maps.append((a, s, fo, p))
        if not self.maps:
            raise SystemExit(f"{path}: no usable cache files")
        self.maps.sort()
        self.main = self.mm[base]
        self.magic = self.main[:16].rstrip(b"\0").decode()
        self.uuid = self.main[0x58:0x68].hex()
        self._sel_base = None
        self._sel_cands = None

    # --- raw access ------------------------------------------------------
    def read(self, addr, n):
        for a, s, fo, p in self.maps:
            if a <= addr < a + s:
                o = fo + (addr - a)
                return self.mm[p][o:o + min(n, s - (addr - a))]
        return None

    def u32(self, addr):
        b = self.read(addr, 4)
        return struct.unpack("<I", b)[0] if b and len(b) >= 4 else None

    def u64(self, addr):
        b = self.read(addr, 8)
        return struct.unpack("<Q", b)[0] if b and len(b) >= 8 else None

    def ptr(self, addr):
        raw = self.u64(addr)
        if not raw:
            return None
        return (raw & ~DELTA_MASK & 0xFFFFFFFFFFFFFFFF) + VALUE_ADD

    def cstr(self, addr, limit=1024):
        b = self.read(addr, limit)
        if not b:
            return None
        e = b.find(b"\0")
        if e < 0:
            return None
        try:
            return b[:e].decode()
        except UnicodeDecodeError:
            return None

    # --- images ----------------------------------------------------------
    def images(self):
        """dyld_cache_image_text_info[]: uuid, loadAddress, textSize, pathOff."""
        off, count = struct.unpack_from("<QQ", self.main, 0x88)
        out = []
        for i in range(count):
            e = off + i * 32
            addr, tsize, poff = struct.unpack_from("<QII", self.main, e + 16)
            end = self.main.find(b"\0", poff)
            out.append((addr, self.main[poff:end].decode(errors="replace")))
        return out

    # --- selector pool ---------------------------------------------------
    def _is_str_start(self, addr):
        """True if addr begins a NUL-terminated printable string."""
        b = self.read(addr - 1, 256)
        if not b or len(b) < 2 or b[0] != 0:
            return False
        e = b.find(b"\0", 1)
        if e < 2:
            return False
        try:
            return b[1:e].decode().isprintable()
        except UnicodeDecodeError:
            return False

    def selector_candidates(self):
        """Every occurrence of objc's selector-pool sentinel."""
        if self._sel_cands is None:
            out = []
            for a, s, fo, p in self.maps:
                m, i = self.mm[p], fo
                while True:
                    i = m.find(SEL_SENTINEL, i, fo + s)
                    if i < 0:
                        break
                    out.append(a + (i - fo))
                    i += 1
            self._sel_cands = out
        return self._sel_cands

    def selector_base(self, sample=()):
        """The sentinel appears in ordinary string sections too, so calibrate
        against real method-list offsets: only the true pool base makes every
        one of them land on a string boundary."""
        if self._sel_base is not None:
            return self._sel_base
        cands = self.selector_candidates()
        if not cands:
            return None
        sample = list(sample)[:64]
        if not sample:
            return cands[0]
        for b in cands:
            if all(self._is_str_start(b + o) for o in sample):
                self._sel_base = b
                return b
        return None


class Dylib:
    """A single image viewed in place inside the cache."""

    def __init__(self, cache, addr, path):
        self.c, self.addr, self.path = cache, addr, path
        self.segs, self.sects = [], {}
        self.export_addr = self.export_size = 0
        ncmds, = struct.unpack("<I", cache.read(addr + 16, 4))
        off = addr + 32
        linkedit = None
        for _ in range(ncmds):
            cmd, size = struct.unpack("<2I", cache.read(off, 8))
            if cmd == LC_SEGMENT_64:
                hdr = cache.read(off, 72)
                name = hdr[8:24].rstrip(b"\0").decode()
                vmaddr, vmsize, fileoff, filesize = struct.unpack_from("<4Q", hdr, 24)
                nsects, = struct.unpack_from("<I", hdr, 64)
                self.segs.append((name, vmaddr, vmsize))
                if name == "__LINKEDIT":
                    linkedit = (vmaddr, fileoff)
                so = off + 72
                for _ in range(nsects):
                    sh = cache.read(so, 80)
                    sn = sh[0:16].rstrip(b"\0").decode()
                    seg = sh[16:32].rstrip(b"\0").decode()
                    sa, ss = struct.unpack_from("<2Q", sh, 32)
                    self.sects[(seg, sn)] = (sa, ss)
                    self.sects.setdefault(sn, (sa, ss))
                    so += 80
            elif cmd == LC_DYLD_EXPORTS_TRIE:
                o, s = struct.unpack("<2I", cache.read(off + 8, 8))
                self._exp_fileoff, self.export_size = o, s
            elif cmd == LC_DYLD_INFO_ONLY:
                o, s = struct.unpack("<2I", cache.read(off + 32, 8))
                if o:
                    self._exp_fileoff, self.export_size = o, s
            off += size
        # LINKEDIT command offsets are file offsets in the original layout;
        # inside the cache they are relative to the shared LINKEDIT mapping.
        if linkedit and self.export_size:
            lvm, lfo = linkedit
            self.export_addr = lvm + (self._exp_fileoff - lfo)

    # --- ObjC ------------------------------------------------------------
    def _methods(self, addr, sel_base):
        if not addr:
            return []
        hdr = self.c.read(addr, 8)
        if not hdr or len(hdr) < 8:
            return []
        ef, count = struct.unpack("<2I", hdr)
        entsize = ef & 0xFFFC
        small = bool(ef & SMALL_METHODS)
        direct = bool(ef & DIRECT_SELECTOR)
        if count > 200000 or not entsize:
            return []
        out = []
        for i in range(count):
            e = addr + 8 + i * entsize
            if small:
                f = self.c.read(e, 12)
                if not f or len(f) < 12:
                    break
                n_off, t_off, i_off = struct.unpack("<3i", f)
                if direct and sel_base:
                    sel = self.c.cstr(sel_base + n_off, 512)
                else:
                    tgt = self.c.ptr(e + n_off)
                    sel = self.c.cstr(tgt, 512) if tgt else None
                types = self.c.cstr(e + 4 + t_off, 512)
                imp = e + 8 + i_off
            else:
                f = self.c.read(e, 24)
                if not f or len(f) < 24:
                    break
                n_p, t_p, imp = struct.unpack("<3Q", f)
                dec = lambda v: ((v & ~DELTA_MASK & 0xFFFFFFFFFFFFFFFF) + VALUE_ADD) if v else 0
                sel = self.c.cstr(dec(n_p), 512)
                types = self.c.cstr(dec(t_p), 512)
            if sel:
                out.append({"sel": sel, "types": types, "imp": imp})
        return out

    def _class(self, addr, sel_base):
        raw = self.c.u64(addr + 32)
        if not raw:
            return None
        data = (((raw & ~DELTA_MASK & 0xFFFFFFFFFFFFFFFF) + VALUE_ADD)) & ~7
        name_p = self.c.ptr(data + 24)
        name = self.c.cstr(name_p, 512) if name_p else None
        if not name or not name.isprintable():
            return None
        return name, self._methods(self.c.ptr(data + 32), sel_base)

    def _selector_offsets(self, addr, size, want=48):
        """Collect raw selector offsets from a few method lists, for use in
        calibrating the cache-wide selector pool base."""
        out = []
        for i in range(size // 8):
            if len(out) >= want:
                break
            cp = self.c.ptr(addr + i * 8)
            if not cp:
                continue
            raw = self.c.u64(cp + 32)
            if not raw:
                continue
            data = (((raw & ~DELTA_MASK & 0xFFFFFFFFFFFFFFFF) + VALUE_ADD)) & ~7
            ml = self.c.ptr(data + 32)
            if not ml:
                continue
            hdr = self.c.read(ml, 8)
            if not hdr or len(hdr) < 8:
                continue
            ef, count = struct.unpack("<2I", hdr)
            if not (ef & SMALL_METHODS and ef & DIRECT_SELECTOR):
                continue
            entsize = ef & 0xFFFC
            for j in range(min(count, 8)):
                f = self.c.read(ml + 8 + j * entsize, 4)
                if f and len(f) == 4:
                    out.append(struct.unpack("<i", f)[0])
        return out

    def objc_classes(self):
        loc = (self.sects.get(("__DATA_CONST", "__objc_classlist"))
               or self.sects.get(("__DATA", "__objc_classlist"))
               or self.sects.get("__objc_classlist"))
        if not loc:
            return []
        addr, size = loc
        sel_base = self.c.selector_base(self._selector_offsets(addr, size))
        out = []
        for i in range(size // 8):
            cp = self.c.ptr(addr + i * 8)
            if not cp:
                continue
            got = self._class(cp, sel_base)
            if not got:
                continue
            name, methods = got
            cls = {"name": name, "methods": methods, "class_methods": []}
            isa = self.c.ptr(cp)
            if isa:
                meta = self._class(isa, sel_base)
                if meta and meta[0] == name:
                    cls["class_methods"] = meta[1]
            out.append(cls)
        return out

    # --- exports ---------------------------------------------------------
    def exports(self):
        if not self.export_addr or not self.export_size:
            return []
        trie = self.c.read(self.export_addr, self.export_size)
        if not trie:
            return []
        out = []

        def uleb(p):
            r = s = 0
            while True:
                b = trie[p]; p += 1
                r |= (b & 0x7F) << s
                if not b & 0x80:
                    return r, p
                s += 7

        def walk(p, prefix, depth=0):
            if p >= len(trie) or depth > 64:
                return
            info_len, p = uleb(p)
            if info_len:
                out.append(prefix)
                p += info_len
            nkids = trie[p]; p += 1
            for _ in range(nkids):
                e = trie.index(b"\0", p)
                seg = trie[p:e].decode(errors="replace")
                p = e + 1
                child, p = uleb(p)
                walk(child, prefix + seg, depth + 1)

        walk(0, "")
        return sorted(out)


def pick(cache, pattern):
    rx = re.compile(pattern, re.I)
    return [(a, p) for a, p in cache.images() if rx.search(p)]


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    cmd, path = sys.argv[1], sys.argv[2]
    pat = sys.argv[3] if len(sys.argv) > 3 and not sys.argv[3].startswith("-") else None
    as_json = "--json" in sys.argv
    c = Cache(path)

    if cmd == "info":
        sb = c.selector_base()
        print(f"magic          : {c.magic}")
        print(f"uuid           : {c.uuid}")
        print(f"subcache files : {len(c.mm)}")
        print(f"mappings       : {len(c.maps)}")
        print(f"images         : {len(c.images())}")
        print(f"selector pool  : {'0x%x' % sb if sb else 'not found'}")
        for a, s, fo, p in c.maps[:4]:
            print(f"  0x{a:012x} +0x{s:09x}  {os.path.basename(p)}")
        print(f"  ... {len(c.maps) - 4} more")

    elif cmd == "list":
        for a, p in c.images():
            if not pat or re.search(pat, p, re.I):
                print(f"0x{a:012x}  {p}")

    elif cmd in ("objc", "symbols", "plan"):
        imgs = pick(c, pat) if pat else c.images()
        if not imgs:
            raise SystemExit(f"no image matches {pat!r}")
        agg = {"classes": 0, "methods": 0, "typed": 0, "syms": 0, "objc_syms": 0}
        for addr, p in imgs:
            d = Dylib(c, addr, p)
            if cmd == "symbols":
                for s in d.exports():
                    print(s)
                continue
            classes = d.objc_classes()
            if cmd == "objc":
                if as_json:
                    print(json.dumps({"image": p, "classes": classes}, indent=1))
                else:
                    for cl in classes:
                        print(f"@interface {cl['name']}")
                        for m in cl["class_methods"]:
                            print(f"  + {m['sel']:<52} {m['types']}")
                        for m in cl["methods"]:
                            print(f"  - {m['sel']:<52} {m['types']}")
                continue
            syms = d.exports()
            ms = [m for cl in classes for m in cl["methods"] + cl["class_methods"]]
            agg["classes"] += len(classes)
            agg["methods"] += len(ms)
            agg["typed"] += sum(1 for m in ms if m["types"])
            agg["syms"] += len(syms)
            agg["objc_syms"] += sum(1 for s in syms if s.startswith(("_OBJC_", "OBJC_")))
        if cmd == "plan":
            print(f"images          : {len(imgs)}")
            print(f"ObjC classes    : {agg['classes']}")
            print(f"ObjC methods    : {agg['methods']}")
            print(f"  型情報あり     : {agg['typed']}  <- thunk 自動生成可能")
            print(f"exported symbols: {agg['syms']}")
            print(f"  ObjC metadata : {agg['objc_syms']}")
            print(f"  C functions   : {agg['syms'] - agg['objc_syms']}  <- 手書き/ヘッダ生成")
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
