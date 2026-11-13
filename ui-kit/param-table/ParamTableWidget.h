/**
 * @file ParamTableWidget.h
 * @brief 参数表格控件：行=参数、列=数据源（X/Y/Z 轴），列可显隐，双击编辑
 *
 * 组件只做四件事：展示行列矩阵、按 pollInterval 发出 pollRequested()、按
 * setColumnVisible() 隐藏/显示数据列、把用户双击编辑的结果校验后发出 valueEdited()。
 * 读取底层、调用底层写函数、勾选框、错误提示全部由调用者实现。
 *
 * 关键接口：
 *   addColumn(id, name)                  → 注册一列数据源（如 X 轴），返回列号
 *   addParam(row)                        → 注册一行参数（名称/单位/范围/精度）
 *   setValue(rowId, columnId, value)     → 回填轮询读到的值，编辑中的单元格不覆盖
 *   setColumnVisible(columnId, visible)  → 控制某列是否显示（勾选框由调用者做）
 *   pollRequested()                      → 每 pollInterval 毫秒一次，调用者去读底层
 *   valueEdited(rowId, columnId, value)  → 双击提交并通过校验，调用者据此写底层
 *   editRejected(rowId, columnId, text)  → 输入非法或越界，控件已恢复原值
 *
 * 典型接线（调用者侧）：
 *   connect(table, &ParamTableWidget::pollRequested, this, [&]() {
 *       for (const auto &axis : axes)
 *           for (const auto &p : params)
 *               table->setValue(p.id, axis.id, device.read(axis.id, p.id));
 *   });
 *   connect(table, &ParamTableWidget::valueEdited, this,
 *           [&](const QString &row, const QString &col, double v) { device.write(col, row, v); });
 *
 * 线程约束：非线程安全，只允许在 GUI 线程调用。
 * 依赖：Qt Widgets（Qt 5.14.2 / Qt 6.x 通用）。
 */

#pragma once

#include <QHash>
#include <QString>
#include <QTableWidget>
#include <QVector>

#include "ParamTableTypes.h"

class QTimer;

namespace uikit {

/**
 * @brief 参数矩阵表格控件（纯展示 + 轮询节奏 + 编辑校验，数据源归调用者）
 */
class ParamTableWidget : public QTableWidget
{
    Q_OBJECT

public:
    explicit ParamTableWidget(QWidget *parent = nullptr);
    ~ParamTableWidget() override;

    // ---- 列（数据源）----

    /**
     * @brief 追加一列数据源，列标题显示在表头
     * @return 数据列序号（0 起，不含参数名列），id 非法或重复时返回 -1
     */
    int addColumn(const ParamColumn &column);

    /// 追加一列数据源，默认可见
    int addColumn(const QString &id, const QString &name);

    /// 移除全部数据列，保留参数行（各行数值清零）
    void clearColumns();

    /// 数据列数量（不含参数名列）
    int dataColumnCount() const noexcept;

    /// 按 id 查数据列序号，不存在时返回 -1
    int columnIndexOf(const QString &id) const noexcept;

    /// 显示/隐藏某数据列；显隐变化发出 columnVisibilityChanged
    void setColumnVisible(const QString &id, bool visible);

    /// 某数据列是否显示
    bool isColumnVisible(const QString &id) const noexcept;

    // ---- 行（参数）----

    /**
     * @brief 追加一行参数
     * @param row 行定义；id 为空或与已有 id 重复时忽略
     * @return 新行行号，被忽略时返回 -1
     * @note 只使用本接口增删行；直接调用 QTableWidget 的 insertRow/removeRow
     *       会让内部行配置失配
     */
    int addParam(const ParamRow &row);

    /// 追加一行参数（范围 ±1e12、精度 3 位）
    int addParam(const QString &id, const QString &name, const QString &unit = QString());

    /// 清空全部参数行，保留数据列
    void clearParams();

    /// 当前参数行数
    int paramCount() const noexcept;

    /// 按 id 查行号，不存在时返回 -1
    int rowOf(const QString &id) const noexcept;

    // ---- 数值读写 ----

    /**
     * @brief 回填某行某列的最新值
     * @note 不发出 valueEdited；该单元格正被编辑时只更新后备值，不打断输入；
     *       传入非有限数值时忽略
     */
    void setValue(const QString &row_id, const QString &column_id, double value);

    /// 按行号与数据列序号回填，语义同 setValue(const QString &, const QString &, double)
    void setValueAt(int row, int data_column, double value);

    /// 读取控件内记录的值；行或列不存在时返回 0
    double value(const QString &row_id, const QString &column_id) const;

    /// 修改某行的可编辑范围（作用于该行所有数据列）
    void setRange(const QString &row_id, double minimum, double maximum);

    /// 修改某行的显示精度，取值会被收敛到 0~9
    void setDecimals(const QString &row_id, int decimals);

    /// 设置某行是否允许双击编辑
    void setEditable(const QString &row_id, bool editable);

    // ---- 轮询 ----

    /// 轮询周期（毫秒），默认 200；小于 10 时按 10 处理
    void setPollInterval(int milliseconds);
    int pollInterval() const noexcept;

    /**
     * @brief 开/关轮询
     * @note 打开时立即发出一次 pollRequested，避免首屏空等一个周期
     */
    void setPollingEnabled(bool enabled);
    bool isPollingEnabled() const noexcept;

signals:
    /// 到达轮询周期，调用者应读取底层并按行列回填
    void pollRequested();

    /// 用户双击编辑并通过校验，调用者应把 value 作为参数传给底层写函数
    void valueEdited(const QString &row_id, const QString &column_id, double value);

    /// 输入不是合法数值或超出范围，控件已恢复原值并回传被拒绝的文本
    void editRejected(const QString &row_id, const QString &column_id, const QString &text);

    /// 数据列显隐变化（含调用 setColumnVisible 引起的变化，便于界面同步）
    void columnVisibilityChanged(const QString &column_id, bool visible);

private slots:
    void onItemChanged(QTableWidgetItem *item);
    void onPollTimeout();

private:
    /// 给某行某数据列补建单元格（新建列或新建行时使用）
    void createValueItem(int row, int data_column);

    /// 参数名列显示文本：名称 + 单位
    static QString displayName(const ParamRow &row);

    /// 值单元格的悬停提示：能否编辑 + 允许范围（未设范围时只提示能否编辑）
    static QString valueToolTip(const ParamRow &row);

    /// 范围或可编辑性变化后，刷新该行所有值单元格的悬停提示
    void updateRowToolTips(int row);

    /// 该单元格是否正处于编辑状态（编辑中不回写内容）
    bool isCellBeingEdited(int row, int table_column) const;

    QVector<ParamRow> rows_;          ///< 行配置，下标即行号
    QHash<QString, int> row_to_index_;

    QVector<ParamColumn> columns_;    ///< 数据列配置，下标即数据列序号
    QHash<QString, int> column_to_index_;

    QVector<QVector<double>> values_; ///< values_[行][数据列]

    QTimer *poll_timer_ {nullptr};
    int poll_interval_ms_ {200};
    int value_column_width_ {96};     ///< 数据列固定宽度，避免刷新时列宽跳动
};

}  // namespace uikit
