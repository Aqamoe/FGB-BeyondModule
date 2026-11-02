# FGB BeyondModule

[![build](https://github.com/Aqamoe/FGB-BeyondModule/actions/workflows/ci.yml/badge.svg)](https://github.com/Aqamoe/FGB-BeyondModule/actions/workflows/ci.yml)

**状态：0.1.0，未验证（untested）**。线格式为社区逆向（无官方文档），字段偏移/有效帧与无效帧
两条路径、置信度传递与断流处理都有单元测试和合成共享内存端到端冒烟；真机（Bigscreen Beyond 2e）
验收待补，见 [docs/verification.md](docs/verification.md)。

[FaceGaze Bridge](https://github.com/Aqamoe/FaceGaze-Bridge) 的 Bigscreen Beyond 眼追原生模块。
直接读取 **Bigscreen 官方「Beyond Eyetracking」应用**写的命名共享内存 `VRCFTMemmapData`
（VRCFT 通用内存映射格式），输出逐眼/合成注视方向与睁眼度。
不需要 VRCFT，不注入任何进程，也不需要管理员权限。

Native eye-tracking module for FaceGaze Bridge. Reads the named shared memory
(`VRCFTMemmapData`) that Bigscreen's official Beyond Eyetracking app publishes and
outputs per-eye / combined gaze directions plus eye openness. No VRCFT, no process
injection, no admin rights.

## 功能 / Features

- **注视**：左眼、右眼、合成三个三维方向（HMD 局部单位向量）直接进 `gaze` 通道，
  置信度按上游 `Confidence` 透传（语义未确认，范围外退保守值）。
- **眼睛**：睁眼度 = `1 - 闭眼量`，逐眼独立；`IsValid != 1` 的帧**一个属性位都不写**，
  不伪造「看正前方」或「全睁」。
- **断流可诊断**：映射不存在或 10 秒无新数据时，模块卡片直接说明
  「官方眼追应用是否在写 VRCFTMemmapData」；恢复后自动回到活跃。
- **诚实的协议**：每个字段的确认状态见 [docs/protocol.md](docs/protocol.md)，
  一手依据是 Bigscreen 官方 VRCFT 模块源码，另与三个独立社区实现交叉核对。

## 不提供 / Not provided

| 数据 | 原因 |
|---|---|
| 表情 | 官方模块自证 Beyond 眼追输出不含表情（`Supported = (eye: true, expression: false)`） |
| 头姿 | 映射里没有对应字段 |
| 瞳孔 | 映射里没有对应字段 |
| 2D 注视点 | 只有三维方向，没有到显示视口的标定依据；不伪造 `POINT_VIEWPORT` |

## 环境要求

| 项 | 要求 |
|---|---|
| 系统 | Windows 10 / 11 x64 |
| 宿主 | FaceGaze Bridge（原生模块 ABI v2） |
| 硬件 | Bigscreen Beyond 2e（带眼追），头显保持连接 |
| 来源 | Bigscreen 官方「Beyond Eyetracking」应用运行中，且**勾选 VRCFT 接收** |
| 其它 | 关闭 VRCFaceTracking：它和本模块抢同一个共享内存机制 |

## 安装

1. 从 Releases 下载 `beyond-0.1.0.zip`。
2. FaceGaze Bridge →「模块库 → 原生模块 → 安装原生模块 (.zip)」，或「模块」页「从 .zip 安装」。
3. 「模块」页把 **Bigscreen Beyond** 设为启动，角色给 `eyes` 与 `gaze`。
4. 重启引擎；戴上头显、眼追应用开始写入后模块卡片应显示活跃。

## 配置

模块目录下可选 `config.ini`：

```ini
; 默认 VRCFTMemmapData；社区另一实现还见过 EyeTrackingMemmapData（语义未确认，仅供对比试验）
shm_name = VRCFTMemmapData
```

改完重启引擎。未知键会被记录一条警告，不会静默失效。

## 从源码构建

需要 Visual Studio 2022（x64）、CMake ≥ 3.24 与 Node 20（脚本用）。

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
```

SDK 与 Kit 由 `sdk.lock.json` / `kit.lock.json` 在配置阶段拉取并校验 SHA256，仓库里不含 SDK 头文件副本。
本地想指向已解压的 Kit：

```powershell
cmake -S . -B build -A x64 -DFGB_KIT_DIR="D:/code/FGB-Module/FGB-Module-Kit"
```

打包与测试：

```powershell
cmake --build build --config Release --target package-modules   # build/Release/beyond-<版本>.zip
ctest --test-dir build -C Release --output-on-failure           # 协议与映射单测
cmake --build build --config Release --target smoke-test        # 真实 ABI + 合成共享内存冒烟（需映射名空闲）
```

## 目录结构

```
src/main.cpp                ABI 生命周期、共享内存打开/重连、断流状态上报
src/beyond_payload.*        64 字节快照解析与 eyes/gaze 帧写入（IsValid 门控）
module/                     fgb-native.json 与图标
tests/                      协议单测、真实 ABI 冒烟
docs/protocol.md            线格式核对：逐项「已确认/未确认」与实现间分歧
docs/verification.md        真机验收清单（同时裁决协议未确认项）
cmake/                      依赖锁解析（SDK / Kit 都是拉取，不复制）
```

## 排错

| 现象 | 处理 |
|---|---|
| 卡片报「打不开共享内存 VRCFTMemmapData」 | 确认官方眼追应用在运行、已勾选 VRCFT 接收、头显已连接；并退出 VRCFT（抢同一机制） |
| 报「已 10 秒以上没有新数据」 | 眼追应用可能暂停写入：重新戴上头显、检查应用是否被更新或改过输出开关 |
| 反复提示 `IsValid != 1` | 上游自己报告无效（未佩戴/未登记/追踪未启用），属预期，本模块不会输出假数据 |
| 注视方向左右/前后反了 | z 符号是**未确认项**（见 docs/protocol.md）：请在 issue 里附版本与观察，改动只在一处 |
| 睁眼度恒为 1 | 旧版眼追应用可能不写闭眼量字段（社区记录在案）：升级官方应用或在 issue 里附版本号 |

## 致谢 / Attribution

- 线格式一手依据：Bigscreen 官方 VRCFT 模块 [VRCFT-Beyond](https://github.com/BigscreenVR/VRCFT-Beyond)；
  交叉核对参考 [ResoniteBeyond2eDriver](https://github.com/zenuru-merps/ResoniteBeyond2eDriver) 与
  [vrupscale](https://github.com/Zakhazerd/vrupscale)。本仓库是该线格式的独立 C++ 实现，未包含其源码。
- ABI 头与宿主：[FaceGaze Bridge](https://github.com/Aqamoe/FaceGaze-Bridge)；共享工具链：[FGB-Module-Kit](https://github.com/Aqamoe/FGB-Module-Kit)。

## 许可

Apache License 2.0，见 [LICENSE](LICENSE) 与 [NOTICE](NOTICE)。
