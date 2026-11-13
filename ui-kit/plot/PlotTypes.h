/**
 * @file PlotTypes.h
 * @brief 曲线组件的公共值类型：数值区间、采样点形状与通道显示配置
 *
 * 只放不依赖 QWidget 的轻量值类型，供数据层与绘制层共用。
 */

#pragma once

#include <cstdint>

#include <QColor>
#include <QString>

namespace uikit {

/**
 * @brief 一维数值区间 [lower, upper]
 */
struct PlotRange
{
    float lower {0.0F};
    float upper {1.0F};

    /// 区间是否有有效跨度
    bool isValid() const noexcept { return upper > lower; }
    /// 区间跨度
    float span() const noexcept { return upper - lower; }
    /// 区间中点
    float center() const noexcept { return (lower + upper) * 0.5F; }
};

/**
 * @brief 采样点的绘制形状
 */
enum class PlotPointShape : int32_t
{
    Circle = 0,  ///< 圆点
    Square = 1,  ///< 方点
    Diamond = 2, ///< 菱形
};

/**
 * @brief 单个通道的显示配置
 *
 * 线、点是两个独立开关：都关闭时该通道只剩图例，不产生任何像素。
 */
struct PlotChannelStyle
{
    QString name;              ///< 图例中显示的名称
    QColor color {Qt::white};  ///< 曲线颜色
    bool visible {true};       ///< 是否参与绘制

    // ---- 线 / 点控制 ----

    bool line_visible {true};   ///< 是否绘制连线（含包络竖线）
    bool point_visible {false}; ///< 是否绘制采样点
    int line_width {2};         ///< 线宽，物理像素，1~8
    int point_size {3};         ///< 点直径，物理像素，1~16
    PlotPointShape point_shape {PlotPointShape::Circle}; ///< 点形状
};

}  // namespace uikit
