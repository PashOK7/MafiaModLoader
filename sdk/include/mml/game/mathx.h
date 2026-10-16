// Small vector math in the engine's convention: row vectors, p_world = p_local * M,
// rows of a rotation matrix are the frame's axes expressed in the parent space.
#pragma once
#include <cmath>

struct V3 {
    float x, y, z;
    V3 operator+(const V3 &o) const { return {x + o.x, y + o.y, z + o.z}; }
    V3 operator-(const V3 &o) const { return {x - o.x, y - o.y, z - o.z}; }
    V3 operator*(float s) const { return {x * s, y * s, z * s}; }
    V3 operator-() const { return {-x, -y, -z}; }
    V3 &operator+=(const V3 &o) { x += o.x; y += o.y; z += o.z; return *this; }
    V3 &operator-=(const V3 &o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
};
inline float Dot(const V3 &a, const V3 &b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 Cross(const V3 &a, const V3 &b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float Len(const V3 &a) { return std::sqrt(Dot(a, a)); }
inline V3 Norm(const V3 &a, const V3 &fallback = {0, 1, 0}) { float l = Len(a); return l > 1e-6f ? a * (1.0f / l) : fallback; }

struct Q { float w, x, y, z; };

// A unit quaternion's rotation as an engine 4x4 (row vectors), by LS3D's own formula (S_quat::RotationMatrix,
// LS3DF 1.2 0x1002EC00) but for every quaternion: LS3D's gives the identity when |w| rounds to 1, which drops every
// turn under about 0.00049 rad (game_build.h quatRotationMatrix). Where LS3D builds it, the same matrix to float
// precision (fps_unlock checks it against the game's function at its load).
inline void QuatMatrix(const Q &q, float out[16]) {
    const float x2 = q.x + q.x, y2 = q.y + q.y, z2 = q.z + q.z;
    const float xx = x2 * q.x, xy = y2 * q.x, xz = z2 * q.x, yy = y2 * q.y, yz = z2 * q.y, zz = z2 * q.z;
    const float wx = x2 * q.w, wy = y2 * q.w, wz = z2 * q.w;
    const float m[16] = {1.0f - (zz + yy), xy - wz, wy + xz, 0,
                         wz + xy, 1.0f - (zz + xx), yz - wx, 0,
                         xz - wy, wx + yz, 1.0f - (yy + xx), 0,
                         0, 0, 0, 1.0f};
    for (int i = 0; i < 16; i++) out[i] = m[i];
}

// 3x3 matrix stored as three rows.
struct M3 {
    V3 r[3];
    static M3 Identity() { return {{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}}; }
    static M3 FromWorld(const float *m) {  // rotation rows of an engine 4x4, scale removed
        return {{Norm({m[0], m[1], m[2]}, {1, 0, 0}), Norm({m[4], m[5], m[6]}), Norm({m[8], m[9], m[10]}, {0, 0, 1})}};
    }
    static M3 Raw(const float *m) { return {{{m[0], m[1], m[2]}, {m[4], m[5], m[6]}, {m[8], m[9], m[10]}}}; }
    V3 Col(int c) const { return c == 0 ? V3{r[0].x, r[1].x, r[2].x} : c == 1 ? V3{r[0].y, r[1].y, r[2].y} : V3{r[0].z, r[1].z, r[2].z}; }
    M3 T() const { return {{Col(0), Col(1), Col(2)}}; }
    M3 operator*(const M3 &b) const {  // (this * b)
        M3 o;
        for (int i = 0; i < 3; i++) o.r[i] = b.r[0] * r[i].x + b.r[1] * r[i].y + b.r[2] * r[i].z;
        return o;
    }
    V3 Apply(const V3 &v) const { return r[0] * v.x + r[1] * v.y + r[2] * v.z; }  // v * M
    M3 Inverse() const {
        V3 c0 = Cross(r[1], r[2]), c1 = Cross(r[2], r[0]), c2 = Cross(r[0], r[1]);
        float det = Dot(r[0], c0);
        float inv = std::fabs(det) > 1e-12f ? 1.0f / det : 0.0f;
        M3 adjT{{c0 * inv, c1 * inv, c2 * inv}};
        return adjT.T();
    }
    void Orthonormalize() {
        r[0] = Norm(r[0], {1, 0, 0});
        r[2] = Norm(Cross(r[0], r[1]), {0, 0, 1});
        r[1] = Cross(r[2], r[0]);
    }
};

// The quaternion of a rotation (rows: the axes) as QuatMatrix lays it out (Shepperd's method), unit length.
inline Q QuatOf(const M3 &m) {
    float m00 = m.r[0].x, m01 = m.r[0].y, m02 = m.r[0].z, m10 = m.r[1].x, m11 = m.r[1].y, m12 = m.r[1].z, m20 = m.r[2].x, m21 = m.r[2].y, m22 = m.r[2].z;
    float tr = m00 + m11 + m22;
    Q q;
    if (tr > 0) {
        float s = std::sqrt(tr + 1.0f) * 2;
        q = {0.25f * s, (m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s};
    } else if (m00 > m11 && m00 > m22) {
        float s = std::sqrt(1.0f + m00 - m11 - m22) * 2;
        q = {(m21 - m12) / s, 0.25f * s, (m01 + m10) / s, (m02 + m20) / s};
    } else if (m11 > m22) {
        float s = std::sqrt(1.0f + m11 - m00 - m22) * 2;
        q = {(m02 - m20) / s, (m01 + m10) / s, 0.25f * s, (m12 + m21) / s};
    } else {
        float s = std::sqrt(1.0f + m22 - m00 - m11) * 2;
        q = {(m10 - m01) / s, (m02 + m20) / s, (m12 + m21) / s, 0.25f * s};
    }
    float n = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
    return n > 1e-9f ? Q{q.w / n, q.x / n, q.y / n, q.z / n} : Q{1, 0, 0, 0};
}

// Basis whose first row points a->b and whose third row is normal to the plane (a,b,c).
inline M3 BasisFrom(const V3 &a, const V3 &b, const V3 &c) {
    V3 x = Norm(b - a, {1, 0, 0});
    V3 z = Norm(Cross(x, c - a), {0, 0, 1});
    if (Len(Cross(x, c - a)) < 1e-6f) {  // degenerate: pick any perpendicular
        z = Norm(Cross(x, std::fabs(x.y) < 0.9f ? V3{0, 1, 0} : V3{1, 0, 0}));
    }
    V3 y = Cross(z, x);
    return {{x, y, z}};
}
