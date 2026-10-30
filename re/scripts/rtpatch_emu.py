"""Apply an RTPatch 7 patch to a directory by running RTPatchApply32 under CPU emulation (PATCHES.md).

usage: python3 -I rtpatch_emu.py <patchw32.dll> <patch .exe or .rtp> <target dir> [RTPatch options...]
       e.g. options -NOBACKUP -NOUNDO -NOCONFIRM, or -LIST to only list the patch's entries.
       python3 -I rtpatch_emu.py --header <patch .exe or .rtp>

patchw32.dll is the engine the game installs in AutoPatch/ (Pocket Soft's; never committed). A
self-applying patch .exe is a stub with the patch file appended after its last section; only that
overlay is read. Nothing from either runs natively: Win32 calls are served by Python, the target
appears as C:\\GAME and a sandbox <target>/../_rtp holds C:\\TEMP; CreateProcess is refused.
The DLL's messages go to stderr; a JSON log of file operations goes to <target>/../rtp-log.json.
Needs unicorn and pefile.
"""
import os, re, struct, sys, shutil, json
import pefile
from unicorn import *
from unicorn.x86_const import *

STUB_BASE = 0x00100000
STACK_BASE, STACK_SIZE = 0x00800000, 0x00200000
HEAP_BASE, HEAP_SIZE = 0x20000000, 0x40000000
TEB = 0x00700000
GDT = 0x00710000

LOG = sys.stderr

A = {}
def api(n, nargs):
    def deco(f):
        A[n] = (nargs, f)
        return f
    return deco


class Emu:
    def __init__(self, dll, root, patch_host):
        self.uc = uc = Uc(UC_ARCH_X86, UC_MODE_32)
        self.pe = pe = pefile.PE(dll)
        self.base = pe.OPTIONAL_HEADER.ImageBase
        size = (pe.OPTIONAL_HEADER.SizeOfImage + 0xfff) & ~0xfff
        uc.mem_map(self.base, size)
        uc.mem_write(self.base, pe.get_memory_mapped_image()[:size])
        uc.mem_map(STUB_BASE, 0x10000)
        uc.mem_map(STACK_BASE, STACK_SIZE)
        uc.mem_map(HEAP_BASE, HEAP_SIZE)
        uc.mem_map(TEB, 0x10000)
        uc.mem_map(GDT, 0x1000)
        self.heap = HEAP_BASE
        self.stubs, self.stub_by_name = {}, {}
        self.next_stub = STUB_BASE
        self.tls, self.ntls = {}, 0
        self.last_error = 0
        self.handles = {}
        self.next_handle = 0x1000
        self.root = root
        self.patch_host = patch_host
        self.registry = {}
        self.events = []
        self.ncb = {}
        self.views = {}
        self.next_view = 0x60000000
        self.messages = []
        self.cwd = 'C:\\GAME'
        for d in pe.DIRECTORY_ENTRY_IMPORT:
            for imp in d.imports:
                name = imp.name.decode() if imp.name else f'{d.dll.decode()}#{imp.ordinal}'
                uc.mem_write(imp.address, struct.pack('<I', self.stub(name)))
        self.exports = {e.name.decode(): self.base + e.address for e in pe.DIRECTORY_ENTRY_EXPORT.symbols if e.name}
        uc.mem_write(TEB, struct.pack('<I', 0xffffffff))
        uc.mem_write(TEB + 0x18, struct.pack('<I', TEB))
        uc.mem_write(TEB + 0x30, struct.pack('<I', TEB + 0x1000))
        self._setup_fs()
        uc.hook_add(UC_HOOK_CODE, self._stub_hook, begin=STUB_BASE, end=STUB_BASE + 0xffff)
        uc.hook_add(UC_HOOK_MEM_INVALID, self._bad_mem)
        self.env = self.alloc(64)
        uc.mem_write(self.env, b'TEMP=C:\\TEMP\0TMP=C:\\TEMP\0\0')
        self.envw = self.alloc(128)
        uc.mem_write(self.envw, 'TEMP=C:\\TEMP\0\0'.encode('utf-16-le'))
        self.cmdline = self.alloc(256)
        uc.mem_write(self.cmdline, b'C:\\PATCH\\patch.exe\0')
        self.retaddr = STUB_BASE + 0xfff0
        uc.mem_write(self.retaddr, b'\xf4')
        self.empty = self.alloc(16)
        self.cb_ret = self.alloc(1024)

    def _setup_fs(self):
        uc = self.uc
        def desc(base, limit, access, flags):
            return struct.pack('<HHBBBB', limit & 0xffff, base & 0xffff, (base >> 16) & 0xff, access,
                               ((limit >> 16) & 0xf) | (flags << 4), (base >> 24) & 0xff)
        uc.mem_write(GDT + 8 * 1, desc(0, 0xfffff, 0x9b, 0xc))
        uc.mem_write(GDT + 8 * 2, desc(0, 0xfffff, 0x93, 0xc))
        uc.mem_write(GDT + 8 * 3, desc(TEB, 0xfff, 0x93, 0x4))
        uc.reg_write(UC_X86_REG_GDTR, (0, GDT, 0x100, 0x0))
        uc.reg_write(UC_X86_REG_SS, 2 << 3)
        uc.reg_write(UC_X86_REG_DS, 2 << 3)
        uc.reg_write(UC_X86_REG_ES, 2 << 3)
        uc.reg_write(UC_X86_REG_FS, 3 << 3)

    def _bad_mem(self, uc, access, addr, size, value, ud):
        eip = uc.reg_read(UC_X86_REG_EIP)
        print(f'bad mem access {access} at {addr:#x} size {size} eip {eip:#x}', file=LOG)
        return False

    def stub(self, name, nargs=None):
        if name in self.stub_by_name:
            return self.stub_by_name[name]
        a = self.next_stub
        self.next_stub += 8
        n = nargs if nargs is not None else (A[name][0] if name in A else -1)
        self.uc.mem_write(a, b'\xc3' if n < 0 else b'\xc2' + struct.pack('<H', 4 * n))
        self.stubs[a] = name
        self.stub_by_name[name] = a
        return a

    def alloc(self, n, zero=True):
        a = (self.heap + 15) & ~15
        self.heap = a + max(n, 1) + 16
        if self.heap > HEAP_BASE + HEAP_SIZE:
            raise MemoryError
        if zero and n:
            self.uc.mem_write(a, b'\0' * n)
        return a

    def r32(self, a): return struct.unpack('<I', self.uc.mem_read(a, 4))[0]
    def w32(self, a, v): self.uc.mem_write(a, struct.pack('<I', v & 0xffffffff))
    def rd(self, a, n): return bytes(self.uc.mem_read(a, n))
    def wr(self, a, b): self.uc.mem_write(a, bytes(b))

    def cstr(self, a):
        if not a: return None
        out = bytearray()
        while True:
            c = self.uc.mem_read(a, 64)
            i = c.find(0)
            if i >= 0:
                out += c[:i]; return out.decode('latin1')
            out += c; a += 64

    def wstr(self, a):
        if not a: return None
        out = []
        while True:
            c = struct.unpack('<H', self.uc.mem_read(a, 2))[0]
            if c == 0: return ''.join(out)
            out.append(chr(c)); a += 2

    def put_cstr(self, a, s, cap=None):
        b = s.encode('latin1') + b'\0'
        if cap is not None and len(b) > cap:
            return len(b)
        self.wr(a, b)
        return len(b) - 1

    def put_wstr(self, a, s, cap=None):
        b = s.encode('utf-16-le') + b'\0\0'
        if cap is not None and len(b) > 2 * cap:
            return len(s) + 1
        self.wr(a, b)
        return len(s)

    def _stub_hook(self, uc, addr, size, ud):
        if addr == self.retaddr:
            return
        name = self.stubs.get(addr)
        esp = uc.reg_read(UC_X86_REG_ESP)
        args = [self.r32(esp + 4 + 4 * i) for i in range(12)]
        if name not in A:
            print(f'UNIMPLEMENTED {name} called from {self.r32(esp):#x}', file=LOG)
            uc.emu_stop()
            self.failed = True
            return
        r = A[name][1](self, args)
        if os.environ.get('RTP_TRACE'):
            print(f'  {name}({", ".join(hex(x) for x in args[:max(A[name][0], 0)])}) = {r if r is None else hex(r & 0xffffffff)}', file=LOG)
        uc.reg_write(UC_X86_REG_EAX, (r or 0) & 0xffffffff)

    # ---- paths
    def host(self, wpath):
        p = wpath.replace('/', '\\')
        if p.upper() == getattr(self, 'patch_win', '').upper():
            return self.patch_host
        if len(p) >= 2 and p[1] == ':':
            if p[0].upper() != 'C':
                return None
            p = p[2:]
        elif not p.startswith('\\'):
            p = self.cwd[2:] + '\\' + p
        parts = []
        for seg in p.split('\\'):
            if seg in ('', '.'): continue
            if seg == '..':
                if parts: parts.pop()
                continue
            parts.append(seg)
        cur = self.root
        for seg in parts:
            nxt = os.path.join(cur, seg)
            if not os.path.exists(nxt) and os.path.isdir(cur):
                for e in os.listdir(cur):
                    if e.lower() == seg.lower():
                        nxt = os.path.join(cur, e); break
            cur = nxt
        return cur

    def newh(self, obj):
        h = self.next_handle
        self.next_handle += 4
        self.handles[h] = obj
        return h

    def call(self, addr, *args):
        uc = self.uc
        esp = STACK_BASE + STACK_SIZE - 0x1000
        for v in reversed(args):
            esp -= 4; self.w32(esp, v)
        esp -= 4; self.w32(esp, self.retaddr)
        uc.reg_write(UC_X86_REG_ESP, esp)
        uc.reg_write(UC_X86_REG_EBP, 0)
        self.failed = False
        uc.emu_start(addr, self.retaddr)
        return uc.reg_read(UC_X86_REG_EAX)


# ---------------- formatting
def fmt(e, f, argp, wide_args=False):
    out = []
    i = 0
    def nxt():
        nonlocal argp
        v = e.r32(argp); argp += 4; return v
    while i < len(f):
        c = f[i]
        if c != '%':
            out.append(c); i += 1; continue
        m = re.match(r'%([-+ #0]*)(\d+|\*)?(?:\.(\d+|\*))?(hh|h|ll|l|I64)?([diouxXcsSpn%])', f[i:])
        if not m:
            out.append(c); i += 1; continue
        i += m.end()
        flags, width, prec, ln, conv = m.groups()
        if width == '*': width = str(nxt())
        if prec == '*': prec = str(nxt())
        if conv == '%': out.append('%'); continue
        if conv in 'di':
            v = nxt(); v = v - (1 << 32) if v & 0x80000000 else v; s = str(v)
        elif conv == 'u': s = str(nxt())
        elif conv == 'x': s = '%x' % nxt()
        elif conv == 'X': s = '%X' % nxt()
        elif conv == 'o': s = '%o' % nxt()
        elif conv == 'p': s = '%08X' % nxt()
        elif conv == 'c': s = chr(nxt() & 0xff)
        elif conv == 's':
            p = nxt(); s = (e.wstr(p) if ln == 'l' else e.cstr(p)) if p else '(null)'
            if prec: s = s[:int(prec)]
        elif conv == 'S':
            p = nxt(); s = e.wstr(p) if p else '(null)'
        else:
            s = ''
        if prec and conv in 'diuxXo':
            s = s.zfill(int(prec))
        if width:
            w = int(width)
            if '-' in flags: s = s.ljust(w)
            elif '0' in flags and conv in 'diuxXo': s = s.zfill(w)
            else: s = s.rjust(w)
        out.append(s)
    return ''.join(out)


# ---------------- APIs
def ret_const(v):
    return lambda e, a: v

for n, k, v in [
    ('GetCurrentProcessId', 0, 0x100), ('GetCurrentProcess', 0, 0xffffffff), ('GetCurrentThread', 0, 0xfffffffe),
    ('GetOEMCP', 0, 437), ('GetACP', 0, 1252), ('SetErrorMode', 1, 0), ('SetFileApisToANSI', 0, 0),
    ('SetFileApisToOEM', 0, 0), ('AreFileApisANSI', 0, 1), ('GetLogicalDrives', 0, 0x4), ('FlushFileBuffers', 1, 1),
    ('ReleaseMutex', 1, 1), ('WaitForSingleObject', 2, 0), ('SetStdHandle', 2, 1), ('GetStdHandle', 1, 0xffffffff),
    ('GetFileType', 1, 1), ('CoInitialize', 1, 0), ('CoUninitialize', 0, 0), ('TranslateMessage', 1, 0),
    ('PeekMessageA', 5, 0), ('DispatchMessageA', 1, 0), ('FreeLibrary', 1, 1), ('SetFileTime', 4, 1),
    ('GetFileVersionInfoSizeA', 2, 0), ('GetFileVersionInfoSizeW', 2, 0), ('GetFileVersionInfoA', 4, 0),
    ('GetFileVersionInfoW', 4, 0), ('VerQueryValueA', 4, 0), ('ImpersonateSelf', 1, 0), ('RevertToSelf', 0, 1),
    ('OpenThreadToken', 4, 0), ('OpenProcessToken', 3, 0), ('SetFileSecurityW', 3, 1), ('GetFileSecurityW', 5, 0),
    ('DdeInitializeA', 4, 1), ('DdeUninitialize', 1, 1), ('DdeConnect', 4, 0), ('DdeDisconnect', 1, 1),
    ('DdeCreateStringHandleA', 3, 0), ('DdeFreeStringHandle', 2, 1), ('DdeClientTransaction', 8, 0),
    ('DdeCreateDataHandle', 7, 0), ('SetEnvironmentVariableA', 2, 1), ('SetEnvironmentVariableW', 2, 1),
    ('FreeSid', 1, 0), ('GetSystemDefaultLangID', 0, 0x409),
]:
    A[n] = (k, ret_const(v))

@api('GetVersion', 0)
def _(e, a): return 0x0a280105
@api('GetCommandLineA', 0)
def _(e, a): return e.cmdline
@api('GetEnvironmentStrings', 0)
def _(e, a): return e.env
@api('GetEnvironmentStringsW', 0)
def _(e, a): return e.envw
@api('GetStartupInfoA', 1)
def _(e, a): e.wr(a[0], struct.pack('<I', 68) + b'\0' * 64); return 0
@api('GetCPInfo', 2)
def _(e, a): e.wr(a[1], struct.pack('<I', 1) + b'?\0' + b'\0' * 12); return 1
@api('GetLastError', 0)
def _(e, a): return e.last_error
@api('MulDiv', 3)
def _(e, a):
    s = lambda v: v - (1 << 32) if v & 0x80000000 else v
    return int(s(a[0]) * s(a[1]) / s(a[2])) if a[2] else -1
@api('GlobalAlloc', 2)
def _(e, a): return e.alloc(a[1])
@api('LocalAlloc', 2)
def _(e, a): return e.alloc(a[1])
@api('GlobalFree', 1)
def _(e, a): return 0
@api('LocalFree', 1)
def _(e, a): return 0
@api('VirtualAlloc', 4)
def _(e, a): return a[0] if a[0] else e.alloc(a[1])
@api('VirtualFree', 3)
def _(e, a): return 1
@api('GetModuleHandleA', 1)
def _(e, a):
    n = e.cstr(a[0]) if a[0] else None
    if n is None or 'patchw32' in n.lower(): return e.base
    return 0x7c800000 if 'kernel32' in n.lower() else 0x7e000000
@api('LoadLibraryA', 1)
def _(e, a):
    print(f'LoadLibraryA({e.cstr(a[0])})', file=LOG); return 0x7e000000
@api('GetProcAddress', 2)
def _(e, a):
    nm = e.cstr(a[1]) if a[1] > 0xffff else f'#{a[1]}'
    print(f'GetProcAddress({nm})', file=LOG)
    if nm in A: return e.stub(nm)
    return 0
@api('GetModuleFileNameA', 3)
def _(e, a):
    s = 'C:\\PATCH\\patchw32.dll' if a[0] in (0, e.base) else 'C:\\WINDOWS\\system32\\x.dll'
    if a[0] == 0: s = 'C:\\PATCH\\patch.exe'
    e.wr(a[1], (s.encode() + b'\0')[:a[2]]); return len(s)
@api('LoadStringA', 4)
def _(e, a): return 0
@api('FindResourceA', 3)
def _(e, a):
    name = a[1] if a[1] < 0x10000 else e.cstr(a[1]); typ = a[2] if a[2] < 0x10000 else e.cstr(a[2])
    for t in e.pe.DIRECTORY_ENTRY_RESOURCE.entries:
        if t.id != typ: continue
        for en in t.directory.entries:
            if en.id == name:
                return e.base + en.directory.entries[0].data.struct.OffsetToData
    print(f'FindResourceA({name!r}, {typ!r}) not found', file=LOG)
    return 0
@api('LoadResource', 2)
def _(e, a): return a[1]
@api('LockResource', 1)
def _(e, a): return a[0]
@api('GetSystemInfo', 1)
def _(e, a):
    e.wr(a[0], struct.pack('<IIIIIIIIHH', 0, 0x1000, 0x10000, 0x7ffeffff, 1, 1, 586, 0x10000, 6, 0x0801)); return 0
@api('GetVersionExA', 1)
def _(e, a): e.wr(a[0] + 4, struct.pack('<IIII', 5, 1, 2600, 2)); return 1
@api('CreateMutexA', 3)
def _(e, a): e.last_error = 0; return e.newh(('mutex',))
@api('CloseHandle', 1)
def _(e, a):
    o = e.handles.pop(a[0], None)
    if o and o[0] == 'file': o[1].close()
    return 1
@api('lstrcmpiA', 2)
def _(e, a):
    x, y = e.cstr(a[0]).lower(), e.cstr(a[1]).lower()
    return (x > y) - (x < y)
@api('wsprintfA', -1)
def _(e, a):
    esp = e.uc.reg_read(UC_X86_REG_ESP)
    s = fmt(e, e.cstr(a[1]), esp + 12)
    return e.put_cstr(a[0], s)
@api('wvsprintfA', 3)
def _(e, a):
    s = fmt(e, e.cstr(a[1]), a[2])
    return e.put_cstr(a[0], s)
@api('OemToCharA', 2)
def _(e, a):
    if a[0] != a[1]: e.put_cstr(a[1], e.cstr(a[0]))
    return 1
@api('CharToOemA', 2)
def _(e, a):
    if a[0] != a[1]: e.put_cstr(a[1], e.cstr(a[0]))
    return 1
@api('MultiByteToWideChar', 6)
def _(e, a):
    n = a[3] if a[3] != 0xffffffff else len(e.cstr(a[2]).encode('latin1')) + 1
    src = e.rd(a[2], n)
    if a[5] == 0: return len(src)
    e.wr(a[4], src[:a[5]].decode('latin1').encode('utf-16-le')); return min(len(src), a[5])
@api('WideCharToMultiByte', 8)
def _(e, a):
    if a[3] == 0xffffffff:
        s = e.wstr(a[2]) + '\0'
    else:
        s = e.rd(a[2], 2 * a[3]).decode('utf-16-le')
    out = s.encode('latin1', 'replace')
    if a[5] == 0: return len(out)
    e.wr(a[4], out[:a[5]]); return min(len(out), a[5])
@api('GetTimeZoneInformation', 1)
def _(e, a): e.wr(a[0], b'\0' * 172); return 0
def _systime(e, p):
    e.wr(p, struct.pack('<8H', 2026, 1, 4, 1, 12, 0, 0, 0))
@api('GetLocalTime', 1)
def _(e, a): _systime(e, a[0]); return 0
@api('GetSystemTime', 1)
def _(e, a): _systime(e, a[0]); return 0
EPOCH = 116444736000000000
@api('FileTimeToSystemTime', 2)
def _(e, a):
    import datetime
    ft = struct.unpack('<Q', e.rd(a[0], 8))[0]
    t = datetime.datetime(1601, 1, 1) + datetime.timedelta(microseconds=ft // 10)
    e.wr(a[1], struct.pack('<8H', t.year, t.month, t.isoweekday() % 7, t.day, t.hour, t.minute, t.second, t.microsecond // 1000)); return 1
@api('SystemTimeToFileTime', 2)
def _(e, a):
    import datetime
    y, mo, dw, d, h, mi, s, ms = struct.unpack('<8H', e.rd(a[0], 16))
    t = datetime.datetime(y, mo, d, h, mi, s, ms * 1000) - datetime.datetime(1601, 1, 1)
    ft = (t.days * 86400 + t.seconds) * 10000000 + t.microseconds * 10
    e.wr(a[1], struct.pack('<Q', ft)); return 1
@api('FileTimeToLocalFileTime', 2)
def _(e, a): e.wr(a[1], e.rd(a[0], 8)); return 1
@api('LocalFileTimeToFileTime', 2)
def _(e, a): e.wr(a[1], e.rd(a[0], 8)); return 1

# ---- directories and paths
@api('GetWindowsDirectoryA', 2)
def _(e, a): return e.put_cstr(a[0], 'C:\\WINDOWS')
@api('GetSystemDirectoryA', 2)
def _(e, a): return e.put_cstr(a[0], 'C:\\WINDOWS\\SYSTEM32')
@api('GetTempPathA', 2)
def _(e, a): return e.put_cstr(a[1], 'C:\\TEMP\\')
@api('GetCurrentDirectoryA', 2)
def _(e, a): return e.put_cstr(a[1], e.cwd)
@api('GetCurrentDirectoryW', 2)
def _(e, a): return e.put_wstr(a[1], e.cwd)
@api('SetCurrentDirectoryA', 1)
def _(e, a):
    p = e.cstr(a[0]); print(f'SetCurrentDirectory({p})', file=LOG)
    e.cwd = full(e, p); return 1
@api('SetCurrentDirectoryW', 1)
def _(e, a):
    e.cwd = full(e, e.wstr(a[0])); return 1

def full(e, p):
    p = p.replace('/', '\\')
    if len(p) >= 2 and p[1] == ':':
        if len(p) == 2 or p[2] != '\\': p = p[:2] + '\\' + p[2:]
    elif p.startswith('\\'):
        p = 'C:' + p
    else:
        p = e.cwd.rstrip('\\') + '\\' + p
    parts = []
    for seg in p[3:].split('\\'):
        if seg in ('', '.'): continue
        if seg == '..':
            if parts: parts.pop()
            continue
        parts.append(seg)
    return p[:3] + '\\'.join(parts)

def _fullpath(e, a, wide):
    p = e.wstr(a[0]) if wide else e.cstr(a[0])
    r = full(e, p)
    n = e.put_wstr(a[2], r, a[1]) if wide else e.put_cstr(a[2], r, a[1])
    if a[3]:
        i = r.rfind('\\') + 1
        e.w32(a[3], a[2] + (2 * i if wide else i))
    return n
@api('GetFullPathNameA', 4)
def _(e, a): return _fullpath(e, a, False)
@api('GetFullPathNameW', 4)
def _(e, a): return _fullpath(e, a, True)
@api('GetShortPathNameA', 3)
def _(e, a): return e.put_cstr(a[1], e.cstr(a[0]), a[2])
@api('GetShortPathNameW', 3)
def _(e, a): return e.put_wstr(a[1], e.wstr(a[0]), a[2])
@api('GetDriveTypeA', 1)
def _(e, a): return 3
@api('GetDriveTypeW', 1)
def _(e, a): return 3
@api('GetDiskFreeSpaceA', 5)
def _(e, a):
    for p, v in zip(a[1:5], (8, 512, 0x100000, 0x200000)):
        if p: e.w32(p, v)
    return 1
@api('GetDiskFreeSpaceExA', 4)
def _(e, a):
    for p in a[1:4]:
        if p: e.wr(p, struct.pack('<Q', 1 << 34))
    return 1
@api('GetVolumeInformationA', 8)
def _(e, a):
    if a[1]: e.put_cstr(a[1], 'GAME')
    if a[3]: e.w32(a[3], 0x12345678)
    if a[4]: e.w32(a[4], 255)
    if a[5]: e.w32(a[5], 0x3)
    if a[6]: e.put_cstr(a[6], 'NTFS')
    return 1

def _attrs(e, p):
    h = e.host(p)
    if os.environ.get('RTP_TRACE'): print(f'    attrs {p}', file=LOG)
    if h is None or not os.path.exists(h):
        e.last_error = 2; return 0xffffffff
    return 0x10 if os.path.isdir(h) else 0x20
@api('GetFileAttributesA', 1)
def _(e, a): return _attrs(e, e.cstr(a[0]))
@api('GetFileAttributesW', 1)
def _(e, a): return _attrs(e, e.wstr(a[0]))
@api('SetFileAttributesA', 2)
def _(e, a): return 1 if os.path.exists(e.host(e.cstr(a[0]))) else 0
@api('SetFileAttributesW', 2)
def _(e, a): return 1 if os.path.exists(e.host(e.wstr(a[0]))) else 0

def _mkdir(e, p):
    e.events.append(('mkdir', p))
    h = e.host(p)
    if os.path.exists(h): e.last_error = 183; return 0
    os.makedirs(h); return 1
@api('CreateDirectoryA', 2)
def _(e, a): return _mkdir(e, e.cstr(a[0]))
@api('CreateDirectoryW', 2)
def _(e, a): return _mkdir(e, e.wstr(a[0]))
def _rmdir(e, p):
    e.events.append(('rmdir', p))
    try: os.rmdir(e.host(p)); return 1
    except OSError: e.last_error = 145; return 0
@api('RemoveDirectoryA', 1)
def _(e, a): return _rmdir(e, e.cstr(a[0]))
@api('RemoveDirectoryW', 1)
def _(e, a): return _rmdir(e, e.wstr(a[0]))

def _delete(e, p):
    e.events.append(('delete', p))
    h = e.host(p)
    if not os.path.isfile(h): e.last_error = 2; return 0
    os.remove(h); return 1
@api('DeleteFileA', 1)
def _(e, a): return _delete(e, e.cstr(a[0]))
@api('DeleteFileW', 1)
def _(e, a): return _delete(e, e.wstr(a[0]))

def _move(e, s, d, flags=0):
    e.events.append(('move', s, d, flags))
    hs = e.host(s)
    if not os.path.exists(hs): e.last_error = 2; return 0
    if d is None:
        os.remove(hs); return 1
    hd = e.host(d)
    if os.path.exists(hd):
        if not flags & 1: e.last_error = 183; return 0
        os.remove(hd)
    shutil.move(hs, hd); return 1
@api('MoveFileA', 2)
def _(e, a): return _move(e, e.cstr(a[0]), e.cstr(a[1]))
@api('MoveFileW', 2)
def _(e, a): return _move(e, e.wstr(a[0]), e.wstr(a[1]))
@api('MoveFileExA', 3)
def _(e, a): return _move(e, e.cstr(a[0]), e.cstr(a[1]) if a[1] else None, a[2])
@api('MoveFileExW', 3)
def _(e, a): return _move(e, e.wstr(a[0]), e.wstr(a[1]) if a[1] else None, a[2])
def _copy(e, s, d, fail):
    e.events.append(('copy', s, d))
    hs, hd = e.host(s), e.host(d)
    if not os.path.isfile(hs): e.last_error = 2; return 0
    if fail and os.path.exists(hd): e.last_error = 80; return 0
    shutil.copyfile(hs, hd); return 1
@api('CopyFileA', 3)
def _(e, a): return _copy(e, e.cstr(a[0]), e.cstr(a[1]), a[2])
@api('CopyFileW', 3)
def _(e, a): return _copy(e, e.wstr(a[0]), e.wstr(a[1]), a[2])

# ---- find
def _finddata(e, p, h, wide):
    st = os.stat(h)
    attr = 0x10 if os.path.isdir(h) else 0x20
    ft = EPOCH + int(st.st_mtime * 10000000)
    name = os.path.basename(h)
    base = struct.pack('<IQQQII8x', attr, ft, ft, ft, 0, st.st_size if attr == 0x20 else 0)
    if wide:
        e.wr(p, base + name.encode('utf-16-le')[:518].ljust(520, b'\0') + b'\0' * 28)
    else:
        e.wr(p, base + name.encode('latin1')[:259].ljust(260, b'\0') + b'\0' * 14)

def _findfirst(e, pat, out, wide):
    e.events.append(('find', pat))
    if '*' not in pat and '?' not in pat:
        hp = e.host(pat)
        if hp is None or not os.path.exists(hp): e.last_error = 2; return 0xffffffff
        h = e.newh(['find', [hp], 0, wide])
        _findnext(e, h, out)
        return h
    i = pat.replace('/', '\\').rfind('\\')
    d, mask = (pat[:i] or '\\', pat[i + 1:]) if i >= 0 else ('.', pat)
    hd = e.host(d)
    if hd is None or not os.path.isdir(hd): e.last_error = 3; return 0xffffffff
    rx = re.compile('^' + re.escape(mask).replace(r'\*', '.*').replace(r'\?', '.') + '$', re.I)
    if mask in ('*.*',): rx = re.compile('.*')
    names = ['.', '..'] if len(d.rstrip('\\')) > 2 else []
    names += sorted(os.listdir(hd))
    hits = [os.path.join(hd, n) for n in names if rx.match(n) and not n.startswith('._') and n != '.DS_Store']
    if not hits: e.last_error = 2; return 0xffffffff
    h = e.newh(['find', hits, 0, wide])
    _findnext(e, h, out)
    return h
def _findnext(e, h, out):
    o = e.handles[h]
    if o[2] >= len(o[1]): e.last_error = 18; return 0
    path = o[1][o[2]]; o[2] += 1
    _finddata(e, out, path, o[3])
    if path.endswith('/.') or path.endswith('/..'):
        n = os.path.basename(path)
        nm = n.encode('utf-16-le') + b'\0\0' if o[3] else n.encode() + b'\0'
        e.wr(out + 44, nm)
    return 1
@api('FindFirstFileA', 2)
def _(e, a): return _findfirst(e, e.cstr(a[0]), a[1], False)
@api('FindFirstFileW', 2)
def _(e, a): return _findfirst(e, e.wstr(a[0]), a[1], True)
@api('FindNextFileA', 2)
def _(e, a): return _findnext(e, a[0], a[1])
@api('FindNextFileW', 2)
def _(e, a): return _findnext(e, a[0], a[1])
@api('FindClose', 1)
def _(e, a): e.handles.pop(a[0], None); return 1

# ---- files
def _create(e, p, access, disp):
    if os.environ.get('RTP_TRACE'): print(f'    create {p} {access:#x} {disp}', file=LOG)
    h = e.host(p)
    if h is None: e.last_error = 3; return 0xffffffff
    exists = os.path.isfile(h)
    if os.path.isdir(h): e.last_error = 5; return 0xffffffff
    if disp == 1 and exists: e.last_error = 80; return 0xffffffff     # CREATE_NEW
    if disp in (3, 5) and not exists: e.last_error = 2; return 0xffffffff
    if disp in (1, 2) or (disp == 5) or (disp == 4 and not exists):
        e.events.append(('create', p))
        open(h, 'wb').close()
    write = bool(access & 0x40000000)
    if write and h == e.patch_host: raise RuntimeError('refusing to write the patch file')
    f = open(h, 'r+b' if write else 'rb')
    e.last_error = 183 if (disp in (2, 4) and exists) else 0
    return e.newh(['file', f, p])
@api('CreateFileA', 7)
def _(e, a): return _create(e, e.cstr(a[0]), a[1], a[4])
@api('CreateFileW', 7)
def _(e, a): return _create(e, e.wstr(a[0]), a[1], a[4])
@api('ReadFile', 5)
def _(e, a):
    o = e.handles.get(a[0])
    if not o or o[0] != 'file': e.last_error = 6; return 0
    d = o[1].read(a[2])
    if d: e.wr(a[1], d)
    if a[3]: e.w32(a[3], len(d))
    return 1
@api('WriteFile', 5)
def _(e, a):
    o = e.handles.get(a[0])
    if a[0] in (0xffffffff,) or not o:
        if a[3]: e.w32(a[3], a[2])
        return 1
    o[1].write(e.rd(a[1], a[2]))
    if a[3]: e.w32(a[3], a[2])
    return 1
@api('SetFilePointer', 4)
def _(e, a):
    o = e.handles.get(a[0])
    if not o: e.last_error = 6; return 0xffffffff
    off = a[1] - (1 << 32) if a[1] & 0x80000000 else a[1]
    if a[2]:
        hi = e.r32(a[2]); off = (hi << 32 | a[1]) - ((1 << 64) if hi & 0x80000000 else 0)
    o[1].seek(off, a[3])
    pos = o[1].tell()
    if a[2]: e.w32(a[2], pos >> 32)
    e.last_error = 0
    return pos & 0xffffffff
@api('SetEndOfFile', 1)
def _(e, a):
    o = e.handles[a[0]]; o[1].truncate(o[1].tell()); return 1
@api('GetFileSize', 2)
def _(e, a):
    o = e.handles.get(a[0])
    if not o: return 0xffffffff
    cur = o[1].tell(); o[1].seek(0, 2); n = o[1].tell(); o[1].seek(cur)
    if a[1]: e.w32(a[1], 0)
    return n
@api('CreateFileMappingA', 6)
def _(e, a):
    o = e.handles.get(a[0])
    size = a[3] << 32 | a[4]
    if o and size:
        f = o[1]; cur = f.tell(); f.seek(0, 2)
        if f.tell() < size and a[2] & 0x4:
            f.truncate(size)
        f.seek(cur)
    return e.newh(['mapping', o, bool(a[2] & 0x4)])
VIEW_BASE = 0x60000000
@api('MapViewOfFile', 5)
def _(e, a):
    m = e.handles[a[0]]
    f = m[1][1]
    off = a[2] << 32 | a[3]
    cur = f.tell(); f.seek(0, 2); total = f.tell(); f.seek(off)
    n = a[4] or total - off
    d = f.read(n); f.seek(cur)
    sz = (n + 0xfff) & ~0xfff or 0x1000
    addr = e.next_view
    e.next_view += sz + 0x1000
    if e.next_view > 0xF0000000:
        e.next_view = VIEW_BASE
        addr = e.next_view; e.next_view += sz + 0x1000
    e.uc.mem_map(addr, sz)
    e.wr(addr, d)
    e.views[addr] = (m, off, n, sz)
    return addr
@api('UnmapViewOfFile', 1)
def _(e, a):
    v = e.views.pop(a[0], None)
    if not v: return 0
    m, off, n, sz = v
    if m[2]:
        f = m[1][1]; cur = f.tell(); f.seek(off); f.write(e.rd(a[0], n)); f.seek(cur)
    e.uc.mem_unmap(a[0], sz)
    return 1

# ---- ini / profile
@api('GetPrivateProfileStringA', 6)
def _(e, a):
    print(f'GetPrivateProfileString({e.cstr(a[0])}, {e.cstr(a[1])}, {e.cstr(a[5])})', file=LOG)
    d = e.cstr(a[2]) or ''
    return e.put_cstr(a[3], d[:a[4] - 1])
@api('GetProfileStringA', 5)
def _(e, a):
    d = e.cstr(a[2]) or ''
    return e.put_cstr(a[3], d[:a[4] - 1])
@api('GetPrivateProfileSectionA', 4)
def _(e, a): e.wr(a[1], b'\0\0'); return 0
@api('GetProfileSectionA', 3)
def _(e, a): e.wr(a[1], b'\0\0'); return 0
for n, k in [('WriteProfileStringA', 3), ('WriteProfileSectionA', 2), ('WritePrivateProfileStringA', 4),
             ('WritePrivateProfileSectionA', 3), ('WriteProfileStringW', 3), ('WritePrivateProfileStringW', 4)]:
    def mk(n):
        def f(e, a):
            e.events.append(('ini', n)); print(f'{n} ignored', file=LOG); return 1
        return f
    A[n] = (k, mk(n))

# ---- process
@api('CreateProcessA', 10)
def _(e, a):
    e.events.append(('createprocess', e.cstr(a[0]), e.cstr(a[1])))
    print(f'REFUSED CreateProcessA({e.cstr(a[0])}, {e.cstr(a[1])})', file=LOG)
    e.last_error = 5; return 0
@api('GetExitCodeProcess', 2)
def _(e, a): e.w32(a[1], 0); return 1
@api('ExitProcess', 1)
def _(e, a):
    print('ExitProcess', file=LOG); e.uc.emu_stop(); return 0
@api('RaiseException', 4)
def _(e, a):
    print(f'RaiseException({a[0]:#x})', file=LOG); e.uc.emu_stop(); return 0
@api('RtlUnwind', 4)
def _(e, a): return 0

# ---- registry
HK = {0x80000000: 'HKCR', 0x80000001: 'HKCU', 0x80000002: 'HKLM', 0x80000003: 'HKU', 0x80000005: 'HKCC'}
def keyname(e, h, sub):
    base = HK.get(h) or e.handles.get(h, [None, '?'])[1]
    return base + ('\\' + sub if sub else '')
def _regopen(e, a, wide, create):
    sub = (e.wstr(a[1]) if wide else e.cstr(a[1])) if a[1] else ''
    k = keyname(e, a[0], sub)
    e.events.append(('regopen' if not create else 'regcreate', k))
    print(f'Reg{"Create" if create else "Open"}Key({k})', file=LOG)
    if not create and k.lower() not in {x.lower() for x in e.registry_keys()}:
        return 2
    h = e.newh(['key', k])
    return h
def _rk(e):
    ks = set()
    for (k, v) in e.registry:
        parts = k.split('\\')
        for i in range(1, len(parts) + 1): ks.add('\\'.join(parts[:i]))
    return ks
Emu.registry_keys = _rk
@api('RegOpenKeyExA', 5)
def _(e, a):
    h = _regopen(e, a, False, False)
    if h == 2: return 2
    e.w32(a[4], h); return 0
@api('RegOpenKeyExW', 5)
def _(e, a):
    h = _regopen(e, a, True, False)
    if h == 2: return 2
    e.w32(a[4], h); return 0
@api('RegCreateKeyExA', 9)
def _(e, a):
    h = _regopen(e, a, False, True); e.w32(a[7], h)
    if a[8]: e.w32(a[8], 1)
    return 0
@api('RegCreateKeyExW', 9)
def _(e, a):
    h = _regopen(e, a, True, True); e.w32(a[7], h)
    if a[8]: e.w32(a[8], 1)
    return 0
@api('RegCloseKey', 1)
def _(e, a): e.handles.pop(a[0], None); return 0
def _regq(e, a, wide):
    k = keyname(e, a[0], '')
    n = (e.wstr(a[1]) if wide else e.cstr(a[1])) if a[1] else ''
    print(f'RegQueryValue({k}, {n})', file=LOG)
    e.events.append(('regquery', k, n))
    v = None
    for (kk, nn), vv in e.registry.items():
        if kk.lower() == k.lower() and nn.lower() == n.lower(): v = vv
    if v is None: return 2
    typ, data = v
    if a[3]: e.w32(a[3], typ)
    if a[5]:
        cap = e.r32(a[5])
        e.w32(a[5], len(data))
        if a[4]:
            if cap < len(data): return 234
            e.wr(a[4], data)
    return 0
@api('RegQueryValueExA', 6)
def _(e, a): return _regq(e, a, False)
@api('RegQueryValueExW', 6)
def _(e, a): return _regq(e, a, True)
def _regset(e, a, wide):
    k = keyname(e, a[0], '')
    n = (e.wstr(a[1]) if wide else e.cstr(a[1])) if a[1] else ''
    data = e.rd(a[4], a[5])
    e.registry[(k, n)] = (a[3], data)
    e.events.append(('regset', k, n, a[3], data.hex()))
    print(f'RegSetValue({k}, {n}, {a[3]}, {data!r})', file=LOG)
    return 0
@api('RegSetValueExA', 6)
def _(e, a): return _regset(e, a, False)
@api('RegSetValueExW', 6)
def _(e, a): return _regset(e, a, True)
for n, k in [('RegDeleteValueA', 2), ('RegDeleteValueW', 2), ('RegDeleteKeyA', 2), ('RegDeleteKeyW', 2)]:
    def mk(n):
        def f(e, a):
            s = (e.wstr(a[1]) if n.endswith('W') else e.cstr(a[1])) if a[1] else ''
            e.events.append((n, keyname(e, a[0], ''), s)); print(f'{n}({keyname(e, a[0], "")}, {s})', file=LOG); return 0
        return f
    A[n] = (k, mk(n))
for n, k in [('RegEnumKeyA', 4), ('RegEnumKeyW', 4), ('RegEnumKeyExA', 8), ('RegEnumValueA', 8), ('RegEnumValueW', 8)]:
    A[n] = (k, ret_const(259))
@api('RegQueryInfoKeyA', 12)
def _(e, a):
    for p in a[1:12]:
        if p and p != a[1]: e.w32(p, 0)
    return 0

# security: report "access allowed"
@api('AllocateAndInitializeSid', 11)
def _(e, a): e.w32(a[10], e.alloc(64)); return 1
@api('GetLengthSid', 1)
def _(e, a): return 16
for n, k in [('InitializeAcl', 3), ('AddAccessAllowedAce', 4), ('InitializeSecurityDescriptor', 2),
             ('SetSecurityDescriptorOwner', 3), ('SetSecurityDescriptorGroup', 3), ('SetSecurityDescriptorDacl', 4),
             ('IsValidSecurityDescriptor', 1)]:
    A[n] = (k, ret_const(1))
@api('AccessCheck', 8)
def _(e, a):
    e.w32(a[6], a[2]); e.w32(a[7], 1); return 1

# tls (if any)
@api('TlsAlloc', 0)
def _(e, a): e.ntls += 1; return e.ntls
@api('TlsGetValue', 1)
def _(e, a): return e.tls.get(a[0], 0)
@api('TlsSetValue', 2)
def _(e, a): e.tls[a[0]] = a[1]; return 1

CB_NAMES = {}

@api('CALLBACK', 2)
def _(e, a):
    mid, parm = a[0], a[1]
    s = None
    if parm and parm > 0xffff:
        try:
            s = e.cstr(parm)
            if s and not all(32 <= ord(c) < 127 or c in '\r\n\t' for c in s[:200]): s = repr(e.rd(parm, 16))
        except UcError:
            s = None
    e.messages.append((mid, parm if s is None else s))
    e.ncb[mid] = e.ncb.get(mid, 0) + 1
    if mid in (0xd, 0xe) and e.ncb[mid] > 3:
        print('prompt loop; aborting', file=LOG)
        return 0
    if mid not in (5, 0x15):
        print(f'CB {mid:#x}: {s if s is not None else hex(parm)}', file=LOG)
    return e.cb_ret


def overlay(path, out):
    d = open(path, 'rb').read()
    if d[:2] != b'MZ':
        return path
    pe = pefile.PE(data=d, fast_load=True)
    end = max(s.PointerToRawData + s.SizeOfRawData for s in pe.sections)
    open(out, 'wb').write(d[end:])
    return out


def header(path):
    d = open(path, 'rb').read()
    if d[:2] == b'MZ':
        pe = pefile.PE(data=d, fast_load=True)
        d = d[max(s.PointerToRawData + s.SizeOfRawData for s in pe.sections):]
    if d[:2] != b'K*':
        sys.exit('not an RTPatch file')
    new_total, old_total = struct.unpack_from('<II', d, 0x10)
    entries = struct.unpack_from('<H', d, 0x1e)[0]
    # Totals are the summed sizes of the MODIFY entries' new and old files (checked against the trees).
    print(f'{entries} entries, new files {new_total} bytes, old files {old_total} bytes')


def main():
    if sys.argv[1] == '--header':
        return header(sys.argv[2])
    dll, patch, target = sys.argv[1:4]
    opts = sys.argv[4:]
    root = os.path.abspath(target + '/../_rtp')
    if os.path.lexists(root + '/GAME'): os.unlink(root + '/GAME')
    if os.path.exists(root): shutil.rmtree(root)
    os.makedirs(root + '/TEMP'); os.makedirs(root + '/WINDOWS/SYSTEM32'); os.makedirs(root + '/PATCH')
    os.symlink(os.path.abspath(target), root + '/GAME')
    rtp = overlay(os.path.abspath(patch), root + '/patch.rtp')
    e = Emu(dll, root, rtp)
    e.patch_win = 'C:\\PATCH\\PATCH.RTP'
    r = e.call(e.base + e.pe.OPTIONAL_HEADER.AddressOfEntryPoint, e.base, 1, 0)
    print(f'DllMain -> {r}', file=LOG)
    cb = e.stub('CALLBACK')
    cmd = ' '.join(opts + ['C:\\GAME', e.patch_win])
    p = e.alloc(len(cmd) + 1); e.put_cstr(p, cmd)
    r = e.call(e.exports['RTPatchApply32@12'], p, cb, 1)
    print(f'RTPatchApply32 -> {r} failed={e.failed}', file=LOG)
    json.dump({'result': r, 'events': e.events, 'messages': e.messages,
               'registry': {f'{k}|{n}': [t, d.hex()] for (k, n), (t, d) in e.registry.items()}},
              open(os.path.abspath(target + '/../rtp-log.json'), 'w'), indent=1, default=str)
    os.unlink(root + '/GAME')
    shutil.rmtree(root)
    return 0 if r == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
