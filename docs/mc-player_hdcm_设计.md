# mc-player Hardware Decode Component Manager（HDCM）设计

| 项目 | 内容 |
|---|---|
| 文档类型 | design-detail（ADD 与 plan 之间的实施细化层）|
| 上游决策 | ADR-016（类别 A）+ ADR-021（HDCM 总体；扩展类别 B/C/D）|
| 上游原理 | `mc-player_架构设计文档_v3.0.md` §5.6.6 |
| 下游 roadmap | `plan/mc-player_重构方案.md` Phase 8（含 8-A ~ 8-E 子拆分）|
| 关联运维文档 | `hardware-decode-dependencies.md`（PowerShell 诊断 + IoT LTSC 特例 + 性能基线）|
| metric 定义 | `mc-player_性能量度规范.md` §6.1（字段定义）/ §10.5（排障）/ §11.2（best-effort）|

本文档定义 HDCM 的组件 manifest / 数据结构 / 状态机 / helper.exe IPC 协议 / metric 字段映射。不含 cmake target 命名、具体 C/C++ 头文件签名、阶段排期。

---

## 1. 范围与定位

HDCM 把 ADR-015 四级降级链各档的 OS 前置依赖统一管理，原则是"用户层零命令"——既不让用户跑 PowerShell，也不让用户外跳 Microsoft Store 客户端：

| 档（ADR-015） | 前置依赖 | HDCM 覆盖 |
|---|---|---|
| 1 | vendor SDK redistributable | 类别 A |
| 2 | GPU driver 暴露 D3D11VA decoder profile | 类别 D（driver 升级影响 profile 暴露）|
| 3 | MediaPlayback feature + MFT category 注册 + 各 codec 的 OS / 第三方扩展 | 类别 C（feature）+ 类别 B（HEVC / AV1 codec 扩展）+ 类别 D（vendor hw MFT 随 driver 安装时注册）|
| 4 | mc-libcodec bundled | 不在 HDCM 范围（始终可用）|

不属于 HDCM 范围：
- mc-libcodec：bundled 软解，无外部依赖
- libdatachannel / libopus：baseline 能力（ADR-005 / ADR-009），bundled
- 主 app 自身升级：超出 HDCM 范围，由 app 升级流程管

---

## 2. 组件 Manifest

每个组件由一条 `HdcmComponentManifest` 定义。下表是 v1 完整清单。

### 2.1 类别 A — Vendor SDK redistributable（沿 ADR-016）

| id | vendor_id 触发 | 组件文件 | 下载源 | SHA-256 | 缓存路径 |
|---|---|---|---|---|---|
| A_NVDEC | `0x10DE`（NVIDIA）| `nvcuvid.dll` + `cudart64_*.dll` | `developer.nvidia.com/...`（独立 redistributable）| 编译期 hardcoded | `%LOCALAPPDATA%\mc-player\sdk\NVIDIA\<version>\` |
| A_oneVPL | `0x8086`（Intel）| `libmfx.dll` / `vpl.dll` | `intel.com/.../oneapi`（独立 redistributable）| 同上 | `%LOCALAPPDATA%\mc-player\sdk\Intel\<version>\` |
| A_AMF | `0x1002`（AMD）| `amfrt64.dll` | —（**无独立 end-user redistributable**，随 AMD Radeon Software / Adrenalin Driver 安装；缺失时走 D_AMD 路径）| — | —（OS `System32` 由 AMD driver installer 写入；HDCM 不持有缓存）|

> **A_AMF 特例**：AMD AMF runtime 不像 NVDEC SDK / oneVPL 提供独立 end-user redistributable，其分发完全捆绑 AMD GPU driver。HDCM 对 A_AMF 不实装独立 `install_fn`——probe 仅二态（无 `installable` 状态）：
>
> - `System32\amfrt64.dll` 命中 → `already_installed`
> - 否则（缺失或 vendor 非 AMD）→ `unavailable_on_this_sku`，UI 上 cross-link 提示 D_AMD 路径升级 AMD GPU driver
>
> 这一特例在 ADR-016 vendor SDK 范围内是允许的工程化让步（ADR-016 决策"按需下载"对应 A_NVDEC / A_oneVPL；A_AMF 因 vendor 分发模型差异降级到"指向 D_AMD"）。

**probe 顺序（A_NVDEC / A_oneVPL，标准类别 A 流程）**：
1. `%LOCALAPPDATA%\mc-player\sdk\<vendor>\` 任一版本子目录命中 → `already_installed`
2. `System32` 命中（驱动自带）→ `already_installed`
3. 当前 GPU vendor 不匹配本组件 → `unavailable_on_this_sku`（如 GPU 是 Intel，A_NVDEC 隐藏）
4. 否则 → `installable`

### 2.2 类别 B — Microsoft Store 媒体扩展

| id | 触发条件 | Store ProductId | License | SKU 限制 |
|---|---|---|---|---|
| B_HEVC_Ext | codec=H.265 + 档 3 ProcessSample 失败 OR 用户主动展开面板 | `9NMZLZ57R3T7`（HEVC Video Extensions, 收费）/ `9N4WGH0Z6VHQ`（OEM bundled 免费版，仅特定设备）| 付费 / OEM bundled | Store 客户端在线 + Win10 1709+；IoT LTSC 无 Store 客户端 → `unavailable_on_this_sku` |
| B_AV1_Ext | codec=AV1 + 档 3 ProcessSample 失败 OR 用户主动展开面板 | `9MVZQVXJBQ9V`（AV1 Video Extension, 免费）| 免费 | 同上；额外要求 OS 支持 AV1 解码 stack（Win10 21H1+ / Win11）|

> ⚠️ **ProductId / PackageFamilyName 校对**：上表 Store ProductId 与下文 `Microsoft.HEVCVideoExtension_8wekyb3d8bbwe` / `Microsoft.AV1VideoExtension_8wekyb3d8bbwe` 为 design intent 占位；**Phase 8-B 实施前必须以 Microsoft Store 官方产品页面 URL 为准**（vendor 可能调整 ID / 上下架版本 / 改 family name）。Store API 直接使用错误 ID 会返回 `STOREAPI_E_INVALID_PRODUCT_KEY` 或类似错误。

**probe 方法**：
1. `Windows::Management::Deployment::PackageManager::FindPackagesForUser(empty, family_name)` 查找已装 PackageFamilyName（`Microsoft.HEVCVideoExtension_8wekyb3d8bbwe` / `Microsoft.AV1VideoExtension_8wekyb3d8bbwe`）→ `already_installed`
2. `Windows::Services::Store::StoreContext::GetDefault()` 返回非空 + 网络可达 → `installable`
3. 否则 → `unavailable_on_this_sku`

**install 方法**：UWP `StoreContext::RequestDownloadAndInstallStorePackagesAsync({product_id})`，C++/WinRT interop 调用；返回 `IAsyncOperationWithProgress<StorePackageInstallResult, StorePackageInstallStatus>`，进度回调上报 `StorePackageInstallStatus::PackageDownloadProgress`。

**License 注意**：HEVC Video Extension 收费版的 license 与 Microsoft 账户绑定，由 Store API 体系托管；mc-player 不持有 license token、不缓存任何 license artifact。

### 2.3 类别 C — Windows Optional Feature

| id | DISM feature name | 影响档位 | SKU 行为 |
|---|---|---|---|
| C_MediaPlayback | `MediaPlayback` | 档 3 整档 + 档 3 vendor hw MFT 注册（GPU driver 安装时联动）| Win10 / Win11 / IoT LTSC 均支持 enable，启用后**通常需重启电脑** |

**probe 方法**：`DismApi::DismGetFeatureInfo(session, L"MediaPlayback", &info)` → 按 `info.FeatureState`：
- `DismStateInstalled` / `DismStateInstallPending` → `already_installed`
- `DismStateUninstallPending` / `DismStateStaged` → `installable`
- `DismStateNotPresent` → `unavailable_on_this_sku`（base image 不含 payload，用户需 Media Feature Pack MSU 离线安装；HDCM 提示路径 → `hardware-decode-dependencies.md` §4.4）

**install 方法**：见 §4 helper.exe IPC 协议。

### 2.4 类别 D — GPU driver 引导

| id | 检测字段（WMI 命名空间 `root\cimv2`）| vendor → minimum recommended（编译期内嵌） | 跳转 URL |
|---|---|---|---|
| D_NVIDIA | `Win32_VideoController.DriverVersion`（filter: `Name LIKE 'NVIDIA%'`）| `>= 31.0.15.4592`（NVDEC SDK 12.x baseline）| `https://www.nvidia.com/Download/index.aspx` |
| D_Intel | 同上（filter: `Name LIKE 'Intel%' AND VideoProcessor LIKE '%Graphics%'`）| `>= 31.0.101.4032`（oneVPL 2.10 baseline）| `https://www.intel.com/.../graphics/drivers.html` |
| D_AMD | 同上（filter: `Name LIKE 'AMD%' OR Name LIKE 'Radeon%'`）| `>= 31.0.21925.2`（AMF 1.4.30 baseline）| `https://www.amd.com/en/support` |

**probe 方法**：`IWbemServices::ExecQuery(WQL)` → 拿当前 active adapter（按 ADR-007 智能选择）的 DriverVersion → 解析 `a.b.c.d` 四段版本 → vs 阈值表比对 → 落后即 `installable`（语义：`driver_below_threshold=1`），否则 `already_installed`。

**install 方法**：`ShellExecuteEx({lpVerb=L"open", lpFile=manifest.D.vendor_url, nShow=SW_SHOWNORMAL})` → 默认浏览器打开 vendor 官网驱动页 → 用户在浏览器 + vendor 自家 installer 内完成。**driver setup.exe 永不在 mc-player 进程内运行**——driver 安装期切显卡 / 黑屏 / 通常要重启，与播放器进程生命周期严重耦合时风险过高。

**阈值表维护**：每次 mc-player 升级一并校对，与 ADR-016 SHA-256 同步策略。运行期检测频率：mc_open 启动期一次，结果缓存到 process lifetime（不在播放期重复 WMI 查询）。

---

## 3. 数据结构（仅示意字段名 + 类型，非具体头文件签名）

### 3.1 HdcmComponentManifest

```
struct HdcmComponentManifest {
    uint32_t struct_size;           // ABI: 首字段 size + version 用于演进
    uint32_t version;
    char id[32];                    // "A_NVDEC" / "B_HEVC_Ext" / "C_MediaPlayback" / "D_NVIDIA"
    HdcmCategory category;          // A / B / C / D
    char display_name_utf8[128];    // 面板显示（已本地化）

    HdcmProbeFn   probe_fn;
    HdcmInstallFn install_fn;       // 类别 D 此字段为 NULL（走 ShellExecuteEx 短路径）

    // 类别相关 payload（按 category union）
    union {
        struct { uint32_t vendor_id; const char* download_url; uint8_t sha256[32]; const wchar_t* cache_subdir; } A;
        struct { const wchar_t* product_id; const wchar_t* package_family_name; }                                  B;
        struct { const wchar_t* dism_feature_name; }                                                                C;
        struct { uint32_t vendor_id; uint32_t threshold[4]; const wchar_t* vendor_url; }                            D;
    };
};
```

### 3.2 HdcmComponentState（运行期）

```
enum HdcmState {
    HDCM_STATE_UNKNOWN = 0,            // 未 probe
    HDCM_STATE_ALREADY_INSTALLED,      // 跳过入口
    HDCM_STATE_INSTALLABLE,            // 入口可见
    HDCM_STATE_UNAVAILABLE_ON_SKU,     // 跳过入口（SKU 限制）
    HDCM_STATE_INSTALLING,             // 用户已点击，正在安装
    HDCM_STATE_INSTALL_FAILED,         // 安装失败（last_error 保留细分）
    HDCM_STATE_RESTART_PENDING,        // 类别 C 启用成功，等用户重启电脑
};

struct HdcmComponentState {
    HdcmState state;
    int32_t   last_error;              // INSTALL_FAILED 时的细分（HRESULT / DISM hr / WinHTTP error）
    uint32_t  install_started_unix_ms; // metric 用
    uint32_t  install_finished_unix_ms;
    uint8_t   restart_required;        // 类别 C: 0=否 / 1=Possible / 2=Required
};
```

---

## 4. helper.exe IPC 协议（类别 C 专用）

主 app 与 `mc_hdcm_helper.exe` 之间用命名管道双向通信。

### 4.1 命名管道命名

`\\.\pipe\mc-player\hdcm-helper-<主app PID>`（含主 app PID 防多实例冲突；helper 启动时通过命令行参数接收 pipe name）。

### 4.2 主 app → helper 请求

| 字段 | 类型 | 含义 |
|---|---|---|
| msg_id | u32 | `0x01` = EnableFeature |
| feature_name_utf16 | wchar_t[64] | DISM feature name（如 `MediaPlayback`）|
| dism_flags | u32 | `DismPackageFeatureState` bit 组合（保留扩展位）|

### 4.3 helper → 主 app 响应

| 字段 | 类型 | 含义 |
|---|---|---|
| msg_id | u32 | `0x81` = EnableFeatureResult |
| dism_hr | i32 | `DismApi` 返回的 HRESULT |
| restart_required | u8 | 0=否 / 1=Possible / 2=Required（来自 `DismGetFeatureInfo::RestartRequired`）|
| error_message_utf16 | wchar_t[256] | 失败时 `DismGetLastErrorMessage` 返回的描述 |

### 4.4 helper.exe 安全约束

- **同 vendor 签名**：编译时配置同一 SignTool 证书（与主 app 同 cert thumbprint）；helper 启动时校验 parent process 的 cert thumbprint 一致 → 否则立即 exit 1，防恶意提权。
- **manifest**：`<requestedExecutionLevel level="requireAdministrator" uiAccess="false" />`。
- **无网络能力**：helper 二进制链接配置不引入 `wininet.dll` / `winhttp.dll` / Winsock；Windows Defender / EDR 风控可基于此特征建立白名单。
- **生命周期**：helper 完成 DISM 调用并发送响应后立即退出，不长驻。
- **parent 校验**：`OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION)` + `GetModuleFileNameExW` + `WinVerifyTrust` cert chain → 同主 app 证书 thumbprint 才继续；否则记 event log 后退出。

---

## 5. 状态机：detect → install → 完成

### 5.1 启动期 batch detect

```
on_app_start():
  for m in HDCM_MANIFEST_TABLE:                     // 静态表，编译期定义
    state = m.probe_fn()
    component_states[m.id] = HdcmComponentState{ state, ... }
    上报 mc.hdcm.component{type=m.category, id=m.id}.state = state

  visible = [m for m in HDCM_MANIFEST_TABLE
             if component_states[m.id].state == HDCM_STATE_INSTALLABLE]
  if visible is empty:
    "硬解组件" 面板入口 hidden
  else:
    "硬解组件" 面板入口 visible 但默认 collapsed（不打扰）
```

batch detect 异步执行，不阻塞 `mc_open` 启动。

### 5.2 用户主动展开面板 + 安装

```
on_user_click_install(component_id):
  m = HDCM_MANIFEST_TABLE[component_id]
  state.state = HDCM_STATE_INSTALLING
  state.install_started_unix_ms = now()
  上报 mc.hdcm.install_attempt_count{type=m.category, id=m.id, result=started}

  result = m.install_fn(progress_callback)          // 异步 + 进度回调

  state.install_finished_unix_ms = now()
  上报 mc.hdcm.last_install_duration_ms{type, id} = finished - started

  if result.success:
    if m.category == C and result.restart_required > 0:
      state.state = HDCM_STATE_RESTART_PENDING
      state.restart_required = result.restart_required
      上报 mc.hdcm.restart_pending{feature=MediaPlayback} = 1
      显示对话框 "立即重启电脑 / 稍后重启"
    else:
      state.state = HDCM_STATE_ALREADY_INSTALLED
    上报 mc.hdcm.install_attempt_count{..., result=success}
  else:
    state.state = HDCM_STATE_INSTALL_FAILED
    state.last_error = result.error_code
    上报 mc.hdcm.install_attempt_count{..., result=网络/校验/UAC拒绝/store_unavailable/...}
    面板显示 "重试 / 跳过"
```

### 5.3 类别 D 一键跳浏览器（无 install_fn）

```
on_user_click_driver_update(vendor):
  m = HDCM_MANIFEST_TABLE["D_<vendor>"]
  ShellExecuteEx({
    lpVerb: L"open",
    lpFile: m.D.vendor_url,
    nShow:  SW_SHOWNORMAL,
  })
  上报 mc.hdcm.install_attempt_count{type=D, id=..., result=external_redirect}
  // 不进入 INSTALLING 状态、不等回调；用户在浏览器外完成
  // 下次 mc_open 启动期重新 detect driver 版本
```

---

## 6. metric 字段映射

详见 `mc-player_性能量度规范.md` §6.1。摘要：

| HDCM 内部状态 | metric 字段 | 类型 / 取值 |
|---|---|---|
| `component_states[id].state` | `mc.hdcm.component{type, id}.state` | gauge label，见 §3.2 enum |
| 安装尝试 + 结果 | `mc.hdcm.install_attempt_count{type, id, result}` | counter；result label：`started / success / user_cancelled / net_error / checksum_mismatch / authenticode_invalid / uac_denied / store_unavailable / dism_payload_missing / helper_crashed / external_redirect` |
| 单次安装耗时 | `mc.hdcm.last_install_duration_ms{type, id}` | gauge ms |
| 类别 C 重启等待 | `mc.hdcm.restart_pending{feature}` | gauge 0/1 |
| 类别 D 阈值落后 | `mc.hdcm.driver_below_threshold{vendor}` | gauge 0/1 |

`mc.decoder.sdk_cache_hit{vendor}`（性能规范 §6 既有字段）保持不变，与 `mc.hdcm.component{type=A, id=A_<vendor>}.state == ALREADY_INSTALLED` 等价；HDCM 引入更通用的视图，sdk_cache_hit 是档 1 专用的 backward-compatible alias，不删除以保持现有运维 dashboard 兼容。

---

## 7. 边缘场景

### 7.1 多卡共存（如 NVIDIA dGPU + Intel iGPU）

- detect_gpu_vendor() 返回 active adapter vendor（按 ADR-007 智能 GPU 选择规则）
- 类别 A 仅显示 active adapter 对应的 vendor SDK；其他 vendor 隐藏（`unavailable_on_this_sku` 即 vendor mismatch）
- 跨屏 transition（ADR-013）切 adapter 时 **不重新弹安装面板** —— HDCM 状态在 mc_open 生命周期内 immutable；下次重 `mc_open` 才重新 detect

### 7.2 用户跳过所有 HDCM 安装

- 启动不阻断；按 ADR-015 四级降级链兜底
- `mc.decoder.tier_actual` 反映实际命中档位
- `mc.hdcm.component{*}.state == INSTALLABLE` 长期保持 → 监控告警可定义"X 天未安装"提醒，**不在 CI 闸内**（ADR-016 既定不阻断启动行为延续）

### 7.3 类别 B Store API 工作但用户中途取消

- `IAsyncOperationWithProgress::Cancel` → Store API 返回 `StorePackageInstallStatus::Canceled`
- HDCM 状态回到 `INSTALLABLE`，下次主动展开面板仍可重试
- 上报 `mc.hdcm.install_attempt_count{..., result=user_cancelled}`

### 7.4 类别 C DISM 失败原因细分

| DISM hr / 错误码 | result label | 兜底建议 |
|---|---|---|
| `ERROR_SXS_ASSEMBLY_NOT_FOUND` / `CBS_E_PACKAGE_NOT_APPLICABLE` | `dism_payload_missing` | base image 不含 payload，需 Media Feature Pack MSU；面板提示 → `hardware-decode-dependencies.md` §4.4 |
| `ERROR_CANCELLED`（UAC 拒绝） | `uac_denied` | 面板提示 "需要管理员权限，请在 UAC 弹窗点'是'" |
| 命名管道断开 | `helper_crashed` | 主 app event log 记录；提示用户重试或导出 log |
| `ERROR_SUCCESS_REBOOT_REQUIRED` | `success` + `restart_required > 0` | 进入 RESTART_PENDING 状态 |

### 7.5 类别 D 阈值表过期

- mc-player 一年未升级 → 编译期内嵌阈值落后实际 vendor 推荐版本 → 误报"driver 落后"
- 缓解：阈值表每次 mc-player 发版校对；用户可在设置面板关闭 D 类提示（不影响 A/B/C）

### 7.6 GDR / B-Frame Policy 与 HDCM 的独立性

- HDCM 影响"档可不可用"（OS 前置）；不影响 ADD §5.6.4 B-Frame Policy / §5.5.3 GDR 处理（codec 内逻辑）
- 两者解耦，HDCM 安装成功不会触发 codec 重协商

---

## 8. 与现有文档的引用关系

- **决策**：ADR-016（类别 A）/ ADR-021（总体扩展，含类别 B/C/D）
- **原理与状态机概要**：`mc-player_架构设计文档_v3.0.md` §5.6.6（顶层）+ 本文 §5（细化）
- **运维诊断（PowerShell 命令、IoT LTSC 特例、性能基线）**：`hardware-decode-dependencies.md`
- **metric 字段 / 阈值 / CI 闸**：`mc-player_性能量度规范.md` §6.1（字段定义）/ §10.5（排障）/ §11.2（best-effort，监控不闸）
- **实施 roadmap**：`plan/mc-player_重构方案.md` Phase 8（含 8-A ~ 8-E 子拆分）
- **probe 复用**：`mc-player_capability_probe_设计.md` §3.5 hardware probe 已 enumerate vendor SDK / driver version；HDCM 复用其结果，不重复 enum
