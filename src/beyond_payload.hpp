#pragma once
// Bigscreen Beyond 眼追「命名共享内存」快照的解析与帧写入。
//
// 数据源：Bigscreen 官方「Beyond Eyetracking」应用（Beyond 2e 内置眼追的 PC 端）
// 写命名映射 VRCFTMemmapData，供其 VRCFT 模块读取；社区实现直接读同一映射。
// 格式没有官方文档，是社区逆向——字段顺序以 BigscreenVR/VRCFT-Beyond 的 C# 声明
// （SharedGazeData，LayoutKind.Sequential）为一手依据，逐项「已确认/未确认」
// 与实现间分歧见 docs/protocol.md，本头只镜像线格式。
//
// 线格式：pack 8 的顺序结构，共 64 字节（60 字节字段 + 4 字节尾部填充）：
//   +0  float LeftEyeX / LeftEyeY / LeftEyeZ        左眼注视方向
//   +12 float RightEyeX / RightEyeY / RightEyeZ     右眼注视方向
//   +24 float CombinedX / CombinedY / CombinedZ     合成注视方向
//   +36 float Confidence                            置信度（语义未确认）
//   +40 int64 Timestamp                             设备时刻，按 Unix 毫秒理解
//   +48 int32  IsValid                              ==1 有效（两个独立实现一致）
//   +52 float LeftEyeClosedAmount                   左眼闭眼量，0 睁 .. 1 闭
//   +56 float RightEyeClosedAmount                  右眼闭眼量
// 注意：第 0 字节就是 LeftEyeX——本映射没有 VRCFT 惯例的计数前缀，也没有版本字段，
// 所以 kit/shm.hpp 的 Counter() 约定在这里不适用，长度校验是唯一可用的健全性检查。

#include "fgb/kit/host.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace beyond {

// 上游布局的 C++ 镜像（与 VRCFT-Beyond 的 C# 声明逐字段对齐）。
// 单测对 sizeof 与全部字段偏移做静态断言；改动字段顺序会直接编译失败。
#pragma pack(push, 8)
struct SharedGazeData {
    float left_eye_x = 0.0f;
    float left_eye_y = 0.0f;
    float left_eye_z = 0.0f;
    float right_eye_x = 0.0f;
    float right_eye_y = 0.0f;
    float right_eye_z = 0.0f;
    float combined_x = 0.0f;
    float combined_y = 0.0f;
    float combined_z = 0.0f;
    float confidence = 0.0f;
    std::int64_t timestamp = 0;
    std::int32_t is_valid = 0;
    float left_eye_closed = 0.0f;
    float right_eye_closed = 0.0f;
};
#pragma pack(pop)

// 结构体总长（含尾部填充）；打开映射时按这个长度要求最小字节数。
constexpr std::size_t kStructBytes = 64;

// Confidence 的语义未确认（已知消费端全都没用它）：范围外/非有限的值
// 按这个保守置信度上报，而不是丢掉整帧——IsValid 才是权威性开关。
constexpr float kUnknownConfidence = 0.5f;

// 注视向量的最小模长：低于它说明这一帧的方向不可用（零向量/未初始化），
// 绝不能伪造 (0,0,-1) 当「看正前方」。
constexpr float kMinGazeLength = 1e-4f;

struct Snapshot {
    bool is_valid = false;  // IsValid == 1
    float left[3]{};        // 逐眼方向；x 右 / y 上（已确认），z 符号未确认（见 protocol.md）
    float right[3]{};
    float combined[3]{};
    float confidence = 0.0f;
    bool confidence_sane = false;  // 有限且落在 [0,1]，可直接透传
    std::int64_t timestamp = 0;    // 原样读出的设备时刻
    std::uint64_t timestamp_ns = 0; // 映射到 ABI *_ts_ns；不可信时 0 = 由宿主补
    float left_closed = 0.0f;
    float right_closed = 0.0f;
    bool left_closed_sane = false;  // 有限且未离谱越界（可截断使用）
    bool right_closed_sane = false;
};

// 从映射视图解析一个快照。这里只做「读 + 健全性标注」，不做有效/无效裁决
// （那是 WriteFrame 的事）。size 不足 kStructBytes 返回 false 并给出中文原因。
bool ParseSnapshot(const std::uint8_t* data, std::size_t size, Snapshot& out, std::string& error);

// 设备时刻 → ABI 的纳秒 epoch：按 Unix 毫秒换算（依据：Resonite 驱动实测拿它和
// 本机 UTC 毫秒做 10 秒断流比较，见 protocol.md）。落在合理纪元区间 [2001, 2100)
// 才换算；其余（0、帧计数、微秒级时间戳等）返回 0，表示「不覆盖宿主时刻」。
std::uint64_t TimestampToNs(std::int64_t raw);

// IsValid/Confidence 真正参与决策：
//   * IsValid != 1 → 返回 false，一个属性位都不置；
//   * 方向 NaN/过短 → 该眼（或合成）的注视位不置，其余属性不受牵连；
//   * 闭眼量 NaN/离谱 → 该眼睁眼度位不置。
// 返回 true 表示至少写了一个属性位。
bool WriteFrame(const Snapshot& snapshot, FgbFrameSlot* slot);

// 映射打不开时追加在错误后的中文提示：指向「官方眼追应用是否在写」。
std::string MissingMappingHint(const std::string& map_name);

} // namespace beyond
