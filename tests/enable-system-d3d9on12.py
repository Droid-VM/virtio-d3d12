"""Prepare a NEW staging driver INF for the system D3D9On12 fallback.

The first UserModeDriverName slot is D3D9. '<>' declares no native driver;
Windows can then select its own D3D9On12 runtime. Do not register the D3D12
UMD as a D3D9 driver. Preserve the D3D10/11/12 slots and all payload files.
The resulting complete package must be signed and installed with pnputil.
"""
import pathlib
import re
import sys

win3d = pathlib.Path(__file__).resolve().parents[2]
package = pathlib.Path(sys.argv[1]).resolve()
if package.parent != win3d / "dist/packages":
    raise SystemExit("Expected a staging directory under win3d/dist/packages")
inf = package / "virtio_wddm_rs.inf"
text = inf.read_text()
for value in ("UserModeDriverName", "UserModeDriverNameWow"):
    pattern = rf"(?m)^(HKR,,\s*{value},\s*%REG_MULTI_SZ%,\s*)([^\r\n]+)$"
    matches = list(re.finditer(pattern, text))
    if len(matches) != 1:
        raise SystemExit(f"Expected exactly one {value}")
    slots = matches[0].group(2).split(',')
    if len(slots) != 4 or not slots[3].strip().endswith('.dll'):
        raise SystemExit(f"Expected four slots with a D3D12 UMD in {value}")
    slots[0] = '<>'
    text = re.sub(pattern, lambda m: m.group(1) + ','.join(slots), text)
inf.write_bytes(text.replace('\n', '\r\n').encode('ascii'))
(package / 'virtio_wddm_rs.cat').unlink(missing_ok=True)
