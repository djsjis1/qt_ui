# 曲线控件 plot

实时波形控件，纯 Qt Widgets 自绘，不依赖 Qt Charts、QCustomPlot 或任何第三方绘图库。

## 定位

**只负责「数据 + 视图状态 + 渲染」，不处理任何鼠标交互。**

缩放、平移、框选、游标拖拽、图例点击全部由调用者实现：控件提供坐标换算接口
（`plotArea` / `pixelToDataX` / `dataToPixel`），调用者把鼠标位置换成数据坐标，
再调用 `setXRange`、`setYRange`、`zoomToRange`、`setCursor` 等方法驱动视图。

这样同一份渲染内核可以接 Widgets、QML 或任意 UI 框架。

## 接入

```cmake
add_subdirectory(ui-kit/plot)
target_link_libraries(你的目标 PRIVATE plot)
```

```cpp
#include "PlotWidget.h"

auto *plot = new uikit::PlotWidget(parent);
plot->setCapacityPerChannel(200000);   // 每通道保留点数，内存 ≈ 通道数 × 点数 × 8B
plot->setSampleInterval(0.001F);       // 采样周期 1ms
plot->setTimeWindow(10.0F);            // 跟随模式下显示最近 10 秒
plot->addChannel(QStringLiteral("CH1"), QColor(0x00, 0xE5, 0x76));
plot->appendFrame({0.5F});             // 写入一帧，内部自动请求重绘
```

## API

### 通道

| 接口 | 说明 |
| --- | --- |
| `int addChannel(name, color)` | 新增通道，返回通道号；颜色无效时按内置色板分配 |
| `void setChannelVisible(ch, visible)` | 通道显隐，变化时发出 `channelVisibilityChanged` |
| `void setAllChannelsVisible(visible)` | 批量显隐全部通道，只重新聚合一次 |
| `bool isChannelVisible(ch) const` | 读取通道显隐 |
| `int channelCount() const` | 通道数量 |
| `void setCapacityPerChannel(n)` | 每通道环形缓冲容量；**会清空数据**，应在 `addChannel` 前调用 |

### 曲线外观（线 / 点）

线、点是两个独立开关，按通道设置；两者都关闭时该通道只剩图例，不产生像素。

| 接口 | 说明 |
| --- | --- |
| `void setChannelLineVisible(ch, visible)` | 是否绘制连线与包络竖线 |
| `void setChannelPointVisible(ch, visible)` | 是否绘制采样点 |
| `void setChannelLineWidth(ch, px)` | 线宽，物理像素，取值限制在 1~8 |
| `void setChannelPointSize(ch, px)` | 点直径，物理像素，取值限制在 1~16 |
| `void setChannelPointShape(ch, shape)` | `PlotPointShape::{Circle, Square, Diamond}` |
| `bool isChannelLineVisible(ch) const` | 读取线开关 |
| `bool isChannelPointVisible(ch) const` | 读取点开关 |
| `int channelLineWidth(ch) const` | 读取线宽 |
| `int channelPointSize(ch) const` | 读取点直径 |
| `PlotPointShape channelPointShape(ch) const` | 读取点形状 |
| `PlotChannelStyle channelStyle(ch) const` | 一次读出该通道全部显示配置 |

点的语义：每列一个点，取该列聚合结果的极值中点。列宽约两像素，中点即该列代表值，
因此点模式的绘制量同样只由绘图区像素宽决定，与采样率和缓冲总量无关。

```cpp
plot->setChannelLineVisible(0, true);
plot->setChannelPointVisible(0, true);
plot->setChannelPointSize(0, 4);
plot->setChannelPointShape(0, uikit::PlotPointShape::Diamond);
```

### 画面元素显隐

与通道无关的装饰元素，默认全部显示。变化时发出 `displayOptionsChanged`。

| 接口 | 说明 |
| --- | --- |
| `void setLegendVisible(visible)` | 图例显隐 |
| `void setGridVisible(visible)` | 网格点线显隐（只影响画布，不影响刻度） |
| `void setAxesVisible(visible)` | 坐标轴边框、刻度线、刻度文字显隐 |
| `bool isLegendVisible() const` | 读取图例显隐 |
| `bool isGridVisible() const` | 读取网格显隐 |
| `bool isAxesVisible() const` | 读取坐标轴显隐 |

隐藏坐标轴只是不画，绘图区尺寸仍按坐标轴边距计算，不会让波形贴到窗口边缘。

### 数据

| 接口 | 说明 |
| --- | --- |
| `void appendFrame(values)` | 按通道顺序写同一时刻的采样值，长度不足只写前 N 路 |
| `void appendValue(ch, value)` | 单通道写入 |
| `void clearData()` | 清空数据，保留通道与视图设置 |

### 按序号写入（丢包检测）

每通道可用自己的递增序号写入，组件在数据层按通道记住上一次序号：

| 接口 | 说明 |
| --- | --- |
| `bool appendSample(ch, seq, y)` | 写入一个带序号的采样点，`seq` 为无符号整数 |
| `bool hasChannelSequence(ch) const` | 该通道是否已收到过序号 |
| `quint64 channelLastSequence(ch) const` | 该通道最后接受的序号 |

```cpp
if (!plot->appendSample(0, seq, value)) {
    // 序号重复或回退，该点被丢弃
}
```

规则：

| 序号差 | 处理 |
| --- | --- |
| 1 | 连续，正常连线 |
| > 1 | 视为丢包，横轴按差值跳过（差 42 就跳过 41 个采样周期），缺口处**曲线断开** |
| ≤ 0 | 丢弃该点并返回 false，避免破坏环形缓冲的单调性 |

每个通道以自己收到的第一个序号作为时间轴起点，各通道独立计数；同时开始采集的通道天然对齐。

两点限制：

1. `appendSample` 与 `appendFrame` / `appendValue` 使用不同的时间轴来源，**同一通道不要混用**。
2. 横轴坐标仍是 `float`。1 kHz 采样下连续运行约 2~4 小时后，1 ms 的间隔开始无法区分，需要重启或改用更大的采样周期。

### 视图

| 接口 | 说明 |
| --- | --- |
| `void setTimeWindow(seconds)` | 跟随模式的横向时间窗 |
| `void setSampleInterval(seconds)` | 采样周期，决定 `appendFrame` 的内部时间推进 |
| `void setPaused(bool)` | 冻结画面，数据仍写入缓冲 |
| `void setFollowLatest(bool)` | 是否跟随最新数据滚动 |
| `void setAutoYRange(bool)` | 是否按可见数据自动调整纵轴 |
| `void setYRange(lo, hi)` | 固定纵轴范围，会关闭自动量程 |
| `void setXRange(lo, hi)` | 直接设横向范围，会关闭跟随 |
| `void zoomToRange(x0, x1, y0, y1)` | 放大到指定范围，同时关闭跟随与自动量程 |
| `PlotRange xRange() / yRange()` | 当前显示范围，供调用者计算缩放后的新范围 |
| `float timeWindow() / sampleInterval()` | 读取当前设置 |
| `bool isPaused() / isFollowLatest() / isAutoYRange()` | 读取当前状态 |

`PlotRange` 定义在 `PlotTypes.h`：`{ float lower; float upper; }`，带 `span()`。

### 游标与标记（由调用者驱动）

| 接口 | 说明 |
| --- | --- |
| `void setCursor(index, x)` | `index` 0 = 游标 A，1 = 游标 B；`x` 为数据坐标 |
| `void clearCursors()` | 清除两个游标 |
| `bool hasCursor(index) const` | 游标是否可见 |
| `float cursorX(index) const` | 游标所在数据坐标 |
| `void setHoverX(x)` | 悬停读数位置；传非有限值等同 `clearHover()` |
| `void clearHover()` | 关闭悬停读数 |
| `void setSelectionRect(rect)` | 框选矩形（控件坐标），仅用于显示 |
| `void clearSelectionRect()` | 清除框选矩形 |

### 坐标换算

| 接口 | 说明 |
| --- | --- |
| `QRectF plotArea() const` | 绘图区（已扣除坐标轴边距） |
| `float pixelToDataX(px)` | 控件横向像素 → 数据坐标 |
| `float pixelToDataY(px)` | 控件纵向像素 → 数据坐标 |
| `QPointF dataToPixel(x, y)` | 数据坐标 → 控件像素 |

### 导出

| 接口 | 说明 |
| --- | --- |
| `bool exportVisibleCsv(path)` | 导出当前视窗内的原始采样点（UTF-8，首列时间 + 各通道） |
| `QPixmap grabPlot()` | 抓取当前画面，可保存 PNG |

### 信号

| 信号 | 说明 |
| --- | --- |
| `followLatestChanged(bool)` | 跟随状态变化 |
| `pausedChanged(bool)` | 暂停状态变化 |
| `autoYRangeChanged(bool)` | 自动量程开关变化 |
| `channelVisibilityChanged(int, bool)` | 通道显隐变化 |
| `frameStatsUpdated(double fps, double paint_ms)` | 每 500ms 汇报帧率与单帧绘制耗时 |

## 调用者如何实现交互

### 滚轮缩放（以鼠标为锚点）

```cpp
bool MyWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (watched != plot_ || event->type() != QEvent::Wheel) {
        return false;
    }
    auto *wheel = static_cast<QWheelEvent *>(event);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    const QPointF pos = wheel->position();
#else
    const QPointF pos = wheel->posF();
#endif

    const float factor = wheel->angleDelta().y() > 0 ? 0.8F : 1.25F;
    const float anchor = plot_->pixelToDataX(static_cast<float>(pos.x()));
    const uikit::PlotRange range = plot_->xRange();

    plot_->setXRange(anchor + (range.lower - anchor) * factor,
                     anchor + (range.upper - anchor) * factor);
    return true;
}
```

### 框选放大

```cpp
// 按下：开始画框
plot_->setSelectionRect(QRectF(press_pos, press_pos));

// 拖动：只更新显示
plot_->setSelectionRect(QRectF(press_pos, current_pos));

// 松开：应用范围并清除框
const QRectF rect = QRectF(press_pos, release_pos).normalized();
plot_->clearSelectionRect();
if (rect.width() > 8 && rect.height() > 8) {
    plot_->zoomToRange(plot_->pixelToDataX(rect.left()),
                       plot_->pixelToDataX(rect.right()),
                       plot_->pixelToDataY(rect.bottom()),
                       plot_->pixelToDataY(rect.top()));
}
```

### 游标

```cpp
plot_->setCursor(0, plot_->pixelToDataX(mouse_x));  // 放游标 A
plot_->setCursor(1, plot_->pixelToDataX(mouse_x));  // 放游标 B
plot_->clearCursors();
```

## 性能模型

```
每通道环形缓冲（固定容量，写满覆盖最旧）
        ↓ 每帧聚合一次
时间窗内全部样本  →  每两像素一列取 min/max（≈ 绘图区宽/2 × 2 点）
        ↓
直接写像素 + Bresenham 连线 → QImage → 一次贴图
```

- 每帧绘制量 ≈ 绘图区像素宽 × 通道数，**与采样率、缓冲总量无关**
- 聚合复杂度 O(时间窗内样本数)
- 实测（3 通道，1440p，125% DPI）：单帧约 12ms，60fps

### 平台建议

| 场景 | 建议 |
| --- | --- |
| 桌面开发 | 默认即可 |
| RK3588 / 1080p | 3 通道约 30~40fps；10 通道会明显卡 |
| 低性能设备 | 减少可见通道、限制时间窗、降低刷新频率 |

## 已知限制

- **非线程安全**：所有接口只能在 GUI 线程调用；采集线程需自行投递到 GUI 线程。
- **x 必须单调递增**：降采样依赖该前提做提前退出。
- 聚合在 `paintEvent` 中按帧执行，时间窗内样本超过数十万点时会成为热点；
  优化方向是写入时按列累加（增量桶），把每帧开销降到 O(新增点数)。
- 无内置日志：需要记录帧率/耗时请订阅 `frameStatsUpdated`。
- 粗线为 2 物理像素硬边，不做抗锯齿（抗锯齿会让绘制慢一个数量级）。
