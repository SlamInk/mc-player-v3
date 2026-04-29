# mc-player 硬件解码依赖与故障排查

适用平台：Windows 10 1809+ / Windows 11 x64（含 IoT LTSC）。
本文记录硬件 H.264 / H.265 解码所依赖的系统组件，以及"明明有 GPU 但走不到硬解"这类故障的诊断与修复路径。

---

## 1. 硬件解码链路

mc-player 的视频解码三阶兜底（`controller.cpp:start_decode_pipeline`）：

```
[1] CodecMftVideo (Windows MFT)              ← 首选
       ├── HW MFT (Intel Quick Sync / NVIDIA NVDEC / AMD UVD)
       └── SW MFT (Microsoft H264/HEVC software decoder)
                   ↓ 失败
[2] CodecDxvaVideo (DXVA-direct，HEVC only)  ← HEVC Extension 缺席兜底
                   ↓ 失败
[3] CodecLibcodecVideo (mc-libcodec 自研软解) ← 最后兜底
```

每一阶都依赖不同的系统组件，缺哪一层降到下一层。

### 必需的系统组件

| 组件 | 提供者 | 缺失后果 |
|---|---|---|
| `mfplat.dll` / `mfreadwrite.dll` | OS 内置 | MF 平台不可用，所有 MFT 路径全废 |
| **MFT category 注册表条目**：`HKLM\SOFTWARE\Microsoft\Windows Media Foundation\Transforms\Categories\{d6c02d4b-6833-45b4-971a-05a4b04bab91}` | OS（MediaPlayback feature） | `MFTEnumEx` 返回 0 个 decoder |
| `mfh264dec.dll`（现代 H.264 SW decoder） | OS（MediaPlayback feature） | H.264 SW MFT 不可用，只能用兼容老 dll `msmpeg2vdec.dll`（性能差） |
| `mfh265dec.dll` | Microsoft HEVC Video Extension（Store） | H.265 SW MFT 不可用，需 mc-libcodec 软解兜底 |
| **Intel/NVIDIA/AMD 显卡驱动 + Media 组件** | GPU 厂商驱动 | hw MFT 不会注册，全链路退到 SW |
| `dxva2.dll` / `d3d11.dll` ID3D11VideoDevice | OS + 驱动 | DXVA-direct 路径不可用 |

---

## 2. Windows IoT LTSC 的特例

Windows IoT Enterprise LTSC 默认**裁剪掉整组 MediaPlayback feature**：

```
PS> Get-WindowsOptionalFeature -Online -FeatureName MediaPlayback
FeatureName     : MediaPlayback
State           : Disabled            ← 默认禁用
```

后果：
- MFT category 注册表 key 不存在
- `mfh264dec.dll` / `mfh265dec.dll` 没装
- Intel/AMD/NV 驱动安装时 hw MFT 注册脚本会跳过（因为基础的 MF subsystem 不在）
- `MFTEnumEx(MFT_ENUM_FLAG_ALL)` 仅能返回 OS 内置兼容老 dll（如 `msmpeg2vdec.dll`）作 fallback —— **这是性能瓶颈来源**

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

### 4.4 走 mc-player 自身的兜底（应急）

如果以上系统改动暂不可行，可以让 mc-player 跳过 MFT 直接用 mc-libcodec 软解。这是临时方案，长期看 hw 解码才是正路。

未来扩展：把 H.264 也加入 DXVA-direct fallback（当前 `controller.cpp:601` 仅 H.265 走），不依赖 MFT 注册。前提是 `ID3D11VideoDevice` + GPU 的 H.264 DXVA profile 可用 —— 这条路径仅依赖 GPU driver、不依赖 MediaPlayback feature。

---

## 5. 性能基线参考

同一路 720p H.264 RTSP 流（reorder=2 真 B 帧）实测：

| 配置 | mc-player vs VLC 时钟差 | 走的 decoder |
|---|---|---|
| IoT LTSC 默认（MediaPlayback Disabled） | **+133ms** | `msmpeg2vdec.dll` SW（兼容老 dll） |
| 装 MediaPlayback + Intel hw MFT | 待验证（预期 +20~50ms 内） | Intel Quick Sync hw MFT |

VLC 同流稳定 ~0ms 偏差（基线）。VLC 用 ffmpeg/libavcodec 软解（汇编 + multithreaded slice/frame parallel），即便没 hw 也比 Microsoft software MFT 快。

---

## 6. 与 ADD / ADR 的对应

- **ADR-002（硬件 MFT 永远 async）**：仅当系统真有 hw MFT 时生效；缺席场景下走 SW（sync 模式合规，因为 software MFT 不是 hw）。
- **ADR-004（HEVC Extension 兜底）**：HEVC 路径已记录此风险，需要 mc-libcodec 软解兜底。本文把 H.264 也归入同类风险（IoT LTSC 场景），未来可考虑加 ADR 条目。
- **ADD §5.6 解码路径**：当前文档主要关注硬件 MFT 的 ABI 与 async 协议；本文补充"硬件 MFT 注册依赖"这一前置条件。

---

## 7. CI / 验收建议

为防止回归发现晚：

- 启动时记录 `MFTEnumEx(ALL)` 的完整结果（已落地，见 `codec_mft_video.cpp:807-826`），运维只需查日志即可定位
- 加 startup metric：`hw_decoder_available: 1/0`（hw_url=1 的 MFT 是否找到），上报到 stats
- 安装文档（README）显式列出"Windows IoT LTSC 必须先 Enable-WindowsOptionalFeature MediaPlayback"
