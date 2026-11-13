---
trigger: glob
glob: "**/*.h,**/*.hpp,**/*.cpp,**/*.cc,**/*.cxx"
---
# C++ 语言与编码规范

> 本文件覆盖：C++ 语言特性、类型与资源安全、并发、命名、注释、接口导出与自测钩子。
> 给谁用：编辑 C++ 源码时加载。构建、依赖、工具链与 CI 见 `C++构建与依赖规范.md`。

## 按任务定位章节

| 正在写什么 | 读哪一节 |
| --- | --- |
| 新类 / 新函数 | 类型安全、初始化、Rule of 0/3/5、命名 |
| 跨模块调用、头文件组织 | 依赖与 include 规范 |
| 撤销/重做、事件通知、算法切换等扩展点设计 | 设计模式 |
| 多线程或异步代码 | C++ 并发实现规范 |
| 公共头文件、导出符号 | 接口兼容与导出（文件后段） |
| UI / 硬件 / 时序等不可自动化路径 | AI 自测钩子 |
| 修改完成、准备交付 | 修改后不自动编译运行（本地默认） |

其余章节按需扫读，不必从开头逐行读。

---

## 修改后不自动编译运行（本地默认）

修改 C++ 源码、头文件、CMake、构建配置或测试后，代理默认不执行构建、编译、链接、运行程序、执行测试、统计覆盖率，以及依赖构建产物的静态检查；完成静态自检后，把改动、待执行命令和预期结果交给用户，等待用户验证与反馈。

- 静态自检：核对改动与需求、声明与定义、include 与命名空间、接口与调用方的一致性，以及可静态发现的编译风险（缺失声明、签名不一致、未包含所需头文件等）。阅读代码不能表述为“已验证”。
- 交付标注：明确写出“未编译、未运行、未测试”及原因，附建议的构建、测试或静态检查命令；已有 CI 结果时注明这是远端证据。不得声称本地验证通过。
- 用户反馈构建或测试日志后，按日志定位并修复；仍不自动重跑，除非用户要求或已获授权。
- 例外：用户在当前任务中明确要求构建、运行或测试，或根 `AGENTS.md` 已授权具体命令、平台和可运行目标。只在授权范围内执行，不扩展到安装依赖、部署、启动外部服务或其他未列出的命令。
- CI 不受此限制：远端流水线按项目配置执行构建与测试；本地不自动执行不免除测试编写、兼容性评估和未验证说明。

这是可调整的工程默认值，不是安全红线；调整须写明范围或记录在 `AGENTS.md`，并遵守 `工程总纲.md` 的授权边界。

---

## C++ 标准

已有项目以构建目标声明的语言标准与工具链为准，入口见根目录 `AGENTS.md`，不因模板默认值降级或升级。新项目尚未选型时建议 C++17；采用 C++20/23 等标准须确认依赖与部署工具链支持。

禁止：混用不同标准特性导致编译器兼容问题；使用已废弃特性（`auto_ptr`、`register`）；依赖未定义行为（UB）。

优先使用 C++17 特性的场景：`std::optional` 替代可空指针返回值；`std::variant` 替代 union；`if constexpr` 替代 SFINAE 模板特化；结构化绑定 `auto [a, b] = pair`。

---

## 类型安全

协议、序列化、文件格式与稳定 ABI 的整数须明确宽度和符号性，按契约选择 `int32_t`、`uint32_t` 等类型；同时规定字节序和编码，不直接序列化原始对象布局。

已有类型迁移先确认实际宽度、布局与兼容性，禁止机械执行 `long` → `int64_t`：Windows 的 `long` 通常是 32 位。普通内部计数可用 `int`，内存尺寸与容量用 `size_t`，传输边界再验证并转换。

禁止有损隐式转换（宽转窄、有符号与无符号混用），必须显式转换并自行确认安全：

```cpp
int32_t b = static_cast<int32_t>(a);  // 宽转窄必须显式
```

无损拓宽转换（如 `int32_t` → `int64_t`）可隐式进行。禁止 C 风格强制转换 `(int*)ptr`，使用 `static_cast` / `reinterpret_cast` / `const_cast`。

---

## 初始化

所有变量必须在声明时初始化，优先花括号初始化（防窄化转换）：

```cpp
int32_t value{42};
std::vector<int32_t> items{1, 2, 3};

class VideoDecoder {
    int32_t frame_count_{0};
    bool is_running_{false};
};
```

---

## 枚举

必须使用 `enum class` 并声明底层类型，禁止裸 `enum`：

```cpp
enum class DecodeStatus : int32_t {
    kSuccess = 0,
    kFailed = -1,
    kTimeout = -2,
};
```

---

## Rule of 0 / 3 / 5

**Rule of 0**：优先设计不需要手动管理资源的类（智能指针 + RAII）。

**Rule of 5**：若类定义了析构函数，必须同时定义或删除拷贝构造、拷贝赋值、移动构造、移动赋值：

```cpp
class VideoBuffer {
public:
    VideoBuffer() = default;
    ~VideoBuffer();

    VideoBuffer(const VideoBuffer&) = delete;
    VideoBuffer& operator=(const VideoBuffer&) = delete;
    VideoBuffer(VideoBuffer&&) noexcept;
    VideoBuffer& operator=(VideoBuffer&&) noexcept;
};
```

移动操作的异常说明见下一节；上例假定资源转移不抛异常，实际实现必须满足该承诺。析构函数不得让异常逃逸。

---

## noexcept

仅对能够保证不抛异常的操作声明 `noexcept`。移动、只读或 `const` 不等于不抛异常；复制字符串、分配内存、获取锁等操作须逐项判断，否则异常逃逸会触发 `std::terminate`。

```cpp
VideoDecoder(VideoDecoder&&) = default;  // 由编译器推导异常说明
int32_t getFrameCount() const noexcept { return frame_count_; }
std::string getName() const { return name_; }  // 复制可能分配内存
```

泛型转发按实际操作使用条件 `noexcept`；不能仅为容器优化而承诺无法保证的异常行为。

---

## 异常策略

优先遵循已有错误处理契约；尚未确定时建议业务层使用策略一。不同机制可以在显式边界转换，不让调用方猜测同一接口如何失败。存在长期取舍时按 ADR 规范记录，不为使用默认值强制新建 ADR：

**策略一（性能敏感项目推荐）**：错误码 + `std::optional`。失败返回 `std::nullopt`，调用方必须检查；需要携带错误详情时返回错误码结构体。`std::expected` 是 C++23 特性，C++17/20 项目禁止使用；确需该语义时按依赖管理规则评估 `tl::expected` 等替代。

```cpp
std::optional<VideoFrame> decodeFrame(const AVPacket* packet);
```

**策略二**：以异常报告无法在当前层处理的失败，在明确边界捕获并转换或恢复；析构清理不得抛出。线程入口必须捕获异常，允许通过 `std::exception_ptr`、`std::future` 等机制安全传递，禁止让异常逃出线程入口。

---

## 命名空间

每个模块定义独立命名空间，推荐 C++17 嵌套语法：

```cpp
namespace player::codec {
class VideoDecoder { /* ... */ };
}
```

禁止：全局命名空间定义业务符号；命名空间嵌套超过 3 层；头文件中使用 `using namespace`。

---

## 依赖与 include 规范

* include 路径以模块根或 `include/` 为基准，禁止 `#include "../其他模块/..."` 形式的跨模块相对引用。
* 跨模块只能 include 对方公共头（`include/` 下的头或模块声明的公共接口）；`*Impl.h` 等实现细节头禁止被外部引用。
* 头文件必须自包含：自身依赖先 include，单独编译即可通过。头文件中优先用前置声明表达指针/引用依赖，仅继承、值语义成员、模板实参等需要完整类型时才 include。
* 跨模块只使用公共 API。需要替换实现、隔离副作用或稳定 ABI 时采用接口、依赖注入或事件；稳定值类型与简单公共工具允许直接使用，不强制为单一实现添加接口或工厂。
* `friend` 仅限模块内部使用，禁止跨模块 friend。

---

## 设计模式

模式服务于真实复杂度，不是优雅的展示品：先有真实痛点，再选模式；最简单能胜任的写法永远优先。

### 常用模式与使用信号

| 模式 | 使用信号 | 项目典型形态 |
| --- | --- | --- |
| Command | 操作需要撤销/重做、排队执行或审计 | TimelineController 撤销栈 |
| Observer / 事件 | 一对多通知，且不想让模块互相依赖 | 模块间经事件/回调解耦 |
| Strategy | 同一接口下多种算法、格式需运行时切换 | 解码器、序列化器选型 |
| Factory | 组装层统一创建、配合注入隐藏具体类型 | main / 工厂创建模块实例 |
| Pimpl | 公共头需要稳定、隔离编译依赖或保护 ABI | 对外导出的头文件 |
| State | 行为随状态迁移、条件分支开始爆炸 | 协议、会话状态机 |

### 禁止（反模式）

* 为模式而模式：仅因命名或假设的未来扩展而增加接口层、工厂或事件总线。
* 预防性抽象：没有真实替换、测试隔离、兼容性或复用需求时不提前设计。
* 每个新模式都应解决具体痛点；是否引入由需求决定，不按一次变更的模式数量限制。
* 借模式绕过其他规则：Singleton 不豁免全局状态禁令，Observer 不豁免分层方向约束。

---

## 全局状态

默认避免可变全局业务状态；只读常量允许共享。确需进程级服务时明确生命周期与并发契约，优先显式传入；Meyers 单例只保证初始化线程安全，不保证成员操作线程安全：

```cpp
class Logger {
public:
    static Logger& instance() {
        static Logger logger;  // Meyers Singleton，线程安全
        return logger;
    }
};
```

Meyers 单例依赖程序退出阶段的静态析构顺序：禁止在其析构中访问其他可能已销毁的单例或全局资源；持有外部资源（文件、连接、线程）的单例必须提供显式 `shutdown()` 并在退出前调用，不得把清理留给退出时的自动析构。

---

## 命名约定

| 对象 | 规则 | 示例 |
| --- | --- | --- |
| 常量 | `k` + PascalCase | `kMaxRetryCount` |
| 成员变量 | snake_case + 尾缀下划线 | `frame_count_` |
| 函数 / 方法 | camelCase，动作 + 对象 | `decodeFrame` |
| 布尔 getter | `is` / `has` / `can` 前缀 | `isRunning` |
| 类文件 | PascalCase，与类名一致 | `VideoDecoder.h` |
| 工具函数文件 | snake_case | `string_utils.h` |
| 头文件扩展名 | 新项目默认 `.h`；已有项目沿用 `.hpp` 等约定 | — |

---

## C++ 并发实现

### 锁

持锁时间必须短，禁止持锁期间调用外部回调：

```cpp
{
    std::lock_guard<std::mutex> lock(mutex_);
    local_data = shared_data_;
}
callback_(local_data);  // 释放锁后再回调
```

多锁获取顺序必须全局一致（防死锁）。优先 `std::lock_guard` / `std::unique_lock`，禁止裸 `mutex.lock()` / `unlock()`。

### 条件变量

必须配合谓词循环检查，防虚假唤醒：

```cpp
cv.wait(lock, [this] { return condition_; });
```

### 原子操作

简单标志位优先 `std::atomic<bool>` 而非 mutex，例如线程退出标志。

### 线程生命周期

线程必须有明确归属和收尾路径：禁止无管理的 `detach()`；通过 `std::atomic<bool>` 停止标志通知退出，由创建方 `join()` 回收；异常路径同样保证 join（必要时用 RAII 包装线程句柄）。

### 线程安全声明

公共类的线程安全等级必须在头注释中声明其一：线程安全、条件线程安全（调用方加锁）、非线程安全（单线程使用）。未声明时按非线程安全处理，禁止跨线程共享。

### 异步回调与生命周期

异步回调必须保证对象在整个访问期间存活。共享所有权场景使用 `weak_ptr::lock()` 持有临时强引用；其他场景可用有同步保证的取消与等待机制。仅检查一个“存活”标志不能防止检查后对象被销毁。裸 `this` 仅在可证明任务结束早于对象销毁时使用。

注册回调时必须返回可注销的句柄；对象析构前注销自身全部回调，不得依赖回调方主动停止。

---

## 注释

* 公共接口必须使用 Doxygen 格式（`@brief` / `@param` / `@return`）。
* 注释解释「为什么」，不解释「做什么」，禁止翻译式注释和 `// 加1`。
* 复杂算法在函数上方说明算法思想与时间复杂度。
* 禁止长期保留注释掉的死代码，应直接删除。

---

## 文件头注释

每个 C++ 文件顶部必须有头注释，说明文件职责、关键公共接口和使用方式。AI 探索陌生代码时先读头注释，准确性直接影响开发效率。

头文件：

```cpp
/**
 * @file VideoDecoder.h
 * @brief 视频解码器——将编码包解码为视频帧
 *
 * 关键接口：
 *   initialize(config) → 初始化
 *   decodeFrame(packet) → std::optional<VideoFrame>
 *
 * @code
 *   VideoDecoder decoder;
 *   decoder.initialize(config);
 *   if (auto frame = decoder.decodeFrame(packet)) { render(*frame); }
 * @endcode
 */
```

源文件只需 `@file` + 一句话职责。工具函数文件另列出导出的函数清单。

强制要求：新增文件必须写头注释；接口变更时同步更新；头注释只描述「是什么」和「怎么用」，不重复实现细节。

---

## 安全

禁止不安全的 C 函数：用 `snprintf` / `std::string` 替代 `strcpy` / `strcat` / `sprintf`；`strncpy` 截断时不保证结尾 `\0`，同样禁止；用 `fgets` / `std::getline` 替代 `gets`；`scanf` 必须限定长度。

边界与整数：数组下标访问前必须检查边界；外部输入的下标优先用 `std::vector::at()`，内部热路径在已证明安全后可用 `operator[]`；外部传入的偏移量和长度必须验证；禁止有符号与无符号混用比较；缓冲区大小计算必须检查溢出。

---

## 字符编码与 const 正确性

* 源码、字符串常量与文件内容统一 UTF-8；Windows API 边界显式转换（`MultiByteToWideChar` / `WideCharToMultiByte`），禁止依赖系统 ACP 隐式转换。
* 依赖 `wchar_t` 的第三方接口只在边界层转换，内部一律 UTF-8，禁止 UTF-8 与宽字符在模块之间来回互转。
* 不修改对象状态的接口声明 `const`；`noexcept` 按专门章节判断。不变的局部值/引用优先用 `const`，数据成员是否为 `const` 同时考虑赋值和移动语义。返回引用时明确生命周期与可变性契约，跨模块避免泄露受保护的内部状态。

---

## 接口兼容与导出

改动公共头文件前必须评估 ABI Break（二进制兼容）与 API Break（源码兼容）。适用范围：导出动态/静态库的项目两者都评估；纯应用程序（无导出库）只评估 API Break，ABI 条款不适用。

已承诺稳定兼容性的公共接口在同一大版本内不得无迁移方案地删除或修改签名；承诺 ABI 稳定时不得改变导出布局。未发布的内部接口按实际调用方与构建边界评估，不把发布库政策强加给纯应用内部重构。

废弃接口必须标注 `[[deprecated("使用 newFunction() 替代")]]`，至少保留一个版本，并在 `docs/api.md` 与 `changelog.md` 记录。

动态库符号统一用宏控制导出（构建库时定义 `PROJECT_EXPORTS`）：

```cpp
#if defined(_WIN32)
    #if defined(PROJECT_EXPORTS)
        #define PROJECT_API __declspec(dllexport)
    #else
        #define PROJECT_API __declspec(dllimport)
    #endif
#else
    #define PROJECT_API __attribute__((visibility("default")))
#endif
```

---

## AI 自测钩子（Test Hook）

### 目的

优先复用现有测试入口。UI、硬件或时序路径缺少自动化覆盖时，评估薄钩子、受控集成测试或可审计人工验证；钩子不是必选项。无法执行时如实报告限制，不为消除“未验证”字样擅自扩大生产代码改动。

### 设计原则：入口与逻辑分离，宏不污染业务代码

1. **根治手段是入口-逻辑分离**：事件处理拆成「事件入口 + 动作逻辑」，UI 回调与钩子转发到同一动作函数。钩子是薄封装，禁止在钩子里写业务逻辑。
2. **集中声明**：钩子集中在类内一处 `PROJECT_TEST_HOOK:` 段，或独立 `<模块>TestHook.h`；禁止在业务函数体内散落 `#ifdef`。
3. **发布版不包含钩子**：仅在集中声明区用编译开关包含钩子，关闭时移除其声明和实现；改成 private 不等于移除。
4. **函数体内零插桩**：若某段逻辑「必须从函数体内部观测」，正确做法是把它拆成独立动作函数供钩子调用。

### 开关与可见性

开关只允许在构建系统中定义，禁止在源文件硬编码 `#define`。CMake 侧写法见 `C++构建与依赖规范.md`「测试钩子开关」。

如需可见性标签宏，可在项目内部配置头统一定义；是否包含钩子由集中声明区的条件编译控制，不使用 public/private 切换假装剔除：

```cpp
#define PROJECT_TEST_HOOK public
```

该宏必须在所有包含此头文件的 target 上保持一致：库本体与依赖钩子的测试目标要么同时开启，要么测试目标不编译。禁止出现库以关闭态编译、测试以开启态链接同一份头文件的情况——那会让同一头文件展开出不同的类定义。

### 示例

```cpp
class TimelineController {
public:
    void onUndoButtonClicked();      // UI 入口：仅转发，不写逻辑

#ifdef PROJECT_ENABLE_TEST_HOOKS
PROJECT_TEST_HOOK:
    // AI 自测钩子：薄封装，仅转发/读取
    void testHook_triggerUndo() { performUndo(); }
    size_t testHook_getUndoDepth() const noexcept {
        return undo_stack_.size();
    }
#endif

private:
    void performUndo();              // 真实逻辑，UI 与钩子共用
    std::vector<std::unique_ptr<Command>> undo_stack_;
};
```

时序 / 异步场景：状态查询钩子同步返回，等待与超时控制放在测试侧，优先使用完成通知或带超时的等待；禁止在钩子内 sleep 或挂起业务线程。

```cpp
#ifdef PROJECT_ENABLE_TEST_HOOKS
PROJECT_TEST_HOOK:
    bool testHook_isRenderFinished() const noexcept { return render_finished_.load(); }
#endif
```

涉及 UI 线程的钩子必须声明调用线程语义；只读查询用 `std::atomic` 或锁保护。

### 强制约束

* 统一 `testHook_` 前缀，每个钩子用一行说明用途；清单与用例维护见 `测试规范.md`。
* 禁止出现在 `include/` 公共头，禁止导出到动态库符号表。
* 单个类内 `PROJECT_TEST_HOOK` 段最多一处，业务函数体内钩子相关 `#ifdef` 必须为零。
* 只暴露验证所需的路径，禁止借钩子开放内部实现（绕过封装）。
* 剔除时关闭构建开关即可；彻底移除只需删除集中的钩子段。
