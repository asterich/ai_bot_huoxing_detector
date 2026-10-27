# QQ 接入与插件流程

本仓库不实现 QQ 登录协议。QQ → 已配置的 OneBot/NapCat 平台适配器 → AstrBot 或 MaiBot → Python 插件 → pybind11 → C++ 核心 → SQLite。

## 环境

目标是 Linux x86_64（Windows 用 WSL2），GCC 16.1.x。先在本仓库执行 `pixi install --locked` 和 `pixi run test`。宿主机器人必须使用同一 Pixi 环境的 Python/动态库运行，或在部署环境按相同依赖重新构建安装 wheel。不要把 Linux `.so` 复制给 Windows Python，也不要只复制扩展文件而漏掉 OpenCV/GCC 运行时。

### AstrBot

1. 在 AstrBot 的同一个 Python 环境中安装本仓库：`python -m pip install --no-build-isolation --no-deps /绝对路径/本仓库`。推荐在 Pixi 环境内启动宿主。
2. 复制 `plugins/astrbot` 至 `AstrBot/data/plugins/astrbot_plugin_huoxing`。
3. 配置 OneBot v11 群消息接收；重载插件。
4. 发原图，再发裁剪/加框/水印版本。第二次应回复首次出现的消息 ID。
5. 再测重启后命中、不同群隔离、合并转发中的图片不被处理。

示例使用宿主 `Image.convert_to_file_path()` 获取媒体，框架负责下载；应在宿主/网关限制媒体下载大小。项目读取和解码最多 8 MiB。只监听群消息，忽略自身消息；同一消息重推通过 `replayed` 抑制重复提醒。每条消息最多一条提醒，处理器忙时跳过本次图片，不保证处理每张图。

参考：[插件开发指南](https://docs.astrbot.app/dev/star/plugin-new.html)、[消息事件](https://docs.astrbot.app/dev/star/guides/listen-message-event.html)、[Image 组件源码](https://github.com/AstrBotDevs/AstrBot/blob/master/astrbot/core/message/components.py)。

### MaiBot

1. 在插件 Runner 使用的 Python 环境安装本仓库。
2. 复制 `plugins/maibot` 到 `MaiBot/plugins/huoxing`，确保 SDK 2.x 兼容。
3. 重启/加载插件，确认 `chat.receive.before_process` 可用。
4. 本示例只接受宿主提供的最顶层 `binary_data_base64`；缺失媒体不下载、不展开转发。
5. `message_info.group_info.group_id`、`message_info.message_id`、`stream_id` 可能随宿主版本/网关封装变化。先用脱敏事件检查映射；缺群号/消息 ID 时跳过，缺流 ID 时只记录命中，不猜目标发送。

参考：[SDK 插件指南](https://docs.mai-mai.org/plugin/)、[Hook 载荷及媒体字段](https://docs.mai-mai.org/plugin/hooks)。旧 `src.plugin_system.BasePlugin` 版本不适用这份 SDK 2.x 示例，需要单独适配事件入口，核心 binding 无需改变。

## 当前验证边界

插件示例应做语法及模拟事件测试，但未连接真实 QQ 账号做端到端验证。不要把核心自动测试通过等同于特定版本宿主已经实机验收。不要对查重结果自动撤回或处罚群友。

图片仅保存用于比对的灰度缩略图、特征、哈希，以及群/消息/发送者 ID 和首次记录时间。时间是机器人接收并登记时间（UTC），不是原始发布时间。管理员在宿主鉴权后可调用 `Detector.delete_scope(scope)` 清理本群记录；备份应包含 SQLite 一致性快照，不能只拷贝正被写入的主文件。
