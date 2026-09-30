"""Generate ABI-preserving lazy tail thunks, including undocumented ordinals.

Ordinal mapping was inspected on Windows ARM64 10.0.26100.9457. Named exports
resolve by name; ordinal-only exports resolve by their original ordinal.
"""
import pathlib
import sys

arch, directory = sys.argv[1:]
out = pathlib.Path(directory)
names = [
    "Direct3DShaderValidatorCreate9", "PSGPError", "PSGPSampleTexture",
    "D3DPERF_BeginEvent", "D3DPERF_EndEvent", "D3DPERF_GetStatus",
    "D3DPERF_QueryRepeatFrame", "D3DPERF_SetMarker", "D3DPERF_SetOptions",
    "D3DPERF_SetRegion", "DebugSetLevel", "DebugSetMute",
    "Direct3D9EnableMaximizedWindowedModeShim",
]
ordinals = [16,17,18,19,22,23]
entries = names + ordinals
header = ["static const char *const forward_names[] = {"]
header += [f'    "{item}",' if isinstance(item,str) else f"    (const char *)(uintptr_t){item}," for item in entries]
header += ["};"]
(out / "forwarders.h").write_text("\n".join(header) + "\n")
public = names + ["Direct3DCreate9", "Direct3DCreate9Ex"]
if arch == "x86_64":
    public = sorted(public)
ordinal_by_name = {name: i + 24 for i,name in enumerate(public)}
exports = ["LIBRARY d3d9", "EXPORTS",
           f"Direct3DCreate9=proxy_Create9 @{ordinal_by_name['Direct3DCreate9']}",
           f"Direct3DCreate9Ex=proxy_Create9Ex @{ordinal_by_name['Direct3DCreate9Ex']}",
           "Direct3DCreate9On12=proxy_Create9On12 @20",
           "Direct3DCreate9On12Ex=proxy_Create9On12Ex @21"]
asm = [".text"]
if arch == "x86_64":
    asm += [".intel_syntax noprefix"]
for i,item in enumerate(entries):
    symbol = f"forward_{i}"
    exports.append(f"{item}={symbol} @{ordinal_by_name[item]}" if isinstance(item,str)
                   else f"ordinal_{item}={symbol} @{item} NONAME")
    asm += [f".globl {symbol}", f".seh_proc {symbol}", f"{symbol}:"]
    if arch == "x86_64":
        # Stack arguments stay in place; save all register arguments across
        # the resolver call, including floating-point parameters.
        asm += ["sub rsp, 168", ".seh_stackalloc 168", ".seh_endprologue"]
        regs = ["rcx", "rdx", "r8", "r9"]
        asm += [f"mov [rsp+{32+j*8}], {r}" for j,r in enumerate(regs)]
        asm += [f"movdqu [rsp+{64+j*16}], xmm{j}" for j in range(4)]
        asm += [f"mov ecx, {i}", "call resolve_forward", "mov r11, rax"]
        asm += [f"mov {r}, [rsp+{32+j*8}]" for j,r in enumerate(regs)]
        asm += [f"movdqu xmm{j}, [rsp+{64+j*16}]" for j in range(4)]
        asm += ["add rsp, 168", "jmp r11"]
    elif arch == "aarch64":
        # x8 is the ARM64 indirect-result register; preserve it as well.
        asm += ["stp x29, x30, [sp, #-224]!", ".seh_save_fplr_x 224", ".seh_endprologue"]
        asm += [f"stp x{j}, x{j+1}, [sp, #{16+j*8}]" for j in range(0,10,2)]
        asm += [f"stp q{j}, q{j+1}, [sp, #{96+j*16}]" for j in range(0,8,2)]
        asm += [f"mov w0, #{i}", "bl resolve_forward", "mov x16, x0"]
        asm += [f"ldp q{j}, q{j+1}, [sp, #{96+j*16}]" for j in range(0,8,2)]
        asm += [f"ldp x{j}, x{j+1}, [sp, #{16+j*8}]" for j in range(0,10,2)]
        asm += [".seh_startepilogue", "ldp x29, x30, [sp], #224", ".seh_save_fplr_x 224", ".seh_endepilogue", "br x16"]
    else:
        raise SystemExit(f"Unsupported architecture: {arch}")
    asm += [".seh_endproc"]
(out / "forwarders.S").write_text("\n".join(asm) + "\n")
(out / "d3d9.def").write_text("\n".join(exports) + "\n")
