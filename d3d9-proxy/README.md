# 应用目录级 D3D9 → 系统 D3D9On12 代理

**驱动 1175 起通常不需要本代理。** 驱动 INF 声明无原生 D3D9 UMD，
Windows 会自动使用系统 D3D9On12。x86、ARM64、x64、ARM64EC 的 12 组
无代理定向测试通过；ATRI 移除代理后也已正常显示标题菜单。
已安装本代理的应用可在关闭后用下方 `-Remove` 命令卸载。
详见 [自动选择验证记录](../tests/README.md)。以下保留显式代理的用法。

将本包对应架构的 `d3d9.dll` 放到实际 DX9 程序的 EXE 同目录，重新启动程序。
`Direct3DCreate9` / `Direct3DCreate9Ex` 会启用 Windows 自带的 D3D9On12；
应用的适配器选择、设备参数和 COM 对象均交给系统运行时处理。
显式 `Direct3DCreate9On12/Ex` 调用保留调用者的 overrides 和 queues。

本包针对 Windows ARM64 guest、VirtIO 驱动 1174（64 位也支持 1173）。无需替换系统 DLL，也无需
修改 INF 或注册表。先确认具体程序的架构：

| 程序 | 使用的目录 |
|---|---|
| ARM64 原生 | `aarch64/` |
| x64 | `x86_64/` |
| ARM64EC | `x86_64/`，使用系统支持的 x64 DLL 互操作 |
| x86（32 位） | `i686/`，需要驱动 1174 或更新版本中的 x86 UMD/Vulkan 链路 |

也可以使用架构检测安装脚本，无需管理员权限：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\install.ps1 -Application 'D:\Game\Game.exe'
```

必须选择实际渲染进程的 EXE，不一定是启动器。脚本遇到不同的现有 `d3d9.dll`
会拒绝覆盖，避免破坏已有 DXVK、ReShade 或游戏自带模块。不要放进 System32、
SysWOW64 或 DriverStore。卸载时关闭应用，使用同一个包执行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\install.ps1 -Application 'D:\Game\Game.exe' -Remove
```

删除前会检查 DLL 哈希；也可手动移走此前安装的代理 DLL。
如果应用使用绝对系统路径加载 D3D9、启动其它目录的子进程或拒绝第三方 DLL，
应用目录代理不一定会生效。该包不修改这类程序的加载策略。

## 构建及实现

在 `virtio-d3d12` 中运行 `bash d3d9-proxy/build.sh`，输出位于
`../dist/d3d9-proxy/`。需要 Python 3 和 `/opt/llvm-mingw`，可通过 `TOOLCHAIN`
指定其它工具链。ARM64EC 测试目录复用 x64 代理。

系统 D3D9 按 `GetSystemDirectoryW()` 返回的绝对路径加载，防止同名递归。
初始化使用 InitOnce，在第一次导出调用时发生，不在 DllMain 内加载 DLL。
其余导出使用保存参数寄存器和原始栈参数的汇编尾跳转；不猜测私有导出的签名。
导出表包括公共名称和当前系统版本的 16–19、22–23 号 ordinal-only 入口；
私有入口随 Windows 版本变化，未保证任意版本兼容。

`tests/probe_d3d9_app.cpp` 是普通 D3D9 导入程序，不调用 9On12 创建 API。
在交互式桌面运行 `run-tests.ps1 -Arch aarch64 -Mode legacy`；架构还可选
`x86_64` / `arm64ec`，模式还可选 `ex` / `flip`。
测试检查应用目录模块路径、并发初始化及 D3DPERF 转发、无效 SDK、VirtIO HAL、
9On12 接口、系统运行时/UMD 模块、Reset 前后各 60 帧呈现及屏幕像素。
测试窗口临时置顶，需保持桌面可见。

上述探针不代表所有游戏、独占全屏、长时间运行或所有共享资源格式均兼容；
未运行全量图形测试。ATRI 的实测结果见下文。

## 2026-09-30 验证结果

Windows guest `192.168.1.121`，驱动 `20.15.1.1173`，系统 D3D9On12。
ARM64、x64、ARM64EC × legacy、ex、flip 共 9 组全部通过，均正常退出 0。
每组验证 Reset 前后各 60 帧、两次屏幕像素检查，以及系统 9On12 和 VirtIO
UMD 的实际模块路径。ARM64EC 使用 x64 代理，渲染加载 ARM64EC UMD。
安装、重复安装、卸载、拒绝覆盖不同 DLL 的检查也通过。
原始日志位于 `win3d/debug-20260930/d3d9-proxy-1173/`。

FLIPEX 的底层修复会将满足条件的 simultaneous-access 渲染纹理放在线性
运行时 allocation 中，因为当前 DDI 不直接传递 API 的 SHARED heap 标志。
这可能影响其它同类纹理的布局和性能；此次验证没有进行帧率或长期压力评估。

## x86 支持（驱动 1174）

`bash x86/build.sh` 构建 32 位 D3D12 UMD、Turnip 和 Vulkan loader。
新驱动包需使用 `x86/add-to-package.py` 加入三份 `_x86.dll` 和
`UserModeDriverNameWow` 的 D3D12 槽位，重新完整签名后由 pnputil 安装。
不能将 x64 代理或 UMD 放入 x86 应用目录。

Windows ARM64 guest 192.168.1.121、驱动 1174 的交互桌面验证：
x86 legacy、ex、flip 三组均退出 0；通过系统 D3D9On12/VirtIO x86 UMD
模块检查、Reset 前后呈现及实际屏幕像素检查。非交互 SSH 会话的显式
D3D9On12 初始化曾返回 D3DERR_NOTAVAILABLE，应在交互桌面运行呈现测试。

ATRI Steam build 7599887（32 位 x86）已安装应用目录代理，启动 Logo、
标题菜单的人物、背景和文字正常显示。进程模块确认加载游戏目录 d3d9.dll、
系统 d3d9on12.dll，以及驱动 1174 的 dx12um_virtio_x86.dll、
droidvm_vulkan_x86.dll 和 vulkan_freedreno_x86.dll。用户随后实际测试并确认
“都正常”。自动交互未独立确认剧情推进；未做长期稳定性或帧率评估。
证据：`win3d/debug-20260930/atri-1174/`。测试前的存档备份保留在 guest
`C:\DroidVM\atri-test\save-backup-before-interaction`。
