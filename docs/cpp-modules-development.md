# C++ 模块开发与内联性能

本项目领域层采用 C++23 命名模块，UI 为普通 C++ 源文件并经过 HuxerUI codegen。
规则以实体所属模块与定义位置为准，不能仅凭文件扩展名判断。

## 接口中的小函数

命名模块中的普通类内函数不隐式 `inline`。这一点同样适用于
`core_store.cppm` 在导出类体内包含的 `core_store_state.inc`。
小访问器、简单状态判断与轻量转发函数若需要在导入方展开，显式写 `inline`，
定义保持在接口定义域；不要只改声明，再把 inline 定义隐藏在实现 `.cpp` 中。

```cpp
export module example;

export struct Status {
    int value = 0;
    inline int get() const noexcept { return value; }
    constexpr bool valid() const noexcept { return value >= 0; }
    bool operator==(const Status&) const = default;
};
```

`constexpr`、`consteval` 与首次声明即 `= default` 的函数仍隐式 inline。
普通头文件/全局模块片段中的类内函数也仍隐式 inline；本项目 `wire_codec.h` 的
`Result::operator bool`、UI 模型方法因此无需重复添加。纯字段 DTO 与编译器生成的
特殊成员不需要人为增加构造函数。模板本身不必然 inline，但实例化所需函数体
可以在没有该关键字时仍对导入方可用。

普通自由函数在头文件或 `.cppm` 中定义都不自动 inline；轻量导出包装可以显式
inline。实现单元内可见的小函数则可由优化器自行展开，不因属于命名模块而
一律失去优化。I/O、解析、JSON 构建、锁和大对象复制需按实际开销评估，不能
按代码行数批量添加。HuxerUI composable 沿用普通 UI 源文件的约定。

`inline` 不保证调用展开，也不使命名模块获得普通头文件的多重定义许可。
不通过批量 `always_inline` 或 `-fmodule-implicit-inline` 解决遗漏；后者在 GCC
中会改变 ABI，不能只给一部分单元启用。

## 性能验证

先检查 CMakeCache 与实际编译命令中的优化和 LTO 设置，不擅自改变用户构建配置。
本次检查时本地 `build` 为 Debug（`-g`，没有优化开关）；Release 为 `-O3 -DNDEBUG`。
GCC 普通无优化构建下，仅写 inline 不会使调用自动展开。

验证跨模块优化时，单独编译一个导入真实项目模块的消费单元，比较修改前后
`objdump -drC` 的调用与重定位。只看模块自身汇编，不能证明导入方获得了函数体。
调用被消除之后，分配、字符串/容器复制、锁与系统调用仍可能存在。例如
`CoreStore::snapshot()` 会复制快照，`CoreModel::Update()` 会复制 `CoreView`；
改 inline 不能代替数据所有权与复制路径的设计。

端到端性能需在相同构建配置、输入和缓存条件下单独测量，区分解析、配置生成、
内核检查与启动等阶段；解析库评估见 [Glaze 迁移与性能复核](glaze-migration.md)。
修改后仍执行仓库规定的目标构建、版本检查与适用回归。

## 2026-10-04 实际检查

补充显式 inline 的 11 处函数：

| 位置 | 函数 |
|---|---|
| `src/vpn.cppm` | `TunRoutePlan::ready/requiresNativeRouteBackend`、`EngineSelection::ok`、`VpnManager::engines/connections/policy` |
| `src/service.cppm` | `ServiceInfo::compatible` |
| `src/store/core_store_state.inc` | `CoreStore::streams/process` |
| `src/store/profiles.cppm` | `ProfilesStore::lastError` |
| `src/utils.cppm` | `nowUnix` |

GCC 16 的独立最小命名模块样例在 `-O2`/`-O3` 下保留普通小函数的外部调用，
显式 inline、constexpr 与 defaulted 比较则展开；`-O0` 下仍保留调用。
导入项目实际模块的消费单元使用已有 Debug BMI、以 `-O3` 编译且不启用 LTO：
修改前上述 11 处都有包装函数调用，修改后全部消失；错误字符串复制的
`operator new/memcpy` 与 `system_clock::now()` 调用仍保留。
这证明导入方优化路径改善，未测得或承诺订阅切换的端到端提速。

代码修改后实际执行：`cmake --build build --target clash-flux`、完整
`cmake --build build`、`./run.sh --version`（v0.3.19）、
`ctest --test-dir build -L clashflux-required --output-on-failure --no-tests=error`
（15/15）和 `git diff --check`，均通过。未在其它平台测量此优化。

依据：[类内函数](https://eel.is/c++draft/class.mfct)、
[defaulted 函数](https://eel.is/c++draft/dcl.fct.def.default)、
[inline 定义域](https://eel.is/c++draft/dcl.inline)、
[ODR](https://eel.is/c++draft/basic.def.odr)、
[GCC 模块选项](https://gcc.gnu.org/onlinedocs/gcc/C_002b_002b-Dialect-Options.html)。
