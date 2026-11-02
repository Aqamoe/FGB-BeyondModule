# Bigscreen Beyond 眼追共享内存（VRCFTMemmapData）协议核对

> **重要前提**：这个格式**没有官方文档**。Bigscreen 只发布了写映射的应用与其 VRCFT 模块
> 源码；「字段含义」层面的一切都是社区逆向。下面逐项标注「已确认 / 未确认」，
> 「已确认」仅指**一手源码或独立第二实现交叉印证**，不等于真机验收。
> 核对日期：2026-11-02。上游源码快照存于本地 `.tmp-upstream/beyond`（不随仓库分发）。

## 来源清单

| # | 项目 | 角色 | 核对点 |
|---|---|---|---|
| S1 | [BigscreenVR/VRCFT-Beyond](https://github.com/BigscreenVR/VRCFT-Beyond) `BeyondExtTrackingModule.cs` | **一手**：官方 VRCFT 模块，读自己应用的映射 | 结构声明、映射名、IsValid 判定、`openness = 1 - closed` |
| S2 | [zenuru-merps/ResoniteBeyond2eDriver](https://github.com/zenuru-merps/ResoniteBeyond2eDriver) `src/b2eSharedGazeData.cs`、`b2eTrackingDriver.cs`、`b2eSettings.cs` | 独立第二实现（C#，直接读同一映射） | 字段顺序逐项照抄 S1 并注明 commit `65c7899`；时间戳按 Unix 毫秒实测；换 handedness 时翻转 z；注释「历史版本缺闭眼量字段」 |
| S3 | [Zakhazerd/vrupscale](https://github.com/Zakhazerd/vrupscale) `src/gaze.cpp`、`test/stub/fakegaze.c` | 独立第三实现（**C++**，pack 8 镜像 + `static_assert(sizeof == 64)`） | 总长 64 字节；x 右 / y 上；作者明确不认定 z 符号（用 \|z\| 判前向）；用 memcmp 判「有没有新帧」（说明无序号/事件）；桩程序往 Timestamp 写帧计数 |
| S4 | [ClassicWasTaken/FaceTrackingBridge](https://github.com/ClassicWasTaken/FaceTrackingBridge) `src/BabbleFrameBridge.Worker/Program.cs` | 第四方（反射加载 S1 官方模块，非独立声明） | 映射名 `VRCFTMemmapData` 作为「来源在线」判据；要求「在 Beyond 应用里启用 VRCFT 输出」 |

## 结构（S1 声明逐字段转录，S2/S3 交叉印证）

C# 声明为 `StructLayout(LayoutKind.Sequential)`、无显式 Pack；C++ 侧 S3 用 `#pragma pack(push,8)`
镜像并断言 `sizeof == 64`。int64 在 +40（10 个 float 后自然 8 字节对齐，无插入填充），
60 字节字段 + 4 字节尾部填充 = **64 字节**。

| 偏移 | 类型 | C# 字段名 | 用法 | 状态 |
|---|---|---|---|---|
| +0 | float | `LeftEyeX` | 左眼方向 x：+右 | **已确认**（S1 声明；S2/S3 同序；x=右由 S3 注释与 S2 转换印证） |
| +4 | float | `LeftEyeY` | 左眼方向 y：+上 | **已确认**（同上，S3 注释「y up」） |
| +8 | float | `LeftEyeZ` | 左眼方向 z | **未确认（符号）**：S2 送入左手机前翻转 z，若其引擎为 +Z 前，则原始为 **-Z 前**；S3 作者明说不确定、按 \|z\| 处理；S3 的桩程序写 +Z 前。本模块**按 -Z 前透传**（与 ABI 同系），真机反号只需改 `WriteFrame` 一处 |
| +12 | float | `RightEyeX` | 右眼，同上 | 同 +0 |
| +16 | float | `RightEyeY` | 右眼，同上 | 同 +4 |
| +20 | float | `RightEyeZ` | 右眼，同上 | 同 +8 |
| +24 | float×3 | `CombinedX/Y/Z` | 合成（双视线聚合）方向 | 字段**已确认**；与逐眼是否恒为平均：**未确认**（S3 只把 combined 当首选、缺失时自算双眼平均，暗示其并非总能信任双眼同时有效） |
| +36 | float | `Confidence` | 置信度 | 存在与偏移**已确认**；语义/量程/逐眼还是整帧——**未确认**：S1/S2/S3 **没有任何实现读它**，真机取值范围未知。本模块仅在 `[0,1]` 内透传，否则退保守值 0.5，不参与有效性裁决 |
| +40 | int64 | `Timestamp` | 设备时刻 | 偏移**已确认**；单位 **推断为 Unix 毫秒**（S2 直接与 `UtcNow` 毫秒比较做 10 秒断流，属实际运行验证过的用法）；由 PC 应用写入还是头显设备时钟、是否与系统 UTC 同基准：**未确认**。S3 的桩往里写帧计数（12345 之类），故本模块只在合理纪元区间 [2001, 2100) 毫秒内才换算成 ns，否则置 0 由宿主按接收时刻补 |
| +48 | int32 | `IsValid` | 有效性 | 偏移**已确认**；「==1 有效」为 S1/S2/S3 一致约定；会出现哪些非零值（如头显摘下）：**未确认**。≠1 时本模块一帧不写 |
| +52 | float | `LeftEyeClosedAmount` | 左眼闭眼量 0 睁..1 闭 | 偏移与公式 `openness = 1 - closed` **已确认**（S1 源码；S3 注释「0 open .. 1 闭」并以 >0.7 判眨眼）。**分歧**：S2 声明该字段时注释「Beyond-ET 当时没有这些字段」——旧版应用是否根本不写 +52/+56（恒 0 → 恒全睁）**未确认**，以本清单为准的 S1 是新版声明 |
| +56 | float | `RightEyeClosedAmount` | 右眼闭眼量 | 同上 |

映射名固定 `VRCFTMemmapData`（S1/S3/S4 一致；**已确认**）。命名空间解析：各读取端都以
**裸名**打开成功（C# `MemoryMappedFile.OpenExisting` 与 C++ `OpenFileMappingW` 都落同一
会话 BaseNamedObjects）；是否也有 `Global\` 需求：**未确认**（无实现提及）。

## 与 VRCFT 其它映射的差异（为什么不能套现成约定）

1. **没有计数前缀**：第 0 字节是 `LeftEyeX`，不是自增计数——Kit `SharedMemory::Counter()`
   的惯例在此**不适用**，判「新帧」只能 memcmp 或看时间戳。
2. **没有版本号**：兼容性只能靠「映射 ≥ 64 字节 + IsValid==1 + 数值健全性」这三重土办法。
3. **无命名事件**：S1 以 100 Hz 轮询（`Thread.Sleep(10)`），没有任何实现提到事件名。
4. **撕裂风险未确认**：写入端 64 字节 memcpy 非原子，读一半被改在数学上可能；
   S1/S2/S3 全部接受这一风险（每 tick 重读 + IsValid 门是共同缓解）。本模块同样每 tick 整读。
5. **备用流名** `EyeTrackingMemmapData`：S2 的设置枚举里有第二个名字，语义与差别**未确认**
   （本模块可用 `config.ini` 的 `shm_name` 切换以做真机对比）。

## 能力裁决

- **gaze**：逐眼 + 合成三维方向就在映射里（S1 只用 x/y 投 2D 是 VRCFT 的表达力限制，不是数据限制）→ **声明**。
- **eyes**：闭眼量 → 睁眼度，S1 实际就这么用 → **声明**（旧版应用可能恒 0，已在上面标注）。
- **expression**：S1 的 `Supported => (true, false)`——官方模块自证 Beyond 眼追**不输出表情** → **不声明**。
- **head**：映射里没有头姿 → **不声明**。
- 瞳孔 / 2D 注视点 / 视口标定：映射没有对应字段、也没有显示视口映射依据 → **不写、不声明**，
  不伪造 `FGB_ATTR_POINT_VIEWPORT`。

## 决策规则（实现与本文件的对应）

| 规则 | 依据 |
|---|---|
| `IsValid != 1` → 整帧不写、一个属性位都不置 | S1/S2/S3 一致；「无效」绝不能被解读成「看正前方/全睁」 |
| 方向 NaN 或模长 < 1e-4 → 该眼（或合成）注视位不写，其它属性不受牵连 | 属性位按属性独立是 ABI 语义；不伪造 |
| 置信度只在 [0,1] 内透传，否则 0.5 | 语义未确认，宁保守不夸大 |
| 闭眼量有限且在 [-0.5, 1.5] 截断使用，否则睁眼度位不写 | 容忍噪声越界，拒绝 NaN/离谱值 |
| `Timestamp` 仅当落在 [1e12, 4.1e12) 毫秒区间才映射为 `*_ts_ns = ms × 10⁶`，否则写 0 | 设备时刻与 ABI 同为 Unix epoch，故只换单位不平移；0 让宿主补 `capture_ts_ns`，社区桩的帧计数不会污染时间线 |

## 真机待办（全部对应 docs/verification.md）

1. z 符号：看向远处正中 → `gaze_dir.z` 应为负；若为正，改 `WriteFrame` 一处并更新本文件。
2. Timestamp：与系统时间差值是否稳定在毫秒级。
3. Confidence：真机扫一遍取值范围与含义（逐眼还是整帧）。
4. ClosedAmount：旧版应用是否恒 0；眨眼时是否到 1。
5. `EyeTrackingMemmapData` 与 VRCFT 流的关系。
