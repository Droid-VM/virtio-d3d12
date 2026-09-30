"""Add the built x86 dependency chain to a NEW, uninstalled driver package.

Run sign-gpu-driver.sh after this command and deploy with pnputil. Never
point this script at DriverStore or an installed package directory.
"""
import pathlib
import shutil
import sys

win3d = pathlib.Path(__file__).resolve().parents[2]
package = pathlib.Path(sys.argv[1]).resolve()
if package.parent != win3d / "dist/packages":
    raise SystemExit("Expected a staging directory under win3d/dist/packages")
files = ["dx12um_virtio_x86.dll", "droidvm_vulkan_x86.dll", "vulkan_freedreno_x86.dll"]
inf = package / "virtio_wddm_rs.inf"
text = inf.read_text()
for name in files:
    shutil.copy2(win3d / "dist/d3d12-x86" / name, package / name)
    if f"{name} = 1,," not in text:
        text = text.replace("[SourceDisksFiles]\n", f"[SourceDisksFiles]\n{name} = 1,,\n")
    if f"{name},,,0" not in text:
        text = text.replace("[VirtioGpu_Files.Usermode]\n", f"[VirtioGpu_Files.Usermode]\n{name},,,0\n")
# Slots 0/1/2 are D3D9/10/11. This package adds D3D12 only (slot 3).
import re
text = re.sub(r"(?m)^HKR,, UserModeDriverNameWow,.*\n", "", text)
line = 'HKR,, UserModeDriverNameWow, %REG_MULTI_SZ%, <>,<>,<>,%13%\\dx12um_virtio_x86.dll\n'
text = text.replace("[VirtioGpu_DeviceSettings]\n", "[VirtioGpu_DeviceSettings]\n" + line)
if line not in text:
    raise SystemExit("Missing device settings section")
inf.write_bytes(text.replace("\n", "\r\n").encode("ascii"))
# Invalidate the old catalog: caller must sign the complete package again.
(package / "virtio_wddm_rs.cat").unlink(missing_ok=True)
