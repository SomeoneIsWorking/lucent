#include "lucent/rotation.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace lucent::rotation {
namespace {

constexpr int polar_steps = 24;

using Quaternion = std::array<double, 4>; // w, x, y, z

double determinant(const Matrix3 &m) {
  return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
         m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
         m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
}

Matrix3 multiply(const Matrix3 &a, const Matrix3 &b) {
  Matrix3 out{};
  for (std::size_t r = 0; r < 3; ++r) {
    for (std::size_t c = 0; c < 3; ++c) {
      out[r][c] = a[r][0] * b[0][c] + a[r][1] * b[1][c] + a[r][2] * b[2][c];
    }
  }
  return out;
}

Matrix3 transpose(const Matrix3 &m) {
  Matrix3 out{};
  for (std::size_t r = 0; r < 3; ++r) {
    for (std::size_t c = 0; c < 3; ++c) {
      out[r][c] = m[c][r];
    }
  }
  return out;
}

struct Polar {
  Matrix3 turn;
  Matrix3 stretch;
};

// Newton iteration Q <- (Q + Q^-T) / 2.
std::optional<Polar> polar(const Matrix3 &m) {
  if (std::abs(determinant(m)) < 1e-9) {
    return std::nullopt;
  }
  Matrix3 q = m;
  for (int step = 0; step < polar_steps; ++step) {
    const double d = determinant(q);
    // Q^-T is the cofactor matrix over the determinant.
    Matrix3 inverse_transpose{};
    for (std::size_t r = 0; r < 3; ++r) {
      for (std::size_t c = 0; c < 3; ++c) {
        const std::size_t r1 = (r + 1) % 3;
        const std::size_t r2 = (r + 2) % 3;
        const std::size_t c1 = (c + 1) % 3;
        const std::size_t c2 = (c + 2) % 3;
        inverse_transpose[r][c] = (q[r1][c1] * q[r2][c2] - q[r1][c2] * q[r2][c1]) / d;
      }
    }
    double change = 0.0;
    for (std::size_t r = 0; r < 3; ++r) {
      for (std::size_t c = 0; c < 3; ++c) {
        const double next = 0.5 * (q[r][c] + inverse_transpose[r][c]);
        change = std::max(change, std::abs(next - q[r][c]));
        q[r][c] = next;
      }
    }
    if (change < 1e-12) {
      break;
    }
  }
  return Polar{q, multiply(transpose(q), m)};
}

Quaternion quaternion_of(const Matrix3 &m) {
  const double trace = m[0][0] + m[1][1] + m[2][2];
  if (trace > 0.0) {
    const double s = std::sqrt(trace + 1.0) * 2.0;
    return {0.25 * s, (m[2][1] - m[1][2]) / s, (m[0][2] - m[2][0]) / s, (m[1][0] - m[0][1]) / s};
  }
  if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
    const double s = std::sqrt(1.0 + m[0][0] - m[1][1] - m[2][2]) * 2.0;
    return {(m[2][1] - m[1][2]) / s, 0.25 * s, (m[0][1] + m[1][0]) / s, (m[0][2] + m[2][0]) / s};
  }
  if (m[1][1] > m[2][2]) {
    const double s = std::sqrt(1.0 + m[1][1] - m[0][0] - m[2][2]) * 2.0;
    return {(m[0][2] - m[2][0]) / s, (m[0][1] + m[1][0]) / s, 0.25 * s, (m[1][2] + m[2][1]) / s};
  }
  const double s = std::sqrt(1.0 + m[2][2] - m[0][0] - m[1][1]) * 2.0;
  return {(m[1][0] - m[0][1]) / s, (m[0][2] + m[2][0]) / s, (m[1][2] + m[2][1]) / s, 0.25 * s};
}

Matrix3 matrix_of(const Quaternion &q) {
  const auto [w, x, y, z] = q;
  return {{{1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y)},
           {2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x)},
           {2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)}}};
}

// Along the shorter arc from `a` to `b`.
Quaternion slerp(const Quaternion &a, Quaternion b, double t) {
  double dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
  if (dot < 0.0) {
    dot = -dot;
    for (double &component : b) {
      component = -component;
    }
  }
  double wa = 1.0 - t;
  double wb = t;
  if (dot < 0.9995) {
    const double angle = std::acos(std::min(dot, 1.0));
    const double sine = std::sin(angle);
    wa = std::sin((1.0 - t) * angle) / sine;
    wb = std::sin(t * angle) / sine;
  }
  Quaternion out{};
  double norm = 0.0;
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = wa * a[i] + wb * b[i];
    norm += out[i] * out[i];
  }
  norm = std::sqrt(norm);
  for (double &component : out) {
    component /= norm;
  }
  return out;
}

} // namespace

std::optional<Matrix3> between(const Matrix3 &from, const Matrix3 &to, double t) {
  std::optional<Polar> a = polar(from);
  std::optional<Polar> b = polar(to);
  if (!a || !b || (determinant(a->turn) < 0.0) != (determinant(b->turn) < 0.0)) {
    return std::nullopt;
  }
  for (Polar *side : {&*a, &*b}) {
    if (determinant(side->turn) < 0.0) {
      // -Q is a rotation and (-Q)(-S) is the same matrix.
      for (std::size_t r = 0; r < 3; ++r) {
        for (std::size_t c = 0; c < 3; ++c) {
          side->turn[r][c] = -side->turn[r][c];
          side->stretch[r][c] = -side->stretch[r][c];
        }
      }
    }
  }
  Matrix3 stretch{};
  for (std::size_t r = 0; r < 3; ++r) {
    for (std::size_t c = 0; c < 3; ++c) {
      stretch[r][c] = a->stretch[r][c] + (b->stretch[r][c] - a->stretch[r][c]) * t;
    }
  }
  const Matrix3 turn = matrix_of(slerp(quaternion_of(a->turn), quaternion_of(b->turn), t));
  return multiply(turn, stretch);
}

} // namespace lucent::rotation
