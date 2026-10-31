/* Water's captured triangles split on a world-aligned grid before GPU tessellation.
 * Clipping preserves the original footprint and level; shared grid intersections use the same coordinates.
 * No camera-dependent CPU mesh rebuild or displaced vertices are read back from the GPU. */
#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

namespace mml { namespace water {
struct MeshPoint { float x, y, z; };

inline std::vector<MeshPoint> ClipWaterCell(const std::vector<MeshPoint> &in, int axis, float edge, bool greater) {
    std::vector<MeshPoint> out;
    if (in.empty()) return out;
    auto coord = [axis](const MeshPoint &p) { return axis == 0 ? p.x : p.z; };
    MeshPoint a = in.back();
    bool ia = greater ? coord(a) >= edge : coord(a) <= edge;
    for (const auto &b : in) {
        bool ib = greater ? coord(b) >= edge : coord(b) <= edge;
        if (ia != ib) {
            // Canonical endpoint order makes either winding compute the identical shared-edge point.
            MeshPoint lo = coord(a) < coord(b) ? a : b, hi = coord(a) < coord(b) ? b : a;
            float t = (edge - coord(lo)) / (coord(hi) - coord(lo));
            MeshPoint p{lo.x + (hi.x - lo.x) * t, lo.y + (hi.y - lo.y) * t, lo.z + (hi.z - lo.z) * t};
            if (axis == 0) p.x = edge; else p.z = edge;
            out.push_back(p);
        }
        if (ib) out.push_back(b);
        a = b; ia = ib;
    }
    return out;
}

inline void WaterPatches(const float *tris, unsigned count, std::vector<float> &out, float cell = 16) {
    for (unsigned i = 0; i < count; i++) {
        const float *t = tris + size_t(i) * 9;
        std::vector<MeshPoint> tri{{t[0], t[1], t[2]}, {t[3], t[4], t[5]}, {t[6], t[7], t[8]}};
        float x0 = std::min({t[0], t[3], t[6]}), x1 = std::max({t[0], t[3], t[6]});
        float z0 = std::min({t[2], t[5], t[8]}), z1 = std::max({t[2], t[5], t[8]});
        for (int z = int(std::floor(z0 / cell)); z < int(std::ceil(z1 / cell)); z++)
            for (int x = int(std::floor(x0 / cell)); x < int(std::ceil(x1 / cell)); x++) {
                auto poly = ClipWaterCell(tri, 0, x * cell, true);
                poly = ClipWaterCell(poly, 0, (x + 1) * cell, false);
                poly = ClipWaterCell(poly, 2, z * cell, true);
                poly = ClipWaterCell(poly, 2, (z + 1) * cell, false);
                for (size_t n = 1; n + 1 < poly.size(); n++) {
                    const auto &a = poly[0], &b = poly[n], &c = poly[n + 1];
                    float area = (b.x - a.x) * (c.z - a.z) - (b.z - a.z) * (c.x - a.x);
                    if (std::fabs(area) < 1e-6f) continue;
                    for (const auto &p : {a, b, c}) out.insert(out.end(), {p.x, p.y, p.z});
                }
            }
    }
}
} }
