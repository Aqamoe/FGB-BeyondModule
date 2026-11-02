# 更新日志

遵循 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/) 与语义化版本。

## [未发布]

## [0.1.0] - 未发布
### 新增
- 读取 Bigscreen 官方「Beyond Eyetracking」应用写的命名共享内存 VRCFTMemmapData
  （64 字节顺序结构，社区逆向格式，逐项核对见 docs/protocol.md），输出逐眼/合成
  注视方向（eyes + gaze 通道）与睁眼度。
- IsValid/Confidence 真实参与决策：无效帧零属性位输出；范围外置信度退保守值；
  NaN/零向量按眼独立剔除，不伪造默认方向。
- 设备时刻按 Unix 毫秒→纳秒透传（仅合理纪元区间内），区间外交由宿主补帧时刻。
- 映射不存在/断流的中文状态提示直接指向「官方眼追应用是否在写」。
- `config.ini` 支持 `shm_name` 覆盖映射名（默认 VRCFTMemmapData）。
- 协议单测（偏移/sizeof/两条有效性与置信度路径/NaN 与越界/断流与缺失文案）
  + 通过真实 ABI 的合成共享内存冒烟测试。

### 已知限制
- 线格式无官方文档，z 符号、Timestamp 单位与基准、Confidence 语义、闭眼量在
  旧版应用中的存在性均为**未确认项**；未在真机验证，状态为未验证。
