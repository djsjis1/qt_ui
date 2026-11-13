# 参数表格控件 param-table

轴参数面板形态的参数矩阵：行 = 参数（名称 / 单位），列 = 数据源（X 轴 / Y 轴 / Z 轴…），
数值列双击输入，列可由调用者勾选显隐。

## 定位

**只负责「展示矩阵 + 轮询节奏 + 编辑校验 + 列显隐」，读取底层、写底层、勾选框、错误提示都在调用者侧。**

控件做的事：

- 列 0 是参数名，之后每列对应 `addColumn()` 注册的一个数据源；
- 按 `pollInterval` 定时发出 `pollRequested()`，调用者收到后读底层，再 `setValue(rowId, columnId, value)` 回填；
- 双击数值列进入编辑，提交时做数值解析与范围校验，通过就发 `valueEdited(rowId, columnId, value)`；
- 越界或非法输入直接恢复原值，并发 `editRejected(rowId, columnId, text)`；
- `setColumnVisible()` 提供列显隐，勾选框由调用者在面板上自行摆放。

## 接入

```cmake
add_subdirectory(ui-kit/param-table)
target_link_libraries(你的目标 PRIVATE param_table)
```

```cpp
#include "ParamTableWidget.h"

auto *table = new uikit::ParamTableWidget(parent);

// 先建列（数据源），再建行（参数）
table->addColumn(QStringLiteral("x"), QStringLiteral("X 轴"));
table->addColumn(QStringLiteral("y"), QStringLiteral("Y 轴"));

uikit::ParamRow row;
row.id = QStringLiteral("feed");
row.name = QStringLiteral("进给速度");
row.unit = QStringLiteral("mm/min");
row.minimum = 0.0;
row.maximum = 5000.0;
row.decimals = 1;
table->addParam(row);

table->setValue(QStringLiteral("feed"), QStringLiteral("x"), 1200.0);
table->setValue(QStringLiteral("feed"), QStringLiteral("y"), 1200.0);

table->setPollInterval(200);
table->setPollingEnabled(true);
```

表格形态：

| 参数 | X 轴 | Y 轴 |
| --- | --- | --- |
| 进给速度 (mm/min) | 1200.0 | 1200.0 |
| 加速度 (mm/s²) | 800 | 800 |

## 调用者怎么接底层

```cpp
// 1) 列勾选框（可选）：一条列一个勾选框，控件只负责列显隐
auto *box = new QCheckBox(QStringLiteral("X 轴"), panel);
connect(box, &QCheckBox::toggled, this, [table](bool on) {
    table->setColumnVisible(QStringLiteral("x"), on);
});

// 2) 轮询：控件只给节奏，读哪个函数由调用者决定
connect(table, &uikit::ParamTableWidget::pollRequested, this, [this]() {
    for (const auto &axis : axes) {
        for (const auto &p : params) {
            table_->setValue(p.id, axis.id, device.read(axis.id, p.id));
        }
    }
});

// 3) 编辑提交：新值作为参数直接调用底层写函数
connect(table, &uikit::ParamTableWidget::valueEdited, this,
        [this](const QString &row_id, const QString &column_id, double value) {
            device.write(column_id, row_id, value);
            table_->setValue(row_id, column_id, device.read(column_id, row_id)); // 回填实际值
        });

// 4) 输入被拒绝：可选，做提示
connect(table, &uikit::ParamTableWidget::editRejected, this,
        [](const QString &row_id, const QString &column_id, const QString &text) {
            /* 提示哪个参数的哪一列被拒绝 */
        });
```

`valueEdited` 在提交时发出（回车或失焦），不依赖轮询。
底层拒绝写入时，`setValue()` 回填底层实际值即可让界面自动回滚。

## API

### 列（数据源）

| 接口 | 说明 |
| --- | --- |
| `int addColumn(id, name)` | 追加数据列，返回列序号；id 为空或重复时返回 -1 |
| `int addColumn(const ParamColumn &)` | 同上，可指定初始 `visible` |
| `void clearColumns()` | 移除全部数据列，保留参数行（数值清零） |
| `int dataColumnCount() const` | 数据列数量（不含参数名列） |
| `int columnIndexOf(id) const` | 按 id 查列序号，不存在返回 -1 |
| `void setColumnVisible(id, visible)` | 显示/隐藏列，变化时发 `columnVisibilityChanged` |
| `bool isColumnVisible(id) const` | 列当前是否显示 |

### 行（参数）

| 接口 | 说明 |
| --- | --- |
| `int addParam(const ParamRow &)` | 追加参数行，返回行号；id 为空或重复时返回 -1 |
| `int addParam(id, name, unit = {})` | 便捷重载，范围 ±1e12、精度 3 位 |
| `void clearParams()` | 清空全部参数行，保留数据列 |
| `int paramCount() const` | 参数行数 |
| `int rowOf(id) const` | 按 id 查行号，不存在返回 -1 |

`ParamRow` 定义在 `ParamTableTypes.h`：

| 字段 | 说明 |
| --- | --- |
| `QString id` | 唯一键，回填与编辑回调都用它 |
| `QString name` | 显示名称，空时显示 id |
| `QString unit` | 单位，非空时显示在参数名后 |
| `double minimum / maximum` | 允许输入范围（作用于该行所有列） |
| `int decimals` | 显示与校验精度（0~9） |
| `bool editable` | 是否允许双击编辑 |

未显式设置范围时（默认 ±1e12），单元格不显示"允许范围"提示。

### 数值读写

| 接口 | 说明 |
| --- | --- |
| `void setValue(rowId, columnId, value)` | 回填最新值，不发 `valueEdited`；该单元格正在编辑时只更新后备值 |
| `void setValueAt(row, dataColumn, value)` | 按行号与列序号回填 |
| `double value(rowId, columnId) const` | 读取控件内记录的值 |
| `void setRange(rowId, lo, hi)` | 修改该行可编辑范围 |
| `void setDecimals(rowId, n)` | 修改该行显示精度 |
| `void setEditable(rowId, editable)` | 切换该行是否可编辑 |

### 轮询

| 接口 | 说明 |
| --- | --- |
| `void setPollInterval(ms)` | 轮询周期，默认 200 ms，小于 10 按 10 处理 |
| `void setPollingEnabled(bool)` | 开/关轮询；打开时立即发一次请求 |
| `int pollInterval() const` / `bool isPollingEnabled() const` | 读取当前设置 |

### 信号

| 信号 | 说明 |
| --- | --- |
| `pollRequested()` | 到达轮询周期，调用者应读底层并按行列回填 |
| `valueEdited(rowId, columnId, value)` | 双击编辑并通过校验，调用者应把 value 传给底层写函数 |
| `editRejected(rowId, columnId, text)` | 输入非法或越界，控件已恢复原值 |
| `columnVisibilityChanged(columnId, visible)` | 列显隐变化，便于界面同步勾选框 |

## 已知限制

- **非线程安全**：所有接口只能在 GUI 线程调用；采集线程需自行投递到 GUI 线程。
- **只使用 `addColumn` / `addParam` / `clearColumns` / `clearParams` 增删**：直接调用
  `QTableWidget` 的 `insertRow`、`removeRow`、`setColumnCount` 会让内部配置与表格错位。
- 只支持数值参数（`double`）；枚举、布尔、文本参数不在本控件范围内。
- 编辑在单元格内完成，没有自定义编辑器（spinbox）：范围与精度校验在提交时执行，
  输入过程中不做限制；单元格悬停会显示可编辑性与允许范围。
- 行范围/精度是行级的，同一行所有数据列共用同一套限制。
- 无内置日志：拒绝事件只通过 `editRejected` 暴露给调用者。
