/**
 * @file MainWidget.cpp
 * @brief 演示主界面的实现：控制栏与轴参数面板布局、控件联动
 *
 * 本文件不产生采样数据，也不读写底层参数：曲线数据由 main.cpp 的定时器经
 * appendFrame() 注入，轴参数的行定义由 main.cpp 经 addAxisParam() 注入，
 * 轮询与编辑结果通过信号转回 main.cpp。
 */

#include "mainwidget.h"

#include <algorithm>
#include <cmath>

#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QEvent>
#include <QFileDialog>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPalette>
#include <QPushButton>
#include <QScreen>
#include <QSignalBlocker>
#include <QStringList>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>

#include "PlotWidget.h"
#include "ParamTableWidget.h"
#include "ParamTableTypes.h"

namespace {

constexpr float kSampleInterval = 0.001F; ///< 采样周期，需与 main.cpp 的数据源一致

}  // namespace

MainWidget::MainWidget(QWidget *parent)
    : QWidget(parent)
{
    setWindowTitle(QStringLiteral("Qt 曲线组件演示 — ui-kit/plot"));

    // 按屏幕可用区域收敛窗口尺寸，避免高 DPI 下窗口超出屏幕把图例和刻度裁掉
    const QScreen *target_screen = screen();
    if (target_screen != nullptr) {
        const QRect available = target_screen->availableGeometry();
        resize(std::min(1600, available.width() - 60), std::min(760, available.height() - 60));
    } else {
        resize(1600, 760);
    }

    applyDarkPalette();
    buildUi();
}

void MainWidget::buildUi()
{
    auto *root = new QHBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(8);

    plot_ = new uikit::PlotWidget(this);
    setupPlot();

    // 控件不处理鼠标事件，交互由本窗口通过事件过滤器实现
    plot_->setMouseTracking(true);
    plot_->installEventFilter(this);

    root->addWidget(createControlPanel());
    root->addWidget(plot_, 1);
    root->addWidget(createParamPanel());
}

QWidget *MainWidget::createParamPanel()
{
    auto *panel = new QWidget(this);
    panel->setFixedWidth(460);

    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    auto *group = new QGroupBox(QStringLiteral("轴参数"), panel);
    auto *group_layout = new QVBoxLayout(group);
    group_layout->setSpacing(6);

    // 列勾选：勾选框由 addAxisColumn 按列定义动态创建，表格只负责列显隐
    auto *check_row = new QHBoxLayout;
    check_row->setSpacing(10);
    auto *check_label = new QLabel(QStringLiteral("显示列"), group);
    check_label->setStyleSheet(QStringLiteral("color: #8FA0AE;"));
    check_row->addWidget(check_label);

    column_check_layout_ = new QHBoxLayout;
    column_check_layout_->setSpacing(10);
    check_row->addLayout(column_check_layout_);
    check_row->addStretch(1);

    auto *show_all_button = new QPushButton(QStringLiteral("全显"), group);
    auto *hide_all_button = new QPushButton(QStringLiteral("全隐"), group);
    check_row->addWidget(show_all_button);
    check_row->addWidget(hide_all_button);

    connect(show_all_button, &QPushButton::clicked, this,
            [this]() { setAllAxisColumnsVisible(true); });
    connect(hide_all_button, &QPushButton::clicked, this,
            [this]() { setAllAxisColumnsVisible(false); });

    group_layout->addLayout(check_row);

    // 表格只负责展示与轮询节奏，参数行与轴列由 main.cpp 通过 addAxis* 追加
    param_table_ = new uikit::ParamTableWidget(group);
    param_table_->setMinimumHeight(260);
    group_layout->addWidget(param_table_);

    param_hint_label_ = new QLabel(
        QStringLiteral("双击数值列编辑，回车提交\n悬停数值可看允许范围，越界输入会被拒绝"), group);
    param_hint_label_->setWordWrap(true);
    param_hint_label_->setStyleSheet(QStringLiteral("color: #8FA0AE;"));
    group_layout->addWidget(param_hint_label_);

    layout->addWidget(group);
    layout->addStretch(1);

    // 轮询请求与编辑结果都转发出去，本窗口不读也不写任何底层参数
    connect(param_table_, &uikit::ParamTableWidget::pollRequested,
            this, &MainWidget::axisParamPollRequested);
    connect(param_table_, &uikit::ParamTableWidget::valueEdited,
            this, &MainWidget::axisParamEdited);
    connect(param_table_, &uikit::ParamTableWidget::editRejected, this,
            [this](const QString &row_id, const QString &column_id, const QString &text) {
                if (param_hint_label_ == nullptr) {
                    return;
                }
                param_hint_label_->setText(
                    QStringLiteral("参数 %1 / %2 的输入 \"%3\" 非法或越界\n已恢复原值，请重新输入")
                        .arg(row_id, column_id, text));
            });

    param_table_->setPollInterval(200);
    param_table_->setPollingEnabled(true);
    return panel;
}

void MainWidget::addAxisColumn(const QString &id, const QString &name)
{
    if (param_table_ == nullptr || param_table_->addColumn(id, name) < 0) {
        return;
    }
    if (column_check_layout_ == nullptr) {
        return;
    }

    auto *box = new QCheckBox(name, param_table_->parentWidget());
    box->setChecked(true);
    column_check_layout_->addWidget(box);
    axis_column_boxes_.append(box);

    connect(box, &QCheckBox::toggled, this, [this, id](bool checked) {
        if (param_table_ != nullptr) {
            param_table_->setColumnVisible(id, checked);
        }
    });
}

void MainWidget::setAllAxisColumnsVisible(bool visible)
{
    for (QCheckBox *box : axis_column_boxes_) {
        if (box != nullptr) {
            box->setChecked(visible); // 触发 toggled，列的显隐随之同步
        }
    }
}

QWidget *MainWidget::createControlPanel()
{
    auto *panel = new QWidget(this);
    panel->setFixedWidth(250);

    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);

    // ---- 通道开关 ----
    auto *channel_group = new QGroupBox(QStringLiteral("通道"), panel);
    auto *channel_layout = new QVBoxLayout(channel_group);
    channel_layout->setSpacing(6);

    const QStringList channel_names = {
        QStringLiteral("CH1 正弦 1.0 Hz"),
        QStringLiteral("CH2 谐波 2.7 Hz"),
        QStringLiteral("CH3 方波 + 噪声"),
    };

    const int channel_count = static_cast<int>(channel_names.size());
    for (int i = 0; i < channel_count; ++i) {
        auto *box = new QCheckBox(channel_names.at(i), channel_group);
        box->setChecked(true);
        channel_boxes_.append(box);
        channel_layout->addWidget(box);

        connect(box, &QCheckBox::toggled, this, [this, i](bool checked) {
            if (plot_ != nullptr) {
                plot_->setChannelVisible(i, checked);
            }
        });
    }

    // 批量开关：勾选框由 channelVisibilityChanged 自动同步，这里无需手工改
    auto *channel_button_row = new QHBoxLayout;
    auto *show_all_channels = new QPushButton(QStringLiteral("全显"), channel_group);
    auto *hide_all_channels = new QPushButton(QStringLiteral("全隐"), channel_group);
    channel_button_row->addWidget(show_all_channels);
    channel_button_row->addWidget(hide_all_channels);
    channel_layout->addLayout(channel_button_row);

    connect(show_all_channels, &QPushButton::clicked, this, [this]() {
        if (plot_ != nullptr) {
            plot_->setAllChannelsVisible(true);
        }
    });
    connect(hide_all_channels, &QPushButton::clicked, this, [this]() {
        if (plot_ != nullptr) {
            plot_->setAllChannelsVisible(false);
        }
    });

    layout->addWidget(channel_group);

    // ---- 曲线样式：线 / 点（接口按通道设置，这里统一作用于全部通道）----
    auto *style_group = new QGroupBox(QStringLiteral("曲线样式"), panel);
    auto *style_layout = new QVBoxLayout(style_group);
    style_layout->setSpacing(8);

    auto *mode_row = new QHBoxLayout;
    mode_row->addWidget(new QLabel(QStringLiteral("绘制"), style_group));
    draw_mode_combo_ = new QComboBox(style_group);
    draw_mode_combo_->addItem(QStringLiteral("线"), 0);
    draw_mode_combo_->addItem(QStringLiteral("点"), 1);
    draw_mode_combo_->addItem(QStringLiteral("线 + 点"), 2);
    draw_mode_combo_->setCurrentIndex(0);
    mode_row->addWidget(draw_mode_combo_, 1);
    style_layout->addLayout(mode_row);

    auto *width_row = new QHBoxLayout;
    width_row->addWidget(new QLabel(QStringLiteral("线宽"), style_group));
    line_width_combo_ = new QComboBox(style_group);
    for (int width : {1, 2, 3, 4, 6}) {
        line_width_combo_->addItem(QStringLiteral("%1 px").arg(width), width);
    }
    line_width_combo_->setCurrentIndex(1);
    width_row->addWidget(line_width_combo_, 1);
    style_layout->addLayout(width_row);

    auto *point_row = new QHBoxLayout;
    point_row->addWidget(new QLabel(QStringLiteral("点大小"), style_group));
    point_size_combo_ = new QComboBox(style_group);
    for (int size : {2, 3, 4, 6, 8}) {
        point_size_combo_->addItem(QStringLiteral("%1 px").arg(size), size);
    }
    point_size_combo_->setCurrentIndex(1);
    point_row->addWidget(point_size_combo_, 1);
    style_layout->addLayout(point_row);

    layout->addWidget(style_group);

    connect(draw_mode_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) { applyChannelStyle(); });
    connect(line_width_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) { applyChannelStyle(); });
    connect(point_size_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) { applyChannelStyle(); });

    // ---- 显示设置 ----
    auto *display_group = new QGroupBox(QStringLiteral("显示"), panel);
    auto *display_layout = new QVBoxLayout(display_group);
    display_layout->setSpacing(8);

    auto *window_row = new QHBoxLayout;
    window_row->addWidget(new QLabel(QStringLiteral("时间窗"), display_group));

    window_combo_ = new QComboBox(display_group);
    window_combo_->addItem(QStringLiteral("2 s"), 2.0F);
    window_combo_->addItem(QStringLiteral("5 s"), 5.0F);
    window_combo_->addItem(QStringLiteral("10 s"), 10.0F);
    window_combo_->addItem(QStringLiteral("20 s"), 20.0F);
    window_combo_->addItem(QStringLiteral("60 s"), 60.0F);
    window_combo_->addItem(QStringLiteral("全部"), -1.0F); // 负值表示"适配全部数据"
    window_combo_->setCurrentIndex(2);
    window_row->addWidget(window_combo_, 1);
    display_layout->addLayout(window_row);

    // 用 activated 而不是 currentIndexChanged：重复点"全部"也要能重新适配
    // 下拉是"用户设置"，只由用户点击改变；程序任何路径都不反写下拉。
    // 用户选的配置记在 configured_fit_all_，勾选跟随/重置时按它恢复
    connect(window_combo_, QOverload<int>::of(&QComboBox::activated), this, [this](int index) {
        last_view_event_ = QStringLiteral("combo%1").arg(index); // 诊断：捕获程序性触发
        if (plot_ == nullptr || window_combo_ == nullptr) {
            return;
        }

        const float seconds = window_combo_->itemData(index).toFloat();
        if (seconds <= 0.0F) {
            configured_fit_all_ = true; // 用户配置了"全部"
            plot_->fitAllData();
        } else {
            configured_fit_all_ = false; // 用户配置了固定时间窗（秒数存在组件 time_window_）
            plot_->setFollowLatest(true);
            plot_->setTimeWindow(seconds);
        }
    });

    pause_button_ = new QPushButton(QStringLiteral("暂停"), display_group);
    pause_button_->setCheckable(true);
    display_layout->addWidget(pause_button_);

    connect(pause_button_, &QPushButton::toggled, this, [this](bool checked) {
        if (plot_ != nullptr) {
            plot_->setPaused(checked);
        }
        if (pause_button_ != nullptr) {
            pause_button_->setText(checked ? QStringLiteral("继续") : QStringLiteral("暂停"));
        }
    });

    auto_range_box_ = new QCheckBox(QStringLiteral("自动量程"), display_group);
    auto_range_box_->setChecked(true);
    display_layout->addWidget(auto_range_box_);

    connect(auto_range_box_, &QCheckBox::toggled, this, [this](bool checked) {
        if (plot_ != nullptr) {
            plot_->setAutoYRange(checked);
        }
    });

    // 画面元素显隐：图例 / 网格 / 坐标轴
    auto *element_row = new QHBoxLayout;
    element_row->setSpacing(8);

    auto *legend_box = new QCheckBox(QStringLiteral("图例"), display_group);
    auto *grid_box = new QCheckBox(QStringLiteral("网格"), display_group);
    auto *axes_box = new QCheckBox(QStringLiteral("坐标轴"), display_group);
    for (QCheckBox *box : {legend_box, grid_box, axes_box}) {
        box->setChecked(true);
        element_row->addWidget(box);
    }
    element_row->addStretch(1);
    display_layout->addLayout(element_row);

    connect(legend_box, &QCheckBox::toggled, this, [this](bool checked) {
        if (plot_ != nullptr) {
            plot_->setLegendVisible(checked);
        }
    });
    connect(grid_box, &QCheckBox::toggled, this, [this](bool checked) {
        if (plot_ != nullptr) {
            plot_->setGridVisible(checked);
        }
    });
    connect(axes_box, &QCheckBox::toggled, this, [this](bool checked) {
        if (plot_ != nullptr) {
            plot_->setAxesVisible(checked);
        }
    });

    // 视图操作：演示 setYRange / setXRange / fitAllData 等程序接口
    auto *view_row = new QHBoxLayout;
    auto *reset_button = new QPushButton(QStringLiteral("重置"), display_group);
    auto *fit_button = new QPushButton(QStringLiteral("全部"), display_group);
    auto *zoom_y_button = new QPushButton(QStringLiteral("Y 放大"), display_group);
    view_row->addWidget(reset_button);
    view_row->addWidget(fit_button);
    view_row->addWidget(zoom_y_button);
    display_layout->addLayout(view_row);

    connect(reset_button, &QPushButton::clicked, this, &MainWidget::resetView);

    connect(fit_button, &QPushButton::clicked, this, [this]() {
        if (plot_ == nullptr) {
            return;
        }
        configured_fit_all_ = true; // 按钮与下拉同源，都算用户配置"全部"
        plot_->fitAllData();
    });

    connect(zoom_y_button, &QPushButton::clicked, this, [this]() {
        if (plot_ == nullptr) {
            return;
        }
        plot_->setYRange(-0.5F, 0.5F); // 固定纵轴范围，观察 Y 轴缩放效果
    });

    // 框选放大 / 双击复位会改变自动量程状态，同步回复选框
    connect(plot_, &uikit::PlotWidget::autoYRangeChanged, this, [this](bool enabled) {
        if (auto_range_box_ == nullptr || auto_range_box_->isChecked() == enabled) {
            return;
        }
        const QSignalBlocker blocker(auto_range_box_);
        auto_range_box_->setChecked(enabled);
    });

    follow_box_ = new QCheckBox(QStringLiteral("跟随最新数据"), display_group);
    follow_box_->setChecked(true);
    display_layout->addWidget(follow_box_);

    connect(follow_box_, &QCheckBox::toggled, this, [this](bool checked) {
        if (plot_ == nullptr) {
            return;
        }
        if (!checked) {
            plot_->setFollowLatest(false); // 冻结画面，配置不动
            return;
        }
        // 勾选跟随：按用户配置恢复——配置了"全部"就恢复全览，否则回时间窗
        if (configured_fit_all_) {
            plot_->fitAllData();
        } else {
            plot_->setFollowLatest(true);
        }
    });

    // 用户拖拽/缩放会让控件自动退出跟随，这里把状态同步回复选框
    connect(plot_, &uikit::PlotWidget::followLatestChanged, this, [this](bool follow) {
        if (follow_box_ == nullptr || follow_box_->isChecked() == follow) {
            return;
        }
        const QSignalBlocker blocker(follow_box_);
        follow_box_->setChecked(follow);
    });

    layout->addWidget(display_group);

    // 点图例同样会切换通道显隐，这里把复选框状态同步回去
    connect(plot_, &uikit::PlotWidget::channelVisibilityChanged, this,
            [this](int channel, bool visible) {
                if (channel < 0 || channel >= static_cast<int>(channel_boxes_.size())) {
                    return;
                }
                QCheckBox *box = channel_boxes_.at(channel);
                if (box->isChecked() == visible) {
                    return;
                }
                const QSignalBlocker blocker(box);
                box->setChecked(visible);
            });

    // ---- 导出 ----
    auto *tool_group = new QGroupBox(QStringLiteral("导出"), panel);
    auto *tool_layout = new QHBoxLayout(tool_group);
    tool_layout->setSpacing(6);

    auto *csv_button = new QPushButton(QStringLiteral("CSV"), tool_group);
    auto *png_button = new QPushButton(QStringLiteral("PNG"), tool_group);
    tool_layout->addWidget(csv_button);
    tool_layout->addWidget(png_button);
    layout->addWidget(tool_group);

    connect(csv_button, &QPushButton::clicked, this, [this]() {
        if (plot_ == nullptr) {
            return;
        }
        const QString path = QFileDialog::getSaveFileName(
            this, QStringLiteral("导出当前视窗数据"), QStringLiteral("waveform.csv"),
            QStringLiteral("CSV 文件 (*.csv)"));
        if (path.isEmpty()) {
            return;
        }
        if (!plot_->exportVisibleCsv(path)) {
            QMessageBox::warning(this, QStringLiteral("导出失败"),
                                 QStringLiteral("无法写入该文件，请检查路径权限。"));
        }
    });

    connect(png_button, &QPushButton::clicked, this, [this]() {
        if (plot_ == nullptr) {
            return;
        }
        const QString path = QFileDialog::getSaveFileName(
            this, QStringLiteral("导出画面"), QStringLiteral("waveform.png"),
            QStringLiteral("PNG 图片 (*.png)"));
        if (path.isEmpty()) {
            return;
        }
        if (!plot_->grabPlot().save(path)) {
            QMessageBox::warning(this, QStringLiteral("导出失败"),
                                 QStringLiteral("无法写入该文件，请检查路径权限。"));
        }
    });

    // ---- 操作提示 ----
    auto *hint = new QLabel(
        QStringLiteral("滚轮缩放 X · Ctrl+滚轮缩放 Y\nShift+拖拽平移 Y · 右键框选放大\n单击放游标 · 双击复位"),
        panel);
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral("color: #8FA0AE;"));
    layout->addWidget(hint);

    layout->addStretch(1);

    frame_stats_label_ = new QLabel(QStringLiteral("刷新 -- fps · 绘制 -- ms"), panel);
    frame_stats_label_->setStyleSheet(QStringLiteral("color: #8FA0AE;"));
    layout->addWidget(frame_stats_label_);

    connect(plot_, &uikit::PlotWidget::frameStatsUpdated, this,
            [this](double fps, double paint_ms) {
                if (frame_stats_label_ == nullptr) {
                    return;
                }
                frame_stats_label_->setText(QStringLiteral("刷新 %1 fps · 绘制 %2 ms")
                                                .arg(fps, 0, 'f', 1)
                                                .arg(paint_ms, 0, 'f', 2));
                // 标题同步一份，便于观察当前视图模式与横向跨度
                const QString mode = plot_->isFitAll() ? QStringLiteral("全部")
                                   : plot_->isFollowLatest() ? QStringLiteral("跟随")
                                                              : QStringLiteral("手动");
                const uikit::PlotRange span_range = plot_->xRange();
                setWindowTitle(QStringLiteral("Qt 曲线组件演示 — %1 fps / %2 ms / %3 / %4 s / %5")
                                   .arg(fps, 0, 'f', 0)
                                   .arg(paint_ms, 0, 'f', 1)
                                   .arg(mode)
                                   .arg(static_cast<double>(span_range.span()), 0, 'f', 1)
                                   .arg(last_view_event_));
            });

    auto *stats = new QLabel(
        QStringLiteral("采样 1 kHz · 每帧绘制 ≈ 像素宽 × 2 点/通道\n原始点存环形缓冲，不进绘制"),
        panel);
    stats->setStyleSheet(QStringLiteral("color: #5A6A78;"));
    layout->addWidget(stats);

    return panel;
}

void MainWidget::setupPlot()
{
    if (plot_ == nullptr) {
        return;
    }

    plot_->setCapacityPerChannel(200000); // 每通道 20 万点，约 1.6MB/通道
    plot_->setSampleInterval(kSampleInterval);
    plot_->setTimeWindow(10.0F);
    plot_->setAutoYRange(true);
    plot_->setFollowLatest(true);

    plot_->addChannel(QStringLiteral("CH1 正弦 1.0 Hz"), QColor(0x00, 0xE5, 0x76));
    plot_->addChannel(QStringLiteral("CH2 谐波 2.7 Hz"), QColor(0xFF, 0xD5, 0x4F));
    plot_->addChannel(QStringLiteral("CH3 方波 + 噪声"), QColor(0x40, 0xC4, 0xFF));
}

void MainWidget::applyChannelStyle()
{
    if (plot_ == nullptr || draw_mode_combo_ == nullptr || line_width_combo_ == nullptr
        || point_size_combo_ == nullptr) {
        return;
    }

    // 0 = 线，1 = 点，2 = 线 + 点
    const int mode = draw_mode_combo_->currentData().toInt();
    const int line_width = line_width_combo_->currentData().toInt();
    const int point_size = point_size_combo_->currentData().toInt();

    for (int i = 0; i < plot_->channelCount(); ++i) {
        plot_->setChannelLineVisible(i, mode != 1);
        plot_->setChannelPointVisible(i, mode >= 1);
        plot_->setChannelLineWidth(i, line_width);
        plot_->setChannelPointSize(i, point_size);
    }
}

void MainWidget::appendFrame(const QVector<float> &values)
{
    if (plot_ != nullptr) {
        plot_->appendFrame(values);
    }
}

void MainWidget::addAxisParam(const uikit::ParamRow &row)
{
    if (param_table_ != nullptr) {
        param_table_->addParam(row);
    }
}

void MainWidget::setAxisParamValue(const QString &row_id, const QString &column_id, double value)
{
    if (param_table_ != nullptr) {
        param_table_->setValue(row_id, column_id, value);
    }
}

bool MainWidget::eventFilter(QObject *watched, QEvent *event)
{
    if (watched != plot_ || plot_ == nullptr) {
        return QWidget::eventFilter(watched, event);
    }

    switch (event->type()) {
    case QEvent::Wheel: {
        last_view_event_ = QStringLiteral("wheel");
        auto *wheel = static_cast<QWheelEvent *>(event);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        const QPointF position = wheel->position();
#else
        const QPointF position = wheel->posF();
#endif
        if (!plot_->plotArea().contains(position)) {
            return false;
        }

        const float factor = (wheel->angleDelta().y() > 0) ? 0.8F : 1.25F;

        // Ctrl + 滚轮缩放纵轴，普通滚轮缩放横轴，都以鼠标位置为锚点
        if (wheel->modifiers().testFlag(Qt::ControlModifier)) {
            const float anchor = plot_->pixelToDataY(static_cast<float>(position.y()));
            const uikit::PlotRange range = plot_->yRange();
            plot_->setYRange(anchor + (range.lower - anchor) * factor,
                             anchor + (range.upper - anchor) * factor);
        } else {
            const float anchor = plot_->pixelToDataX(static_cast<float>(position.x()));
            const uikit::PlotRange range = plot_->xRange();
            plot_->setXRange(anchor + (range.lower - anchor) * factor,
                             anchor + (range.upper - anchor) * factor);
        }
        return true;
    }

    case QEvent::MouseButtonPress: {
        last_view_event_ = QStringLiteral("press");
        auto *mouse = static_cast<QMouseEvent *>(event);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        const QPointF position = mouse->position();
#else
        const QPointF position = mouse->localPos();
#endif
        if (!plot_->plotArea().contains(position)) {
            return false;
        }

        if (mouse->button() == Qt::RightButton) {
            rubber_band_ = true;
            rubber_start_ = position;
            plot_->setSelectionRect(QRectF(position, position));
            return true;
        }

        if (mouse->button() == Qt::LeftButton) {
            dragging_ = true;
            drag_last_ = position;
            press_position_ = position;
            press_moved_ = false;
        }
        return false;
    }

    case QEvent::MouseMove: {
        auto *mouse = static_cast<QMouseEvent *>(event);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        const QPointF position = mouse->position();
#else
        const QPointF position = mouse->localPos();
#endif

        if (rubber_band_) {
            plot_->setSelectionRect(QRectF(rubber_start_, position));
            return true;
        }

        if (dragging_) {
            if ((position - press_position_).manhattanLength() > 3.0) {
                press_moved_ = true;
            }

            const QRectF area = plot_->plotArea();

            // Shift + 拖拽平移纵轴，否则平移横轴
            if (mouse->modifiers().testFlag(Qt::ShiftModifier)) {
                const float delta_pixel = static_cast<float>(position.y() - drag_last_.y());
                const float height_pixel = static_cast<float>(std::max(1.0, area.height()));
                const uikit::PlotRange range = plot_->yRange();
                const float delta = delta_pixel * range.span() / height_pixel;
                plot_->setYRange(range.lower + delta, range.upper + delta);
            } else {
                const float delta_pixel = static_cast<float>(position.x() - drag_last_.x());
                const float width_pixel = static_cast<float>(std::max(1.0, area.width()));
                const uikit::PlotRange range = plot_->xRange();
                const float delta = -delta_pixel * range.span() / width_pixel;
                plot_->setXRange(range.lower + delta, range.upper + delta);
            }
            drag_last_ = position;
            return true;
        }

        if (plot_->plotArea().contains(position)) {
            plot_->setHoverX(plot_->pixelToDataX(static_cast<float>(position.x())));
        } else {
            plot_->clearHover();
        }
        return false;
    }

    case QEvent::MouseButtonRelease: {
        auto *mouse = static_cast<QMouseEvent *>(event);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        const QPointF position = mouse->position();
#else
        const QPointF position = mouse->localPos();
#endif

        if (rubber_band_) {
            rubber_band_ = false;
            plot_->clearSelectionRect();

            const QRectF rect = QRectF(rubber_start_, position).normalized();
            if (rect.width() > 8.0 && rect.height() > 8.0) {
                plot_->zoomToRange(plot_->pixelToDataX(static_cast<float>(rect.left())),
                                   plot_->pixelToDataX(static_cast<float>(rect.right())),
                                   plot_->pixelToDataY(static_cast<float>(rect.bottom())),
                                   plot_->pixelToDataY(static_cast<float>(rect.top())));
            }
            return true;
        }

        if (dragging_ && mouse->button() == Qt::LeftButton) {
            dragging_ = false;

            // 位移很小按单击处理：依次放置游标 A、B，之后移动较近的一个
            if (!press_moved_ && plot_->plotArea().contains(position)) {
                const float x = plot_->pixelToDataX(static_cast<float>(position.x()));
                if (!plot_->hasCursor(0)) {
                    plot_->setCursor(0, x);
                } else if (!plot_->hasCursor(1)) {
                    plot_->setCursor(1, x);
                } else if (std::fabs(x - plot_->cursorX(0)) <= std::fabs(x - plot_->cursorX(1))) {
                    plot_->setCursor(0, x);
                } else {
                    plot_->setCursor(1, x);
                }
            }
            return true;
        }
        return false;
    }

    case QEvent::Leave:
        plot_->clearHover();
        return false;

    default:
        break;
    }

    return QWidget::eventFilter(watched, event);
}

void MainWidget::resetView()
{
    if (plot_ == nullptr) {
        return;
    }
    plot_->clearCursors();
    plot_->setYRange(-1.0F, 1.0F);
    plot_->setAutoYRange(true);
    plot_->setFollowLatest(true);
}

void MainWidget::applyDarkPalette()
{
    QPalette palette = this->palette();
    palette.setColor(QPalette::Window, QColor(0x14, 0x19, 0x1E));
    palette.setColor(QPalette::WindowText, QColor(0xC8, 0xD3, 0xDC));
    palette.setColor(QPalette::Base, QColor(0x0F, 0x13, 0x17));
    palette.setColor(QPalette::AlternateBase, QColor(0x1A, 0x20, 0x26));
    palette.setColor(QPalette::Text, QColor(0xC8, 0xD3, 0xDC));
    palette.setColor(QPalette::Button, QColor(0x22, 0x2A, 0x32));
    palette.setColor(QPalette::ButtonText, QColor(0xC8, 0xD3, 0xDC));
    palette.setColor(QPalette::Highlight, QColor(0x2E, 0x7C, 0xD6));
    palette.setColor(QPalette::HighlightedText, QColor(0xFF, 0xFF, 0xFF));
    setPalette(palette);
}
