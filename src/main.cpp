// Bigscreen Beyond → FaceGaze Bridge 原生模块。
//
// 读取 Bigscreen 官方「Beyond Eyetracking」应用写的命名共享内存 VRCFTMemmapData，
// 输出逐眼/合成注视方向与睁眼度（eyes + gaze 通道）。
// 不需要 VRCFT，不注入任何进程，也不需要管理员权限。
//
// 线格式是社区逆向（无官方文档），逐项依据与分歧见 docs/protocol.md；
// IsValid != 1 的帧一个属性位都不写，时间戳只在合理纪元区间内才映射到 ABI 纳秒。

#include "beyond_payload.hpp"

#include "fgb/kit/config.hpp"
#include "fgb/kit/module.hpp"
#include "fgb/kit/shm.hpp"

#include <cstdint>
#include <cstring>
#include <string>

namespace {

constexpr char kDefaultMapName[] = "VRCFTMemmapData";
constexpr std::uint64_t kNoDataTimeoutNs = 10ull * 1000ull * 1000ull * 1000ull;
constexpr std::uint64_t kWarnIntervalNs = 5ull * 1000ull * 1000ull * 1000ull;
// 映射不存在的重试节奏：来源应用可能比引擎晚启动，每秒看一眼就够，
// 也避免打不开时每 tick 刷一条状态。
constexpr std::uint64_t kRetryIntervalNs = 1ull * 1000ull * 1000ull * 1000ull;

// 映射名是纯 ASCII（VRCFTMemmapData / EyeTrackingMemmapData），手写窄→宽转换；
// 非 ASCII 字符按 '?' 处理，打不开时日志会原样回显，不静默改名。
std::wstring ToWide(const std::string& text) {
    std::wstring out;
    out.reserve(text.size());
    for (const char raw : text) {
        const unsigned char c = static_cast<unsigned char>(raw);
        out.push_back(c < 0x80 ? static_cast<wchar_t>(c) : L'?');
    }
    return out;
}

class BeyondSource : public fgb::kit::IModuleSource {
public:
    bool Start(fgb::kit::Host& host, const FgbInitParams& params, std::string& error) override {
        (void)error;
        fgb::kit::ConfigIni config;
        std::string config_error;
        const std::string directory = params.module_dir != nullptr ? params.module_dir : "";
        std::string map_name = kDefaultMapName;
        config.AllowKeys({"shm_name"});
        if (config.Load(directory, "config.ini", config_error)) {
            map_name = config.Get("shm_name", kDefaultMapName);
            if (!config_error.empty()) {
                host.Warn("config.ini：" + config_error);
            }
            if (!config.UnknownKeys().empty()) {
                host.Warn("config.ini 有未识别的键：" + config.UnknownKeys()[0]);
            }
        }
        map_name_ = map_name;
        map_name_wide_ = ToWide(map_name);

        host.Info("Bigscreen Beyond 读取共享内存 " + map_name_ + "，等待眼追应用写入");
        host.StatusStarting("等待 Bigscreen Beyond 眼追数据");
        return true;
    }

    bool Read(fgb::kit::Host& host, FgbFrameSlot& slot, std::uint64_t now_ns) override {
        if (!OpenMapping(host, now_ns)) {
            return false;
        }

        beyond::Snapshot snapshot;
        std::string error;
        if (!beyond::ParseSnapshot(shm_.data(), shm_.Size(), snapshot, error)) {
            host.WarnThrottled(warn_ns_, now_ns, kWarnIntervalNs, "共享内存读数不可用：" + error);
            return false;
        }

        // 断流检测：这个映射没有计数前缀也没有事件，只能盯内容变化。
        // 写入端每帧都会更新 Timestamp/方向，只要应用在写，64 字节总会变。
        if (!have_last_ || std::memcmp(last_, shm_.data(), beyond::kStructBytes) != 0) {
            std::memcpy(last_, shm_.data(), beyond::kStructBytes);
            have_last_ = true;
            last_change_ns_ = now_ns;
        }
        if (!CheckFreshness(host, now_ns)) {
            return false;
        }

        if (!beyond::WriteFrame(snapshot, &slot)) {
            if (!snapshot.is_valid) {
                host.WarnThrottled(warn_ns_, now_ns, kWarnIntervalNs,
                                   "IsValid != 1：眼追应用报告当前帧无效"
                                   "（头显未连接、未佩戴或眼追未启用时会出现），本帧不输出");
            } else {
                host.WarnThrottled(warn_ns_, now_ns, kWarnIntervalNs,
                                   "帧有效但方向与闭眼量都不可用（NaN/零向量），本帧不输出");
            }
            return false;
        }

        if (!active_reported_) {
            active_reported_ = true;
            host.Info("Bigscreen Beyond 数据正常：置信度 " + FormatConfidence(snapshot) +
                      "，睁眼度 左 " + FormatClosed(snapshot.left_closed, snapshot.left_closed_sane) +
                      " 右 " + FormatClosed(snapshot.right_closed, snapshot.right_closed_sane));
        }
        return true;
    }

    void Stop(fgb::kit::Host& host) override {
        shm_.Close();
        host.StatusStopped("Bigscreen Beyond 已停止");
    }

private:
    bool OpenMapping(fgb::kit::Host& host, std::uint64_t now_ns) {
        if (shm_.Opened()) {
            return true;
        }
        if (retry_ns_ != 0 && now_ns - retry_ns_ < kRetryIntervalNs) {
            return false;
        }
        retry_ns_ = now_ns;
        std::string error;
        if (!shm_.Open(map_name_wide_, beyond::kStructBytes, error)) {
            // Host 只有 Warn 有内置限频，状态错误自己按同样的节奏刷。
            if (status_error_ns_ == 0 || now_ns - status_error_ns_ >= kWarnIntervalNs) {
                status_error_ns_ = now_ns;
                host.StatusError(error + beyond::MissingMappingHint(map_name_));
            }
            return false;
        }
        host.Info("已打开共享内存 " + map_name_ + "，映射 " + std::to_string(shm_.Size()) + " 字节");
        // 重开（来源应用重启）后复位变化检测与断流状态。
        have_last_ = false;
        last_change_ns_ = 0;
        healthy_ = false;
        silent_reported_ = false;
        active_reported_ = false;
        return true;
    }

    // 自管的新鲜度（不套 kit::Staleness：它的文案面向网络来源，这里是本机映射）。
    bool CheckFreshness(fgb::kit::Host& host, std::uint64_t now_ns) {
        const bool fresh = last_change_ns_ != 0 && now_ns >= last_change_ns_ &&
                           now_ns - last_change_ns_ <= kNoDataTimeoutNs;
        if (fresh) {
            if (!healthy_) {
                healthy_ = true;
                if (silent_reported_) {
                    host.StatusActive("Bigscreen Beyond 数据已恢复");
                    silent_reported_ = false;
                }
            }
            return true;
        }
        healthy_ = false;
        if (silent_reported_) {
            return false;
        }
        silent_reported_ = true;
        host.StatusError("共享内存 " + map_name_ + " 已 10 秒以上没有新数据，"
                         "确认 Bigscreen 眼追应用是否还在写入（头显连接、VRCFT 接收勾选未变）");
        return false;
    }

    static std::string FormatConfidence(const beyond::Snapshot& snapshot) {
        if (!snapshot.confidence_sane) {
            return "不可用（范围外/NaN），按保守值 " + std::to_string(beyond::kUnknownConfidence) +
                   " 上报";
        }
        return std::to_string(snapshot.confidence);
    }

    static std::string FormatClosed(float closed, bool sane) {
        if (!sane) {
            return "不可用";
        }
        return std::to_string(closed);
    }

    fgb::kit::SharedMemory shm_;
    std::string map_name_;
    std::wstring map_name_wide_;
    std::uint64_t retry_ns_ = 0;
    std::uint64_t warn_ns_ = 0;
    std::uint64_t status_error_ns_ = 0;
    std::uint8_t last_[beyond::kStructBytes]{};
    bool have_last_ = false;
    std::uint64_t last_change_ns_ = 0;
    bool healthy_ = false;
    bool silent_reported_ = false;
    bool active_reported_ = false;
};

} // namespace

FGB_KIT_EXPORT(BeyondSource, "com.fgbbridge.beyond", "Bigscreen Beyond",
               FGB_CAP_EYES | FGB_CAP_GAZE)
