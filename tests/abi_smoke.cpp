// 走真实 ABI 的端到端冒烟：加载 DLL → query → initialize → 喂自建共享内存的合成帧
// → update → 检查提交帧。
//
// 它会占用映射名 VRCFTMemmapData —— 运行前关闭 Bigscreen 眼追应用与 VRCFT，
// 它们都在写/抢同一个机制。因此不挂进 ctest，只由 `cmake --build ... --target smoke-test` 触发。

#include "beyond_payload.hpp"

#include "fgb/kit/testing/check.hpp"
#include "fgb/kit/testing/host_mock.hpp"
#include "fgb_module_api.h"

#include <winsock2.h>
#include <windows.h>

#include <chrono>
#include <cstring>
#include <string>
#include <thread>

namespace {

constexpr wchar_t kMapName[] = L"VRCFTMemmapData";

// 合成有效帧：单位方向（-Z 前）、Unix 毫秒时刻、置信度 0.9、左眼半闭 0.3。
beyond::SharedGazeData MakeValidFrame() {
    beyond::SharedGazeData raw{};
    const float left[3] = {0.1f, 0.2f, -0.974672f};
    const float right[3] = {0.1f, 0.2f, -0.974672f};
    const float combined[3] = {0.0f, 0.0f, -1.0f};
    std::memcpy(&raw.left_eye_x, left, sizeof(left));
    std::memcpy(&raw.right_eye_x, right, sizeof(right));
    std::memcpy(&raw.combined_x, combined, sizeof(combined));
    raw.confidence = 0.9f;
    raw.timestamp = 1750000000123ll;
    raw.is_valid = 1;
    raw.left_eye_closed = 0.3f;
    raw.right_eye_closed = 0.0f;
    return raw;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("用法：%s <beyond.dll 路径>\n", argv[0]);
        return 2;
    }

    HMODULE library = ::LoadLibraryExA(argv[1], nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    CHECK(library != nullptr);
    if (library == nullptr) {
        std::printf("LoadLibrary 失败，错误码 %lu（DLL 是否已构建？依赖是否在同目录？）\n",
                    ::GetLastError());
        return CheckSummary("beyond-abi-smoke");
    }

    const auto query = reinterpret_cast<decltype(&fgb_module_query_v2)>(
        ::GetProcAddress(library, "fgb_module_query_v2"));
    CHECK(query != nullptr);

    fgb::kit::testing::HostMock host;
    FgbInitParams params = host.MakeParams();

    const FgbModule* module = query(FGB_MODULE_ABI_V2);
    CHECK(module != nullptr);
    if (module == nullptr) {
        ::FreeLibrary(library);
        return CheckSummary("beyond-abi-smoke");
    }
    CHECK(module->id == std::string{"com.fgbbridge.beyond"});
    CHECK_EQ(module->abi_version, FGB_MODULE_ABI_V2);
    CHECK_EQ(module->capabilities,
             static_cast<std::uint32_t>(FGB_CAP_EYES | FGB_CAP_GAZE));
    CHECK(query(FGB_MODULE_ABI_V2 + 1) == nullptr);  // ABI 不匹配必须拒绝

    // ---- 阶段 1：映射还不存在 → 初始化成功但不出帧，状态报错指向眼追应用 ----
    void* self = nullptr;
    CHECK_EQ(module->initialize(&self, host.services(), &params), 1);
    if (self == nullptr) {
        std::printf("initialize 失败：%s\n", host.LastStatusDetail().c_str());
        ::FreeLibrary(library);
        return CheckSummary("beyond-abi-smoke");
    }
    module->update(self);
    CHECK(host.committed().empty());
    CHECK(host.HasStatus(FGB_STATUS_ERROR));
    CHECK(host.LastStatusDetail().find("VRCFTMemmapData") != std::string::npos);
    CHECK(host.LastStatusDetail().find("Beyond Eyetracking") != std::string::npos);
    CHECK(host.LastStatusDetail().find("VRCFT 接收") != std::string::npos);
    module->teardown(self);

    // ---- 阶段 2：自建映射写「无效帧」→ 仍不提交（IsValid 参与决策） ----
    HANDLE mapping = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                          static_cast<DWORD>(beyond::kStructBytes), kMapName);
    CHECK(mapping != nullptr);
    if (mapping == nullptr) {
        std::printf("CreateFileMapping 失败，错误码 %lu（VRCFTMemmapData 是否被真实眼追应用占用？）\n",
                    ::GetLastError());
        ::FreeLibrary(library);
        return CheckSummary("beyond-abi-smoke");
    }
    auto* view = static_cast<beyond::SharedGazeData*>(
        ::MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, beyond::kStructBytes));
    CHECK(view != nullptr);
    if (view == nullptr) {
        ::CloseHandle(mapping);
        ::FreeLibrary(library);
        return CheckSummary("beyond-abi-smoke");
    }
    *view = beyond::SharedGazeData{};  // 全零：IsValid == 0

    CHECK_EQ(module->initialize(&self, host.services(), &params), 1);
    // 新实例第一次 update 就会尝试打开映射（重试节流只在失败后生效）
    for (int attempt = 0; attempt < 5; ++attempt) {
        module->update(self);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(host.committed().empty());  // 无效帧一条都不提交

    // ---- 阶段 2b：写入端停滞（数据不变）→ 10 秒后按断流报错 ----
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(11500);
    while (std::chrono::steady_clock::now() < deadline) {
        module->update(self);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    CHECK(host.LastStatusDetail().find("没有新数据") != std::string::npos);

    // ---- 阶段 3：写入合成有效帧 → 提交并逐项核对 ----
    *view = MakeValidFrame();
    for (int attempt = 0; attempt < 100 && host.committed().empty(); ++attempt) {
        module->update(self);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(!host.committed().empty());
    if (!host.committed().empty()) {
        const FgbFrameSlot& slot = host.committed().back();
        CHECK((slot.attributes & FGB_ATTR_LEFT_GAZE) != 0);
        CHECK((slot.attributes & FGB_ATTR_RIGHT_GAZE) != 0);
        CHECK((slot.attributes & FGB_ATTR_COMBINED_GAZE) != 0);
        CHECK((slot.attributes & FGB_ATTR_LEFT_OPENNESS) != 0);
        CHECK((slot.attributes & FGB_ATTR_RIGHT_OPENNESS) != 0);
        CHECK((slot.attributes & FGB_ATTR_EXPRESSION) == 0);  // Beyond 没有表情
        CHECK_NEAR(slot.left_eye.openness, 0.7, 1e-5);
        CHECK_NEAR(slot.left_eye.gaze_dir[2], -0.974672f, 1e-5);
        CHECK_NEAR(slot.combined_gaze_confidence, 0.9, 1e-5);
        // 设备时刻（Unix 毫秒）按 epoch 换算成纳秒，不平移不缩放
        CHECK_EQ(slot.left_eye.gaze_ts_ns, 1750000000123ull * 1000000ull);
        CHECK(host.HasStatus(FGB_STATUS_STARTING) || host.HasStatus(FGB_STATUS_ACTIVE));
    }

    // ---- 阶段 4：来源转无效 → 不再追加提交 ----
    const std::size_t committed_before = host.committed().size();
    view->is_valid = 0;
    for (int attempt = 0; attempt < 10; ++attempt) {
        module->update(self);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK_EQ(host.committed().size(), committed_before);

    module->teardown(self);
    ::UnmapViewOfFile(view);
    ::CloseHandle(mapping);
    ::FreeLibrary(library);
    return CheckSummary("beyond-abi-smoke");
}
