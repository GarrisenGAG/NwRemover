#!/usr/bin/env python3
"""Generate MatCutter.rc PiPL resource (Windows, little-endian), matching
the layout produced by Adobe PiPLTool / AE_General.r."""
import struct, sys

def rev4(s):  # 4cc -> reversed bytes as RC string
    return s[::-1]

def qstr(b):  # bytes -> RC quoted string with \xNN escapes
    out = []
    for c in b:
        if 32 <= c < 127 and c not in (34, 92):
            out.append(chr(c))
        else:
            out.append("\\x%02x" % c)
    return '"' + ''.join(out) + '"'

def pad4(b):
    return b + b'\x00' * ((4 - len(b) % 4) % 4)

def pstring(s):
    b = s.encode('ascii')
    return pad4(bytes([len(b)]) + b)

def cstring(s):
    return pad4(s.encode('ascii') + b'\x00')

lines = []
def w(v):  # 32-bit value as two RC words
    lines.append("0x%04x, 0x%04x," % (v & 0xFFFF, (v >> 16) & 0xFFFF))
def raw(s):
    lines.append(s + ",")

def prop(key, data, is_string=False):
    raw(qstr(rev4('8BIM').encode('ascii')))   # vendor "MIB8"
    raw(qstr(rev4(key).encode('ascii')))
    w(0)                                     # ID
    w(len(data))                             # length
    if is_string:
        raw(qstr(data))
    else:
        padded = data + b'\x00' * ((4 - len(data) % 4) % 4)
        for i in range(0, len(padded), 2):
            lines.append("0x%04x," % struct.unpack_from('<H', padded, i)[0])

NAME = "NwRemover"
MATCH = "GH NwRemover"
CATEGORY = "Audio"
VERSION = 525825          # PF_VERSION(1,0,0,RELEASE=3,1)
OUTFLAGS = 0x9C000000     # AUDIO_EFFECT_ONLY | AUDIO_FLOAT_ONLY | AUDIO_IIR | SEND_UPDATE_PARAMS_UI
OUTFLAGS2 = 0x08000000    # SUPPORTS_THREADED_RENDERING
SPEC = (13, 20)

props = []
def add(key, data, is_string=False): props.append((key, data, is_string))

add('kind', rev4('eFKT').encode('ascii'))            # AEEffect
add('name', pstring(NAME), True)
add('catg', pstring(CATEGORY), True)
add('8664', cstring("EffectMain"), True)             # CodeWin64X86
add('ePVR', struct.pack('<HH', 2, 0))                # AE_PiPL_Version
add('eSVR', struct.pack('<HH', *SPEC))               # AE_Effect_Spec_Version
add('eVER', struct.pack('<I', VERSION))
add('eINF', struct.pack('<H', 0))                    # info flags (len 2 + pad)
add('eGLO', struct.pack('<I', OUTFLAGS))
add('eGL2', struct.pack('<I', OUTFLAGS2))
add('eMNA', pstring(MATCH), True)
add('aeFL', struct.pack('<I', 0))                    # reserved info

lines.append("16000 PiPL DISCARDABLE")
lines.append("BEGIN")
lines.append("0x0001,")          # Windows PiPL signature word
w(0)                             # kCurrentPiPLVersion
w(len(props))                    # property count
for key, data, is_string in props:
    prop(key, data, is_string)
lines.append("END")

out = sys.argv[1] if len(sys.argv) > 1 else "NwRemover.rc"
with open(out, 'w', newline='\n') as f:
    f.write('\n'.join(lines) + '\n')
print("wrote", out, "properties:", len(props))