/**
 * @file ParamTableTypes.h
 * @brief 参数表格的公共值类型：一行参数与一列数据源的定义
 *
 * 只放不依赖 QWidget 的轻量值类型，供控件与调用者共用。
 * 表格形态：列 0 固定为参数名，之后每列对应一个注册的数据源（如 X 轴、Y 轴）。
 */

#pragma once

#include <QString>

namespace uikit {

/// 未显式设置范围时使用的默认上下界；表格据此判断是否展示"允许范围"提示
inline constexpr double kDefaultParamMinimum = -1.0e12;
inline constexpr double kDefaultParamMaximum = 1.0e12;

/**
 * @brief 一列数据源的定义（如 X 轴、Y 轴）
 *
 * id 是读写与编辑回调使用的唯一键，name 是列标题。
 */
struct ParamColumn
{
    QString id;            ///< 唯一键，读写与编辑回调都用它
    QString name;          ///< 列标题，留空时显示 id
    bool visible {true};   ///< 初始是否显示
};

/**
 * @brief 一行参数的定义
 *
 * 范围与精度作用于该行在所有数据列上的单元格。
 */
struct ParamRow
{
    QString id;                               ///< 唯一键，用于回填与编辑回调
    QString name;                             ///< 表格显示名称，留空时显示 id
    QString unit;                             ///< 单位，非空时显示在参数名后
    double minimum {kDefaultParamMinimum};    ///< 允许输入的下界
    double maximum {kDefaultParamMaximum};    ///< 允许输入的上界
    int decimals {3};                         ///< 显示与校验的十进制位数（0~9）
    bool editable {true};                     ///< 是否允许双击编辑
};

}  // namespace uikit
