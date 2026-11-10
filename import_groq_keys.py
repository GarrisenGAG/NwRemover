#!/usr/bin/env python3
"""Import Groq API keys from a GROQ_API_KEY_N=gsk_... text file into NwRemover.json.
Usage: python import_groq_keys.py [keys_file] [config_file]
Keys are never printed."""
import json, re, sys, os

keys_file = sys.argv[1] if len(sys.argv) > 1 else r"C:\Users\ghaos\Downloads\groq.txt"
cfg_file = sys.argv[2] if len(sys.argv) > 2 else r"D:\AEplugin\NwRemover.json"

text = open(keys_file, "r", encoding="utf-8", errors="ignore").read()
found = []
for m in re.finditer(r"GROQ_API_KEY_\w*\s*=\s*(\S+)", text):
    v = m.group(1).strip()
    # accept only plausible full keys (gsk_ + long body); skip truncated placeholders
    if v.startswith("gsk_") and len(v) >= 20 and v not in found:
        found.append(v)

cfg = {}
if os.path.exists(cfg_file):
    try:
        cfg = json.load(open(cfg_file, "r", encoding="utf-8"))
    except Exception:
        cfg = {}
cfg["groq_api_keys"] = found
with open(cfg_file, "w", encoding="utf-8", newline="\n") as f:
    json.dump(cfg, f, indent=2, ensure_ascii=False)
    f.write("\n")

print("imported keys:", len(found), "-> ", cfg_file)
if not found:
    print("WARNING: no valid gsk_ keys found (file empty or values truncated)")