#pragma once
#include <algorithm>
#include <cstdlib>

inline constexpr int kMaxBrushSize = 1024;
struct PaintRegion
{
    int left, top, right, bottom;
    bool empty() const
    {
        return left >= right || top >= bottom;
    }
};
inline PaintRegion brushRegion(int x, int y, int size, int width, int height)
{
    size = std::clamp(size, 1, kMaxBrushSize);
    const int left = x - size / 2, top = y - size / 2;
    return {std::clamp(left, 0, width), std::clamp(top, 0, height), std::clamp(left + size, 0, width),
            std::clamp(top + size, 0, height)};
}
// Integer interpolation keeps fast pointer motion connected at every zoom.
// Starting points are already painted on mouse-down or the previous event.
template <class Paint> void continuePaintLine(int x, int y, int targetX, int targetY, Paint paint)
{
    const int dx = std::abs(targetX - x), dy = std::abs(targetY - y);
    const int stepX = x < targetX ? 1 : -1, stepY = y < targetY ? 1 : -1;
    int error = dx - dy;
    while (x != targetX || y != targetY)
    {
        const int twiceError = error * 2;
        if (twiceError > -dy)
        {
            error -= dy;
            x += stepX;
        }
        if (twiceError < dx)
        {
            error += dx;
            y += stepY;
        }
        paint(x, y);
    }
}

// Merge collinear square stamps into rectangles before CPU filling/GPU dispatch.
template <class Paint>
void continueBrushLine(int x, int y, int targetX, int targetY, int size, int width, int height, Paint paint)
{
    PaintRegion run{};
    bool pending = false;
    const auto emit = [&](PaintRegion next) {
        if (next.empty())
            return;
        const bool horizontal = pending && run.top == next.top && run.bottom == next.bottom && next.left <= run.right &&
                                next.right >= run.left;
        const bool vertical = pending && run.left == next.left && run.right == next.right && next.top <= run.bottom &&
                              next.bottom >= run.top;
        if (horizontal || vertical)
            run = {std::min(run.left, next.left), std::min(run.top, next.top), std::max(run.right, next.right),
                   std::max(run.bottom, next.bottom)};
        else
        {
            if (pending)
                paint(run);
            run = next;
            pending = true;
        }
    };
    PaintRegion previous{};
    bool stamped = false;
    continuePaintLine(x, y, targetX, targetY, [&](int px, int py) {
        const auto next = brushRegion(px, py, size, width, height);
        if (next.empty())
            return;
        if (!stamped || next.right <= previous.left || next.left >= previous.right || next.bottom <= previous.top ||
            next.top >= previous.bottom)
            emit(next);
        else
        {
            // Every stamp in this segment has the same ink. Paint only the
            // exposed strips, so large diagonal brushes avoid quadratic overdraw.
            emit({next.left, next.top, next.right, std::min(next.bottom, previous.top)});
            emit({next.left, std::max(next.top, previous.bottom), next.right, next.bottom});
            const int top = std::max(next.top, previous.top), bottom = std::min(next.bottom, previous.bottom);
            emit({next.left, top, std::min(next.right, previous.left), bottom});
            emit({std::max(next.left, previous.right), top, next.right, bottom});
        }
        previous = next;
        stamped = true;
    });
    if (pending)
        paint(run);
}
