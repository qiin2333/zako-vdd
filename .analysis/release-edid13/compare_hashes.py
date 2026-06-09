from pathlib import Path
import hashlib
refs = {
 'release_dll': Path('.analysis/release-edid13/extracted/ZakoVDD.dll'),
 'release_inf': Path('.analysis/release-edid13/extracted/ZakoVDD.inf'),
 'release_cat': Path('.analysis/release-edid13/extracted/zakovdd.cat'),
 'release_xml': Path('.analysis/release-edid13/extracted/vdd_settings.xml'),
}
candidates = [
 Path('Virtual Display Driver (HDR)/x64/Release/ZakoVDD/ZakoVDD.dll'),
 Path('Virtual Display Driver (HDR)/x64/Release/ZakoVDD/ZakoVDD.inf'),
 Path('Virtual Display Driver (HDR)/x64/Release/ZakoVDD/zakovdd.cat'),
 Path('Virtual Display Driver (HDR)/x64/Release/ZakoVDD/vdd_settings.xml'),
 Path('Virtual Display Driver (HDR)/ZakoVDD/x64/Release/ZakoVDD/ZakoVDD.dll'),
 Path('Virtual Display Driver (HDR)/ZakoVDD/x64/Release/ZakoVDD/ZakoVDD.inf'),
 Path('Virtual Display Driver (HDR)/ZakoVDD/x64/Release/ZakoVDD/zakovdd.cat'),
]

def sha(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()
ref_hash = {k: sha(v) for k,v in refs.items()}
print('REFERENCE HASHES')
for k,v in ref_hash.items():
    print(k, v)
print('--- MATCH CHECKS ---')
for p in candidates:
    if p.exists():
        h = sha(p)
        matches = [k for k,v in ref_hash.items() if v == h]
        print(p, h, 'MATCH' if matches else '', ','.join(matches))
