/**
 * @file PlotWidget.h
 * @brief 实时曲线控件：只负责"数据 + 视图状态 + 渲染"，不处理任何鼠标交互
 *
 * 本控件是纯展示组件：缩放、平移、框选、游标拖拽、图例点击等交互全部由调用者
 * 通过下面的接口组合实现。控件提供坐标换算接口（plotArea / pixelToDataX /
 * dataToPixel），调用者据此把鼠标位置转成数据坐标，再调用 setXRange、
 * setYRange、zoomToRange、setCursor 等方法驱动视图。
 *
 * 关键接口：
 *   addChannel(name, color)      → 新增通道，返回通道号
 *   appendFrame(values)          → 写入同一时刻的各通道采样值
 *   setTimeWindow(seconds)       → 跟随模式下的横向时间窗
 *   setXRange / setYRange        → 直接指定显示范围（会关闭跟随 / 自动量程）
 *   zoomToRange(x0, x1, y0, y1)  → 放大到指定数据范围
 *   setCursor(0/1, x)            → 放置或移动游标 A/B
 *   setHoverX(x)                 → 悬停读数位置
 *   setSelectionRect(rect)       → 框选可视化的矩形（控件坐标）
 *
 * 性能模型：每帧绘制的点数由「绘图区像素宽度 × 2 × 可见通道数」决定，与采样率
 * 和缓冲总量无关；原始采样点保存在每通道固定容量的环形缓冲里。
 *
 * 单帧渲染管线：
 *   appendFrame / appendSample  →  每通道环形缓冲
 *              ↓ paintEvent
 *   rebuildDisplayData          →  按「绘图区宽 / 2」列做 min/max 聚合
 *              ↓
 *   drawCurves                  →  ① 数据坐标换算成像素列
 *                                  ② 分块填充包络竖线（64×64 块，命中 L1）
 *                                  ③ 相邻列 Bresenham 连线 + 采样点
 *              ↓
 *   drawImage                   →  整数位置 1:1 贴到控件
 *
 * 线程约束：非线程安全，只允许在 GUI 线程调用。
 * 依赖：Qt Widgets（Qt 5.14.2 / Qt 6.x 通用），不依赖 Qt Charts / QCustomPlot。
 *
 * @code
 *   auto *plot = new uikit::PlotWidget(this);
 *   plot->addChannel(QStringLiteral("CH1"));
 *   plot->appendFrame({0.5F});
 *
 *   // 调用者自己处理滚轮：把像素位置换成数据坐标再设置范围
 *   const float x = plot->pixelToDataX(event->position().x());
 *   plot->setXRange(x - 5.0F, x + 5.0F);
 * @endcode
 */

#pragma once

#include <QElapsedTimer>
#include <QImage>
#include <QPointF>
#include <QPixmap>
#include <QRectF>
#include <QVector>
#include <QWidget>

#include "PlotDataBuffer.h"
#include "PlotTypes.h"

class QPainter;
class QPaintEvent;
class QResizeEvent;
class QTimer;

namespace uikit {

/**
 * @brief 实时曲线控件（纯展示，交互由调用者驱动）
 */
class PlotWidget : public QWidget
{
    Q_OBJECT

public:
    explicit PlotWidget(QWidget *parent = nullptr);
    ~PlotWidget() override;

    // ---- 通道 ----

    /**
     * @brief 新增一个通道
     * @param name  图例名称，为空时自动命名为 CH1、CH2……
     * @param color 曲线颜色，无效 QColor 时按内置色板分配
     * @return 通道号，从 0 开始
     */
    int addChannel(const QString &name, const QColor &color = QColor());

    /// 设置通道是否绘制；变化时发出 channelVisibilityChanged
    void setChannelVisible(int channel, bool visible);
    /// 批量设置全部通道的显隐，只触发一次重新聚合
    void setAllChannelsVisible(bool visible);
    bool isChannelVisible(int channel) const noexcept;
    int channelCount() const noexcept;

    // ---- 曲线外观：线 / 点（按通道独立设置）----

    /// 是否绘制连线；关闭后该通道只剩包络外的点（若开启）
    void setChannelLineVisible(int channel, bool visible);
    /// 是否绘制采样点；每列一个点，取该列聚合结果的极值中点
    void setChannelPointVisible(int channel, bool visible);
    /// 线宽，物理像素，取值被限制在 1~8
    void setChannelLineWidth(int channel, int physical_pixels);
    /// 点直径，物理像素，取值被限制在 1~16
    void setChannelPointSize(int channel, int physical_pixels);
    /// 点形状，默认圆点
    void setChannelPointShape(int channel, PlotPointShape shape);

    bool isChannelLineVisible(int channel) const noexcept;
    bool isChannelPointVisible(int channel) const noexcept;
    int channelLineWidth(int channel) const noexcept;
    int channelPointSize(int channel) const noexcept;
    PlotPointShape channelPointShape(int channel) const noexcept;
    /// 读取通道的完整显示配置；通道号非法时返回默认配置
    PlotChannelStyle channelStyle(int channel) const;

    /**
     * @brief 设置每通道可保留的采样点数
     * @note 会清空现有数据，应在 addChannel 之前调用
     */
    void setCapacityPerChannel(int samples);

    // ---- 数据 ----

    /**
     * @brief 写入一帧：按通道号顺序写同一个时间点的值
     * @param values 各通道采样值，长度不足时只写前 N 路
     * @note 写入后自动请求重绘；同一轮事件循环内的多次写入会被 Qt 合并
     */
    void appendFrame(const QVector<float> &values);

    /// 写入单个通道的一个采样点，时间轴推进一个采样周期
    void appendValue(int channel, float value);

    /**
     * @brief 按序号写入单通道采样点（每通道各自独立序号）
     * @param channel 通道号
     * @param seq     调用方给出的无符号递增序号
     * @param y       采样值
     * @return false 表示序号重复/回退或通道号非法，该点被丢弃
     *
     * 序号差 > 1 视为丢包：横轴按差值跳过错过的采样周期，曲线在缺口处断开。
     * 与 appendFrame / appendValue 使用不同的时间轴来源，同一通道不要混用两种写法。
     */
    bool appendSample(int channel, quint64 seq, float y);

    /// 该通道是否已收到过序号
    bool hasChannelSequence(int channel) const noexcept;
    /// 该通道最后接受的序号
    quint64 channelLastSequence(int channel) const noexcept;

    /// 清空全部曲线数据，保留通道与视图设置
    void clearData();

    // ---- 视图 ----

    /// 横向时间窗长度（秒），默认 10 秒；仅在跟随模式下生效
    void setTimeWindow(float seconds);
    /// 采样周期（秒），默认 0.01 秒，决定 appendFrame 的内部时间推进
    void setSampleInterval(float seconds);
    /// 暂停/恢复画面刷新；暂停期间数据仍写入缓冲
    void setPaused(bool paused);
    /// 是否跟随最新数据滚动
    void setFollowLatest(bool follow);
    /// 是否按可见数据自动调整纵轴
    void setAutoYRange(bool enabled);
    /// 固定纵轴范围（会关闭自动量程）
    void setYRange(float lower, float upper);
    /**
     * @brief 直接设置横向显示范围（会关闭"跟随最新"）
     */
    void setXRange(float lower, float upper);
    /**
     * @brief 放大到指定的数据范围
     * @note 会关闭跟随最新与自动量程；参数非法时忽略
     */
    void zoomToRange(float x_lower, float x_upper, float y_lower, float y_upper);

    /**
     * @brief 切换到"全部数据"视图
     *
     * 横向覆盖缓冲区内的全部样本，并随新数据增长自动扩展；纵向自动按全部数据缩放。
     * 视为一种特殊视图模式：调用 setXRange / zoomToRange / setTimeWindow /
     * setFollowLatest(true) 会自动退出该模式。
     */
    void fitAllData();

    float timeWindow() const noexcept;
    float sampleInterval() const noexcept;
    bool isPaused() const noexcept;
    bool isFollowLatest() const noexcept;
    bool isAutoYRange() const noexcept;
    /// 是否处于"全部数据"视图
    bool isFitAll() const noexcept;
    /// 当前横向显示范围，供调用者计算缩放/平移后的新范围
    PlotRange xRange() const noexcept;
    /// 当前纵向显示范围
    PlotRange yRange() const noexcept;

    // ---- 画面元素显隐 ----

    /// 是否绘制图例
    void setLegendVisible(bool visible);
    /// 是否绘制网格点线
    void setGridVisible(bool visible);
    /// 是否绘制坐标轴边框、刻度线与刻度文字
    void setAxesVisible(bool visible);

    bool isLegendVisible() const noexcept;
    bool isGridVisible() const noexcept;
    bool isAxesVisible() const noexcept;

    // ---- 游标（由调用者设置位置）----

    /**
     * @brief 设置或移动游标
     * @param index 0 = 游标 A，1 = 游标 B
     * @param x     游标所在的数据坐标（时间轴）
     */
    void setCursor(int index, float x);
    /// 清除两个游标
    void clearCursors();
    /// 指定游标是否可见
    bool hasCursor(int index) const noexcept;
    /// 游标的数据坐标；未激活时返回 0
    float cursorX(int index) const noexcept;

    // ---- 悬停读数（由调用者设置位置）----

    /// 设置悬停读数的时间位置；传非有限值等同于 clearHover()
    void setHoverX(float x);
    /// 关闭悬停读数
    void clearHover();

    // ---- 框选可视化（由调用者驱动）----

    /// 设置框选矩形（控件坐标），仅用于显示；传空矩形等同于 clearSelectionRect()
    void setSelectionRect(const QRectF &rect);
    /// 清除框选矩形
    void clearSelectionRect();

    // ---- 坐标换算（供调用者实现交互）----

    /// 绘图区（控件坐标，已扣除坐标轴边距）
    QRectF plotArea() const;

    /// 控件横向像素 → 数据坐标
    float pixelToDataX(float pixel) const;
    /// 控件纵向像素 → 数据坐标
    float pixelToDataY(float pixel) const;
    /// 数据坐标 → 控件像素
    QPointF dataToPixel(float x, float y) const;

    // ---- 导出 ----

    /**
     * @brief 导出当前视窗内的原始采样点为 CSV（UTF-8，首列时间 + 各通道）
     * @return 写入成功返回 true
     */
    bool exportVisibleCsv(const QString &file_path) const;

    /// 抓取当前画面，用于保存 PNG
    QPixmap grabPlot();

signals:
    void followLatestChanged(bool follow);
    void pausedChanged(bool paused);
    /// 自动量程开关变化
    void autoYRangeChanged(bool enabled);
    /// "全部数据"视图开关变化，便于界面同步下拉/按钮状态
    void fitAllChanged(bool enabled);
    /// 每 500 ms 汇报一次刷新帧率与平均单帧绘制耗时
    void frameStatsUpdated(double fps, double paint_ms);
    /// 通道显隐变化（含调用 setChannelVisible 引起的变化，便于界面同步）
    void channelVisibilityChanged(int channel, bool visible);
    /// 通道的线宽、点大小、线/点开关等外观变化
    void channelStyleChanged(int channel);
    /// 图例、网格、坐标轴的显隐发生变化
    void displayOptionsChanged();

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private slots:
    void onStatsTimeout();

private:
    /// 按当前视图范围重新聚合各通道显示数据，并更新自动量程
    void rebuildDisplayData();

    /**
     * @brief 用数据范围更新纵轴，带滞回迟滞
     *
     * 每帧无条件重算会让视窗内极值随样本进出而持续微调，整幅曲线和刻度文字
     * 一起跳动，视觉上就是"画面在闪"。这里只在数据越界或明显变小时才调整。
     */
    void updateAutoYRange(float data_lower, float data_upper);

    /// 切换"全部数据"视图状态，状态变化时发出 fitAllChanged
    void setFitAllInternal(bool enabled);

    /// 按当前字体重新计算坐标轴边距，适配高 DPI 缩放
    void updateMargins();

    void drawBackground(QPainter &painter) const;
    void drawAxes(QPainter &painter) const;
    void drawCurves(QPainter &painter) const;
    void drawCursors(QPainter &painter) const;
    void drawHover(QPainter &painter) const;
    void drawSelectionRect(QPainter &painter) const;
    void drawLegend(QPainter &painter) const;
    void drawEmptyHint(QPainter &painter) const;

    float mapXToPixel(float x, const QRectF &area) const;
    float mapYToPixel(float y, const QRectF &area) const;

    PlotDataBuffer buffer_;
    QVector<PlotChannelStyle> styles_;
    QVector<QVector<float>> display_xy_; ///< 各通道降采样后的交错 x,y

    /// 曲线画布：直接写像素比 QPainter 画折线快一个数量级，供 drawCurves 复用
    mutable QImage canvas_;

    /// 每帧复用的列像素缓存：按 x, y_min, y_max 交错存放
    mutable QVector<int> span_pixels_;
    /// 与 span_pixels_ 一一对应的列时间坐标，供缺口判断
    mutable QVector<float> span_data_x_;

    PlotRange x_range_;
    PlotRange y_range_ {-1.0F, 1.0F};

    float time_window_ {10.0F};
    float sample_interval_ {0.01F};
    float next_x_ {0.0F};

    bool paused_ {false};
    bool follow_latest_ {true};
    bool auto_y_range_ {true};
    bool fit_all_ {false}; ///< "全部数据"视图：每帧把横向范围对齐到缓冲区全跨度

    bool legend_visible_ {true}; ///< 图例显隐
    bool grid_visible_ {true};   ///< 网格显隐
    bool axes_visible_ {true};   ///< 坐标轴边框、刻度线、刻度文字显隐

    /// 垂直游标：只关心时间位置与是否激活
    struct Cursor
    {
        float x {0.0F};
        bool active {false};
    };

    Cursor cursor_a_;
    Cursor cursor_b_;

    /// 悬停读数位置（数据坐标）与开关，由调用者设置
    bool hover_active_ {false};
    float hover_x_ {0.0F};

    /// 框选矩形（控件坐标），由调用者设置
    QRectF selection_rect_;

    qreal margin_left_ {68.0};  ///< 按字体计算的左边距，高 DPI 下自适应
    qreal margin_bottom_ {40.0};

    QTimer *stats_timer_ {nullptr};
    QElapsedTimer stats_clock_;
    double paint_ms_ {0.0}; ///< 单帧绘制耗时的指数平滑值
    int frame_count_ {0};
};

}  // namespace uikit
