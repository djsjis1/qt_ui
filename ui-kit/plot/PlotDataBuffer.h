/**
 * @file PlotDataBuffer.h
 * @brief 曲线数据层：每通道定长环形缓冲 + 按屏幕列聚合（min/max 包络）降采样
 *
 * 设计目标：把"存了多少点"和"每帧画多少点"解耦。
 *
 * 原始采样点按通道写入固定容量的环形缓冲，写满后覆盖最旧数据，内存占用恒定；
 * 绘制前调用 downsample()，把时间窗内的所有样本压缩成"每列两点"（最小值、最大值）。
 * 于是每帧绘制量只与绘图区像素宽度相关，与采样率和缓冲总量无关。
 *
 * 复杂度一览（n = 缓冲内样本数，w = 时间窗内样本数，c = 列数）：
 *   append        O(1)
 *   downsample    O(log n + w)   二分找起点后只扫窗口内数据
 *   valueAt       O(w)           游标是低频交互，直接线性找最近样本
 *   timeRange     O(通道数)      只读首尾两个下标，不扫数据
 *   valueRange    O(n)           全量遍历，只在"全部"视图切换时用
 *
 * 使用方式：
 * @code
 *   uikit::PlotDataBuffer buffer(200000);   // 每通道保留 20 万点
 *   buffer.setChannelCount(3);
 *   buffer.append(0, t, value);
 *
 *   QVector<float> xy;                       // 交错存 x,y
 *   buffer.downsample(0, t0, t1, columns, xy);
 * @endcode
 *
 * 线程约束：非线程安全，只在 GUI 线程使用。
 */

#pragma once

#include <cstddef>
#include <vector>

#include <QVector>
#include <QtGlobal>

namespace uikit {

/**
 * @brief 多通道环形采样缓冲
 */
class PlotDataBuffer
{
public:
    /**
     * @brief 构造
     * @param capacity_per_channel 每通道保留的采样点数上限，内存约为
     *        通道数 × 容量 × 8 字节
     */
    explicit PlotDataBuffer(int capacity_per_channel = 200000);

    /**
     * @brief 调整通道数
     * @note 已有通道的数据保留，新增通道为空；减少通道数会丢弃末尾通道的数据
     */
    void setChannelCount(int count);

    int channelCount() const noexcept;
    int capacityPerChannel() const noexcept;

    /**
     * @brief 写入一个采样点
     * @note 通道越界时忽略；x 应保持单调递增，降采样依赖该前提做提前退出
     */
    void append(int channel, float x, float y);

    // ---- 按序号写入（每通道各自独立序号）----

    /// 采样周期（秒），appendBySeq 用它把序号差换算成横轴坐标
    void setSampleInterval(float seconds);
    float sampleInterval() const noexcept;

    /**
     * @brief 按序号写入单通道采样点
     * @param seq 调用方给出的递增序号（无符号），同一通道内必须递增
     * @return false 表示该点被丢弃（序号重复/回退，或早于全局基准序号）
     * @note 序号差 > 1 视为丢包：横轴按差值跳过，绘制层据此断开曲线
     */
    bool appendBySeq(int channel, quint64 seq, float y);

    /// 该通道是否已收到过序号
    bool hasSequence(int channel) const noexcept;
    /// 该通道最后接受的序号
    quint64 lastSequence(int channel) const noexcept;
    /// 该通道最近一个样本的横轴坐标；无样本时返回 0
    float lastSampleX(int channel) const noexcept;

    /// 清空全部通道数据，保留容量与通道数
    void clear();

    /// 某通道当前样本数
    int sampleCount(int channel) const noexcept;

    /**
     * @brief 把 [x0, x1] 内的样本按 columns 列聚合为 min/max 包络
     * @param out_xy 输出交错排列的 x,y；每列最多 2 点（先最小值、后最大值）
     * @note 复杂度 O(时间窗内样本数)，与缓冲总容量无关
     */
    void downsample(int channel, float x0, float x1, int columns, QVector<float> &out_xy) const;

    /**
     * @brief 取 x 处最近样本的 y 值，供游标读数使用
     * @return 找到样本返回 true
     * @note 复杂度 O(时间窗内样本数)；游标是低频交互，不需要二分
     */
    bool valueAt(int channel, float x, float &out_y) const;

    /**
     * @brief 按"自最旧样本起"的序号取样本
     * @return 下标有效返回 true
     * @note 用于导出：各通道由 appendFrame 同步写入时，同一序号对应同一时刻
     */
    bool sampleByIndex(int channel, int index, float &out_x, float &out_y) const;

    /**
     * @brief 全部样本的时间范围（合并所有通道）
     * @return 至少有一个样本时返回 true
     */
    bool timeRange(float &out_lower, float &out_upper) const;

    /**
     * @brief 某通道全部样本的值范围
     * @return 该通道有样本时返回 true
     */
    bool valueRange(int channel, float &out_lower, float &out_upper) const;

private:
    struct ChannelStorage
    {
        std::vector<float> xs;
        std::vector<float> ys;
        std::size_t head {0};  ///< 下一个写入位置
        std::size_t count {0}; ///< 当前样本数，不超过容量

        quint64 last_seq {0}; ///< 该通道最后一次接受的序号
        quint64 seq_base {0}; ///< 该通道首个序号，作为本通道时间轴起点
        bool has_seq {false}; ///< 是否已收到过序号
    };

    std::vector<ChannelStorage> channels_;
    int capacity_per_channel_ {0};

    float sample_interval_ {0.01F}; ///< 序号换算横轴用的采样周期
};

}  // namespace uikit
