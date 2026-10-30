"""Print the build number compiled into an install's modules (PATCHES.md): 2090 on the disc.

usage: python3 -I build_number.py <install dir with Engine/>

OMF's DE_COMF2Manager::Activate and Core's WinSystemInit push it as an immediate; the patch
files' names are not reliable. Needs capstone (via pe_funcdiff).
"""
import os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pe_funcdiff


def build(path, prefix):
    m = pe_funcdiff.load(path)
    for name, (a, b) in pe_funcdiff.functions(m).items():
        if name.startswith(prefix):
            text = ' '.join(pe_funcdiff.listing(m, a, b))
            return sorted({int(x, 16) for x in re.findall(r'0x[0-9a-f]+', text) if 2000 <= int(x, 16) < 3000})
    return []


root = sys.argv[1]
print('OMF', build(os.path.join(root, 'Engine/OMF.ModuleDLL'), '?Activate@DE_COMF2Manager@@'),
      'Core', build(os.path.join(root, 'Engine/Core.ModuleDLL'), '?WinSystemInit@@'))
