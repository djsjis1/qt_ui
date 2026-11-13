/**
 * @file main.cpp
 * @brief 演示程序入口：创建主窗口，并用定时器模拟采集数据源与轴参数控制器
 *
 * 数据源在 main() 里：10 ms 一批、每批 10 点，合计 1 kHz，
 * 合成三路信号（正弦 / 谐波 / 方波+噪声）喂给 MainWidget::appendFrame()。
 * 轴参数矩阵同样在这里：列=轴（X/Y/Z）、行=参数，列定义、行定义、初值与
 * 合法范围都由 MockAxisController 提供，界面只负责展示与转发。
 * 接真实设备时，把合成逻辑与 MockAxisController 替换为采集/设备接口即可，控件侧无需改动。
 */

#include <QApplication>
#include <QHash>
#include <QRandomGenerator>
#include <QString>
#include <QTimer>
#include <QVector>

#include <cmath>

#include "mainwidget.h"
#include "ParamTableTypes.h"

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kTimerIntervalMs = 10;      ///< 定时器周期：每 10 ms 投递一批
constexpr int kSamplesPerTick = 10;       ///< 每批点数，合计 1 kHz
constexpr double kSampleInterval = 0.001; ///< 采样周期（秒）

/// 方波上叠加的小幅噪声，用来验证降采样不会抹掉尖峰
float randomNoise()
{
    return static_cast<float>(QRandomGenerator::global()->bounded(2000)) / 10000.0F - 0.1F;
}

/**
 * @brief 一列数据源（轴）：表格的一个数据列
 */
struct AxisColumnSpec
{
    QString id;   ///< 底层轴键
    QString name; ///< 显示名称
};

/**
 * @brief 一行参数：范围/精度 + 每个轴的初值
 */
struct ParamRowSpec
{
    QString id;              ///< 底层参数键
    QString name;            ///< 显示名称
    QString unit;            ///< 单位
    double minimum;          ///< 设备允许下界
    double maximum;          ///< 设备允许上界
    int decimals;            ///< 显示精度
    QVector<double> initial; ///< 各轴初值，顺序与 axisColumnSpecs() 一致
};

/**
 * @brief 轴列定义表（唯一来源：表格列与勾选框都读这里）
 */
const QVector<AxisColumnSpec> &axisColumnSpecs()
{
    static const QVector<AxisColumnSpec> specs = {
        {QStringLiteral("x"), QStringLiteral("X 轴")},
        {QStringLiteral("y"), QStringLiteral("Y 轴")},
        {QStringLiteral("z"), QStringLiteral("Z 轴")},
    };
    return specs;
}

/**
 * @brief 参数行定义表（唯一来源：表格行定义与底层校验都读这里）
 */
const QVector<ParamRowSpec> &paramRowSpecs()
{
    static const QVector<ParamRowSpec> specs = {
        {QStringLiteral("feed"), QStringLiteral("进给速度"), QStringLiteral("mm/min"), 0.0, 5000.0,
         1, {1200.0, 1200.0, 600.0}},
        {QStringLiteral("accel"), QStringLiteral("加速度"), QStringLiteral("mm/s²"), 10.0, 5000.0, 0,
         {800.0, 800.0, 500.0}},
        {QStringLiteral("jog"), QStringLiteral("点动步距"), QStringLiteral("mm"), 0.01, 10.0, 2,
         {1.0, 1.0, 0.5}},
    };
    return specs;
}

/**
 * @brief 模拟底层轴控制器：按（轴, 参数）保存当前值，写入时做设备侧校验与量化
 *
 * 真实设备把这里换成 SDK 调用即可；界面侧无需改动。
 */
class MockAxisController
{
public:
    MockAxisController()
    {
        const QVector<AxisColumnSpec> &axes = axisColumnSpecs();
        for (int axis_index = 0; axis_index < axes.size(); ++axis_index) {
            for (const ParamRowSpec &row : paramRowSpecs()) {
                values_.insert(key(axes.at(axis_index).id, row.id),
                               row.initial.value(axis_index, row.minimum));
            }
        }
    }

    /// 轮询读取：返回某轴某参数的当前值，未知键返回 0
    double read(const QString &axis_id, const QString &param_id) const
    {
        return values_.value(key(axis_id, param_id), 0.0);
    }

    /// 写入某轴某参数；轴或参数未知、超出设备允许范围时返回 false
    bool write(const QString &axis_id, const QString &param_id, double value)
    {
        const ParamRowSpec *spec = findRowSpec(param_id);
        if (spec == nullptr || value < spec->minimum || value > spec->maximum) {
            return false;
        }
        // 模拟设备侧量化：按参数显示精度取整后再落盘
        const double scale = std::pow(10.0, spec->decimals);
        values_[key(axis_id, param_id)] = std::round(value * scale) / scale;
        return true;
    }

private:
    static QString key(const QString &axis_id, const QString &param_id)
    {
        return axis_id + QLatin1Char('/') + param_id;
    }

    static const ParamRowSpec *findRowSpec(const QString &param_id)
    {
        for (const ParamRowSpec &spec : paramRowSpecs()) {
            if (spec.id == param_id) {
                return &spec;
            }
        }
        return nullptr;
    }

    QHash<QString, double> values_;
};

}  // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    MainWidget widget;
    widget.show();

    // 轴参数矩阵：列=轴、行=参数，行列定义与合法范围都来自数据源侧
    MockAxisController axis;

    for (const AxisColumnSpec &column : axisColumnSpecs()) {
        widget.addAxisColumn(column.id, column.name); // 面板同步生成该列的勾选框
    }

    for (const ParamRowSpec &spec : paramRowSpecs()) {
        uikit::ParamRow row;
        row.id = spec.id;
        row.name = spec.name;
        row.unit = spec.unit;
        row.minimum = spec.minimum;
        row.maximum = spec.maximum;
        row.decimals = spec.decimals;
        widget.addAxisParam(row);
    }

    // 首屏初值：不等第一个轮询周期，窗口显示时表格已经是底层真实值
    for (const AxisColumnSpec &column : axisColumnSpecs()) {
        for (const ParamRowSpec &spec : paramRowSpecs()) {
            widget.setAxisParamValue(spec.id, column.id, axis.read(column.id, spec.id));
        }
    }

    // 表格按自身周期发轮询请求，这里读底层后按行列回填；本连接建立前的首轮请求可丢弃
    QObject::connect(&widget, &MainWidget::axisParamPollRequested, &widget, [&widget, &axis]() {
        for (const AxisColumnSpec &column : axisColumnSpecs()) {
            for (const ParamRowSpec &spec : paramRowSpecs()) {
                widget.setAxisParamValue(spec.id, column.id, axis.read(column.id, spec.id));
            }
        }
    });

    // 双击提交的参数直接作为底层写函数的入参；下一行的回填体现了写入结果，
    // 设备拒绝时读回的仍是旧值，界面随之回滚
    QObject::connect(&widget, &MainWidget::axisParamEdited, &widget,
                     [&widget, &axis](const QString &row_id, const QString &column_id, double value) {
                         axis.write(column_id, row_id, value);
                         widget.setAxisParamValue(row_id, column_id, axis.read(column_id, row_id));
                     });

    // 模拟采集线程：定时器按 1 kHz 往控件里写数据
    double phase = 0.0;
    QTimer sample_timer;
    sample_timer.setTimerType(Qt::PreciseTimer);
    QObject::connect(&sample_timer, &QTimer::timeout, &widget, [&widget, &phase]() {
        for (int i = 0; i < kSamplesPerTick; ++i) {
            const float ch1 = static_cast<float>(0.8 * std::sin(2.0 * kPi * 1.0 * phase));
            const float ch2 = static_cast<float>(0.5 * std::sin(2.0 * kPi * 2.7 * phase + 0.6));
            const float square = (std::sin(2.0 * kPi * 0.5 * phase) >= 0.0) ? 0.45F : -0.45F;

            widget.appendFrame({ch1, ch2, square + randomNoise()});
            phase += kSampleInterval;
        }
    });
    sample_timer.start(kTimerIntervalMs);

    return app.exec();
}
