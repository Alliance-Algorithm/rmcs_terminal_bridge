# tl::expected（vendored）

- 上游：https://github.com/TartanLlama/expected
- 版本：**v1.3.1**
- 文件：`expected.hpp`（单头文件）、`COPYING`（CC0-1.0 法律文本）
- 许可证：**CC0-1.0**（见 `COPYING` 与同目录的 `*.license`）
- 更新方式：替换 `expected.hpp` 与 `COPYING`，同步更新本文件的版本号。

只用于 `teleop_ipc` 的 `Result<T>`；不修改上游代码。头文件以 `SYSTEM` include 方式引入，避免上游警告进入本仓库的 `-Werror` 策略。
