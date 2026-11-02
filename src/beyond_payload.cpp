#include "beyond_payload.hpp"

#include <cmath>
#include <cstring>

namespace beyond {

namespace {

// Unix 毫秒的合理纪元区间：2001-09-09 .. 2100-01-01。
// 区间外的一律视为「来源没在写真实时刻」（社区桩程序会往这里写帧计数），
// 用 0 交给宿主按接收时刻补，而不是拿假时刻污染属性时间戳。
constexpr std::int64_t kEpochMsLow = 1000000000000ll;
constexpr std::int64_t kEpochMsHigh = 4102444800000ll;

bool AllFinite(const float* values, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        if (!std::isfinite(values[i])) {
            return false;
        }
    }
    return true;
}

// 上游宣称是方向向量但没人保证已归一：自己归一，长度不足视为「不可用」，
// 返回 false 让调用方跳过这个属性位（不伪造看正前方）。
bool NormalizeDir(const float* in, float (&out)[3]) {
    const float length = std::sqrt(in[0] * in[0] + in[1] * in[1] + in[2] * in[2]);
    if (!(length > kMinGazeLength)) {
        return false;
    }
    out[0] = in[0] / length;
    out[1] = in[1] / length;
    out[2] = in[2] / length;
    return true;
}

// 闭眼量容忍小幅越界（0.001 的负噪声之类）后截断使用；
// NaN 或离谱值（>2）视为该属性不可用。
bool ClosedUsable(float closed) {
    return std::isfinite(closed) && closed >= -0.5f && closed <= 1.5f;
}

} // namespace

std::uint64_t TimestampToNs(std::int64_t raw) {
    if (raw < kEpochMsLow || raw >= kEpochMsHigh) {
        return 0;
    }
    // 设备时刻与 ABI 同为 Unix epoch，只是毫秒 → 纳秒，不做任何平移或缩放；
    // 跨源合并的先后顺序因此仍可与宿主时钟直接比较。
    return static_cast<std::uint64_t>(raw) * 1000000ull;
}

bool ParseSnapshot(const std::uint8_t* data, std::size_t size, Snapshot& out, std::string& error) {
    out = Snapshot{};
    if (data == nullptr) {
        error = "共享内存视图为空";
        return false;
    }
    if (size < kStructBytes) {
        error = "共享内存只有 " + std::to_string(size) + " 字节，少于 SharedGazeData 所需的 " +
                std::to_string(kStructBytes) + " 字节（Bigscreen 眼追应用版本可能不匹配）";
        return false;
    }

    SharedGazeData raw{};
    std::memcpy(&raw, data, sizeof(raw));

    out.is_valid = raw.is_valid == 1;  // 两个独立社区实现都以 ==1 判有效，其余值视为无效
    out.left[0] = raw.left_eye_x;
    out.left[1] = raw.left_eye_y;
    out.left[2] = raw.left_eye_z;
    out.right[0] = raw.right_eye_x;
    out.right[1] = raw.right_eye_y;
    out.right[2] = raw.right_eye_z;
    out.combined[0] = raw.combined_x;
    out.combined[1] = raw.combined_y;
    out.combined[2] = raw.combined_z;

    out.confidence = raw.confidence;
    out.confidence_sane = std::isfinite(raw.confidence) && raw.confidence >= 0.0f &&
                          raw.confidence <= 1.0f;

    out.timestamp = raw.timestamp;
    out.timestamp_ns = TimestampToNs(raw.timestamp);

    out.left_closed = raw.left_eye_closed;
    out.right_closed = raw.right_eye_closed;
    out.left_closed_sane = ClosedUsable(raw.left_eye_closed);
    out.right_closed_sane = ClosedUsable(raw.right_eye_closed);
    return true;
}

bool WriteFrame(const Snapshot& snapshot, FgbFrameSlot* slot) {
    if (slot == nullptr) {
        return false;
    }
    // 无效帧：一个属性位都不置。宿主侧该眼维持「这一帧没有数据」，
    // 而不是被我们伪造成闭眼或看正前方。
    if (!snapshot.is_valid) {
        return false;
    }

    // 置信度语义未确认（已知消费端全部忽略该字段）：能透传就透传，
    // 不能就退到保守值；它不参与「写不写」的裁决，IsValid 才参与。
    const float confidence =
        snapshot.confidence_sane ? fgb::kit::Clamp01(snapshot.confidence) : kUnknownConfidence;

    float direction[3];
    if (AllFinite(snapshot.left, 3) && NormalizeDir(snapshot.left, direction)) {
        std::memcpy(slot->left_eye.gaze_dir, direction, sizeof(direction));
        slot->left_eye.gaze_confidence = confidence;
        slot->left_eye.gaze_ts_ns = snapshot.timestamp_ns;
        slot->attributes |= FGB_ATTR_LEFT_GAZE;
    }
    if (AllFinite(snapshot.right, 3) && NormalizeDir(snapshot.right, direction)) {
        std::memcpy(slot->right_eye.gaze_dir, direction, sizeof(direction));
        slot->right_eye.gaze_confidence = confidence;
        slot->right_eye.gaze_ts_ns = snapshot.timestamp_ns;
        slot->attributes |= FGB_ATTR_RIGHT_GAZE;
    }
    if (AllFinite(snapshot.combined, 3) && NormalizeDir(snapshot.combined, direction)) {
        std::memcpy(slot->combined_gaze_dir, direction, sizeof(direction));
        slot->combined_gaze_confidence = confidence;
        slot->combined_gaze_ts_ns = snapshot.timestamp_ns;
        slot->attributes |= FGB_ATTR_COMBINED_GAZE;
    }

    if (snapshot.left_closed_sane) {
        slot->left_eye.openness = 1.0f - fgb::kit::Clamp01(snapshot.left_closed);
        slot->left_eye.openness_confidence = confidence;
        slot->left_eye.openness_ts_ns = snapshot.timestamp_ns;
        slot->attributes |= FGB_ATTR_LEFT_OPENNESS;
    }
    if (snapshot.right_closed_sane) {
        slot->right_eye.openness = 1.0f - fgb::kit::Clamp01(snapshot.right_closed);
        slot->right_eye.openness_confidence = confidence;
        slot->right_eye.openness_ts_ns = snapshot.timestamp_ns;
        slot->attributes |= FGB_ATTR_RIGHT_OPENNESS;
    }

    constexpr std::uint64_t kEyeBits = FGB_ATTR_LEFT_GAZE | FGB_ATTR_RIGHT_GAZE |
                                       FGB_ATTR_COMBINED_GAZE | FGB_ATTR_LEFT_OPENNESS |
                                       FGB_ATTR_RIGHT_OPENNESS;
    return (slot->attributes & kEyeBits) != 0;
}

std::string MissingMappingHint(const std::string& map_name) {
    return "；请确认 Bigscreen 官方「Beyond Eyetracking」应用正在写入 " + map_name +
           "：头显已连接、眼追已完成登记，并在应用里勾选 VRCFT 接收。" +
           "同一机制只能有一个接收端，请同时关闭 VRCFaceTracking。";
}

} // namespace beyond
