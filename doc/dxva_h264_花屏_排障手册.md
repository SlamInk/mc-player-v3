# DXVA-direct H.264 花屏排障手册

适用范围:mc-player tier 2 DXVA-direct 路径(`ID3D11VideoDevice`),Windows 10 1809+ / Windows 11 x64,所有 GPU vendor。
本文沉淀 2026-05-08 ~ 2026-05-09 解决"紫色错位渐变 / 顶部 Y 全 0 partial decode"花屏的完整调查链 + root fix + 失败教训。配套 memory:`dxva_h264_ffmpeg_align_root_fix.md`。

---

## 1. 症状识别

### 1.1 视觉特征

| 症状 | 视觉表现 | NV12 readback 特征 | 根因类别 |
|---|---|---|---|
| **A. 紫色 / 绿色错位渐变** | 大色块斜向条纹,带 macroblock 锐边 | Y 顶部首行全 0x00 / 0x14;中部底部部分有真实数据 | driver partial decode(本文修复重点) |
| **B. 整图深绿色** | 屏幕几乎纯深绿 | Y 全 0(`avg=0 nonconst<5%`) | driver silent fail(ADR-022/023,见 `hardware-decode-dependencies.md`) |
| **C. 整图均匀灰** | 单色屏(如 0x80 / 0x14 / 0x11) | Y 全等于某个常量 | driver decode failure(bitstream 拒收;`SubmitDecoderBuffers` 名义返回 OK 但 driver 内部跳过) |
| **D. 大色块错位 + 块状 macroblock** | 红/绿/蓝错位 + 棋盘 | Y 顶部 0,中部 partial,运动区域明显错乱 | ref 链断裂(P/B 帧 RefFrameList 中 dpb slot 已被 driver 自身写覆盖) |
| **E. 屏幕黑(伴 codec 持续 emit)** | 整屏全 0 黑 | Y 数据正常 | render 路径 SRV 创建错位 / shader bug / DCOMP commit 时序 |

**症状 A** 是本文的核心根因。**症状 D** 由 ADR-003 dual-bind fence + reserved_anchor_slot 解决(见 §5.2)。

### 1.2 快速分诊命令

```pwsh
# 1. 跑 mc-player + 留前 3 帧 readback 信息
Remove-Item D:\Workspace\Code\mc-player-v3\mc-player.log -ErrorAction SilentlyContinue
& 'D:\Workspace\Code\mc-player-v3\build\ninja-msvc-RelWithDebInfo\demo-host\mc-player.exe' rtsp://<host>/<path>
# Ctrl+C 退出后查日志
Select-String -Path D:\Workspace\Code\mc-player-v3\mc-player.log -Pattern "NV12 readback#[1-3] Y全图"
Select-String -Path D:\Workspace\Code\mc-player-v3\mc-player.log -Pattern "NV12 readback#[1-3] Y\[r="
```

按 `avg / min / max / nonconst%` 与 §1.1 表对应:
- `avg=47 min=16 max=236 nonconst=99.9%` + 不同 row hex 有变化 → 正常解码,屏幕花屏请查 §6 渲染路径
- `avg=47 min=11 max=11 nonconst=0%` 全 0x11 → 症状 C(driver 拒 bitstream)
- `Y[r=0]=00 00 00 ...` 顶部全 0 + 中部底部正常 → 症状 A(本文修复)

### 1.3 用 ffmpeg 分离 driver 路径 vs 流问题

```pwsh
# ffmpeg dxva 走同一驱动同一硬件,如果 ffmpeg 干净说明流和驱动都 OK,问题在 mc-player 提交格式
& 'D:\Portable\ffmpeg\bin\ffmpeg.exe' -y -hwaccel d3d11va -i rtsp://<host>/<path> -frames:v 1 -an reference-frame.png
```

如果 `reference-frame.png` 是清晰画面,**而 mc-player 仍花屏 → 100% 是 mc-player 提交格式问题**,直接跳到 §3。

如果 ffmpeg 也花屏 → 流编码本身有问题,与 mc-player 无关。

---

## 2. 调查方法论(从源头逐节点对齐)

### 2.1 错误的调查方式(2026-05-08 复盘)

**反模式**:incremental hack 验证。
- 加 staging Map sync 测一次 → 仍花屏
- 改 3-byte SC 测一次 → driver 整图 0x14
- 加 padding 到 SliceBytesInBuffer 测一次 → IDR 顶部 0x80
- skip disposable B 帧 emit 测一次 → 黑屏
- ......

每次只改一项验证,看似科学但**每个 fix 都依赖其他未对齐的字段**,单独应用都让 driver 进入新的失败状态。**结论:driver 提交格式是一个 atomic 协议契约,字段间相互依赖,必须一次性对齐才能验证**。

### 2.2 正确的调查方式

按数据流分支节点逐一与 ffmpeg `libavcodec/dxva2_h264.c` 字节级对比,**所有差异列出后再一次性修复**:

| 节点 | mc-player 文件 | ffmpeg 对照 | 检查项 |
|---|---|---|---|
| GUID 选择 | `codec_dxva_video.cpp::init_decoder_h264` | `dxva2.c::dxva_get_decoder_configuration` | 优先级表顺序 / Intel ClearVideo workaround |
| Config 选择 | 同上 | 同上 | `ConfigBitstreamRaw=2` 优先(H.264) |
| DPB texture | 同上 | `hwcontext_d3d11va.c::d3d11va_alloc` | dual-bind `BIND_DECODER \| BIND_SHADER_RESOURCE` |
| Picture pool | 自管 dpb_tex | `picture_pool_t` refcount | mc-player 用 `reserved_anchor_slot` 替代 |
| PicParams | `dxva_h264.cpp::fill_pic_params` | `dxva2_h264.c::ff_dxva2_h264_fill_picture_parameters` | RefFrameList[16] / FrameNumList / FieldOrderCntList / wBitFields / num_ref_idx 全字段 |
| IQM | `flush_pending_pic_h264` 内 | `ff_dxva2_h264_fill_scaling_lists` | flat 16 (默认) vs PPS scaling matrix(zigzag permute) |
| Slice header parse | `dxva_h264.cpp::parse_slice_header_min` | `h264_parse.c::ff_h264_parse_ref_count` 等 | num_ref_idx override / direct_spatial_mv_pred_flag |
| **Bitstream 提交** | `flush_pending_pic_h264` 内 | **`dxva2_h264.c::commit_bitstream_and_slice_buffer`** | **本文修复重点** — start code 长度 / SliceBytesInBuffer 计算 / padding 处理 |
| SubmitDecoderBuffers 顺序 | 同上 | `dxva2.c::ff_dxva2_common_end_frame` | PP → IQM → BS → SC |

工具:
```pwsh
# 字节级对比 PicParams
Get-Content -Encoding Byte D:\Workspace\Code\mc-player-v3\dxva-h264-picparams1.bin | Format-Hex | Out-File picparams-mcplayer.txt
# ffmpeg 没内置 dump,需修 dxva2_h264.c 加 fwrite 后 rebuild,或者读源码逐字段对比
```

---

## 3. Root cause(2026-05-09 实战定位)

### 3.1 driver 行为分析

Intel UHD 730 driver(IoT LTSC build 26100)在 `ConfigBitstreamRaw=2` 模式下:

1. **driver 自己解析 slice header** — mc-player 不传 long slice control,driver 通过 BS 中的 NAL data 解析。
2. **driver 通过 `SliceControl[i].SliceBytesInBuffer` 确定每个 NAL 边界** — 不靠扫描 start code prefix。
3. **driver 期望每 slice 在 BS buffer 中由 3-byte start code (`00 00 01`) 引导** — 这是 ffmpeg 提交的格式;driver 内部 NAL 起点 = `BSNALunitDataLocation + 3`。
4. **末 slice 之后必须有 zero padding 到 128-byte 对齐** — 且 `slice_ctrl[last].SliceBytesInBuffer` 必须**包含 padding**,否则 driver 在 last slice 末尾扫到非预期字节。

mc-player 之前的实现违反了 #3 #4:
- `bitstream_buf` 累积 `nal_full = au.subspan(sc_start, ...)` 含 **4-byte start code** (leading 0x00 + 3-byte SC)
- `slice_ctrl[i].SliceBytesInBuffer = nal_full.size()` 含 4-byte SC,不含 padding
- 末 slice padding 写入 buffer 但**不计入 SliceBytesInBuffer**

driver 看到 4-byte SC 时,内部把 leading 0x00 当成上一 slice 的 trailing zero,后续 NAL header parse 偏移 → driver 状态混乱 → output partial NV12(顶部 macroblock 缺失)→ 视觉紫色错位。

### 3.2 Why 单独应用任一修复都失败

driver 的提交格式约定是**一致性契约**:
- 单独改 3-byte SC + 不改 `SliceBytesInBuffer` 计算 → driver 看到的 SC 长度与 SliceBytesInBuffer 不匹配 → 整图 0x14
- 单独把 padding 加到 `SliceBytesInBuffer` 而 `bitstream_buf` 还含 4-byte SC → driver 在第一个 NAL 内部 offset 算错 + 末尾多出 padding 字节 → IDR 顶部 0x80
- staging Map sync 强制 GPU 完成 video decode,但 **driver 解码本身就错** → sync 完成的是错误数据 → 仍花屏

**只有完整对齐 ffmpeg `commit_bitstream_and_slice_buffer` 的全套字段才能让 driver 走对路径**。

---

## 4. Root fix(完整对齐 ffmpeg `commit_bitstream_and_slice_buffer`)

### 4.1 NAL 累积(`on_au_h264` 内每 slice NAL)

```cpp
// raw NAL = NAL header byte + RBSP_with_EPB,不含 start code prefix
const std::size_t sc_byte_count = (sc_start < i) ? (i + 3 - sc_start) : 3;
const std::size_t raw_nal_off   = sc_start + sc_byte_count;
const std::size_t raw_nal_size  = (sc_start + nal_full.size()) - raw_nal_off;
DXVA_Slice_H264_Short sc{};
sc.BSNALunitDataLocation = static_cast<UINT>(bitstream_buf.size());  // offset in bitstream_buf
sc.SliceBytesInBuffer    = static_cast<UINT>(raw_nal_size);          // raw NAL size (no SC)
sc.wBadSliceChopping     = 0;
bitstream_buf.insert(bitstream_buf.end(),
                      au.begin() + raw_nal_off,
                      au.begin() + raw_nal_off + raw_nal_size);
h264_slice_ctrl.push_back(sc);
```

### 4.2 flush 时 commit(`flush_pending_pic_h264` 内,EndFrame 之前)

```cpp
UINT  dxva_bs_size_avail = 0;
void* dxva_bs_ptr        = nullptr;
UINT  dxva_bs_used       = 0;
bool  ok_bs              = false;

if (SUCCEEDED(video_ctx->GetDecoderBuffer(decoder.Get(),
                                           D3D11_VIDEO_DECODER_BUFFER_BITSTREAM,
                                           &dxva_bs_size_avail, &dxva_bs_ptr))) {
    uint8_t* const dxva_data = static_cast<uint8_t*>(dxva_bs_ptr);
    uint8_t*       current   = dxva_data;
    uint8_t* const end_buf   = dxva_data + dxva_bs_size_avail;
    const uint8_t  start_code[3] = { 0, 0, 1 };          // 3-byte SC
    constexpr UINT sc_size = 3;
    bool overflow = false;

    // 逐 slice 写 [3-byte SC + raw NAL] 到 driver buffer,同时重写 slice_ctrl[i]
    for (auto& sc : h264_slice_ctrl) {
        const UINT raw_size = sc.SliceBytesInBuffer;
        if (sc_size + raw_size > static_cast<UINT>(end_buf - current)) { overflow = true; break; }
        const UINT new_loc = static_cast<UINT>(current - dxva_data);
        std::memcpy(current, start_code, sc_size);  current += sc_size;
        std::memcpy(current, bitstream_buf.data() + sc.BSNALunitDataLocation, raw_size);
        current += raw_size;
        sc.BSNALunitDataLocation = new_loc;          // 重写为 dxva buffer 中实际 offset
        sc.SliceBytesInBuffer    = sc_size + raw_size;  // 含 SC
    }

    // 末 slice padding zeros 到 128-byte 对齐 + SliceBytesInBuffer += padding
    if (!overflow && !h264_slice_ctrl.empty()) {
        const UINT cur_off = static_cast<UINT>(current - dxva_data);
        const UINT remain  = static_cast<UINT>(end_buf - current);
        UINT padding       = (128u - (cur_off & 127u)) & 127u;
        if (padding > remain) padding = remain;
        if (padding > 0) {
            std::memset(current, 0, padding);  current += padding;
            h264_slice_ctrl.back().SliceBytesInBuffer += padding;   // 关键:末 slice 含 padding
        }
    }

    dxva_bs_used = static_cast<UINT>(current - dxva_data);
    ok_bs = SUCCEEDED(video_ctx->ReleaseDecoderBuffer(decoder.Get(),
                                                       D3D11_VIDEO_DECODER_BUFFER_BITSTREAM));
}

// SubmitDecoderBuffers:bd[BS].DataSize = current - dxva_data,128 对齐
D3D11_VIDEO_DECODER_BUFFER_DESC bd[4]{};
// ... bd[0..2] PP/IQM/BS,bd[3] SC ...
bd[2].DataSize = dxva_bs_used;
bd[3].DataSize = static_cast<UINT>(h264_slice_ctrl.size() * sizeof(DXVA_Slice_H264_Short));
video_ctx->SubmitDecoderBuffers(decoder.Get(), 4, bd);
```

### 4.3 验证 readback 字段

修复生效后前 3 帧 readback Y 应满足:
- `nonconst > 95%`(动态范围,非均匀填充)
- `min < 60` 且 `max > 200`(覆盖暗到亮)
- `Y[r=0] / Y[r=mid] / Y[r=last]` 三行 hex 各自有像素变化(不是 16 字节同值)

参考样本(2026-05-09 实测,Intel UHD 730 + 太空主题流):
```
H.264 NV12 readback#1 Y全图 sum=171693 avg=47 min=16 max=236 nonconst=3598/3600 (99.9%)
H.264 NV12 readback#1 Y[r=0   ,0..15]=11 11 11 11 ... 11   ← 太空黑色背景,合理
H.264 NV12 readback#1 Y[r=360 ,0..15]=1A 1A ... 1B 1B    ← 中间星球渐变
H.264 NV12 readback#1 Y[r=704 ,0..15]=11 11 ... 11        ← 底部黑色,合理
```

注:**单行全等 ≠ partial decode** 当其他行有变化且整图 nonconst 高时,这只是流的真实场景(如太空黑色顶部、纯色背景)。

---

## 5. 配套修复(belt-and-suspenders)

### 5.1 reserved_anchor_slot

renderer 的 `last_good` 持有 `(dpb_tex, slice=N)` 引用。如果 `alloc_dpb_slot_h264` 后续把 slot N 选为 victim → driver 写新帧覆盖 → watchdog redraw 用 last_good sample 看到错乱内容 → 屏幕花屏。

修复:IDR emit 后 `reserved_anchor_slot = cur_dpb_idx`,alloc 第一/二遍 + 兜底分支永远跳过。下一 IDR 的 first slice 处理时 `reserved_anchor_slot = UINT_MAX` 释放。等价 VLC `picture_pool_t` refcount 的简化版。

### 5.2 recent_emit_slots ring(短期保护)

最近 N 个 emit 的 dpb slot 不被 alloc victim,防 in-flight 帧被覆盖。N=4 ≈ render 端 1-2 帧滞后余量。
**初始化必须 `UINT_MAX`,默认 0 会让 alloc 误判 slot 0 为 recently emitted**。

### 5.3 dpb_size 设大

`dpb_size = max(num_ref_frames + 2, 16)`,上限 24。给 ref + reorder + anchor + in-flight 留余量。VLC d3d11va.c 实测 24 surfaces。

### 5.4 GUID 优先级表

`{ VLD_NoFGT(1B81BE68), VLD_FGT(1B81BE69), VLD_WITHFMOASO_NoFGT(1B81BE94) }`,跳过 STEREO/MULTIVIEW(1B81BEA2/A4/A6 — H.264 3D/MVC 专用,普通流不能用)。

---

## 6. 失败教训清单(留给后人,2026-05-08 ~ 09 实测)

| 修复尝试 | 单独应用结果 | 教训 |
|---|---|---|
| staging Map sync(1×1 像素 + Map READ) | 仍紫色花屏 | driver decode 本身错 → sync 拷出的也是错数据 |
| 改 4-byte → 3-byte start code(只改 sc_start) | driver 整图 Y=0x14 | SliceBytesInBuffer 仍为 nal_full.size,与 SC 长度不一致 → driver 拒解 |
| `slice_ctrl.back().SliceBytesInBuffer += pad`(只加 padding) | IDR Y=0x80 | bitstream_buf 仍含 4-byte SC → 多种格式混合 driver state error |
| disp-B(`slice_type=1 + nal_ref_idc=0`)skip emit | 屏蔽症状,IDR/P/ref-B 仍可能 partial | 不修因,只是减少花屏帧曝光时间 |
| EVENT query 每帧 wait | 读 readback 看似 OK 但屏幕仍花屏 | EVENT 等的是 immediate ctx queue,driver video engine 私有 fence 不见 |
| 回退 commit 954d3f5 baseline(out_pool 中间层) | 仍花屏 | 不是 dual-bind vs out_pool 差异,而是 bitstream 提交格式 |

**核心教训**:**driver 提交格式是 atomic 协议契约,字段间互相依赖。任何单字段改动若不与全套对齐 → 让 driver 进入新的失败状态**。

---

## 7. 验收 checklist

修复完成必须全部满足:

- [ ] `mc-player.log` 中 `H.264 NV12 readback#1` 的 `Y全图 nonconst > 95%`
- [ ] readback#1 / #2 / #3 三帧的 `Y[r=0]` / `Y[r=mid]` / `Y[r=last]` hex 至少两行有变化
- [ ] `gate poisoned=1 enter_count <= 5`(seq_gap 是网络问题,只要 IDR 来时 gate 能 leave)
- [ ] 屏幕实测画面清晰(用 `Get-Process mc-player | %{$_.MainWindowHandle}` + `System.Drawing.Graphics.CopyFromScreen` 抓 client area 验证,见 §1.3)
- [ ] vs `ffmpeg -hwaccel d3d11va -i <url>` 输出图视觉一致

---

## 8. 参考实现

- ffmpeg `libavcodec/dxva2_h264.c::commit_bitstream_and_slice_buffer` (line 302-443) — 字节级权威参考
- ffmpeg `libavcodec/dxva2.c::ff_dxva2_common_end_frame` — SubmitDecoderBuffers 顺序
- VLC `modules/codec/avcodec/d3d11va.c` — picture pool + dual-bind 创建
- mc-player `mc-player/src/media/codec_dxva_video.cpp::flush_pending_pic_h264` — 当前实装

ADR 关联:ADR-003(dual-bind fence)/ ADR-014(Frame Validity Gate)/ ADR-015(四级降级链)/ ADR-022(strict gate)/ ADR-023(silent fail 隔离)。
