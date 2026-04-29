# mc-player 硬件解码依赖与故障排查

适用平台：Windows 10 1809+ / Windows 11 x64（含 IoT LTSC）。
本文记录硬件 H.264 / H.265 解码所依赖的系统组件，以及"明明有 GPU 但走不到硬解"这类故障的诊断与修复路径。

---

## 1. 硬件解码链路（ADR-015 四级降级链）

mc-player 的视频解码按 ADR-015 四级降级链选档（`controller.cpp:start_decode_pipeline`），原则是 **硬件 path 最短优先**——host 抽象层数从短到长：

```
[1] Vendor SDK 直驱（NVDEC / Intel oneVPL / AMD AMF）   ← 首选，host 抽象层数 0
       ├── NVIDIA: nvcuvid.dll (NVDEC SDK)
       ├── Intel:  libmfx.dll / vpl.dll (oneVPL)
       └── AMD:    amfrt64.dll (AMF)
       触发: GPU vendor 匹配 + SDK 已下载 (ADR-016)
                   ↓ 失败 / SDK 缺失
[2] DXVA-direct (ID3D11VideoDevice)                    ← 跨 vendor 通用，host 抽象层数 1
       覆盖 H.264 / H.265 Main / H.265 Main10 / AV1 Profile0
       不依赖 MFT category 注册
                   ↓ 失败 / profile 不支持
[3] CodecMftVideo (Windows MFT hardware async)         ← OS 标准抽象兼容档，host 抽象层数 2
       仅 hw_url=1 && async=1 的 MFT 接受
       Microsoft Software MFT (sync) 不算硬解，跳过
                   ↓ 失败 / 仅 sync software MFT 可用
[4] CodecLibcodecVideo (mc-libcodec 自研软解)          ← 最后兜底，CPU SIMD
       allow_software_decode=1 才启用；否则上报 MC_ERR_NO_HARDWARE
```

每一档依赖不同的系统组件，缺哪一档降到下一档。**前一档失败立即降级；运行期错误优先档内复位（flush + 重协商），不可恢复才跨档**。

### 各档依赖的系统组件

| 档 | 关键组件 | 提供者 | 缺失后果 |
|---|---|---|---|
| 1 | `nvcuvid.dll` (NVIDIA) | NVIDIA Video Codec SDK redistributable | 档 1 跳过；ADR-016 内置下载面板提示用户下载 |
| 1 | `libmfx.dll` / `vpl.dll` (Intel) | Intel oneVPL redistributable | 档 1 跳过 |
| 1 | `amfrt64.dll` (AMD) | AMD AMF runtime（GPU 驱动通常自带） | 档 1 跳过 |
| 2 | `d3d11.dll` `ID3D11VideoDevice` | OS + 驱动 | DXVA-direct 不可用；档 2 跳过 |
| 2 | GPU driver 暴露 D3D11VA decoder profile（CheckVideoDecoderProfile） | GPU 驱动 | 该 codec 在档 2 不可用；常见于驱动严重过期 |
| 3 | `mfplat.dll` / `mfreadwrite.dll` | OS 内置 | MF 平台不可用，档 3 全档跳过 |
| 3 | **MFT category 注册表条目**：`HKLM\SOFTWARE\Microsoft\Windows Media Foundation\Transforms\Categories\{d6c02d4b-6833-45b4-971a-05a4b04bab91}` | OS（MediaPlayback feature） | `MFTEnumEx` 返回 0 个 decoder；档 3 跳过 |
| 3 | **Intel / NVIDIA / AMD hardware MFT 注册脚本** | GPU 厂商驱动 + MediaPlayback feature 同时具备时才注册 | hw MFT 不会注册，档 3 跳过（仍可走档 1/2/4） |
| 3 | `mfh264dec.dll` / `mfh265dec.dll`（OS software MFT） | OS / Microsoft HEVC Extension | 即使存在也不被档 3 接受（sync software MFT 由 ADR-002 排除） |
| 4 | mc-player 自带 `mc-libcodec` | 项目内置 | 档 4 不可用 → 全链断 |

---

## 2. Windows IoT LTSC 的特例

Windows IoT Enterprise LTSC 默认**裁剪掉整组 MediaPlayback feature**：

```
PS> Get-WindowsOptionalFeature -Online -FeatureName MediaPlayback
FeatureName     : MediaPlayback
State           : Disabled            ← 默认禁用
```

后果（按 ADR-015 四级降级链评估）：
- MFT category 注册表 key 不存在 → **档 3 整档跳过**
- `mfh264dec.dll` / `mfh265dec.dll` 没装 → 即使存在也不被档 3 接受（sync software MFT 由 ADR-002 排除）
- Intel/AMD/NV 驱动安装时 hw MFT 注册脚本会跳过（因为基础的 MF subsystem 不在）→ 档 3 真硬件 MFT 也没有
- **但档 1 / 档 2 仍可工作**：vendor SDK 不依赖 MFT subsystem；DXVA-direct 仅依赖 `d3d11.dll` + GPU driver 的 D3D11VA DDI，与 MediaPlayback feature 无关
- 旧策略下（ADR-015 之前）会把 OS 兼容老 dll `msmpeg2vdec.dll` 当硬解，造成 +100ms 延时性能瓶颈 —— 新策略已按 ADR-015 排除该路径

### 已观察到的故障表现

`mc-player.log` 启动时输出：

```
CodecMftVideo: MFTEnumEx(ALL) for H264 found 1
  [0] 'Microsoft H264 Video Decoder MFT' hw_url=0 async=0     ← 只 1 个，且是 SW + sync
CodecMftVideo: ActivateObject ok, sync=1                       ← 走的是 sync software MFT
```

正常机器（启用 MediaPlayback + 装 GPU driver）应该看到 2-3 个，至少 1 个 `hw_url=1 async=1`：

```
CodecMftVideo: MFTEnumEx(ALL) for H264 found 3
  [0] 'Intel(R) Quick Sync Video H.264 Decoder MFT' hw_url=1 async=1
  [1] 'NVIDIA H.264 Decoder MFT' hw_url=1 async=1
  [2] 'Microsoft H264 Video Decoder MFT' hw_url=0 async=0
```

---

## 3. 诊断命令清单（PowerShell）

按顺序排查：

### 3.1 GPU 是否被识别
```powershell
Get-CimInstance Win32_VideoController |
    Select-Object Name, DriverVersion, AdapterCompatibility, VideoProcessor |
    Format-List
```
应能看到 Intel/NVIDIA/AMD 真实 GPU；只看到虚拟显卡（如向日葵的 OrayIddDriver）说明 GPU driver 没装。

### 3.2 MediaPlayback feature 状态
```powershell
Get-WindowsOptionalFeature -Online -FeatureName MediaPlayback
Get-WindowsOptionalFeature -Online -FeatureName WindowsMediaPlayer
```

### 3.3 关键解码 DLL 是否存在
```powershell
$dlls = @('msmpeg2vdec.dll', 'mfh264dec.dll', 'mfh265dec.dll', 'mfh264enc.dll')
foreach ($d in $dlls) {
    $sys = Test-Path "$env:WINDIR\System32\$d"
    Write-Output ("{0}: System32={1}" -f $d, $sys)
}
```

期望：`mfh264dec.dll = True`。如果 False → MediaPlayback 缺失。

### 3.4 MFT video decoder 注册表
```powershell
$key = 'HKLM:\SOFTWARE\Microsoft\Windows Media Foundation\Transforms\Categories\{d6c02d4b-6833-45b4-971a-05a4b04bab91}'
if (Test-Path $key) {
    Get-ChildItem $key | ForEach-Object {
        $clsid = $_.PSChildName
        $name = (Get-ItemProperty "HKLM:\SOFTWARE\Classes\CLSID\$clsid" -ErrorAction SilentlyContinue).'(default)'
        Write-Output ("  {0}  {1}" -f $clsid, $name)
    }
} else {
    Write-Output "MFT category key MISSING — MediaPlayback not installed"
}
```

期望：列出至少 1-3 个 video decoder MFT。

### 3.5 已装的 Microsoft 媒体扩展
```powershell
Get-AppxPackage -AllUsers |
    Where-Object { $_.Name -match 'Media|HEVC|VP9|AV1' } |
    Select-Object Name, Version | Format-Table -AutoSize
```

期望对低延时摄像机播放：`Microsoft.HEVCVideoExtension` 装了即可（H.264 由 OS 自带提供）。

---

## 4. 修复路径

按优先级：

### 4.1 启用 MediaPlayback feature（首选）

管理员 PowerShell：
```powershell
Enable-WindowsOptionalFeature -Online -FeatureName MediaPlayback -All
# 可能需要重启
```

验证：
1. 重启
2. 重新跑 §3.3 / §3.4 检查
3. `mfh264dec.dll` 应已出现，MFT 注册表 category 应有条目
4. 跑 mc-player，`MFTEnumEx` 应能找到 hw MFT

### 4.2 安装 Media Feature Pack（次选）

如果 4.1 行不通，从 Microsoft Update Catalog 下对应 OS build 的 MFP MSU：
- 查 build：`(Get-CimInstance Win32_OperatingSystem).BuildNumber`（如 26100）
- 下载：[Microsoft Update Catalog](https://www.catalog.update.microsoft.com/) 搜索 "Media Feature Pack <build>"
- 双击 .msu 安装；可能需要重启

### 4.3 重装 GPU driver（与 4.1 / 4.2 配合）

MediaPlayback 装好后，重新执行 GPU 驱动安装包（Intel Graphics Command Center / NVIDIA GeForce Experience / AMD Adrenalin），让驱动重新注册自家 hw MFT。

### 4.4 启用档 1 vendor SDK（最佳路径）

ADR-016 内置下载面板会在首次启动检测 GPU vendor 后引导用户从 vendor 官网下载对应 SDK redistributable，缓存到 `%LOCALAPPDATA%\mc-player\sdk\<vendor>\<version>\`。手动安装路径（不走面板）：

- **NVIDIA**：装 [NVIDIA Video Codec SDK](https://developer.nvidia.com/video-codec-sdk) 或 GeForce/Studio Driver（自带 `nvcuvid.dll`）。
- **Intel**：装 [oneVPL Runtime](https://www.intel.com/content/www/us/en/developer/tools/vpl/overview.html) 或最新 Intel Graphics Driver（自带 oneVPL runtime）。
- **AMD**：[AMD GPU 驱动](https://www.amd.com/en/support) 通常自带 AMF runtime；新装显卡驱动即可。

档 1 启用后 stats 上 `decoder_kind=VENDOR_SDK_*`，延时基线 ≤5 ms（1080p H.264）。

### 4.5 走 mc-player 自身的兜底（应急）

如果上述硬解组件全部不可行，可让 mc-player 直接走档 4 mc-libcodec 软解。这是最终兜底，不是首选——长期看应让档 1 / 档 2 / 档 3 至少一档可用以达成 §1.2 延时目标。

**当前状态**：`codec_dxva_video.cpp` 当前仅实装 HEVC（HEVC Extension 缺席场景），需要补 H.264 实装才能完整覆盖档 2；档 1 vendor SDK 仍在路线规划。短期优先级：档 2 H.264 → 档 1 NVDEC（最广覆盖）→ 档 1 oneVPL → 档 1 AMF。

---

## 5. 性能基线参考

同一路 720p H.264 RTSP 流（SPS reorder=2，编码器实际 -bf 0 无真 B 帧）实测：

| 配置 | mc-player vs VLC 时钟差 | 走的 decoder（ADR-015 档位） |
|---|---|---|
| IoT LTSC 默认（MediaPlayback Disabled） | **+101 ms** | OS 内置 sync software MFT（被旧策略当硬解，新策略下属档 4 软解兜底） |
| 装 MediaPlayback + Intel hw MFT | 预期 +5~12 ms 内 | 档 3 MFT hardware async (Intel Quick Sync) |
| Intel oneVPL bundled / D3D11VA 可用 | 预期 ≤ +2 ms（接近 VLC 基线） | 档 1 vendor SDK 或档 2 DXVA-direct |

VLC 同流稳定 ~0 ms 偏差作基线。VLC 用 ffmpeg/libavcodec D3D11VA 直驱（应用层管 DPB），即便走软解也是 multithreaded SIMD，比 Microsoft sync software MFT 快。

---

## 6. 与 ADD / ADR 的对应

- **ADR-015（硬件解码四级降级链）**：本文档是 ADR-015 在 IoT LTSC / MediaPlayback feature 缺失等环境约束下的工程化指南；§1 的链路图与 ADR-015 决策一一对应。
- **ADR-016（Vendor SDK 内置下载面板）**：档 1 的运行期获取机制，对应 §4.4 的手动安装路径。
- **ADR-001（MFT 缩窄为 OS 标准抽象兼容档）**：本文档 §1 档 3 行的 "仅 hw_url=1 && async=1 接受" 即 ADR-001 缩窄后的范围。
- **ADR-002（硬件 MFT 永远 async）**：仅约束档 3 hardware MFT；sync software MFT 由 ADR-015 直接归到档 4。
- **ADR-004（HEVC Extension 缺失自动软解）—— Superseded by ADR-015**：原决策的"HEVC MFT 缺失即软解"被四档链替代；HEVC 缺失现按 vendor SDK → DXVA-direct → libcodec 顺序兜底。
- **ADD §5.6**：解码路径四档原理；本文补充"各档系统组件依赖"这一前置条件。
- **ADD §7.4**：HEVC Extension 缺失场景的四档兜底解释。

---

## 7. CI / 验收建议

为防止回归发现晚：

- 启动时记录每档的探测结果：vendor SDK probe (vendor 匹配 + dll 存在) / DXVA-direct profile 列表 / `MFTEnumEx(ALL)` 完整输出 / libcodec 自检；按档输出便于定位"为什么没走到更高档"
- startup metric：`decoder_kind` ∈ {`VENDOR_SDK_NVDEC`, `VENDOR_SDK_ONEVPL`, `VENDOR_SDK_AMF`, `DXVA_DIRECT`, `MFT_HARDWARE`, `LIBCODEC`}，上报到 stats；运维监控 `decoder_kind` 分布即可发现"档位下沉"问题
- e2e_latency_p95 metric 与 decoder_kind 联合，自动告警"档 4 软解 + p95 > 25ms"等异常
- 安装文档（README）按场景给指导：
  - **极致延时**：建议安装 vendor SDK runtime（档 1）→ 镶 ADR-016 内置面板下载
  - **常规部署**：装最新 GPU driver 即可，档 2 DXVA-direct 自动启用
  - **Windows IoT LTSC**：optional `Enable-WindowsOptionalFeature -Online -FeatureName MediaPlayback -All`，仅为启用档 3（备份兼容档）；档 1 / 档 2 不受 MediaPlayback feature 影响
