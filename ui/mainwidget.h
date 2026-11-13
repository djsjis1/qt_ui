/**
 * @file MainWidget.h
 * @brief 演示主界面：左侧控制栏 + 中间实时曲线 + 右侧轴参数矩阵表格
 *
 * 本类只负责界面与控件联动，不产生数据：
 * 采样数据由 main.cpp 里的定时器通过 appendFrame() 注入，
 * 轴参数的行列定义与底层读写同样由 main.cpp 提供，界面只转发。
 * 接真实采集模块时只需替换那一处的数据源。
 *
 * 控制项：通道显隐、时间窗、暂停、自动量程、跟随最新；轴参数列显隐、轮询与编辑。
 */

#pragma once

#include <QString>
#include <QVector>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QHBoxLayout;
class QLabel;
class QPushButton;
class QEvent;

namespace uikit {
class PlotWidget;
class ParamTableWidget;
struct ParamRow;
}

/**
 * @brief 演示窗口
 */
class MainWidget : public QWidget
{
    Q_OBJECT

public:
    explicit MainWidget(QWidget *parent = nullptr);

    /**
     * @brief 向曲线控件写入一帧采样数据
     * @param values 各通道值，顺序与 setupPlot() 中 addChannel 的顺序一致
     * @note 必须在 GUI 线程调用；时间轴推进由控件内部采样周期决定
     */
    void appendFrame(const QVector<float> &values);

    /**
     * @brief 向轴参数表格追加一列数据源（如 X 轴）
     * @param id   列键，与底层参数读取一一对应
     * @param name 列标题
     * @note 同时在该面板上生成一个勾选框，控制该列的显隐
     */
    void addAxisColumn(const QString &id, const QString &name);

    /**
     * @brief 向轴参数表格追加一行参数
     * @param row 行定义（id、名称、单位、范围、精度），由数据源侧提供
     */
    void addAxisParam(const uikit::ParamRow &row);

    /**
     * @brief 把轮询读到的参数值回填到表格
     * @note 只更新显示，不会触发 axisParamEdited
     */
    void setAxisParamValue(const QString &row_id, const QString &column_id, double value);

signals:
    /// 表格到达轮询周期，调用方应读取底层参数后调用 setAxisParamValue 回填
    void axisParamPollRequested();

    /// 用户双击编辑并通过范围校验，调用方应把该值作为参数传给底层写函数
    void axisParamEdited(const QString &row_id, const QString &column_id, double value);

protected:
    /**
     * @brief 在演示层实现曲线控件的鼠标交互
     *
     * 控件本身不处理任何鼠标事件，缩放、平移、框选、游标都由这里把鼠标位置
     * 换算成数据坐标后，调用 PlotWidget 的接口完成。
     */
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void buildUi();
    QWidget *createControlPanel();
    QWidget *createParamPanel();
    void setupPlot();
    void applyDarkPalette();
    void resetView();

    /// 一键显示/隐藏全部轴列（同步勾选框状态）
    void setAllAxisColumnsVisible(bool visible);

    /// 把"曲线样式"三个下拉的当前取值应用到全部通道
    void applyChannelStyle();

    uikit::PlotWidget *plot_ {nullptr};
    uikit::ParamTableWidget *param_table_ {nullptr};

    QVector<QCheckBox *> channel_boxes_; ///< 与通道显隐状态互相同步
    QComboBox *window_combo_ {nullptr};
    QComboBox *draw_mode_combo_ {nullptr};   ///< 线 / 点 / 线+点
    QComboBox *line_width_combo_ {nullptr};  ///< 线宽，物理像素
    QComboBox *point_size_combo_ {nullptr};  ///< 点直径，物理像素
    QPushButton *pause_button_ {nullptr};
    QCheckBox *auto_range_box_ {nullptr};
    QCheckBox *follow_box_ {nullptr};
    QLabel *frame_stats_label_ {nullptr};
    QLabel *param_hint_label_ {nullptr};  ///< 轴参数表格下方的操作/拒绝提示
    QHBoxLayout *column_check_layout_ {nullptr}; ///< 由列定义动态填充的勾选框行
    QVector<QCheckBox *> axis_column_boxes_; ///< 轴列勾选框，用于一键全显/全隐

    /// 临时诊断：最近一次触发视口变化的事件名，输出到标题定位状态被谁改掉
    QString last_view_event_ {QStringLiteral("init")};

    /// 用户在下拉/按钮里配置的宽度：true=全部，false=固定时间窗（秒数在组件 time_window_）。
    /// 只由用户操作改写；勾选跟随、重置时按它恢复，程序不反写下拉
    bool configured_fit_all_ {false};

    // 演示层的交互状态
    bool dragging_ {false};
    QPointF drag_last_;
    bool rubber_band_ {false};
    QPointF rubber_start_;
    QPointF press_position_;
    bool press_moved_ {false};
};
