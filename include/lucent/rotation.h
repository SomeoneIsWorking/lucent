#pragma once

#include <array>
#include <optional>

namespace lucent::rotation {

// Row-major 3x3 matrix.
using Matrix3 = std::array<std::array<double, 3>, 3>;

// The matrix `t` of the way from `from` to `to`: the rotation of each polar decomposition (M = Q *
// S) slerped along the shorter arc, the symmetric stretch blended linearly. Nothing when either
// matrix is singular or one is a mirror and the other a rotation, since no turn leads between them.
std::optional<Matrix3> between(const Matrix3 &from, const Matrix3 &to, double t);

} // namespace lucent::rotation
