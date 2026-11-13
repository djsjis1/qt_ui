---
trigger: glob
glob: "**/CMakeLists.txt,**/*.cmake,**/CMakePresets.json,**/CMakeUserPresets.json,**/vcpkg.json,**/vcpkg-configuration.json,**/conanfile.txt,**/conanfile.py,**/conan.lock,**/.clang-format,**/.clang-tidy"
---
# C++ 构建、依赖与工具链规范

> 本文件覆盖：CMake 写法、构建类型与 Sanitizer 分配、第三方依赖治理、格式化与静态分析、CI 门禁。
> 给谁用：编辑 CMake、构建脚本、依赖清单或工具链配置时加载。源码写法见 `C++项目规范.md`。

---

## CMake

使用 target-based CMake；本规范示例最低需要 3.15，项目实际最低版本由顶层构建配置声明，采用更新特性时同步提高并验证要求。禁止全局的 `include_directories()` / `link_libraries()`，统一用 `target_include_directories()` / `target_link_libraries()` 并显式声明 `PRIVATE` / `PUBLIC` / `INTERFACE` 可见性。

可执行目标与库目标各自声明依赖，不依赖目录级隐式继承。

独立模块建立对应 CMake 目标，通过 `target_link_libraries()` 显式声明链接依赖，不靠混杂的 include 目录掩盖边界。CMake 不理解业务分层，也允许部分静态库循环；分层与循环仍需评审或独立依赖图检查，不能以构建通过代替架构验证。

---

## 构建类型

| 类型 | 用途 | Sanitizer | 测试钩子（`PROJECT_ENABLE_TEST_HOOKS`） |
| --- | --- | --- | --- |
| `Debug` | 开发调试、AI 自测 | Linux 验证任务开启 ASAN + UBSAN | 按需开启 |
| `RelWithDebInfo` | 集成测试 | 按项目验证矩阵配置 | 按需开启 |
| `Release` | 生产发布 | 禁止开启 | 必须关闭 |

并发相关变更在支持的平台额外运行 TSAN。TSAN 不与 ASAN 同时启用；与 UBSAN 的组合取决于工具链，本模板默认使用独立 TSAN 任务。二选一并在项目内固定：

- 独立选项：`-DPROJECT_SANITIZE=tsan`（推荐，不改构建类型语义）；
- 独立构建类型：`DebugTSan`（需同步配置 CI matrix）。

发布版本禁止开启任何 Sanitizer。

Sanitizer 要求适用于 Linux（gcc/clang）工具链。MSVC 不提供 UBSan，其 ASan（`/fsanitize=address`）覆盖也有限：本地 Windows 构建至少完成零警告编译与全部单测，Sanitizer 检查放到 Linux CI 执行；纯 Windows 项目须在根目录 `AGENTS.md` 的命令入口声明替代验证并记录原因。

---

## 依赖管理

引入第三方库必须：

1. 评估许可证与项目兼容；
2. 锁定具体版本号，禁止 `latest`；
3. 已有成熟开源方案时优先直接集成，不重复造轮子，二选一：
   - 集成到 `third/` 目录并在 `third/README.md` 记录版本、来源、许可证（适合 header-only 或源码集成）；
   - 系统包管理或 FetchContent 锁定版本。
4. 引入或升级前核查已知漏洞与许可证；在 PR 或交付中记录来源、日期和结论。无法联网时说明未完成核查，不宣称安全；周期性复查交由项目 CI 或负责人安排。
5. 在依赖清单附近记录用途；只有影响架构或存在长期取舍时更新对应文档/ADR，不为每个小库创建架构文档。

```cmake
include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        vX.Y.Z  # 示例占位符：替换为核实的固定 tag 或 commit
)
```

精确版本由依赖声明或锁文件维护；入口只指向该配置。引入或替换依赖默认按 L 评估，是否需要方案比较与 ADR 由 `AI工作流规范.md` 的实际风险规则决定。

---

## 工具链

配置细节以项目内的 `.clang-format` / `.clang-tidy` 与 CI 为准，本规则只声明行为要求：

- 格式化遵循 `.clang-format`（推荐 `BasedOnStyle: Google`、`IndentWidth: 4`、`ColumnLimit: 100`），提交前自动格式化，禁止手动格式化与工具格式化混用。
- 静态分析遵循 `.clang-tidy`，推荐启用 `bugprone-*` / `cppcoreguidelines-*` / `modernize-*` / `performance-*` / `readability-*`。屏蔽第三方库警告用 `NOLINT` / `#pragma` 并注释原因，作用域限缩到最小。
- 自家代码在已声明的 CI 工具链上零警告：GCC/Clang 通常使用 `-Wall -Wextra -Werror`，MSVC 使用 `/W4 /WX`。禁止无差别屏蔽或用强制转换掩盖真实问题；工具链升级导致的新诊断应逐项评估。
- 第三方头文件以 `SYSTEM` 属性引入（`target_include_directories(... SYSTEM ...)`；FetchContent 的 `SYSTEM` 选项需 CMake 3.25+，旧版本把三方头文件目录以 SYSTEM 转发给消费方），其警告不计入零警告门禁；自家代码不享有该豁免。

---

## 测试钩子开关

钩子的设计规则见 `C++项目规范.md`「AI 自测钩子」，这里只定义构建侧写法。

以下假定 `core` 与 `core_tests` 已创建，常规单测源已加入目标。开关默认关闭；开启后只在 Debug/RelWithDebInfo 生效，单配置与多配置生成器使用相同条件：

```cmake
option(PROJECT_ENABLE_TEST_HOOKS "启用测试构建中的自测钩子" OFF)

set(_project_test_hooks
    "$<AND:$<BOOL:${PROJECT_ENABLE_TEST_HOOKS}>,$<OR:$<CONFIG:Debug>,$<CONFIG:RelWithDebInfo>>>")
target_compile_definitions(core PUBLIC
    "$<${_project_test_hooks}:PROJECT_ENABLE_TEST_HOOKS>")

# 钩子用例与宏使用同一配置条件；绝对路径避免跨目录解释歧义
# 常规单测独立加入，不受钩子开关影响
target_sources(core_tests PRIVATE
    "$<${_project_test_hooks}:${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/timeline/TimelineController_hook_test.cpp>")
```

新增自定义测试配置时显式扩展允许列表；不得把所有非 Release 配置都默认视作测试构建。验证至少覆盖开关 ON/OFF 与 Debug/Release 组合；Release 即使开关为 ON 也不能获得钩子宏或源文件。

关键点：

- 宏必须用 `PUBLIC` 传递给消费方。写成 `PRIVATE` 时，链接该库的测试目标拿不到宏，钩子成员不可见会直接编译失败；若测试目标自行补定义，则同一头文件在两侧展开出不同的类定义。两种后果都比多注入一个宏严重。
- 钩子用例必须与常规单测分文件（`<类名>_hook_test.cpp`），否则无法只排除钩子部分，Release 构建会整体失败。

---

## CI 门禁

产品代码变更按项目矩阵执行格式化、静态分析、编译、测试与适用的 Sanitizer；纯文档等变更可以只运行对应检查。仓库配置的必需检查不得绕过；环境缺失时记录验证缺口，不自行关闭门禁。

工作流的权限、Action 版本固定与不可信 PR 隔离见 `Git工作流规范.md`。
