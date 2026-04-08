// basement.h

#ifndef BASEMENT_H
#define BASEMENT_H

#include <cstdint>
#include <array>

inline constexpr int MAP_ROWS = 109;
inline constexpr int MAP_COLS = 59;
using TileMap = std::array<std::array<uint8_t, MAP_COLS>, MAP_ROWS>;

extern const TileMap map;

#endif // BASEMENT_H
