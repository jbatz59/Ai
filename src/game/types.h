#pragma once
#include <cmath>

namespace cg::game {

struct Vec3 {
  float x = 0, y = 0, z = 0;
  Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
  Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
  Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
  float Dot(const Vec3& o) const { return x * o.x + y * o.y + z * o.z; }
  float Length() const { return std::sqrt(Dot(*this)); }
  Vec3 Normalized() const { float l = Length(); return l > 1e-6f ? *this * (1.0f / l) : Vec3{}; }
};

// Row-major 4x4 (m[row][col]); WorldToScreen expects a combined view*projection matrix where a
// point p transforms as clip = p.x*row0 + p.y*row1 + p.z*row2 + row3 (DirectX convention).
struct Mat4 {
  float m[4][4]{};
};

// Mafia: DE world axes (Illusion Engine): X/Y horizontal plane, Z up.
inline constexpr Vec3 kUp{0, 0, 1};

}  // namespace cg::game
