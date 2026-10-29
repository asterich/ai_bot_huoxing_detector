# 小服务器部署：AstrBot + SnowLuma + 火星检测

本流程在一台服务器上运行 QQ 连接端、AstrBot 和查重插件。AstrBot 使用本项目的 Pixi 环境，SnowLuma 使用 Docker，通过 OneBot v11 反向 WebSocket 连接。

整理日期：2026-10-29。原生核心的构建、测试和安装记录见 [VALIDATION.md](VALIDATION.md)；真实 QQ + SnowLuma + AstrBot 的整条链路仍需按本文验收。

## 1. 服务器与运行结构

建议从 **Ubuntu 24.04 x86_64、4 核 CPU、8GB 内存、60GB SSD** 起步，先接一个测试群。这是为 QQ 客户端、AstrBot、工具链和源码编译留余量的建议配置，尚未做真实群负载压测。当前 Pixi 锁文件只覆盖 Linux x86_64。

纯查重使用 CPU，不需要 GPU 或模型 API。需要 AI 群聊时，可以另外给 AstrBot 配置模型 API。

```text
QQ 群 ↔ SnowLuma / Linux QQ（Docker）
             ↕ OneBot v11 反向 WebSocket
          AstrBot（本项目 Pixi 环境）
             ↓ 火星检测 Python 插件
          pybind11 → C++26 核心 → SQLite
```

关键约定：

- 以下服务器命令使用同一个有 sudo 权限的普通 SSH 用户执行。
- 工程统一放在该用户的 `~/qqbot`，不要中途换用户或移动环境目录。
- AstrBot 必须使用安装了本项目 binding 的 Pixi 环境启动，才能找到对应的 OpenCV 和 GCC 运行库。
- SnowLuma 支持 OneBot v11，但没有可验证的对比数据证明它比 NapCat 风控概率更低。许可和适配器说明见 [INTEGRATION.md](INTEGRATION.md)。

## 2. 安装基础工具与 Pixi

在服务器上执行：

```bash
sudo apt update
sudo apt install -y git curl ca-certificates ffmpeg docker.io
sudo systemctl enable --now docker

mkdir -p "$HOME/qqbot"

curl -fsSL https://pixi.sh/install.sh | bash
export PATH="$HOME/.pixi/bin:$PATH"

pixi --version
sudo docker version
```

Pixi 安装脚本来自 [Pixi 官方安装说明](https://pixi.prefix.dev/latest/installation/)。重新登录后若找不到命令，可以直接使用 `$HOME/.pixi/bin/pixi`。

## 3. 下载工程并安装依赖

本文对应本项目 PR #2 的 `codex/forward-top-level` 分支；AstrBot 固定使用 [v4.29.3](https://github.com/AstrBotDevs/AstrBot/releases/tag/v4.29.3)，位于插件声明的 `>=4.16,<5` 范围内。

```bash
cd "$HOME/qqbot"

git clone --branch codex/forward-top-level \
  https://github.com/asterich/ai_bot_huoxing_detector.git huoxing

git clone --depth 1 --branch v4.29.3 \
  https://github.com/AstrBotDevs/AstrBot.git AstrBot

cd "$HOME/qqbot/huoxing"
pixi install --locked

# Limit parallel compilation during native-extension installation.
export CMAKE_BUILD_PARALLEL_LEVEL=2
pixi run install
pixi run test-gcc

# Install the host dependencies into this same Pixi environment.
pixi run python -m pip install -r ../AstrBot/requirements.txt
pixi run python -m pip check
pixi run python -c "import huoxing; print(huoxing.__file__)"
pixi run python examples/demo.py
```

成功标准：GCC 测试通过，`import huoxing` 正常，演示中的裁剪图片和顶层转发资源都返回重复结果。首次安装需要下载工具链和原生库；`pixi install --locked` 会同时安装锁文件中的 GCC、Clang 等开发依赖，本流程只运行 GCC 验证。

AstrBot 自身依赖在同一 Pixi 环境内通过 pip 安装，**尚未纳入本项目的 `pixi.lock`**。升级 AstrBot 或其他插件依赖后，应重新执行 `pip check`、模块导入和演示。若 `pip check` 报错，先处理依赖冲突再继续。

## 4. 安装插件并前台启动 AstrBot

```bash
mkdir -p "$HOME/qqbot/AstrBot/data/plugins/astrbot_plugin_huoxing"

cp -a "$HOME/qqbot/huoxing/plugins/astrbot/." \
  "$HOME/qqbot/AstrBot/data/plugins/astrbot_plugin_huoxing/"

cd "$HOME/qqbot/AstrBot"
"$HOME/.pixi/bin/pixi" run \
  --manifest-path "$HOME/qqbot/huoxing/pixi.toml" \
  python main.py
```

保持终端运行。AstrBot 管理面板通常位于 `6185`。按启动日志中的初始凭据登录并修改密码；首次启动也可能下载面板资源，需要服务器能访问相关发布资源。参见 [AstrBot 源码部署说明](https://docs.astrbot.app/deploy/astrbot/cli.html)。

本流程后续也通过 `pixi run` 启动 AstrBot；另建 uv/venv 环境启动会切换 Python，可能找不到已安装的原生扩展。

### 通过 SSH 管理面板

在你自己的电脑上另开一个终端，把下面的用户名和服务器 IP 换成实际值：

```bash
ssh -N \
  -L 6185:127.0.0.1:6185 \
  -L 5099:127.0.0.1:5099 \
  -L 6081:127.0.0.1:6081 \
  用户名@服务器IP
```

保持隧道运行，然后在自己电脑的浏览器中访问：

| 本机地址 | 用途 |
|---|---|
| `http://127.0.0.1:6185` | AstrBot 管理面板 |
| `http://127.0.0.1:5099` | SnowLuma 管理面板，第 5 步启动后可用 |
| `http://127.0.0.1:6081` | QQ 远程桌面，第 5 步启动后可用 |

云服务器安全组只需放行 SSH。面板通过隧道管理，`6199` 仅供同一服务器上的连接端使用，不开放公网入站。

## 5. 启动 SnowLuma 并登录 QQ

在服务器上另开一个 SSH 终端执行：

```bash
sudo docker run -d \
  --name snowluma \
  --restart unless-stopped \
  --shm-size=1g \
  --ulimit nofile=65536:1048576 \
  --cap-add=SYS_PTRACE \
  --security-opt seccomp=unconfined \
  --add-host=host.docker.internal:host-gateway \
  -e TZ=Asia/Shanghai \
  -e SNOWLUMA_WEBUI_HOST=0.0.0.0 \
  -e SNOWLUMA_QQ_FLAGS="--disable-gpu --disable-software-rasterizer --disable-gpu-compositing" \
  -p 127.0.0.1:5099:5099 \
  -p 127.0.0.1:6081:6081 \
  -v qq-gateway-data:/app/data \
  -v qq-client-config:/app/.config \
  -v qq-client-data:/app/.local/share \
  motricseven7/snowluma:latest

sudo docker logs --tail 100 snowluma
```

镜像包含 Linux QQ 和远程桌面。`SYS_PTRACE` 与 `seccomp` 设置用于接入 QQ 进程，其他运行参数与持久卷可对照 [SnowLuma 官方容器说明](https://github.com/SnowLuma/SnowLuma.Docker.Framework) 和 [官方 Compose 配置](https://github.com/SnowLuma/SnowLuma.Docker.Framework/blob/main/docker-compose.yml)。

通过 SSH 隧道打开 `6081` 的远程桌面并扫码登录 QQ，再到 `5099` 面板确认账号在线。面板和远程桌面的初始密码看容器日志。

首次可用 `latest` 跑通；验收后记录镜像 digest，后续重建时将命令末尾的镜像替换为记录的 `仓库名@sha256:...`。查看当前容器对应的镜像标识：

```bash
sudo docker inspect --format '{{.Image}}' snowluma
sudo docker image inspect motricseven7/snowluma:latest --format '{{json .RepoDigests}}'
```

上面分别是容器使用的本地镜像 ID 和该标签当前对应的仓库 digest；如果已经拉过更新镜像，应按容器的镜像 ID 查询对应的 `RepoDigests`，不要误记更新后的版本。

## 6. 配置 OneBot v11 连接

在 AstrBot 面板的“消息平台”中新增 **OneBot v11**：

| 配置 | 值 |
|---|---|
| ID | `qq-snowluma` |
| 启用 | 开启 |
| 反向 WebSocket 监听地址 | `0.0.0.0` |
| 监听端口 | `6199` |
| Token | 自己设置一串随机令牌，两端一致 |

在 SnowLuma 中新增 **反向 WebSocket / WebSocket 客户端**，填写：

```text
地址：ws://host.docker.internal:6199/ws
Token：与 AstrBot 完全一致
```

SnowLuma 从 Docker 容器主动连接宿主机 AstrBot；第 5 步的 `--add-host` 提供了这个宿主机地址。服务器如另有防火墙，需要允许 Docker 容器访问宿主机 `6199`。

成功时 AstrBot 日志出现 `aiocqhttp(OneBot v11) 适配器已连接`。端点路径、监听方式和 Token 的含义参见 [AstrBot OneBot v11 接入说明](https://docs.astrbot.app/platform/aiocqhttp.html)。

## 7. 测试群验收

在 AstrBot 的访问控制中先允许一个测试群，并在插件页确认“火星梗图检测”已加载、已启用。纯查重阶段可先关闭自动 AI 回复。

1. 发原图，再发裁剪、加框、水印版本，确认能提示首次消息 ID。
2. 重启 AstrBot 后再发相同图片，确认历史记录保留。
3. 换一个群发送同图，确认按群隔离，不引用另一个群的历史。
4. 重复发送同一聊天记录包。相同顶层资源 ID 可命中；ID 变化时不保证命中，仅卡片相同属于弱证据，默认不提醒。
5. 使用一张从未单独发送过的测试图制作转发包，然后再单独发该图，确认它不会因为转发包出现过而直接被登记为重复。

插件只读取外层图片和整包顶层引用，不获取或遍历包内正文、图片或嵌套节点。状态含义见 [FORWARD.md](FORWARD.md)。机器人接入真实 QQ 的验收结果应单独记录，不能把原生自动测试通过视为实机验收通过。

## 8. 设置退出 SSH 后继续运行

前台验收成功后，用 `Ctrl+C` 停止第 4 步的 AstrBot，再用同一个部署用户创建 systemd 用户服务：

```bash
mkdir -p "$HOME/.config/systemd/user"

cat > "$HOME/.config/systemd/user/astrbot.service" <<'EOF'
[Unit]
Description=AstrBot with Huoxing Detector

[Service]
WorkingDirectory=%h/qqbot/AstrBot
ExecStart=%h/.pixi/bin/pixi run --manifest-path %h/qqbot/huoxing/pixi.toml python main.py
Environment=PYTHONUNBUFFERED=1
Restart=on-failure
RestartSec=10

[Install]
WantedBy=default.target
EOF

sudo loginctl enable-linger "$USER"
systemctl --user daemon-reload
systemctl --user enable --now astrbot
systemctl --user status astrbot --no-pager
```

该服务依赖用户的 HOME 路径；搬迁工程后应同步修改服务并重新安装原生扩展。SnowLuma 容器已经通过 `--restart unless-stopped` 设置自动重启。

日常操作：

```bash
journalctl --user -u astrbot -f
sudo docker logs -f snowluma

systemctl --user restart astrbot
sudo docker restart snowluma
```

两条日志命令都会持续占用终端；按 `Ctrl+C` 退出查看，再执行其他命令。

## 9. 数据、备份与维护

- 插件通过 AstrBot 的 `StarTools.get_data_dir()` 保存 `images.sqlite`，属于 AstrBot 的 `data` 数据目录；备份应覆盖整个 `~/qqbot/AstrBot/data`。
- SnowLuma 的三个持久卷是 `qq-gateway-data`、`qq-client-config`、`qq-client-data`，包含连接配置和 QQ 登录状态。重建容器时继续使用同名卷。
- 对 AstrBot 文件做离线备份前先执行 `systemctl --user stop astrbot`，复制完整数据目录后再 `start`。在线备份 SQLite 应使用一致性备份接口，不能只复制正在写入的主文件。
- 备份 QQ 数据卷前停止 SnowLuma，备份完成后恢复运行。备份包含账号会话，应存放在仅自己可访问的位置。
- 验收后记录本项目提交、AstrBot tag、SnowLuma digest，以及 `pixi run python -m pip freeze` 的输出；升级前保留备份和这些版本信息。
- 当前插件每群默认最多保存 2000 个不同像素版本。达到容量会报错并回滚，需要通过宿主鉴权后的清理入口或后续容量配置处理；不会自动淘汰旧记录。候选图片线性扫描，处理器忙时会跳过新任务。

## 10. 常见故障

| 现象 | 排查方向 |
|---|---|
| `No module named huoxing` | 确认服务使用本文的 `pixi run --manifest-path ...`，并在同一环境重新执行 `pixi run install`。 |
| 找不到 `libopencv`、`libstdc++` 等动态库 | 在 Pixi 环境内启动 AstrBot；不要单独复制 `.so` 到另一套 Python。 |
| AstrBot 面板打不开 | 查看前台日志或 `journalctl`；检查 SSH 隧道，以及本机 `6185` 是否被占用。 |
| OneBot 连接失败 | 检查 AstrBot 监听地址、`6199/ws`、Token、Docker 的宿主机别名和防火墙规则。 |
| 插件未出现在列表中 | 核对 `data/plugins/astrbot_plugin_huoxing/main.py` 与 `metadata.yaml`，查看加载日志。 |
| 能收到消息但图片被跳过 | 检查群访问控制、插件启用状态、媒体下载路径及日志。输入限制为 8 MiB / 1600 万像素，拒绝动图；处理器忙时也会跳过。若收到容器内部文件路径，需让适配器提供宿主可下载的 URL 或配置一致的媒体挂载路径。 |
| 整包转发没有提醒 | 检查顶层资源 ID 是否存在、是否变化；相同卡片和空 ID 不作为可靠内容重复。 |
| 日志出现容量错误 | 检查该群保存的不同图片数量，清理或调整容量后重试。 |

核心能力、数据内容和清理接口见 [README](../README.md)；接入边界见 [INTEGRATION.md](INTEGRATION.md)。
