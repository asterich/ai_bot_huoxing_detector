# 火星梗图检测器

QQ群梗图改图查重。核心使用 **C++26 named modules**，支持 **GCC ≥16.1 / Clang ≥21**，通过 **pybind11** 提供 Python binding；**Pixi** 管理工具链、原生库和 Python 环境。锁定环境以 GCC 16.1 和 Clang 21 为验证基线。支持裁剪、边框、局部水印、缩放与 JPEG 重压缩的视觉重复检测，不需要模型 API 或 GPU。

目标平台是 Linux x86_64；Windows 使用 WSL2。原生 Windows/MSVC 不在当前构建目标内。

## 开始使用

在安装 [Pixi](https://pixi.sh/) 的 Linux/WSL 终端中：

```sh
git clone https://github.com/asterich/ai_bot_huoxing_detector.git
cd ai_bot_huoxing_detector
git switch codex/cpp26-core
pixi install --locked
pixi run test
pixi run install
pixi run huoxing original.png --scope qq:bot:group --message-id 100
pixi run huoxing cropped.png --scope qq:bot:group --message-id 101
```

每次 CLI 命令都在检查后登记；同一 `scope + message_id + part` 重试返回缓存结果且 `replayed=true`。

`pixi run test` 包含 `clang-format --dry-run --Werror` 检查、GCC 和 Clang 两套独立构建及 C++/Python 测试。`pixi run test-gcc`、`pixi run test-clang` 可分别验证；`pixi run format` 自动格式化。`pixi run install` 把 GCC 构建安装为本环境可导入的包。测试直接加载各构建目录产出的扩展，避免误测另一个编译器安装的版本。

```python
from huoxing import Detector, result_dict

detector = Detector("data/huoxing.sqlite")
with open("meme.png", "rb") as f:
    result = detector.ingest(f.read(8 * 1024 * 1024 + 1),
        scope="qq:机器人账号:群号", message_id="消息ID", sender_id="发送者QQ", part=0)
print(result_dict(result))
```

异步机器人应通过 `huoxing.worker.ImageWorker` 调用，避免阻塞事件循环。C++ binding 在复制输入后释放 GIL；Worker 只允许一个在途任务，忙时跳过，不建立无限队列。宿主取消等待后，已开始的后台任务仍可能登记图片，但不会因此启动更多后台任务。

## 算法

1. Python 限制文件大小与像素数，处理 EXIF 和透明背景，输出 RGB。
2. C++ 对完整 RGB 和尺寸计算 SHA-256，走 SQLite 群内哈希索引匹配。不同文件编码但解码像素一致也能命中。
3. 最长边缩至 960，提取最多约 1000 个 SIFT 局部特征。
4. 对本群历史图片逐一做双向近邻比值筛选（0.72），去掉相同像素位置的重复特征，RANSAC 验证单应关系。
5. 至少 12 个内点、内点比例 >=0.55、至少一张图的匹配点凸包覆盖 >=20%。
6. 对齐公共区域，至少 1024 像素且占目标图 >=15%；模糊后灰度差 <32 的像素占比 >=72% 才判为视觉重复。

裁剪保留的特征仍可匹配；边框通常不影响原内容的局部几何；局部水印允许部分像素不一致。几何覆盖和公共区域像素校验用于降低只共享一个小 logo 带来的误报。阈值在 `src/modules/matching.cppm`，属于启发式策略，结果不是校准后的概率。这是局部特征匹配与视觉去重，没有使用 YOLO 目标框检测或语义向量检索。

原图和每个改图版本分别保存，不做近似匹配结果的传递合并，避免 A≈B、B≈C 就直接推导 A≈C。精确像素相同的图复用同一记录；近似匹配返回按登记顺序找到的第一个匹配版本，未保证相似度最高或整组梗图的绝对最早发布时间。

## 模块与数据库

```text
src/modules/huoxing.cppm  C++ 公共入口：import huoxing;
src/modules/types.cppm    结果、特征及证据值类型
src/modules/features.cppm RGB 哈希、灰度缩放、SIFT 提取
src/modules/matching.cppm 双向特征匹配、RANSAC、像素验证
src/modules/database.cppm SQLite RAII、Repository、查询及持久化
src/modules/detector.cppm 检测流程编排，不含 SQL
src/binding.cpp          pybind11
schema.sql               唯一 schema 来源，构建时嵌入核心
python/huoxing/           解码、CLI、异步 Worker
plugins/astrbot/          AstrBot 插件示例
plugins/maibot/           MaiBot SDK 2.x 示例
tests/core_tests.cpp     原生测试
tests/python/            binding、算法、存储测试
pixi.toml / pixi.lock     环境声明与锁文件
.clang-format            C++ 统一格式约定
```

`images` 保存群作用域、完整 RGB 哈希、灰度缩略图、局部点、描述子；`occurrences` 保存消息及分段序号、发送者、UTC 登记时间、匹配证据。`meta` 标识数据与特征格式。按群索引、唯一约束、外键及 `BEGIN IMMEDIATE` 保证先查再写的原子性；WAL 支持并发读取。每次调用使用独立数据库连接。

不保存原始彩色图片或完整聊天内容，但保存灰度缩略图、视觉特征和消息标识。管理员在宿主鉴权后可调用 `delete_scope(scope)` 删除本群图片及关联消息记录。C++/Python 原型数据库格式不兼容，不要混用原型数据库。

## 接入、测试与局限

见 [AstrBot/MaiBot 接入流程](docs/INTEGRATION.md)。QQ 连接由宿主适配器负责。插件不递归访问聊天记录、引用消息或转发节点。

输入限制：8 MiB、1600 万像素、宽高至少 16。拒绝动图，不默默仅取第一帧。低纹理、纯文字图、严重裁剪、强遮挡、镜像、重绘可能漏报；相同模板的小幅文字修改可能仍被当作视觉重复。本项目不判断语义是否相同。

当前是全量候选比对，单次开销随群内图片数线性增长，适合小规模原型；每群默认最多 2000 个不同像素哈希，超过容量显式报错并回滚，不悄悄截断历史。缩略图和 SIFT 描述子会占用较多磁盘。正式上线前应使用真实群梗图样本评估误报/漏报、处理耗时和容量，再决定索引及阈值。

自动测试使用固定种子生成的图案正反例，包括裁剪、边框、水印、组合修改、重压缩、不同图片、小共同区域、幂等、群隔离、重启、容量回滚、并发和无效输入。测试通过不代表真实 QQ 或所有梗图达到某个准确率。插件未连接真实 QQ 做端到端验证。

工具参考：[GCC C++26 支持](https://gcc.gnu.org/projects/cxx-status.html)、[pybind11 构建](https://pybind11.readthedocs.io/en/stable/cmake/)。GCC 对 C++26 的支持仍包含实验性部分；本项目明确开启 `-std=c++26`，未依赖反射等额外实验开关。

CMake 用 `FILE_SET CXX_MODULES` 管理模块依赖，Ninja 按扫描结果排序。第三方 C 库、OpenCV、标准库头文件位于 global module fragment；项目接口通过 named modules 导出。不使用 `import std`，避免依赖不同标准库的标准模块打包差异。更高编译器版本允许配置，但只有实际测试过的版本才计入验证结果。

本功能分支还提供整包聊天记录顶层查重，见 [FORWARD.md](docs/FORWARD.md)。原生 `huoxing.forward` 编排资源 ID / 可见卡片证据，`huoxing.forward_database` 独立持久化；两者均不获取或展开包内消息。使用本分支时将上面的 `git switch` 改为 `codex/forward-top-level`。
