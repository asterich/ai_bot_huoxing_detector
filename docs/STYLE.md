# 原生代码风格与模块约定

本约定用于 PR #2 及其后续维护。原生代码注释使用英文；部署和使用文档使用中文。

## 模块边界

| 模块 | 职责 |
|---|---|
| `huoxing.types` / `huoxing.forward_types` | 拥有数据的结果、特征和证据类型 |
| `huoxing.hashing` | SHA-256 上下文、RGB 规范化前缀、十六进制编码 |
| `huoxing.vision` / `huoxing.format` | 单处声明并导出 OpenCV / fmt 的原始实体，保持第三方 ABI |
| `huoxing.features` | 灰度缩放与 SIFT 提取 |
| `huoxing.matching` | 双向近邻、RANSAC、公共区域像素验证 |
| `huoxing.sqlite` | 连接、语句、事务的 RAII 所有权 |
| `huoxing.database` / `huoxing.forward_database` | 图片或整包的 SQL 与持久化 |
| `huoxing.detector` / `huoxing.forward` | 检测流程与证据决策，不含 SQL |
| `huoxing` | 面向调用方的公共模块入口 |

匹配流程按阶段拆分；各阶段的参数与阈值沿用上一版本。不要在风格修改中调整算法规则。

## 标准库、fmt 与字符串生命周期

项目使用真正的 `import std;`，标准库 BMI 由 CMake 从工具链提供的 `libstdc++.modules.json` 及源码生成。OpenCV、OpenSSL、SQLite、fmt 等第三方头文件仍放在 global module fragment；没有手工伪造一个名叫 `std` 的包装模块。

OpenCV 和 fmt 头文件各自由一个适配模块持有，再通过 `using` 导出原始实体。这样可以避免 GCC 16.1 在标准模块参与时重复合并 OpenCV 默认参数声明，同时不会把第三方类型重新定义成项目模块所属的类型。OpenCV 类型宏通过少量具名常量提供，C 的标准错误流宏留在 fmt 适配模块内部。

`src/std_compat.hpp` 只在 GCC 16.1 的第三方头文件边界预先统一 libstdc++ 全局声明，解决该版本加载部分标准库声明时的语言链接冲突。Clang 不启用这段文本前置处理；两种编译器都仍由 CMake 构建并导入真正的 `std` 模块。不要把这段兼容处理扩散到业务模块。

Pixi 固定 CMake 4.4.4，因为该版本的标准库模块发现机制需要与版本对应的实验开关。升级 CMake 时应一并核对其官方开关值。GCC 与 Clang 都使用 libstdc++，与本环境的 OpenCV C++ ABI 保持一致。

格式化、十六进制编码、测试输出使用 `fmt::format`、`fmt::format_to` 或 `fmt::println`。不再使用 `std::ostringstream`、流式 I/O 或 `std::to_string` 拼接显示文本。

只在调用期间借用的 scope、消息 ID、SQL、资源 ID 和卡片文本使用 `std::string_view`。长期保留的数据库路径与返回结果继续使用 `std::string`，避免悬空引用。SQLite 文本参数用显式长度和 `SQLITE_TRANSIENT` 复制；不能假定 view 在逻辑末尾有空字符。读取 BLOB 得到的 `std::span` 只在当前游标位置有效，解码前必须确认长度，跨游标操作前必须复制。

`std::views::filter` 表达邻居筛选，`std::views::iota` 表达索引区间，`std::ranges::any_of` 表达投影有效性检查。局部 view 只用于其底层容器仍然存活的范围内，不把 view 保存进结果。

## 空行、注释与自动格式化

`.clang-format` 负责以下约定：

- 类、结构、函数定义之间保留空行；访问修饰符与成员之间也保留空行。
- 长参数列表一行一个参数，关闭把所有参数挤到下一行的行为。
- 换行参数使用块缩进，闭合括号单独对齐；长循环头沿用一致的括号缩进。
- 控制语句使用花括号；二元运算符的换行规则保持一致。
- 最多保留一个连续空行，避免无意义的大片空白。

函数体按逻辑阶段手工分段，例如输入验证、所有权转换、查询、登记与提交。注释说明阈值理由、边界、所有权和异常行为，而不是逐行翻译代码。运行 `pixi run format` 修改格式，运行 `pixi run format-check` 检查格式；`pixi run test` 包含格式检查和双编译器测试。
