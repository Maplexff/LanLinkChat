# LanLinkChat

这是一个基于 `Qt6 + CMake + Widgets` 的局域网聊天原型，目标是让你在 `Qt Creator` 中直接打开并用自带 kit 编译。

## 功能

- UDP 广播发现局域网内在线节点
- 支持手动点击“手动发现”按钮立即重发发现广播
- 点对点文字聊天
- 聊天记录本地持久化，重启后可继续查看历史会话
- 联系人与群聊支持未读消息计数提示
- 文件发送与接收
- 文件接收时自动避让重名文件，并在传输不完整时丢弃损坏内容
- 群聊创建与群消息扇出
- 基于摄像头视频帧与 PCM 音频流的局域网音视频通话
- 聊天页与音视频页分开展示，避免消息区和通话区混在同一界面

## 构建方式

1. 优先可直接用 `Qt Creator` 打开项目根目录下的 `LanLinkChat.pro`
2. 如果你希望走 `CMake`，也可以打开 `CMakeLists.txt`
3. 选择一个带 `Qt 6` 的桌面 kit
4. 直接构建并运行

## 说明

- 该项目不依赖中心服务器，节点通过 UDP 广播互相发现，然后通过 TCP 建立会话。
- 文件接收后默认保存到应用数据目录下的 `downloads` 子目录。
- 会话历史和未读状态会保存在本机 `QSettings` 中，用于下次启动恢复。
- 音视频通话当前使用 TCP 连接传输视频帧与 PCM 音频，语音不依赖额外服务器。
- 如果 `CMake` 导入时报 `Qt6_DIR-NOTFOUND` 或缺少 `Release/Debug` 配置，通常是 Qt Creator 的 kit 或旧 build 缓存有问题，此时可直接改用 `.pro` 导入，或清理已有 build 目录后重新选择正确的 Qt 6 kit。
- 群聊是去中心化的，本机创建群后会把群信息同步给成员，群消息由发送者复制分发到各成员。

## 复制依赖
powershell -ExecutionPolicy Bypass -File "D:\videoCourse\LanLinkChat\deploy_release.ps1"

