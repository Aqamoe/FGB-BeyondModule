#include "beyond_payload.hpp"

#include "fgb/kit/testing/check.hpp"
#include "fgb/kit/testing/host_mock.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

using namespace fgb::kit;
using beyond::ParseSnapshot;
using beyond::SharedGazeData;
using beyond::Snapshot;

namespace {

// ---- 线格式静态断言：字段顺序照 VRCFT-Beyond 的 C# 声明，改动即编译失败 ----
static_assert(sizeof(SharedGazeData) == 64, "SharedGazeData 必须是 64 字节（含尾部填充）");
static_assert(offsetof(SharedGazeData, left_eye_x) == 0, "LeftEyeX 在 +0（映射没有计数前缀）");
static_assert(offsetof(SharedGazeData, left_eye_z) == 8, "LeftEyeZ 在 +8");
static_assert(offsetof(SharedGazeData, right_eye_x) == 12, "RightEyeX 在 +12");
static_assert(offsetof(SharedGazeData, combined_x) == 24, "CombinedX 在 +24");
static_assert(offsetof(SharedGazeData, confidence) == 36, "Confidence 在 +36");
static_assert(offsetof(SharedGazeData, timestamp) == 40, "Timestamp 在 +40（8 字节对齐）");
static_assert(offsetof(SharedGazeData, is_valid) == 48, "IsValid 在 +48");
static_assert(offsetof(SharedGazeData, left_eye_closed) == 52, "LeftEyeClosedAmount 在 +52");
static_assert(offsetof(SharedGazeData, right_eye_closed) == 56, "RightEyeClosedAmount 在 +56");

// 一个「像真机」的有效帧：单位方向（-Z 前，见 protocol.md 的未确认项与依据）、
// Unix 毫秒时刻、置信度 0.9、左眼半闭 0.3。
SharedGazeData MakeValidFrame() {
    SharedGazeData raw{};
    const float left[3] = {0.1f, 0.2f, -0.974672f};    // 已经归一
    const float right[3] = {0.1f, 0.2f, -0.974672f};
    const float combined[3] = {0.0f, 0.0f, -1.0f};
    std::memcpy(&raw.left_eye_x, left, sizeof(left));
    std::memcpy(&raw.right_eye_x, right, sizeof(right));
    std::memcpy(&raw.combined_x, combined, sizeof(combined));
    raw.confidence = 0.9f;
    raw.timestamp = 1750000000123ll;  // Unix 毫秒
    raw.is_valid = 1;
    raw.left_eye_closed = 0.3f;
    raw.right_eye_closed = 0.0f;
    return raw;
}

FgbFrameSlot FreshSlot() {
    FgbFrameSlot slot{};
    slot.struct_size = sizeof(FgbFrameSlot);
    slot.abi_version = FGB_MODULE_ABI_V2;
    slot.shape_count = FGB_MAX_SHAPES;
    return slot;
}

constexpr std::uint64_t kEyeBits = FGB_ATTR_LEFT_GAZE | FGB_ATTR_RIGHT_GAZE |
                                   FGB_ATTR_COMBINED_GAZE | FGB_ATTR_LEFT_OPENNESS |
                                   FGB_ATTR_RIGHT_OPENNESS;

} // namespace

int main() {
    std::string error;

    // ---- 逐偏移落位：每个字段写在声明的字节偏移上，解析必须原样对上 ----
    SharedGazeData probe = MakeValidFrame();
    probe.left_eye_x = 0.11f;
    probe.left_eye_y = 0.12f;
    probe.left_eye_z = 0.13f;
    probe.right_eye_x = 0.21f;
    probe.right_eye_y = 0.22f;
    probe.right_eye_z = 0.23f;
    probe.combined_x = 0.31f;
    probe.combined_y = 0.32f;
    probe.combined_z = 0.33f;
    probe.confidence = 0.44f;
    probe.is_valid = 1;
    probe.left_eye_closed = 0.55f;
    probe.right_eye_closed = 0.66f;

    Snapshot parsed{};
    CHECK(ParseSnapshot(reinterpret_cast<const std::uint8_t*>(&probe), sizeof(probe), parsed,
                        error));
    CHECK_NEAR(parsed.left[0], 0.11f, 1e-6);
    CHECK_NEAR(parsed.left[2], 0.13f, 1e-6);
    CHECK_NEAR(parsed.right[1], 0.22f, 1e-6);
    CHECK_NEAR(parsed.combined[2], 0.33f, 1e-6);
    CHECK_NEAR(parsed.confidence, 0.44f, 1e-6);
    CHECK_EQ(parsed.timestamp, 1750000000123ll);
    CHECK(parsed.is_valid);
    CHECK_NEAR(parsed.left_closed, 0.55f, 1e-6);
    CHECK_NEAR(parsed.right_closed, 0.66f, 1e-6);
    static_assert(sizeof(SharedGazeData) == beyond::kStructBytes, "kStructBytes 必须等于 sizeof");

    // ---- 有效帧路径：属性位、数值、时间戳、置信度 ----
    SharedGazeData valid = MakeValidFrame();
    Snapshot frame{};
    CHECK(ParseSnapshot(reinterpret_cast<const std::uint8_t*>(&valid), sizeof(valid), frame,
                        error));
    FgbFrameSlot slot = FreshSlot();
    CHECK(beyond::WriteFrame(frame, &slot));
    CHECK_EQ(slot.attributes & kEyeBits, kEyeBits);  // 逐眼 + 合成 + 双眼睁眼度全写
    CHECK((slot.attributes & (FGB_ATTR_EXPRESSION | FGB_ATTR_HEAD)) == 0);

    CHECK_NEAR(slot.left_eye.gaze_dir[0], 0.1f, 1e-5);
    CHECK_NEAR(slot.left_eye.gaze_dir[2], -0.974672f, 1e-5);  // -Z 前（透传，见 protocol.md）
    const float length = std::sqrt(slot.left_eye.gaze_dir[0] * slot.left_eye.gaze_dir[0] +
                                   slot.left_eye.gaze_dir[1] * slot.left_eye.gaze_dir[1] +
                                   slot.left_eye.gaze_dir[2] * slot.left_eye.gaze_dir[2]);
    CHECK_NEAR(length, 1.0, 1e-5);
    CHECK_NEAR(slot.combined_gaze_dir[2], -1.0f, 1e-6);

    // 睁眼度 = 1 - 闭眼量（官方与 Resonite 两个实现同式）
    CHECK_NEAR(slot.left_eye.openness, 0.7f, 1e-6);
    CHECK_NEAR(slot.right_eye.openness, 1.0f, 1e-6);

    // 置信度透传；时间戳：Unix 毫秒 → 纳秒，不做平移
    CHECK_NEAR(slot.left_eye.gaze_confidence, 0.9f, 1e-6);
    CHECK_NEAR(slot.combined_gaze_confidence, 0.9f, 1e-6);
    CHECK_EQ(slot.left_eye.gaze_ts_ns, 1750000000123ull * 1000000ull);
    CHECK_EQ(slot.right_eye.openness_ts_ns, 1750000000123ull * 1000000ull);
    CHECK_EQ(slot.combined_gaze_ts_ns, 1750000000123ull * 1000000ull);

    // ---- 无效帧路径：IsValid != 1 时一个位都不置，不伪造任何默认值 ----
    SharedGazeData invalid = MakeValidFrame();
    invalid.is_valid = 0;
    Snapshot invalid_frame{};
    CHECK(ParseSnapshot(reinterpret_cast<const std::uint8_t*>(&invalid), sizeof(invalid),
                        invalid_frame, error));
    CHECK(!invalid_frame.is_valid);
    FgbFrameSlot dead = FreshSlot();
    CHECK(!beyond::WriteFrame(invalid_frame, &dead));
    CHECK_EQ(dead.attributes, 0ull);
    CHECK_NEAR(dead.left_eye.gaze_dir[2], 0.0f, 1e-6);  // 保持全零，而不是 (0,0,-1)
    // 其它非 1 的取值同样按无效处理（社区实现的约定是 ==1 才有效）；解析本身成功
    invalid.is_valid = 2;
    CHECK(ParseSnapshot(reinterpret_cast<const std::uint8_t*>(&invalid), sizeof(invalid),
                        invalid_frame, error));
    CHECK(!invalid_frame.is_valid);

    // ---- 置信度传递：范围外/NaN 退保守值 0.5，但不否决整帧 ----
    SharedGazeData weird_conf = MakeValidFrame();
    weird_conf.confidence = 1.7f;  // 超出 [0,1]：语义未确认，不能用截断值冒充高置信
    Snapshot wc{};
    CHECK(ParseSnapshot(reinterpret_cast<const std::uint8_t*>(&weird_conf), sizeof(weird_conf),
                        wc, error));
    CHECK(!wc.confidence_sane);
    FgbFrameSlot wc_slot = FreshSlot();
    CHECK(beyond::WriteFrame(wc, &wc_slot));
    CHECK_NEAR(wc_slot.left_eye.gaze_confidence, beyond::kUnknownConfidence, 1e-6);

    weird_conf.confidence = std::nanf("");
    CHECK(ParseSnapshot(reinterpret_cast<const std::uint8_t*>(&weird_conf), sizeof(weird_conf),
                        wc, error));
    FgbFrameSlot nan_slot = FreshSlot();
    CHECK(beyond::WriteFrame(wc, &nan_slot));
    CHECK_NEAR(nan_slot.left_eye.gaze_confidence, beyond::kUnknownConfidence, 1e-6);
    CHECK_NEAR(nan_slot.left_eye.openness, 0.7f, 1e-6);  // 其它字段照常

    weird_conf.confidence = 0.32f;
    CHECK(ParseSnapshot(reinterpret_cast<const std::uint8_t*>(&weird_conf), sizeof(weird_conf),
                        wc, error));
    CHECK(wc.confidence_sane);
    FgbFrameSlot pass_slot = FreshSlot();
    CHECK(beyond::WriteFrame(wc, &pass_slot));
    CHECK_NEAR(pass_slot.combined_gaze_confidence, 0.32f, 1e-6);

    // ---- NaN / 越界 / 零向量：按眼独立裁决，坏的那只不置位 ----
    SharedGazeData broken_dir = MakeValidFrame();
    broken_dir.left_eye_y = std::nanf("");
    broken_dir.right_eye_x = 0.0f;  // 合成与双眼全零向量 → 方向不可用
    broken_dir.right_eye_y = 0.0f;
    broken_dir.right_eye_z = 0.0f;
    broken_dir.combined_x = 0.0f;
    broken_dir.combined_y = 0.0f;
    broken_dir.combined_z = 0.0f;
    broken_dir.left_eye_closed = std::nanf("");  // 闭眼量 NaN：眼睁眼度位也不置
    Snapshot broken{};
    CHECK(ParseSnapshot(reinterpret_cast<const std::uint8_t*>(&broken_dir), sizeof(broken_dir),
                        broken, error));
    CHECK(!broken.left_closed_sane);
    FgbFrameSlot partial = FreshSlot();
    CHECK(beyond::WriteFrame(broken, &partial));  // 右眼睁眼度仍可用 → 有输出
    CHECK_EQ(partial.attributes & FGB_ATTR_LEFT_GAZE, 0ull);
    CHECK_EQ(partial.attributes & FGB_ATTR_RIGHT_GAZE, 0ull);
    CHECK_EQ(partial.attributes & FGB_ATTR_COMBINED_GAZE, 0ull);
    CHECK_EQ(partial.attributes & FGB_ATTR_LEFT_OPENNESS, 0ull);
    CHECK((partial.attributes & FGB_ATTR_RIGHT_OPENNESS) != 0);
    CHECK_NEAR(partial.right_eye.openness, 1.0f, 1e-6);

    // 闭眼量小幅越界：截断而不是丢属性（1.5 内视为噪声，>1.5 或 NaN 视为不可用）
    SharedGazeData clamped = MakeValidFrame();
    clamped.left_eye_closed = 1.2f;
    clamped.right_eye_closed = 3.0f;
    Snapshot cl{};
    CHECK(ParseSnapshot(reinterpret_cast<const std::uint8_t*>(&clamped), sizeof(clamped), cl,
                        error));
    CHECK(cl.left_closed_sane);
    CHECK(!cl.right_closed_sane);
    FgbFrameSlot clamp_slot = FreshSlot();
    CHECK(beyond::WriteFrame(cl, &clamp_slot));
    CHECK_NEAR(clamp_slot.left_eye.openness, 0.0f, 1e-6);
    CHECK_EQ(clamp_slot.attributes & FGB_ATTR_RIGHT_OPENNESS, 0ull);

    // ---- 时间戳映射：合理 Unix 毫秒才换算；帧计数/0/微秒全部回退 0（宿主补时刻） ----
    CHECK_EQ(beyond::TimestampToNs(1750000000123ll), 1750000000123ull * 1000000ull);
    CHECK_EQ(beyond::TimestampToNs(0ll), 0ull);
    CHECK_EQ(beyond::TimestampToNs(12345ll), 0ull);            // 社区桩程序的帧计数
    CHECK_EQ(beyond::TimestampToNs(-5ll), 0ull);               // 负数
    CHECK_EQ(beyond::TimestampToNs(1750000000123456ll), 0ull); // 微秒级？区间外，不猜

    SharedGazeData junk_ts = MakeValidFrame();
    junk_ts.timestamp = 42ll;
    Snapshot jt{};
    CHECK(ParseSnapshot(reinterpret_cast<const std::uint8_t*>(&junk_ts), sizeof(junk_ts), jt,
                        error));
    CHECK_EQ(jt.timestamp_ns, 0ull);
    FgbFrameSlot jt_slot = FreshSlot();
    CHECK(beyond::WriteFrame(jt, &jt_slot));
    CHECK_EQ(jt_slot.left_eye.gaze_ts_ns, 0ull);  // 0 = 用帧的 capture_ts_ns（宿主接收时刻）

    // ---- 长度不足必须拒绝，文案给出期望字节数 ----
    SharedGazeData small = MakeValidFrame();
    Snapshot too_short{};
    CHECK(!ParseSnapshot(reinterpret_cast<const std::uint8_t*>(&small), 60, too_short, error));
    CHECK(error.find("60 字节") != std::string::npos);
    CHECK(error.find("64 字节") != std::string::npos);
    CHECK(!ParseSnapshot(nullptr, 128, too_short, error));

    // ---- 映射缺失提示：必须指向「官方眼追应用是否在写」 ----
    const std::string hint = beyond::MissingMappingHint("VRCFTMemmapData");
    CHECK(hint.find("VRCFTMemmapData") != std::string::npos);
    CHECK(hint.find("Beyond Eyetracking") != std::string::npos);
    CHECK(hint.find("VRCFT 接收") != std::string::npos);
    CHECK(hint.find("VRCFaceTracking") != std::string::npos);

    return CheckSummary("beyond-payload");
}
