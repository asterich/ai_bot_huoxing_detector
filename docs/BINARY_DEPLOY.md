# CI 构建、服务器安装成品

服务器不再执行 `pixi run install`、`pip install .` 或 editable 安装。编译和回归都在 GitHub 托管 Ubuntu runner 完成。日常更新使用带校验文件的发布包；不自动部署、不连接线上 SSH，也不在服务器执行回归测试。

## 产物与兼容范围

每个 PR，以及 `codex/cpp26-core` / `master` 的更新触发 GCC、Clang 的原生及 Python 回归。全部通过后，用 GCC 构建 wheel，使用同一 `pixi.lock` 在另一个路径安装独立 Pixi 环境，禁用源码编译器，安装最终 wheel，运行整套 Python 回归，并演练安装和回退。只有通过验证的产物才上传到 Actions Artifact（保留 30 天）。

发布包为 `huoxing-linux-x86_64-cp312-<提交号>.tar.gz`，另有外层 `.sha256`。内部包含 wheel、同提交的 AstrBot 入口、`deploy.py`、`manifest.json` 和 `SHA256SUMS`。manifest 记录提交、wheel 版本、锁文件哈希、Conda 包版本/build、numpy/Pillow 版本和系统要求。

第一版仅支持 Linux x86_64、CPython 3.12、glibc >=2.39 和匹配锁文件的现有 Pixi 环境；额外安装的 AstrBot pip 依赖不自动更改。服务器环境不匹配时在停机前中止，不下载依赖、不重建环境、不回退到源码编译。这个 wheel 不是通用 manylinux 包。原生扩展使用相对 RPATH 查找该 Pixi 前缀中的动态库；CI 的换目录验证防止依赖构建机路径。

构建版本仍沿用项目版本号，部署身份以完整 Git 提交和文件哈希为准。同版本不同提交可以通过指定的本地 wheel 强制重装。插件与库必须使用同一个发布包。

## 获取与安装

在对应 Actions run 页面下载 artifact，解压外层 ZIP；或下载 GitHub Release 的 `.tar.gz` 与 `.sha256`，一起传到服务器。以下命令只校验、安装和启动检查，不编译或跑回归。

```bash
cd ~/Downloads/huoxing-release  # 改成实际下载目录
sha256sum -c huoxing-linux-x86_64-cp312-<提交号>.tar.gz.sha256
tar -xzf huoxing-linux-x86_64-cp312-<提交号>.tar.gz
cd huoxing-linux-x86_64-cp312-<提交号>
sha256sum -c SHA256SUMS

# 直接使用现有运行环境 Python，避免触发 Pixi 环境更新
runtime_python="$HOME/qqbot/huoxing/.pixi/envs/default/bin/python"
"$runtime_python" deploy.py check
"$runtime_python" deploy.py install
```

首次切换也使用 `install`：pip 会移除原来的 editable 安装元数据并装入 wheel。脚本先验证兼容性和校验文件，然后停止用户服务 `astrbot.service`，备份已安装包文件（包括 editable finder）、插件入口及插件数据，执行仅本地 wheel 的 `pip install --no-index --no-deps --only-binary=:all:`，更新入口，确认实际导入位于运行环境内，重启并检查服务状态。默认目录为 `~/qqbot/AstrBot`；可用 `--astrbot`、`--state-dir`、`--service` 覆盖。

包的兼容性检查和导入检查都不是 QQ 端到端验收；启动检查也不能保证服务之后不会出错。首次部署前应确认服务器已恢复正常，并结束上一次残留构建进程。

## 回退

每次部署会打印备份目录，默认位于 `~/qqbot/huoxing-deploy/时间-随机标识`；`current.json` 记录最近部署的提交和对应旧版本备份。安装或启动检查失败会自动恢复旧包和插件；也可手动回退：

```bash
"$HOME/qqbot/huoxing/.pixi/envs/default/bin/python" deploy.py rollback \
  --backup "$HOME/qqbot/huoxing-deploy/<具体备份目录>"
```

回退恢复备份时的服务运行状态。数据库不会自动覆盖，避免抹掉升级后收到的新消息；停机时的数据副本保存在备份的 `plugin-data/` 中。本方案不自动执行数据库迁移，涉及 schema 变化需单独设计迁移与恢复。首次更新前若旧安装本就损坏，回退只能恢复原有状态。

## 长期发布

确认提交后，维护者显式创建并推送 `huoxing-v*` 标签，例如 `huoxing-v0.2.0-1`。标签触发同一套测试、打包与安装验证，再创建 GitHub Release，上传相同提交的包和校验文件。PR 构建没有发布权限；脚本不覆盖已有同名 Release 资产，不自动给分支打标签。
