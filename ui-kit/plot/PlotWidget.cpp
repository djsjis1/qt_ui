/**
 * @file PlotWidget.cpp
 * @brief PlotWidget 的实现：视图状态、按像素列聚合、分块填充与坐标换算
 *
 * 单帧耗时分布（3 通道、1440p 实测约 12ms）：
 *   清空画布        ~0.5ms
 *   网格 + 坐标轴   ~2ms
 *   曲线像素        ~7ms   ← 主要开销，已改用分块填充与二分起点
 *   贴图            ~1.5ms
 *
 * 曲线的画法演进，改动前请先确认瓶颈仍在像素写入：
 *   1. QPainter 画折线：成本与线段总像素长度成正比，包络竖线能吃掉 50ms 以上。
 *   2. 逐列写 QImage 像素 + Bresenham 连线：降到 6~7ms。
 *   3. 分块填充（当前）：把跨行写改成块内写，减少缓存未命中。
 */

#include "PlotWidget.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>

#include <QFile>
#include <QFont>
#include <QFontMetrics>
#include <QPainter>
#include <QPaintEvent>
#include <QPen>
#include <QPointF>
#include <QRgb>
#include <QResizeEvent>
#include <QStringList>
#include <QTimer>

namespace uikit {
namespace {

constexpr int kMarginRight = 18;
constexpr int kMarginTop = 18;
constexpr int kGridColumns = 5;
constexpr int kGridRows = 5;
constexpr float kMinTimeWindow = 0.1F;
constexpr float kMinSampleInterval = 1e-6F;
constexpr int kStatsIntervalMs = 500;
constexpr int kGridDotStep = 6; ///< 网格点线间隔（像素）
constexpr int kDefaultLineWidth = 2;
constexpr int kMaxLineWidth = 8;
constexpr int kMaxPointSize = 16;
constexpr float kAutoRangePaddingRatio = 0.15F; ///< 自动量程留白比例
constexpr float kAutoRangeShrinkRatio = 0.5F;   ///< 数据不足当前范围一半时才收缩
constexpr int kEnvelopeWeight = 110;            ///< 包络竖线前景色权重（0~256）
constexpr float kColumnLogicalPixels = 2.0F;    ///< 每列占用的逻辑像素宽，与列数计算一致
constexpr double kGapColumnTolerance = 1.5;     ///< 相邻输出列时间差超过该列宽倍数视为丢包
constexpr int kSpanStride = 3;                  ///< span_pixels_ 中每列占的整数个数
constexpr int kTileSize = 64;                   ///< 分块边长（像素），块内数据可驻留 L1

const QColor kBackgroundColor(0x0F, 0x13, 0x17);
const QColor kCanvasColor(0x10, 0x15, 0x1A);
const QColor kAxisColor(0x5A, 0x6A, 0x78);
const QColor kGridColor(0x24, 0x2E, 0x38);
const QColor kTextColor(0x8F, 0xA0, 0xAE);
const QColor kMutedTextColor(0x4A, 0x55, 0x60);
const QColor kLegendBackgroundColor(0, 0, 0, 60);

const QColor kChannelColors[] = {
    QColor(0x00, 0xE5, 0x76),  // 绿
    QColor(0xFF, 0xD5, 0x4F),  // 黄
    QColor(0x40, 0xC4, 0xFF),  // 蓝
    QColor(0xFF, 0x6B, 0x6B),  // 红
    QColor(0xB3, 0x9D, 0xDB),  // 紫
    QColor(0x4D, 0xD0, 0xE1),  // 青
    QColor(0xF2, 0x93, 0x2F),  // 橙
    QColor(0x7E, 0xD3, 0x21),  // 黄绿
};
constexpr int kChannelColorCount = static_cast<int>(std::size(kChannelColors));

/// 按刻度步长决定小数位，避免标签出现一长串尾数
QString formatTickValue(float value, float step)
{
    int decimals = 0;
    if (step > 0.0F && step < 1.0F) {
        const double raw = std::ceil(-std::log10(static_cast<double>(step)));
        decimals = std::min(4, static_cast<int>(raw));
    }
    return QString::number(static_cast<double>(value), 'f', decimals);
}

/// 图例框与行高在绘制与命中测试之间共用，避免两处几何计算漂移
QRectF legendGeometry(const QRectF &area, const QFont &base_font,
                      const QVector<PlotChannelStyle> &styles, int *line_height_out)
{
    QFont legend_font = base_font;
    legend_font.setPointSizeF(8.5);
    const QFontMetrics metrics(legend_font);

    int text_width = 0;
    for (const PlotChannelStyle &style : styles) {
        text_width = std::max(text_width, metrics.horizontalAdvance(style.name));
    }

    const int line_height = metrics.height() + 2;
    if (line_height_out != nullptr) {
        *line_height_out = line_height;
    }

    const qreal box_width = static_cast<qreal>(text_width) + 34.0;
    const qreal box_height = static_cast<qreal>(line_height * static_cast<int>(styles.size())) + 8.0;
    return QRectF(area.right() - box_width - 10.0, area.top() + 8.0, box_width, box_height);
}

/// 在 QImage 上画指定宽度的直线（Bresenham，纯整数运算）。
/// 曲线每帧有数千段，QPainter 画同样的线慢一个数量级。
void plotLine(QImage &image, int x0, int y0, int x1, int y1, QRgb color, int width)
{
    const int image_width = image.width();
    const int image_height = image.height();

    const int dx = std::abs(x1 - x0);
    const int dy = -std::abs(y1 - y0);
    const int step_x = (x0 < x1) ? 1 : -1;
    const int step_y = (y0 < y1) ? 1 : -1;
    int error = dx + dy;

    for (;;) {
        for (int w = 0; w < width; ++w) {
            const int y = y0 + w;
            if (x0 >= 0 && x0 < image_width && y >= 0 && y < image_height) {
                reinterpret_cast<QRgb *>(image.scanLine(y))[x0] = color;
            }
        }
        if (x0 == x1 && y0 == y1) {
            break;
        }
        const int error2 = 2 * error;
        if (error2 >= dy) {
            error += dy;
            x0 += step_x;
        }
        if (error2 <= dx) {
            error += dx;
            y0 += step_y;
        }
    }
}

/// 把前景色按权重混入背景色。包络竖线用满色时，噪声区每列高度逐帧变化，
/// 视觉上像在闪；降低对比后密度感保留，闪烁明显减弱。
QRgb blendColor(QRgb foreground, QRgb background, int foreground_weight)
{
    const int weight = std::clamp(foreground_weight, 0, 255);
    const int inverse = 255 - weight;
    const int red = (((foreground >> 16) & 0xFF) * weight + ((background >> 16) & 0xFF) * inverse) / 255;
    const int green = (((foreground >> 8) & 0xFF) * weight + ((background >> 8) & 0xFF) * inverse) / 255;
    const int blue = ((foreground & 0xFF) * weight + (background & 0xFF) * inverse) / 255;
    return qRgb(red, green, blue);
}

/// 在 QImage 上画一个实心点（纯整数运算，逐像素直接写）
void plotDot(QImage &image, int center_x, int center_y, QRgb color, int size, PlotPointShape shape)
{
    if (size <= 0) {
        return;
    }

    const int radius = size / 2;
    const int circle_limit = radius * radius + radius; // 圆形边缘的整数近似
    for (int dy = -radius; dy <= radius; ++dy) {
        const int y = center_y + dy;
        if (y < 0 || y >= image.height()) {
            continue;
        }

        QRgb *row = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int dx = -radius; dx <= radius; ++dx) {
            const int x = center_x + dx;
            if (x < 0 || x >= image.width()) {
                continue;
            }

            bool filled = false;
            switch (shape) {
            case PlotPointShape::Square:
                filled = true;
                break;
            case PlotPointShape::Diamond:
                filled = (std::abs(dx) + std::abs(dy)) <= radius;
                break;
            case PlotPointShape::Circle:
            default:
                filled = (dx * dx + dy * dy) <= circle_limit;
                break;
            }
            if (filled) {
                row[x] = color;
            }
        }
    }
}

/// 按 1/2/5×10^n 取"整齐"的刻度步长
float niceStep(float span, int divisions)
{
    if (!(span > 0.0F) || divisions <= 0) {
        return 0.0F;
    }

    const float raw = span / static_cast<float>(divisions);
    const float magnitude = std::pow(10.0F, std::floor(std::log10(raw)));
    const float normalized = raw / magnitude;

    float nice = 10.0F;
    if (normalized <= 1.0F) {
        nice = 1.0F;
    } else if (normalized <= 2.0F) {
        nice = 2.0F;
    } else if (normalized <= 5.0F) {
        nice = 5.0F;
    }
    return nice * magnitude;
}

/// 在 [lower, upper] 内按固定步长列出刻度值
QVector<float> tickValues(float lower, float upper, float step)
{
    QVector<float> ticks;
    if (!(step > 0.0F) || !(upper > lower)) {
        return ticks;
    }

    // 起点对齐到步长的整数倍：滚动时标签只在跨过刻度点时整体移动一格，
    // 不会每帧改动数字，避免刻度文字"跳字"
    const double first = std::ceil(static_cast<double>(lower) / step - 1.0e-6) * step;
    for (double value = first; value <= static_cast<double>(upper) + step * 1.0e-3; value += step) {
        ticks.append(static_cast<float>(value));
    }
    return ticks;
}

/**
 * @brief 把包络竖线按分块方式写入画布
 *
 * 逐列跨行写时，每写一个像素地址就跳一整行，64 字节的缓存行只用到 4~8 字节；
 * 分块后同一块内的相邻列会反复命中同一批缓存行，显著减少访存停顿。
 * spans 按 x, y_min, y_max 交错存放，且按 x 升序（降采样按列号递增输出）。
 */
void fillSpansTiled(QImage &image, const QVector<int> &spans, QRgb color, int line_width)
{
    const int image_width = image.width();
    const int image_height = image.height();
    const int span_count = static_cast<int>(spans.size()) / kSpanStride;
    if (span_count <= 0 || image_width <= 0 || image_height <= 0) {
        return;
    }

    for (int tile_y = 0; tile_y < image_height; tile_y += kTileSize) {
        const int tile_y_end = std::min(tile_y + kTileSize, image_height);

        for (int tile_x = 0; tile_x < image_width; tile_x += kTileSize) {
            const int tile_x_end = std::min(tile_x + kTileSize, image_width);

            // 二分跳过本块左侧的列，避免每块都从第一列开始扫
            int lo = 0;
            int hi = span_count;
            while (lo < hi) {
                const int mid = lo + (hi - lo) / 2;
                if (spans[mid * kSpanStride] < tile_x) {
                    lo = mid + 1;
                } else {
                    hi = mid;
                }
            }

            for (int i = lo; i < span_count; ++i) {
                const int x = spans[i * kSpanStride];
                if (x >= tile_x_end) {
                    break;
                }

                const int y_begin = std::max(spans[i * kSpanStride + 1], tile_y);
                const int y_end = std::min(spans[i * kSpanStride + 2], tile_y_end - 1);
                for (int y = y_begin; y <= y_end; ++y) {
                    QRgb *row = reinterpret_cast<QRgb *>(image.scanLine(y));
                    for (int w = 0; w < line_width && x + w < image_width; ++w) {
                        row[x + w] = color;
                    }
                }
            }
        }
    }
}

}  // namespace

PlotWidget::PlotWidget(QWidget *parent)
    : QWidget(parent)
{
    setMinimumSize(320, 200);
    setAutoFillBackground(false);
    // 自绘控件铺满整个区域，声明不透明可省掉 Qt 每次重绘前的背景擦除
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setFocusPolicy(Qt::StrongFocus);

    x_range_.lower = next_x_ - time_window_;
    x_range_.upper = next_x_;

    updateMargins();

    // 帧率/耗时统计与绘制解耦，避免在 paintEvent 里发信号引起递归重绘
    stats_clock_.start();
    stats_timer_ = new QTimer(this);
    stats_timer_->setInterval(kStatsIntervalMs);
    connect(stats_timer_, &QTimer::timeout, this, &PlotWidget::onStatsTimeout);
    stats_timer_->start();
}

PlotWidget::~PlotWidget() = default;

// ---- 通道 ----

int PlotWidget::addChannel(const QString &name, const QColor &color)
{
    const int index = channelCount();

    PlotChannelStyle style;
    style.name = name.isEmpty() ? QStringLiteral("CH%1").arg(index + 1) : name;
    style.color = color.isValid() ? color : kChannelColors[index % kChannelColorCount];
    style.visible = true;
    style.line_width = kDefaultLineWidth;

    styles_.append(style);
    buffer_.setChannelCount(channelCount());
    display_xy_.append(QVector<float>());

    update();
    return index;
}

void PlotWidget::setChannelVisible(int channel, bool visible)
{
    if (channel < 0 || channel >= channelCount() || styles_[channel].visible == visible) {
        return;
    }
    styles_[channel].visible = visible;
    rebuildDisplayData();
    update();
    emit channelVisibilityChanged(channel, visible);
}

void PlotWidget::setAllChannelsVisible(bool visible)
{
    bool changed = false;
    for (PlotChannelStyle &style : styles_) {
        if (style.visible != visible) {
            style.visible = visible;
            changed = true;
        }
    }
    if (!changed) {
        return;
    }

    // 批量切换只重新聚合一次，避免逐通道触发 N 次全量降采样
    rebuildDisplayData();
    update();

    const int count = channelCount();
    for (int i = 0; i < count; ++i) {
        emit channelVisibilityChanged(i, visible);
    }
}

bool PlotWidget::isChannelVisible(int channel) const noexcept
{
    return (channel >= 0 && channel < channelCount()) ? styles_[channel].visible : false;
}

int PlotWidget::channelCount() const noexcept
{
    return static_cast<int>(styles_.size());
}

// ---- 曲线外观：线 / 点 ----

void PlotWidget::setChannelLineVisible(int channel, bool visible)
{
    if (channel < 0 || channel >= channelCount() || styles_[channel].line_visible == visible) {
        return;
    }
    styles_[channel].line_visible = visible;
    update();
    emit channelStyleChanged(channel);
}

void PlotWidget::setChannelPointVisible(int channel, bool visible)
{
    if (channel < 0 || channel >= channelCount() || styles_[channel].point_visible == visible) {
        return;
    }
    styles_[channel].point_visible = visible;
    update();
    emit channelStyleChanged(channel);
}

void PlotWidget::setChannelLineWidth(int channel, int physical_pixels)
{
    if (channel < 0 || channel >= channelCount()) {
        return;
    }
    const int width = std::clamp(physical_pixels, 1, kMaxLineWidth);
    if (styles_[channel].line_width == width) {
        return;
    }
    styles_[channel].line_width = width;
    update();
    emit channelStyleChanged(channel);
}

void PlotWidget::setChannelPointSize(int channel, int physical_pixels)
{
    if (channel < 0 || channel >= channelCount()) {
        return;
    }
    const int size = std::clamp(physical_pixels, 1, kMaxPointSize);
    if (styles_[channel].point_size == size) {
        return;
    }
    styles_[channel].point_size = size;
    update();
    emit channelStyleChanged(channel);
}

void PlotWidget::setChannelPointShape(int channel, PlotPointShape shape)
{
    if (channel < 0 || channel >= channelCount() || styles_[channel].point_shape == shape) {
        return;
    }
    styles_[channel].point_shape = shape;
    update();
    emit channelStyleChanged(channel);
}

bool PlotWidget::isChannelLineVisible(int channel) const noexcept
{
    return (channel >= 0 && channel < channelCount()) ? styles_[channel].line_visible : false;
}

bool PlotWidget::isChannelPointVisible(int channel) const noexcept
{
    return (channel >= 0 && channel < channelCount()) ? styles_[channel].point_visible : false;
}

int PlotWidget::channelLineWidth(int channel) const noexcept
{
    return (channel >= 0 && channel < channelCount()) ? styles_[channel].line_width
                                                      : kDefaultLineWidth;
}

int PlotWidget::channelPointSize(int channel) const noexcept
{
    return (channel >= 0 && channel < channelCount()) ? styles_[channel].point_size : 0;
}

PlotPointShape PlotWidget::channelPointShape(int channel) const noexcept
{
    return (channel >= 0 && channel < channelCount()) ? styles_[channel].point_shape
                                                      : PlotPointShape::Circle;
}

PlotChannelStyle PlotWidget::channelStyle(int channel) const
{
    if (channel < 0 || channel >= channelCount()) {
        return PlotChannelStyle();
    }
    return styles_[channel];
}

void PlotWidget::setCapacityPerChannel(int samples)
{
    buffer_ = PlotDataBuffer(samples);
    // 重建后要把采样周期带过去，序号换算与绘制时间轴都依赖它
    buffer_.setSampleInterval(sample_interval_);
    buffer_.setChannelCount(channelCount());

    display_xy_.clear();
    for (int i = 0; i < channelCount(); ++i) {
        display_xy_.append(QVector<float>());
    }
    update();
}

// ---- 数据 ----

void PlotWidget::appendFrame(const QVector<float> &values)
{
    const int count = std::min(static_cast<int>(values.size()), channelCount());
    for (int i = 0; i < count; ++i) {
        buffer_.append(i, next_x_, values[i]);
    }
    next_x_ += sample_interval_;

    if (!paused_) {
        // 不自己做节流：Qt 会把同一轮事件循环里的多次 update 合并成一次重绘，
        // 帧率由显示器刷新率决定
        update();
    }
}

void PlotWidget::appendValue(int channel, float value)
{
    if (channel < 0 || channel >= channelCount()) {
        return;
    }
    buffer_.append(channel, next_x_, value);
    next_x_ += sample_interval_;

    if (!paused_) {
        update();
    }
}

void PlotWidget::clearData()
{
    buffer_.clear();

    const int count = static_cast<int>(display_xy_.size());
    for (int i = 0; i < count; ++i) {
        display_xy_[i].clear();
    }

    next_x_ = 0.0F;
    x_range_.lower = -time_window_;
    x_range_.upper = 0.0F;
    update();
}

// ---- 显示设置 ----

void PlotWidget::setTimeWindow(float seconds)
{
    setFitAllInternal(false); // 指定时间窗即退出"全部数据"视图
    time_window_ = std::max(seconds, kMinTimeWindow);
    if (follow_latest_) {
        x_range_.lower = next_x_ - time_window_;
        x_range_.upper = next_x_;
    }
    // 未跟随时时间窗只影响恢复跟随后的范围，但显示数据仍需按当前视图重新聚合
    rebuildDisplayData();
    update();
}

void PlotWidget::setSampleInterval(float seconds)
{
    sample_interval_ = std::max(seconds, kMinSampleInterval);
    buffer_.setSampleInterval(sample_interval_);
}

bool PlotWidget::appendSample(int channel, quint64 seq, float y)
{
    if (channel < 0 || channel >= channelCount()) {
        return false;
    }

    if (!buffer_.appendBySeq(channel, seq, y)) {
        return false; // 序号重复/回退，调用方可以据此计数丢包或重传
    }

    // 跟随窗口按所有通道里最新的时刻滚动：appendSample 不走 next_x_ 累加，
    // 这里必须把全局"最新时刻"抬到该通道的最新样本
    next_x_ = std::max(next_x_, buffer_.lastSampleX(channel));

    if (!paused_) {
        update();
    }
    return true;
}

bool PlotWidget::hasChannelSequence(int channel) const noexcept
{
    return buffer_.hasSequence(channel);
}

quint64 PlotWidget::channelLastSequence(int channel) const noexcept
{
    return buffer_.lastSequence(channel);
}

void PlotWidget::setPaused(bool paused)
{
    if (paused_ == paused) {
        return;
    }
    paused_ = paused;

    if (!paused_) {
        update();
    }
    emit pausedChanged(paused_);
}

void PlotWidget::setFollowLatest(bool follow)
{
    if (follow_latest_ == follow) {
        return;
    }
    follow_latest_ = follow;

    if (follow_latest_) {
        if (fit_all_) {
            // 全览 + 跟随：范围由 paintEvent 每帧对齐缓冲全跨度，这里只需重绘
            update();
        } else {
            // 时间窗 + 跟随：回到配置的时间窗宽度滚动。
            // 手动缩放出的临时宽度不保留——下拉配置了什么就用什么
            x_range_.lower = next_x_ - time_window_;
            x_range_.upper = next_x_;
            rebuildDisplayData();
            update();
        }
    }
    // 取消跟随 = 冻结当前视口；宽度配置（时间窗/全部）保留不动，
    // 重新勾选跟随时按配置恢复
    emit followLatestChanged(follow_latest_);
}

void PlotWidget::setAutoYRange(bool enabled)
{
    if (auto_y_range_ == enabled) {
        return;
    }
    auto_y_range_ = enabled;
    if (auto_y_range_) {
        rebuildDisplayData();
        update();
    }
    emit autoYRangeChanged(auto_y_range_);
}

void PlotWidget::setXRange(float lower, float upper)
{
    if (!(upper > lower)) {
        return;
    }

    setFitAllInternal(false); // 手动改范围即退出"全部数据"视图
    follow_latest_ = false;
    x_range_.lower = lower;
    x_range_.upper = upper;

    rebuildDisplayData();
    update();
    emit followLatestChanged(false);
}

void PlotWidget::zoomToRange(float x_lower, float x_upper, float y_lower, float y_upper)
{
    if (!(x_upper > x_lower) || !(y_upper > y_lower)) {
        return;
    }

    setFitAllInternal(false);
    follow_latest_ = false;
    auto_y_range_ = false;
    x_range_.lower = x_lower;
    x_range_.upper = x_upper;
    y_range_.lower = y_lower;
    y_range_.upper = y_upper;

    rebuildDisplayData();
    update();

    emit followLatestChanged(false);
    emit autoYRangeChanged(false);
}

void PlotWidget::setFitAllInternal(bool enabled)
{
    if (fit_all_ == enabled) {
        return;
    }
    fit_all_ = enabled;
    emit fitAllChanged(enabled);
}

void PlotWidget::fitAllData()
{
    setFitAllInternal(true);
    follow_latest_ = true; // 全览本身随数据增长，勾选状态保持一致
    auto_y_range_ = true;

    // 立即应用一次，避免下一帧之前仍用旧范围
    float x_lower = 0.0F;
    float x_upper = 0.0F;
    if (buffer_.timeRange(x_lower, x_upper) && x_upper > x_lower) {
        x_range_.lower = x_lower;
        x_range_.upper = x_upper;
    }

    rebuildDisplayData();
    update();

    emit followLatestChanged(true);
    emit autoYRangeChanged(true);
}

void PlotWidget::setYRange(float lower, float upper)
{
    const bool was_auto = auto_y_range_;
    auto_y_range_ = false;
    y_range_.lower = std::min(lower, upper);
    y_range_.upper = std::max(lower, upper);
    if (y_range_.upper - y_range_.lower < 1e-6F) {
        y_range_.upper = y_range_.lower + 1.0F;
    }
    if (was_auto) {
        emit autoYRangeChanged(false);
    }
    update();
}

float PlotWidget::timeWindow() const noexcept
{
    return time_window_;
}

float PlotWidget::sampleInterval() const noexcept
{
    return sample_interval_;
}

bool PlotWidget::isPaused() const noexcept
{
    return paused_;
}

bool PlotWidget::isFollowLatest() const noexcept
{
    return follow_latest_;
}

bool PlotWidget::isAutoYRange() const noexcept
{
    return auto_y_range_;
}

bool PlotWidget::isFitAll() const noexcept
{
    return fit_all_;
}

PlotRange PlotWidget::xRange() const noexcept
{
    return x_range_;
}

PlotRange PlotWidget::yRange() const noexcept
{
    return y_range_;
}

// ---- 画面元素显隐 ----

void PlotWidget::setLegendVisible(bool visible)
{
    if (legend_visible_ == visible) {
        return;
    }
    legend_visible_ = visible;
    update();
    emit displayOptionsChanged();
}

void PlotWidget::setGridVisible(bool visible)
{
    if (grid_visible_ == visible) {
        return;
    }
    grid_visible_ = visible;
    update();
    emit displayOptionsChanged();
}

void PlotWidget::setAxesVisible(bool visible)
{
    if (axes_visible_ == visible) {
        return;
    }
    axes_visible_ = visible;
    update();
    emit displayOptionsChanged();
}

bool PlotWidget::isLegendVisible() const noexcept
{
    return legend_visible_;
}

bool PlotWidget::isGridVisible() const noexcept
{
    return grid_visible_;
}

bool PlotWidget::isAxesVisible() const noexcept
{
    return axes_visible_;
}

// ---- 刷新与绘制 ----

void PlotWidget::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);

    QElapsedTimer frame_timer;
    frame_timer.start();

    // 视图范围由"宽度配置 + 是否跟随"共同决定：
    //   全部 + 跟随 → X 对齐缓冲全跨度并随数据扩展
    //   时间窗 + 跟随 → X = 最新 N 秒滑动
    //   不跟随（含被冻结的全览）→ 保持当前范围不动
    if (fit_all_ && follow_latest_) {
        float range_lower = 0.0F;
        float range_upper = 0.0F;
        if (buffer_.timeRange(range_lower, range_upper) && range_upper > range_lower) {
            x_range_.lower = range_lower;
            x_range_.upper = range_upper;
        }
    } else if (follow_latest_) {
        x_range_.lower = next_x_ - time_window_;
        x_range_.upper = next_x_;
    }
    rebuildDisplayData();

    QPainter painter(this);
    drawBackground(painter);
    drawAxes(painter);
    drawCurves(painter);
    drawCursors(painter);
    drawHover(painter);
    drawSelectionRect(painter);
    drawLegend(painter);
    drawEmptyHint(painter);

    // 指数平滑，避免单帧抖动让读数乱跳
    const double elapsed_ms = static_cast<double>(frame_timer.nsecsElapsed()) / 1.0e6;
    paint_ms_ = (paint_ms_ <= 0.0) ? elapsed_ms : paint_ms_ * 0.9 + elapsed_ms * 0.1;
    ++frame_count_;
}

void PlotWidget::onStatsTimeout()
{
    const double elapsed_s = static_cast<double>(stats_clock_.restart()) / 1000.0;
    const double fps = (elapsed_s > 0.0) ? static_cast<double>(frame_count_) / elapsed_s : 0.0;
    frame_count_ = 0;
    emit frameStatsUpdated(fps, paint_ms_);
}

void PlotWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    // 高 DPI 或字体变化会改变刻度文字尺寸，边距要跟着重算
    updateMargins();
    // 绘图区宽度变化后每帧列数随之变化，按新宽度重新聚合一次
    rebuildDisplayData();
}

void PlotWidget::rebuildDisplayData()
{
    const int count = channelCount();
    // 每两像素一列：视觉上与逐像素聚合几乎无差别，绘制量减半
    const int columns =
        std::max(1, static_cast<int>(plotArea().width()) / static_cast<int>(kColumnLogicalPixels));

    if (static_cast<int>(display_xy_.size()) != count) {
        display_xy_.clear();
        for (int i = 0; i < count; ++i) {
            display_xy_.append(QVector<float>());
        }
    }

    float y_lower = std::numeric_limits<float>::max();
    float y_upper = std::numeric_limits<float>::lowest();
    bool has_data = false;

    for (int i = 0; i < count; ++i) {
        QVector<float> &xy = display_xy_[i];
        if (!styles_[i].visible) {
            xy.clear();
            continue;
        }

        buffer_.downsample(i, x_range_.lower, x_range_.upper, columns, xy);

        const int point_values = static_cast<int>(xy.size());
        for (int k = 1; k < point_values; k += 2) {
            y_lower = std::min(y_lower, xy[k]);
            y_upper = std::max(y_upper, xy[k]);
            has_data = true;
        }
    }

    if (auto_y_range_ && has_data) {
        updateAutoYRange(y_lower, y_upper);
    }
}

void PlotWidget::updateAutoYRange(float data_lower, float data_upper)
{
    const float data_span = data_upper - data_lower;

    // 数据仍在当前范围内、且没有明显缩小时保持范围不动。
    // 每帧无条件重算会让极值随样本进出视窗持续微调，整幅曲线和刻度文字一起跳，
    // 视觉上就是"画面在闪"
    if (y_range_.isValid()) {
        const float current_span = y_range_.span();
        const bool inside = (data_lower >= y_range_.lower) && (data_upper <= y_range_.upper);
        if (inside && data_span >= current_span * kAutoRangeShrinkRatio) {
            return;
        }
    }

    const float padding = (data_span > 0.0F) ? data_span * kAutoRangePaddingRatio : 1.0F;
    y_range_.lower = data_lower - padding;
    y_range_.upper = data_upper + padding;
}

void PlotWidget::drawBackground(QPainter &painter) const
{
    // 绘图区由曲线画布整体覆盖，这里只铺一次整窗背景，省掉一次全区域填充
    painter.fillRect(rect(), kBackgroundColor);
}

void PlotWidget::drawAxes(QPainter &painter) const
{
    if (!axes_visible_) {
        return;
    }

    const QRectF area = plotArea();

    painter.setPen(QPen(kAxisColor));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(area);

    QFont tick_font = font();
    tick_font.setPointSizeF(8.0);
    painter.setFont(tick_font);
    const QFontMetrics metrics = painter.fontMetrics();

    // 横轴刻度按 1/2/5×10^n 的整齐步长取值：滚动时标签只在跨过刻度点时整体移动，
    // 不会每帧改动数字，避免刻度文字"跳字"
    const float x_step = niceStep(x_range_.span(), kGridColumns);
    for (float value : tickValues(x_range_.lower, x_range_.upper, x_step)) {
        const qreal x = mapXToPixel(value, area);
        if (x < area.left() - 1.0 || x > area.right() + 1.0) {
            continue;
        }

        painter.setPen(QPen(kAxisColor));
        painter.drawLine(QPointF(x, area.bottom()), QPointF(x, area.bottom() + 4.0));

        const QString text = formatTickValue(value, x_step);
        const int text_width = metrics.horizontalAdvance(text);
        // 标签压在纵轴或右侧边框上就不画，避免与坐标轴重叠
        if (x - text_width * 0.5 < 2.0 || x + text_width * 0.5 > width() - 2.0) {
            continue;
        }
        painter.setPen(QPen(kTextColor));
        painter.drawText(QPointF(x - text_width * 0.5, area.bottom() + 18.0), text);
    }

    // 纵轴刻度同样按整齐步长，范围稳定时标签内容也随之稳定
    const float y_step = niceStep(y_range_.span(), kGridRows);
    for (float value : tickValues(y_range_.lower, y_range_.upper, y_step)) {
        const qreal y = mapYToPixel(value, area);
        if (y < area.top() - 1.0 || y > area.bottom() + 1.0) {
            continue;
        }

        painter.setPen(QPen(kAxisColor));
        painter.drawLine(QPointF(area.left() - 4.0, y), QPointF(area.left(), y));

        const QString text = formatTickValue(value, y_step);
        painter.setPen(QPen(kTextColor));
        painter.drawText(QPointF(area.left() - 8.0 - metrics.horizontalAdvance(text),
                                 y + metrics.ascent() * 0.5 - 1.0),
                         text);
    }
}

/**
 * @brief 把各通道的聚合结果画进离屏画布
 *
 * 分三个阶段，目的是让"坐标换算"和"像素写入"各自连续执行：
 *   ① 换算：遍历一次数据，把 (时间, min, max) 变成像素列缓存
 *   ② 填充：按 64×64 分块写入包络竖线，避免逐列跨行写的缓存未命中
 *   ③ 连线/画点：相邻列之间 Bresenham，数据缺口处断开
 *
 * 为什么不用 QPainter 直接画折线：软件光栅的成本与线段总像素长度成正比，
 * 包络竖线累计可达百万像素，一帧就要 50ms 以上。
 */
void PlotWidget::drawCurves(QPainter &painter) const
{
    const QRectF area = plotArea();
    const qreal dpr = devicePixelRatioF();
    const int pixel_width = static_cast<int>(std::lround(area.width() * dpr));
    const int pixel_height = static_cast<int>(std::lround(area.height() * dpr));
    if (pixel_width <= 0 || pixel_height <= 0) {
        return;
    }

    // 画布按物理像素建并标注 DPR，贴图时 1:1 不插值。若按逻辑像素建，Qt 会放大插值，
    // 滚动时线条发糊、发闪
    if (canvas_.size() != QSize(pixel_width, pixel_height)) {
        canvas_ = QImage(pixel_width, pixel_height, QImage::Format_ARGB32_Premultiplied);
        canvas_.setDevicePixelRatio(dpr);
    }
    canvas_.fill(kCanvasColor);

    // 网格与坐标轴刻度共用同一套刻度值，保证网格线正好落在刻度上
    const QRgb grid_color = kGridColor.rgb();
    if (grid_visible_) {
        const float grid_step_x = niceStep(x_range_.span(), kGridColumns);
        for (float value : tickValues(x_range_.lower, x_range_.upper, grid_step_x)) {
            const int x =
                static_cast<int>(std::lround((mapXToPixel(value, area) - area.left()) * dpr));
            if (x <= 0 || x >= pixel_width - 1) {
                continue;
            }
            for (int y = 0; y < pixel_height; y += kGridDotStep) {
                reinterpret_cast<QRgb *>(canvas_.scanLine(y))[x] = grid_color;
            }
        }

        const float grid_step_y = niceStep(y_range_.span(), kGridRows);
        for (float value : tickValues(y_range_.lower, y_range_.upper, grid_step_y)) {
            const int y =
                static_cast<int>(std::lround((mapYToPixel(value, area) - area.top()) * dpr));
            if (y <= 0 || y >= pixel_height - 1) {
                continue;
            }
            QRgb *row = reinterpret_cast<QRgb *>(canvas_.scanLine(y));
            for (int x = 0; x < pixel_width; x += kGridDotStep) {
                row[x] = grid_color;
            }
        }
    }

    // 只有一列内本应容纳多个样本时，出现空列才说明丢包；采样率低于列密度时
    // 空列只是采样稀疏，此时不能断线
    const int display_columns =
        std::max(1, static_cast<int>(area.width()) / static_cast<int>(kColumnLogicalPixels));
    const double column_width = static_cast<double>(x_range_.span()) / display_columns;
    const bool gap_check_enabled = static_cast<double>(sample_interval_) < column_width;

    // 直接写像素 + Bresenham 连线：QPainter 画包络折线时线段总长接近百万像素，
    // 软件光栅只有约 20Mpx/s，一帧要 50ms 以上
    const QRgb canvas_color = kCanvasColor.rgb();
    const int count = channelCount();
    for (int i = 0; i < count; ++i) {
        if (!styles_[i].visible) {
            continue;
        }

        const PlotChannelStyle &style = styles_[i];
        const bool draw_line = style.line_visible;
        const bool draw_point = style.point_visible && style.point_size > 0;
        if (!draw_line && !draw_point) {
            continue;
        }

        const QVector<float> &xy = display_xy_[i];
        const int n = static_cast<int>(xy.size());
        if (n < 4) {
            continue;
        }

        const QRgb color = style.color.rgb();
        // 包络竖线用混色：整列铺满前景色时，噪声区每列高度逐帧变化，视觉上像在闪
        const QRgb envelope_color = blendColor(color, canvas_color, kEnvelopeWeight);
        const int line_width = std::clamp(style.line_width, 1, kMaxLineWidth);
        const int point_size = std::clamp(style.point_size, 1, kMaxPointSize);
        const PlotPointShape point_shape = style.point_shape;

        // 第一阶段：把数据坐标一次性换算成像素列，缓存供填充与连线共用
        span_pixels_.clear();
        span_data_x_.clear();
        span_pixels_.reserve((n / 4) * kSpanStride);
        span_data_x_.reserve(n / 4);
        for (int k = 0; k + 3 < n; k += 4) {
            const int x = std::clamp(
                static_cast<int>(std::lround((mapXToPixel(xy[k], area) - area.left()) * dpr)), 0,
                pixel_width - 1);
            const float y_a = (mapYToPixel(xy[k + 1], area) - static_cast<float>(area.top())) * dpr;
            const float y_b = (mapYToPixel(xy[k + 3], area) - static_cast<float>(area.top())) * dpr;

            span_pixels_.append(x);
            span_pixels_.append(
                std::clamp(static_cast<int>(std::min(y_a, y_b)), 0, pixel_height - 1));
            span_pixels_.append(
                std::clamp(static_cast<int>(std::max(y_a, y_b)), 0, pixel_height - 1));
            span_data_x_.append(xy[k]);
        }

        // 第二阶段：包络竖线按分块方式写入，减少逐列跨行写造成的缓存未命中
        if (draw_line) {
            fillSpansTiled(canvas_, span_pixels_, envelope_color, line_width);
        }

        // 第三阶段：相邻列连线（缺口处断开）与采样点
        const int span_count = static_cast<int>(span_data_x_.size());
        for (int s = 0; s < span_count; ++s) {
            const int x = span_pixels_[s * kSpanStride];
            const int y_min = span_pixels_[s * kSpanStride + 1];
            const int y_max = span_pixels_[s * kSpanStride + 2];

            if (draw_line && s > 0) {
                // 缺口处不连线，让曲线断开
                const float data_gap = span_data_x_[s] - span_data_x_[s - 1];
                const bool continuous =
                    !gap_check_enabled
                    || static_cast<double>(data_gap) <= column_width * kGapColumnTolerance;
                if (continuous) {
                    // 上下包络分别连接相邻列，保证是连续的线而不是离散的点
                    const int previous_x = span_pixels_[(s - 1) * kSpanStride];
                    plotLine(canvas_, previous_x, span_pixels_[(s - 1) * kSpanStride + 1], x, y_min,
                             color, line_width);
                    plotLine(canvas_, previous_x, span_pixels_[(s - 1) * kSpanStride + 2], x, y_max,
                             color, line_width);
                }
            }

            if (draw_point) {
                // 每列一个点，取该列聚合结果的极值中点：列宽只有两像素，等同该列代表值
                plotDot(canvas_, x, (y_min + y_max) / 2, color, point_size, point_shape);
            }
        }
    }

    // 整数位置贴图并关闭平滑：贴在半像素上会被重采样，滚动时线条相位反复变化
    painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
    painter.drawImage(area.topLeft().toPoint(), canvas_);
}

void PlotWidget::drawCursors(QPainter &painter) const
{
    if (!cursor_a_.active) {
        return;
    }

    const QRectF area = plotArea();
    QFont cursor_font = font();
    cursor_font.setPointSizeF(8.0);
    painter.setFont(cursor_font);
    const QFontMetrics metrics = painter.fontMetrics();

    const QColor color_a(0xFF, 0x9F, 0x43);
    const QColor color_b(0x9F, 0x7B, 0xFF);

    painter.save();
    painter.setClipRect(area);

    const auto draw_cursor_line = [&](const Cursor &cursor, const QColor &color,
                                      const QString &label) {
        if (!cursor.active) {
            return;
        }
        const qreal x = mapXToPixel(cursor.x, area);
        painter.setPen(QPen(color, 1.0, Qt::DashLine));
        painter.drawLine(QPointF(x, area.top()), QPointF(x, area.bottom()));
        painter.drawText(QPointF(x + 3.0, area.bottom() - 6.0), label);
    };
    draw_cursor_line(cursor_a_, color_a, QStringLiteral("A"));
    draw_cursor_line(cursor_b_, color_b, QStringLiteral("B"));

    // 读数框固定在左上角，避免跟着游标乱跑
    QStringList lines;
    lines << QStringLiteral("A  %1 s").arg(static_cast<double>(cursor_a_.x), 0, 'f', 3);
    if (cursor_b_.active) {
        lines << QStringLiteral("B  %1 s").arg(static_cast<double>(cursor_b_.x), 0, 'f', 3);
        lines << QStringLiteral("ΔT %1 s")
                     .arg(static_cast<double>(cursor_b_.x - cursor_a_.x), 0, 'f', 3);

        // ΔV 只取第一个可见通道，避免读数框无限变宽
        for (int i = 0; i < channelCount(); ++i) {
            if (!styles_[i].visible) {
                continue;
            }
            float y_a = 0.0F;
            float y_b = 0.0F;
            if (buffer_.valueAt(i, cursor_a_.x, y_a) && buffer_.valueAt(i, cursor_b_.x, y_b)) {
                lines << QStringLiteral("ΔV(%1) %2")
                             .arg(styles_[i].name)
                             .arg(static_cast<double>(y_b - y_a), 0, 'f', 3);
            }
            break;
        }
    }

    int text_width = 0;
    for (const QString &line : lines) {
        text_width = std::max(text_width, metrics.horizontalAdvance(line));
    }
    const int line_height = metrics.height() + 1;
    const QRectF box(area.left() + 8.0, area.top() + 8.0,
                     static_cast<qreal>(text_width) + 14.0,
                     static_cast<qreal>(line_height * static_cast<int>(lines.size())) + 10.0);

    painter.setPen(QPen(kGridColor));
    painter.setBrush(kLegendBackgroundColor);
    painter.drawRoundedRect(box, 3.0, 3.0);

    painter.setPen(QPen(kTextColor));
    for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
        painter.drawText(
            QPointF(box.left() + 7.0, box.top() + 6.0 + line_height * i + metrics.ascent()),
            lines[i]);
    }

    painter.restore();
}

void PlotWidget::drawHover(QPainter &painter) const
{
    if (!hover_active_) {
        return;
    }
    const QRectF area = plotArea();
    const qreal hover_x = mapXToPixel(hover_x_, area);
    if (hover_x < area.left() || hover_x > area.right()) {
        return;
    }

    QFont hover_font = font();
    hover_font.setPointSizeF(8.0);
    painter.setFont(hover_font);

    painter.setPen(QPen(kMutedTextColor, 1.0, Qt::DotLine));
    painter.drawLine(QPointF(hover_x, area.top()), QPointF(hover_x, area.bottom()));

    // 读数从已聚合的显示数据里取最近列，成本 O(列数)，不需要扫原始缓冲
    QStringList lines;
    for (int i = 0; i < channelCount(); ++i) {
        if (!styles_[i].visible) {
            continue;
        }
        const QVector<float> &xy = display_xy_[i];
        const int n = static_cast<int>(xy.size());
        if (n < 4) {
            continue;
        }

        int best = -1;
        float best_distance = 0.0F;
        for (int k = 0; k + 3 < n; k += 4) {
            const float distance = std::fabs(mapXToPixel(xy[k], area) - static_cast<float>(hover_x));
            if (best < 0 || distance < best_distance) {
                best = k;
                best_distance = distance;
            }
        }
        if (best < 0) {
            continue;
        }

        const float value = xy[best + 3];
        lines << QStringLiteral("%1  %2")
                     .arg(styles_[i].name)
                     .arg(static_cast<double>(value), 0, 'f', 3);

        painter.setPen(Qt::NoPen);
        painter.setBrush(styles_[i].color);
        painter.drawEllipse(QPointF(hover_x, mapYToPixel(value, area)), 3.0, 3.0);
    }

    if (lines.isEmpty()) {
        return;
    }

    const QFontMetrics metrics = painter.fontMetrics();
    int text_width = 0;
    for (const QString &line : lines) {
        text_width = std::max(text_width, metrics.horizontalAdvance(line));
    }
    const int line_height = metrics.height() + 1;

    // 靠近右边界时把读数框翻到鼠标左侧，避免被裁掉
    qreal box_x = hover_x + 14.0;
    if (box_x + text_width + 14.0 > area.right()) {
        box_x = hover_x - text_width - 20.0;
    }
    const QRectF box(box_x, area.top() + 8.0, static_cast<qreal>(text_width) + 14.0,
                     static_cast<qreal>(line_height * static_cast<int>(lines.size())) + 10.0);

    painter.setPen(QPen(kGridColor));
    painter.setBrush(kLegendBackgroundColor);
    painter.drawRoundedRect(box, 3.0, 3.0);

    painter.setPen(QPen(kTextColor));
    for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
        painter.drawText(
            QPointF(box.left() + 7.0, box.top() + 6.0 + line_height * i + metrics.ascent()),
            lines[i]);
    }
}

void PlotWidget::drawSelectionRect(QPainter &painter) const
{
    if (selection_rect_.isEmpty()) {
        return;
    }

    painter.setPen(QPen(QColor(0x40, 0xC4, 0xFF), 1.0, Qt::DashLine));
    painter.setBrush(QColor(0x40, 0xC4, 0xFF, 40));
    painter.drawRect(selection_rect_);
}

void PlotWidget::drawLegend(QPainter &painter) const
{
    const int count = channelCount();
    if (!legend_visible_ || count == 0) {
        return;
    }

    QFont legend_font = font();
    legend_font.setPointSizeF(8.5);
    painter.setFont(legend_font);
    const QFontMetrics metrics = painter.fontMetrics();

    int line_height = 0;
    const QRectF box = legendGeometry(plotArea(), font(), styles_, &line_height);

    painter.setPen(QPen(kGridColor));
    painter.setBrush(kLegendBackgroundColor);
    painter.drawRoundedRect(box, 3.0, 3.0);

    for (int i = 0; i < count; ++i) {
        const PlotChannelStyle &style = styles_[i];
        const qreal center_y = box.top() + 4.0 + line_height * i + line_height * 0.5;
        const qreal marker_left = box.left() + 8.0;
        const qreal marker_right = box.left() + 20.0;

        // 图例标示与曲线的线/点配置保持一致
        painter.setPen(QPen(style.color, std::max(1, style.line_width)));
        if (style.line_visible) {
            painter.drawLine(QPointF(marker_left, center_y), QPointF(marker_right, center_y));
        }

        if (style.point_visible && style.point_size > 0) {
            const qreal radius = std::min<qreal>(3.0, style.point_size * 0.5);
            painter.setPen(Qt::NoPen);
            painter.setBrush(style.color);
            painter.drawEllipse(QPointF((marker_left + marker_right) * 0.5, center_y), radius,
                                radius);
            painter.setBrush(Qt::NoBrush);
        }

        painter.setPen(QPen(style.visible ? kTextColor : kMutedTextColor));
        painter.drawText(QPointF(box.left() + 26.0, center_y + metrics.ascent() * 0.5 - 1.0),
                         style.name);
    }
}

void PlotWidget::drawEmptyHint(QPainter &painter) const
{
    const int count = static_cast<int>(display_xy_.size());
    for (int i = 0; i < count; ++i) {
        if (!display_xy_[i].isEmpty()) {
            return;
        }
    }

    painter.setPen(QPen(kMutedTextColor));
    painter.drawText(plotArea(), Qt::AlignCenter, QStringLiteral("等待数据…"));
}

// ---- 导出 ----

bool PlotWidget::exportVisibleCsv(const QString &file_path) const
{
    if (channelCount() == 0 || file_path.isEmpty()) {
        return false;
    }

    QFile file(file_path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }

    QString header = QStringLiteral("time_s");
    for (const PlotChannelStyle &style : styles_) {
        header += QStringLiteral(",") + style.name;
    }
    file.write(header.toUtf8());
    file.write("\n");

    // appendFrame 让各通道共享同一时间轴，按序号同步取样即可
    const int count = buffer_.sampleCount(0);
    for (int i = 0; i < count; ++i) {
        float x = 0.0F;
        float first_y = 0.0F;
        if (!buffer_.sampleByIndex(0, i, x, first_y)) {
            continue;
        }
        Q_UNUSED(first_y);
        if (x < x_range_.lower || x > x_range_.upper) {
            continue;
        }

        QByteArray line = QByteArray::number(static_cast<double>(x), 'f', 6);
        for (int channel = 0; channel < channelCount(); ++channel) {
            float channel_x = 0.0F;
            float channel_y = 0.0F;
            line += ',';
            if (buffer_.sampleByIndex(channel, i, channel_x, channel_y)) {
                line += QByteArray::number(static_cast<double>(channel_y), 'f', 6);
            }
        }
        file.write(line);
        file.write("\n");
    }
    return true;
}

QPixmap PlotWidget::grabPlot()
{
    return grab();
}

// ---- 坐标变换 ----

QRectF PlotWidget::plotArea() const
{
    // 边距取整：绘图区落在整数逻辑像素上，贴图时不会被推到半像素位置反复重采样
    const int left = static_cast<int>(std::lround(margin_left_));
    const int bottom = static_cast<int>(std::lround(margin_bottom_));
    const int available_width = std::max(1, width() - left - kMarginRight);
    const int available_height = std::max(1, height() - kMarginTop - bottom);
    return QRectF(static_cast<qreal>(left), static_cast<qreal>(kMarginTop),
                  static_cast<qreal>(available_width), static_cast<qreal>(available_height));
}

void PlotWidget::updateMargins()
{
    QFont tick_font = font();
    tick_font.setPointSizeF(8.0);
    const QFontMetrics metrics(tick_font);

    // 左边要容下最宽的刻度标签（负号 + 小数位），底部要容下刻度文字
    margin_left_ = static_cast<qreal>(metrics.horizontalAdvance(QStringLiteral("-000.00"))) + 16.0;
    margin_bottom_ = static_cast<qreal>(metrics.height()) + 16.0;
}

float PlotWidget::mapXToPixel(float x, const QRectF &area) const
{
    const float span = x_range_.span();
    if (span <= 0.0F) {
        return static_cast<float>(area.left());
    }
    const float ratio = (x - x_range_.lower) / span;
    return static_cast<float>(area.left()) + static_cast<float>(area.width()) * ratio;
}

float PlotWidget::mapYToPixel(float y, const QRectF &area) const
{
    const float span = y_range_.span();
    if (span <= 0.0F) {
        return static_cast<float>(area.bottom());
    }
    const float ratio = (y - y_range_.lower) / span;
    return static_cast<float>(area.bottom()) - static_cast<float>(area.height()) * ratio;
}

// ---- 游标与标记（由调用者驱动）----

void PlotWidget::setCursor(int index, float x)
{
    Cursor &cursor = (index == 0) ? cursor_a_ : cursor_b_;
    cursor.x = x;
    cursor.active = true;
    update();
}

void PlotWidget::clearCursors()
{
    if (!cursor_a_.active && !cursor_b_.active) {
        return;
    }
    cursor_a_ = Cursor();
    cursor_b_ = Cursor();
    update();
}

bool PlotWidget::hasCursor(int index) const noexcept
{
    return (index == 0) ? cursor_a_.active : cursor_b_.active;
}

float PlotWidget::cursorX(int index) const noexcept
{
    return (index == 0) ? cursor_a_.x : cursor_b_.x;
}

void PlotWidget::setHoverX(float x)
{
    if (std::isfinite(x)) {
        hover_x_ = x;
        hover_active_ = true;
    } else {
        hover_active_ = false;
    }
    update();
}

void PlotWidget::clearHover()
{
    if (!hover_active_) {
        return;
    }
    hover_active_ = false;
    update();
}

void PlotWidget::setSelectionRect(const QRectF &rect)
{
    if (rect.isEmpty()) {
        clearSelectionRect();
        return;
    }
    selection_rect_ = rect.normalized();
    update();
}

void PlotWidget::clearSelectionRect()
{
    if (selection_rect_.isEmpty()) {
        return;
    }
    selection_rect_ = QRectF();
    update();
}

// ---- 坐标换算 ----

float PlotWidget::pixelToDataX(float pixel) const
{
    const QRectF area = plotArea();
    const float width_pixel = static_cast<float>(area.width());
    if (width_pixel <= 0.0F) {
        return x_range_.lower;
    }
    const float ratio = (pixel - static_cast<float>(area.left())) / width_pixel;
    return x_range_.lower + x_range_.span() * ratio;
}

float PlotWidget::pixelToDataY(float pixel) const
{
    const QRectF area = plotArea();
    const float height_pixel = static_cast<float>(area.height());
    if (height_pixel <= 0.0F) {
        return y_range_.lower;
    }
    const float ratio = (static_cast<float>(area.bottom()) - pixel) / height_pixel;
    return y_range_.lower + y_range_.span() * ratio;
}

QPointF PlotWidget::dataToPixel(float x, float y) const
{
    const QRectF area = plotArea();
    return QPointF(mapXToPixel(x, area), mapYToPixel(y, area));
}

}  // namespace uikit
