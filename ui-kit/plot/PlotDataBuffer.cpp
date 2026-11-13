/**
 * @file PlotDataBuffer.cpp
 * @brief PlotDataBuffer 的实现：环形写入与按列 min/max 聚合
 */

#include "PlotDataBuffer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace uikit {

PlotDataBuffer::PlotDataBuffer(int capacity_per_channel)
    : capacity_per_channel_(std::max(capacity_per_channel, 1))
{
}

void PlotDataBuffer::setChannelCount(int count)
{
    const int safe_count = std::max(count, 0);
    if (safe_count == static_cast<int>(channels_.size())) {
        return;
    }

    channels_.resize(static_cast<std::size_t>(safe_count));
    for (ChannelStorage &storage : channels_) {
        if (storage.xs.empty()) {
            storage.xs.resize(static_cast<std::size_t>(capacity_per_channel_));
            storage.ys.resize(static_cast<std::size_t>(capacity_per_channel_));
        }
    }
}

int PlotDataBuffer::channelCount() const noexcept
{
    return static_cast<int>(channels_.size());
}

int PlotDataBuffer::capacityPerChannel() const noexcept
{
    return capacity_per_channel_;
}

void PlotDataBuffer::append(int channel, float x, float y)
{
    if (channel < 0 || channel >= channelCount()) {
        return;
    }

    ChannelStorage &storage = channels_[static_cast<std::size_t>(channel)];
    if (storage.xs.empty()) {
        return;
    }

    storage.xs[storage.head] = x;
    storage.ys[storage.head] = y;
    storage.head = (storage.head + 1) % storage.xs.size();
    if (storage.count < storage.xs.size()) {
        ++storage.count;
    }
}

void PlotDataBuffer::clear()
{
    for (ChannelStorage &storage : channels_) {
        storage.head = 0;
        storage.count = 0;
        storage.last_seq = 0;
        storage.seq_base = 0;
        storage.has_seq = false;
    }
}

void PlotDataBuffer::setSampleInterval(float seconds)
{
    sample_interval_ = std::max(seconds, 1.0e-6F);
}

float PlotDataBuffer::sampleInterval() const noexcept
{
    return sample_interval_;
}

bool PlotDataBuffer::appendBySeq(int channel, quint64 seq, float y)
{
    if (channel < 0 || channel >= channelCount()) {
        return false;
    }

    ChannelStorage &storage = channels_[static_cast<std::size_t>(channel)];
    if (storage.xs.empty()) {
        return false;
    }

    // 序号重复或回退：直接丢弃。环形缓冲的降采样靠 x 有序做提前退出，
    // 放进一条更早的数据会让它之后的可视数据整段消失
    if (storage.has_seq && seq <= storage.last_seq) {
        return false;
    }

    // 每个通道以自己收到的第一个序号为起点：各通道独立计数、同时开始时天然对齐
    if (!storage.has_seq) {
        storage.seq_base = seq;
    }

    // 用与该通道起点的差值换算时间：差值本身很小，float 能精确表示算出的 x；
    // 绝对序号无论多大都不影响这一步的精度
    const double offset = static_cast<double>(seq - storage.seq_base);
    const float x = static_cast<float>(offset * static_cast<double>(sample_interval_));

    storage.last_seq = seq;
    storage.has_seq = true;

    append(channel, x, y);
    return true;
}

bool PlotDataBuffer::hasSequence(int channel) const noexcept
{
    if (channel < 0 || channel >= channelCount()) {
        return false;
    }
    return channels_[static_cast<std::size_t>(channel)].has_seq;
}

quint64 PlotDataBuffer::lastSequence(int channel) const noexcept
{
    if (channel < 0 || channel >= channelCount()) {
        return 0;
    }
    return channels_[static_cast<std::size_t>(channel)].last_seq;
}

float PlotDataBuffer::lastSampleX(int channel) const noexcept
{
    if (channel < 0 || channel >= channelCount()) {
        return 0.0F;
    }

    const ChannelStorage &storage = channels_[static_cast<std::size_t>(channel)];
    if (storage.count == 0 || storage.xs.empty()) {
        return 0.0F;
    }

    const std::size_t newest = (storage.head + storage.xs.size() - 1) % storage.xs.size();
    return storage.xs[newest];
}

int PlotDataBuffer::sampleCount(int channel) const noexcept
{
    if (channel < 0 || channel >= channelCount()) {
        return 0;
    }
    return static_cast<int>(channels_[static_cast<std::size_t>(channel)].count);
}

/**
 * @brief 把时间窗内的样本按屏幕像素列压成 min/max 包络
 *
 * 为什么这么做：波形每秒可能有百万个采样点，逐点画既画不完、也看不出细节。
 * 屏幕横向只有约一千列像素，每列能表达的其实只有"这一段数据的上下边界"。
 * 于是把每列覆盖的所有样本合并成一个最小值和最大值，用一条竖线表示，
 * 每帧绘制量就与采样率、缓冲总量彻底无关，只与绘图区宽度有关。
 *
 * 三个实现要点：
 *   1. 二分定位首个 x >= x0 的样本。时间窗通常只占缓冲的一小段，
 *      从最旧样本线性扫会把窗口外的大多数数据白遍历一遍。
 *   2. 环形缓冲的回绕点把样本切成两段分别顺序遍历，避免每个样本做一次取模。
 *   3. 输出严格按列号递增，绘制层依赖这个顺序做二分定位（见 fillSpansTiled）。
 */
void PlotDataBuffer::downsample(int channel, float x0, float x1, int columns,
                                QVector<float> &out_xy) const
{
    out_xy.clear();
    if (columns <= 0 || !(x1 > x0)) {
        return;
    }
    if (channel < 0 || channel >= channelCount()) {
        return;
    }

    const ChannelStorage &storage = channels_[static_cast<std::size_t>(channel)];
    if (storage.count == 0) {
        return;
    }

    const std::size_t capacity = storage.xs.size();
    const float column_width = (x1 - x0) / static_cast<float>(columns);

    std::vector<float> column_min(static_cast<std::size_t>(columns), 0.0F);
    std::vector<float> column_max(static_cast<std::size_t>(columns), 0.0F);
    std::vector<std::uint8_t> column_used(static_cast<std::size_t>(columns), 0U);

    // 环形缓冲里最旧的样本下标；样本按时间递增写入，回绕后仍然有序
    const std::size_t oldest = (storage.head + capacity - storage.count) % capacity;

    // 回绕点把样本切成连续两段，分别顺序遍历，避免每个点都做一次取模
    const std::size_t first_length = std::min(storage.count, capacity - oldest);

    const auto accumulate = [&](std::size_t index) {
        const float x = storage.xs[index];
        if (x < x0) {
            return true;
        }
        if (x > x1) {
            return false;
        }

        const float y = storage.ys[index];
        const int column = std::clamp(static_cast<int>((x - x0) / column_width), 0, columns - 1);
        const std::size_t slot = static_cast<std::size_t>(column);

        if (column_used[slot] == 0U) {
            column_used[slot] = 1U;
            column_min[slot] = y;
            column_max[slot] = y;
        } else {
            column_min[slot] = std::min(column_min[slot], y);
            column_max[slot] = std::max(column_max[slot], y);
        }
        return true;
    };

    // 二分定位第一个 x >= x0 的样本：时间窗通常只占缓冲的一小段，
    // 从最旧样本线性扫会白遍历窗口外的绝大多数数据
    const std::size_t first_end = std::min(storage.count, first_length);
    std::size_t start = 0;
    {
        std::size_t lo = 0;
        std::size_t hi = storage.count;
        while (lo < hi) {
            const std::size_t mid = lo + (hi - lo) / 2;
            const std::size_t index = (mid < first_end) ? (oldest + mid) : (mid - first_length);
            if (storage.xs[index] < x0) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        start = lo;
    }

    bool keep_going = true;
    for (std::size_t i = start; i < first_end && keep_going; ++i) {
        keep_going = accumulate(oldest + i);
    }
    for (std::size_t i = std::max(start, first_length); i < storage.count && keep_going; ++i) {
        keep_going = accumulate(i - first_length);
    }

    for (int column = 0; column < columns; ++column) {
        const std::size_t slot = static_cast<std::size_t>(column);
        if (column_used[slot] == 0U) {
            continue;
        }
        const float x = x0 + (static_cast<float>(column) + 0.5F) * column_width;
        out_xy.append(x);
        out_xy.append(column_min[slot]);
        out_xy.append(x);
        out_xy.append(column_max[slot]);
    }
}

bool PlotDataBuffer::valueAt(int channel, float x, float &out_y) const
{
    if (channel < 0 || channel >= channelCount()) {
        return false;
    }

    const ChannelStorage &storage = channels_[static_cast<std::size_t>(channel)];
    if (storage.count == 0) {
        return false;
    }

    const std::size_t capacity = storage.xs.size();
    const std::size_t oldest = (storage.head + capacity - storage.count) % capacity;
    const std::size_t first_length = std::min(storage.count, capacity - oldest);

    bool found = false;
    float best_distance = 0.0F;

    const auto probe = [&](std::size_t index) {
        const float distance = std::fabs(storage.xs[index] - x);
        if (!found || distance < best_distance) {
            best_distance = distance;
            out_y = storage.ys[index];
            found = true;
        }
    };

    for (std::size_t i = 0; i < first_length; ++i) {
        probe(oldest + i);
    }
    for (std::size_t i = first_length; i < storage.count; ++i) {
        probe(i - first_length);
    }
    return found;
}

bool PlotDataBuffer::sampleByIndex(int channel, int index, float &out_x, float &out_y) const
{
    if (channel < 0 || channel >= channelCount() || index < 0) {
        return false;
    }

    const ChannelStorage &storage = channels_[static_cast<std::size_t>(channel)];
    if (static_cast<std::size_t>(index) >= storage.count) {
        return false;
    }

    const std::size_t capacity = storage.xs.size();
    const std::size_t oldest = (storage.head + capacity - storage.count) % capacity;
    const std::size_t slot = (oldest + static_cast<std::size_t>(index)) % capacity;

    out_x = storage.xs[slot];
    out_y = storage.ys[slot];
    return true;
}

bool PlotDataBuffer::timeRange(float &out_lower, float &out_upper) const
{
    bool found = false;
    float lower = 0.0F;
    float upper = 0.0F;

    for (const ChannelStorage &storage : channels_) {
        if (storage.count == 0) {
            continue;
        }

        const std::size_t capacity = storage.xs.size();
        const std::size_t oldest = (storage.head + capacity - storage.count) % capacity;
        const std::size_t newest = (storage.head + capacity - 1) % capacity;

        lower = found ? std::min(lower, storage.xs[oldest]) : storage.xs[oldest];
        upper = found ? std::max(upper, storage.xs[newest]) : storage.xs[newest];
        found = true;
    }

    if (found) {
        out_lower = lower;
        out_upper = upper;
    }
    return found;
}

bool PlotDataBuffer::valueRange(int channel, float &out_lower, float &out_upper) const
{
    if (channel < 0 || channel >= channelCount()) {
        return false;
    }

    const ChannelStorage &storage = channels_[static_cast<std::size_t>(channel)];
    if (storage.count == 0) {
        return false;
    }

    const std::size_t capacity = storage.xs.size();
    const std::size_t oldest = (storage.head + capacity - storage.count) % capacity;
    const std::size_t first_length = std::min(storage.count, capacity - oldest);

    float lower = storage.ys[oldest];
    float upper = lower;
    const auto scan = [&](std::size_t index) {
        lower = std::min(lower, storage.ys[index]);
        upper = std::max(upper, storage.ys[index]);
    };

    for (std::size_t i = 0; i < first_length; ++i) {
        scan(oldest + i);
    }
    for (std::size_t i = first_length; i < storage.count; ++i) {
        scan(i - first_length);
    }

    out_lower = lower;
    out_upper = upper;
    return true;
}

}  // namespace uikit
