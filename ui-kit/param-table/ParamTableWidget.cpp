/**
 * @file ParamTableWidget.cpp
 * @brief 参数矩阵表格实现：行列注册、列显隐、轮询定时器、双击编辑的解析与范围校验
 *
 * 数据流（控件不接触任何底层设备接口）：
 *
 *   调用者 ──setValue(row, col, v)──► 单元格
 *          ◄──pollRequested()──────── 每 pollInterval 毫秒一次
 *
 *   用户双击 ──► 解析文本 ──► 与 ParamRow 的范围/精度比对
 *                              ├─ 通过 → valueEdited(row, col, value)，调用者写底层
 *                              └─ 拒绝 → 恢复原值 + editRejected(row, col, text)
 *
 * 轮询回填时会跳过"正在编辑的单元格"，否则用户还没敲完就被新值冲掉。
 */

#include "ParamTableWidget.h"

#include <cmath>

#include <QFontMetrics>
#include <QHeaderView>
#include <QSignalBlocker>
#include <QStringList>
#include <QTableWidgetItem>
#include <QTimer>

namespace uikit {

namespace {

/// 数据列固定宽度的留白，避免轮询刷新时列宽跳动
constexpr int kValueColumnPadding = 24;

/// 十进制位数收敛到可显示且可解析的范围
int clampDecimals(int decimals)
{
    return (decimals < 0) ? 0 : ((decimals > 9) ? 9 : decimals);
}

/// 按显示精度格式化数值
QString formatValue(double value, int decimals)
{
    return QString::number(value, 'f', clampDecimals(decimals));
}

}  // namespace

ParamTableWidget::ParamTableWidget(QWidget *parent)
    : QTableWidget(parent)
{
    // 列 0 固定为参数名，数据列由 addColumn 追加
    setColumnCount(1);
    setHorizontalHeaderLabels({tr("参数")});

    verticalHeader()->setVisible(false);
    horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    setSelectionBehavior(QAbstractItemView::SelectRows);
    setSelectionMode(QAbstractItemView::SingleSelection);
    // 只响应双击（或 F2 / 直接输入），单击只选中整行
    setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    setAlternatingRowColors(true);
    setShowGrid(false);
    setWordWrap(false);

    const QFontMetrics metrics(font());
    value_column_width_ =
        metrics.horizontalAdvance(QStringLiteral("-000000.000")) + kValueColumnPadding;

    connect(this, &QTableWidget::itemChanged, this, &ParamTableWidget::onItemChanged);

    poll_timer_ = new QTimer(this);
    // 轮询只用来取数，不需要毫秒级精度，粗定时器更省唤醒
    poll_timer_->setTimerType(Qt::CoarseTimer);
    connect(poll_timer_, &QTimer::timeout, this, &ParamTableWidget::onPollTimeout);
}

ParamTableWidget::~ParamTableWidget() = default;

// ---- 列 ----

int ParamTableWidget::addColumn(const ParamColumn &column)
{
    if (column.id.isEmpty() || column_to_index_.contains(column.id)) {
        return -1;
    }

    const int data_column = static_cast<int>(columns_.size());
    columns_.append(column);
    column_to_index_.insert(column.id, data_column);

    const QSignalBlocker blocker(this);
    const int table_column = data_column + 1;
    setColumnCount(table_column + 1);

    auto *header = new QTableWidgetItem(column.name.isEmpty() ? column.id : column.name);
    header->setFlags(header->flags() & ~Qt::ItemIsEditable);
    setHorizontalHeaderItem(table_column, header);

    horizontalHeader()->setSectionResizeMode(table_column, QHeaderView::Fixed);
    setColumnWidth(table_column, value_column_width_);

    for (int row = 0; row < rows_.size(); ++row) {
        values_[row].append(0.0);
        createValueItem(row, data_column);
    }

    if (!column.visible) {
        setColumnHidden(table_column, true);
    }
    return data_column;
}

int ParamTableWidget::addColumn(const QString &id, const QString &name)
{
    ParamColumn column;
    column.id = id;
    column.name = name;
    return addColumn(column);
}

void ParamTableWidget::clearColumns()
{
    const QSignalBlocker blocker(this);
    columns_.clear();
    column_to_index_.clear();
    for (QVector<double> &row_values : values_) {
        row_values.clear();
    }
    setColumnCount(1); // 只保留参数名列
}

int ParamTableWidget::dataColumnCount() const noexcept
{
    return static_cast<int>(columns_.size());
}

int ParamTableWidget::columnIndexOf(const QString &id) const noexcept
{
    return column_to_index_.value(id, -1);
}

void ParamTableWidget::setColumnVisible(const QString &id, bool visible)
{
    const int data_column = columnIndexOf(id);
    if (data_column < 0) {
        return;
    }
    columns_[data_column].visible = visible;
    setColumnHidden(data_column + 1, !visible);
    emit columnVisibilityChanged(id, visible);
}

bool ParamTableWidget::isColumnVisible(const QString &id) const noexcept
{
    const int data_column = columnIndexOf(id);
    return data_column >= 0 && columns_.at(data_column).visible;
}

// ---- 行 ----

int ParamTableWidget::addParam(const ParamRow &row)
{
    if (row.id.isEmpty() || row_to_index_.contains(row.id)) {
        return -1;
    }

    const int row_index = static_cast<int>(rows_.size());
    rows_.append(row);
    row_to_index_.insert(row.id, row_index);
    values_.append(QVector<double>(columns_.size(), 0.0));

    // 建行期间的 setItem 也会触发 itemChanged，这里统一屏蔽
    const QSignalBlocker blocker(this);
    insertRow(row_index);

    auto *name_item = new QTableWidgetItem(displayName(row));
    name_item->setFlags(name_item->flags() & ~Qt::ItemIsEditable);
    setItem(row_index, 0, name_item);

    for (int data_column = 0; data_column < columns_.size(); ++data_column) {
        createValueItem(row_index, data_column);
    }
    return row_index;
}

int ParamTableWidget::addParam(const QString &id, const QString &name, const QString &unit)
{
    ParamRow row;
    row.id = id;
    row.name = name;
    row.unit = unit;
    return addParam(row);
}

void ParamTableWidget::clearParams()
{
    const QSignalBlocker blocker(this);
    rows_.clear();
    row_to_index_.clear();
    values_.clear();
    setRowCount(0);
}

int ParamTableWidget::paramCount() const noexcept
{
    return static_cast<int>(rows_.size());
}

int ParamTableWidget::rowOf(const QString &id) const noexcept
{
    return row_to_index_.value(id, -1);
}

void ParamTableWidget::createValueItem(int row, int data_column)
{
    const ParamRow &spec = rows_.at(row);
    auto *item = new QTableWidgetItem(formatValue(values_.at(row).at(data_column), spec.decimals));
    item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    item->setToolTip(valueToolTip(spec));
    if (!spec.editable) {
        item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    }
    setItem(row, data_column + 1, item);
}

QString ParamTableWidget::displayName(const ParamRow &row)
{
    const QString base = row.name.isEmpty() ? row.id : row.name;
    if (row.unit.isEmpty()) {
        return base;
    }
    return QStringLiteral("%1 (%2)").arg(base, row.unit);
}

QString ParamTableWidget::valueToolTip(const ParamRow &row)
{
    QStringList lines;
    if (row.editable) {
        lines.append(tr("双击编辑，回车提交"));
    }
    if (row.minimum != kDefaultParamMinimum || row.maximum != kDefaultParamMaximum) {
        QString range = tr("允许范围 %1 ~ %2")
                            .arg(formatValue(row.minimum, row.decimals),
                                 formatValue(row.maximum, row.decimals));
        if (!row.unit.isEmpty()) {
            range += QLatin1Char(' ') + row.unit;
        }
        lines.append(range);
    }
    return lines.join(QLatin1Char('\n'));
}

void ParamTableWidget::updateRowToolTips(int row)
{
    if (row < 0 || row >= static_cast<int>(rows_.size())) {
        return;
    }

    const QString tip = valueToolTip(rows_.at(row));
    for (int data_column = 0; data_column < columns_.size(); ++data_column) {
        QTableWidgetItem *value_item = item(row, data_column + 1);
        if (value_item != nullptr) {
            value_item->setToolTip(tip);
        }
    }
}

// ---- 数值读写 ----

void ParamTableWidget::setValue(const QString &row_id, const QString &column_id, double value)
{
    setValueAt(rowOf(row_id), columnIndexOf(column_id), value);
}

void ParamTableWidget::setValueAt(int row, int data_column, double value)
{
    if (row < 0 || row >= static_cast<int>(rows_.size()) || data_column < 0
        || data_column >= static_cast<int>(columns_.size()) || !std::isfinite(value)) {
        return;
    }

    const ParamRow &spec = rows_.at(row);
    QTableWidgetItem *value_item = item(row, data_column + 1);

    // 该单元格正在输入：只更新后备值，单元格留给用户，避免把光标下的内容改掉
    if (value_item == nullptr || isCellBeingEdited(row, data_column + 1)) {
        values_[row][data_column] = value;
        return;
    }

    const QString text = formatValue(value, spec.decimals);
    if (value_item->text() == text) {
        values_[row][data_column] = value;
        return;
    }

    values_[row][data_column] = value;
    const QSignalBlocker blocker(this);
    value_item->setText(text);
}

double ParamTableWidget::value(const QString &row_id, const QString &column_id) const
{
    const int row = rowOf(row_id);
    const int data_column = columnIndexOf(column_id);
    if (row < 0 || data_column < 0) {
        return 0.0;
    }
    return values_.at(row).at(data_column);
}

void ParamTableWidget::setRange(const QString &row_id, double minimum, double maximum)
{
    const int row = rowOf(row_id);
    if (row < 0 || minimum > maximum) {
        return;
    }
    rows_[row].minimum = minimum;
    rows_[row].maximum = maximum;
    updateRowToolTips(row);
}

void ParamTableWidget::setDecimals(const QString &row_id, int decimals)
{
    const int row = rowOf(row_id);
    if (row < 0) {
        return;
    }
    rows_[row].decimals = clampDecimals(decimals);

    const QSignalBlocker blocker(this);
    for (int data_column = 0; data_column < columns_.size(); ++data_column) {
        QTableWidgetItem *value_item = item(row, data_column + 1);
        if (value_item != nullptr) {
            value_item->setText(formatValue(values_.at(row).at(data_column), rows_.at(row).decimals));
        }
    }
}

void ParamTableWidget::setEditable(const QString &row_id, bool editable)
{
    const int row = rowOf(row_id);
    if (row < 0) {
        return;
    }
    rows_[row].editable = editable;
    updateRowToolTips(row);

    for (int data_column = 0; data_column < columns_.size(); ++data_column) {
        QTableWidgetItem *value_item = item(row, data_column + 1);
        if (value_item == nullptr) {
            continue;
        }
        if (editable) {
            value_item->setFlags(value_item->flags() | Qt::ItemIsEditable);
        } else {
            value_item->setFlags(value_item->flags() & ~Qt::ItemIsEditable);
        }
    }
}

// ---- 轮询 ----

void ParamTableWidget::setPollInterval(int milliseconds)
{
    poll_interval_ms_ = (milliseconds < 10) ? 10 : milliseconds;
    if (poll_timer_ != nullptr && poll_timer_->isActive()) {
        poll_timer_->start(poll_interval_ms_);
    }
}

int ParamTableWidget::pollInterval() const noexcept
{
    return poll_interval_ms_;
}

void ParamTableWidget::setPollingEnabled(bool enabled)
{
    if (poll_timer_ == nullptr) {
        return;
    }

    if (!enabled) {
        poll_timer_->stop();
        return;
    }

    poll_timer_->start(poll_interval_ms_);
    emit pollRequested(); // 立即取一次，首屏不用空等一个周期
}

bool ParamTableWidget::isPollingEnabled() const noexcept
{
    return poll_timer_ != nullptr && poll_timer_->isActive();
}

void ParamTableWidget::onPollTimeout()
{
    emit pollRequested();
}

// ---- 编辑 ----

void ParamTableWidget::onItemChanged(QTableWidgetItem *item)
{
    // 只有数据列可编辑；参数名列与内部回写（已被 QSignalBlocker 屏蔽）都不处理
    if (item == nullptr || item->column() < 1) {
        return;
    }

    const int row = item->row();
    const int data_column = item->column() - 1;
    if (row < 0 || row >= static_cast<int>(rows_.size()) || data_column < 0
        || data_column >= static_cast<int>(columns_.size())) {
        return;
    }

    ParamRow &spec = rows_[row];
    if (!spec.editable) {
        return;
    }

    const QString column_id = columns_.at(data_column).id;
    const double current = values_.at(row).at(data_column);

    const QString text = item->text().trimmed();
    bool parsed = false;
    const double value = text.toDouble(&parsed);

    if (!parsed || !std::isfinite(value) || value < spec.minimum || value > spec.maximum) {
        {
            const QSignalBlocker blocker(this);
            item->setText(formatValue(current, spec.decimals));
        }
        // 必须在 blocker 作用域外发信号，否则自定义信号也会被一并阻断
        emit editRejected(spec.id, column_id, text);
        return;
    }

    if (value == current) {
        // 数值没变也统一回写格式，避免留下 "1.0" 之类的临时文本
        const QSignalBlocker blocker(this);
        item->setText(formatValue(current, spec.decimals));
        return;
    }

    values_[row][data_column] = value;
    {
        const QSignalBlocker blocker(this);
        item->setText(formatValue(value, spec.decimals));
    }
    emit valueEdited(spec.id, column_id, value);
}

bool ParamTableWidget::isCellBeingEdited(int row, int table_column) const
{
    // state() 是 QAbstractItemView 的 protected 接口，子类内可用于判断编辑器是否打开
    return state() == QAbstractItemView::EditingState && currentIndex().row() == row
        && currentIndex().column() == table_column;
}

}  // namespace uikit
