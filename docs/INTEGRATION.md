# QQ 接入与插件流程

本仓库不实现 QQ 登录协议。QQ → OneBot v11 适配器（例如 SnowLuma / NapCat）→ AstrBot 或 MaiBot → Python 插件 → pybind11 → C++ 核心 → SQLite。宿主应使用与所选适配器兼容的接收入口，检测核心不依赖具体登录实现。

### SnowLuma 与 NapCat

[SnowLuma 官方仓库](https://github.com/SnowLuma/SnowLuma) 声明支持 OneBot v11，可作为接入选项。它没有提供可与 NapCat 比较的账号风控率数据，因此本项目不声称更换适配器就能降低某个比例的风控概率。官方也明确说明属于未经腾讯授权的第三方项目。

SnowLuma 当前采用源码可见的非商业许可，明确不属于 OSI 开源许可；商用或公开分发修改版需核对并取得所需授权。仅连接外部已部署的 OneBot 服务不需要把它的源码纳入本仓库。部署前应核对所用版本的许可和宿主兼容性。

## 环境

目标是 Linux x86_64（Windows 用 WSL2），支持 GCC >=16.1、Clang >=21。先在本仓库执行 `pixi install --locked`、`pixi run test`、`pixi run install`。宿主机器人必须使用同一 Pixi 环境的 Python/动态库运行，或在部署环境按相同依赖重新构建安装 wheel。不要把 Linux `.so` 复制给 Windows Python，也不要只复制扩展文件而漏掉 OpenCV/GCC 运行时。

### AstrBot

从零搭建小服务器，请先阅读 [AstrBot + SnowLuma 部署文档](DEPLOY_ASTRBOT.md)，其中提供可执行命令、目录约定、端口配置、验收和 systemd 后台运行流程。以下为已有宿主的接入摘要。

1. 在 AstrBot 的同一个 Python 环境中安装本仓库：`python -m pip install --no-build-isolation --no-deps /绝对路径/本仓库`。推荐在 Pixi 环境内启动宿主。
2. 复制 `plugins/astrbot` 至 `AstrBot/data/plugins/astrbot_plugin_huoxing`。
3. 配置 OneBot v11 群消息接收；重载插件。
4. 发原图，再发裁剪/加框/水印版本。第二次应回复首次出现的消息 ID。
5. 再测重启后命中、不同群隔离、合并转发中的图片不被处理。

示例使用宿主 `Image.convert_to_file_path()` 获取媒体，框架负责下载；应在宿主/网关限制媒体下载大小。项目读取和解码最多 8 MiB。只监听群消息，忽略自身消息；同一消息重推通过 `replayed` 抑制重复提醒。每条消息最多一条提醒，处理器忙时跳过本次图片，不保证处理每张图。

图片分类在下载之前进行：忽略 `face`、`mface`、`emoji`，以及 `file=marketface` 或 `sub_type` / `subType` 非零的 `image`。普通图片的子类型可以缺省或为 `0`。AstrBot 的 OneBot 入口使用原始外层消息段分类，再构造 Image，避免宿主转换丢失表情子类型；保留原始分段序号，跳过表情后仍可处理同消息中的普通图片。其他适配器使用其 Image 组件保留的子类型字段。按平台消息类型区分，不根据图片内容猜测是否为表情包。

跳过日志包含 `message_id`、`part`、异常类型和具体原因：文件字节数与 8 MiB 上限、实际宽高与 1600 万像素上限、尺寸过小、格式与帧数、下载超时等。MPO/JPEG 多图像文件中有 Apple/Adobe HDR 增益图 XMP 标记时明确输出“带 HDR 增益图的照片”，不按动图描述；未识别的多图像文件单独说明。若同时像素超限并包含 HDR 辅助图，两项原因都会输出，例如：

```text
火星检测跳过图片：message_id=... part=0 ValueError: 图片像素过多：5712x4284=24470208 pixels，上限 16000000 pixels；暂不支持带 HDR 增益图的照片：format=MPO, frames=2，包含辅助图像
```

本次只改进拒绝策略和诊断信息，不自动缩放大图或选取 HDR 主图；原有大小和多图像限制继续生效。

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
