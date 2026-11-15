#pragma once

#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>
#include "acoustics.h"

namespace acoustic {
struct Facet {
    V3 a, b, c, normal;
    bool hit;
};

// Millimetre keys weld the shared edges of neighbouring native triangles.
struct PointKey {
    std::array<int, 3> xyz;
    auto operator<=>(const PointKey &) const = default;
};
struct EdgeKey {
    PointKey a, b;
    bool operator==(const EdgeKey &) const = default;
};
struct EdgeHash {
    size_t operator()(const EdgeKey &edge) const {
        size_t hash = 2166136261u;
        for (const auto &point : {edge.a, edge.b})
            for (int v : point.xyz) hash = (hash ^ static_cast<uint32_t>(v)) * 16777619u;
        return hash;
    }
};
inline PointKey Weld(V3 p) {
    return {{static_cast<int>(std::lround(p.x * 1000)), static_cast<int>(std::lround(p.y * 1000)),
             static_cast<int>(std::lround(p.z * 1000))}};
}
inline EdgeKey EdgeOf(V3 a, V3 b) {
    auto x = Weld(a), y = Weld(b);
    if (y < x) std::swap(x, y);
    return {x, y};
}
inline auto Edges(const Facet &f) {
    return std::array{EdgeOf(f.a, f.b), EdgeOf(f.b, f.c), EdgeOf(f.c, f.a)};
}
inline bool SamePlane(const Facet &a, const Facet &b) {
    return std::abs(Dot(a.normal, b.normal)) > .998f &&
           std::abs(Dot(b.a - a.a, a.normal)) < .025f;
}

// Extend the hit set over actual adjacent floor/wall triangles. Never bridge a doorway
// or a missing face with a fitted polygon.
inline std::vector<size_t> ConnectedSurfaces(const std::vector<Facet> &facets) {
    std::unordered_map<EdgeKey, std::vector<size_t>, EdgeHash> neighbours;
    std::vector<size_t> visible;
    std::vector<bool> selected(facets.size());
    for (size_t i = 0; i < facets.size(); ++i) {
        if (facets[i].hit) { visible.push_back(i); selected[i] = true; }
        for (const auto &edge : Edges(facets[i])) neighbours[edge].push_back(i);
    }
    for (size_t k = 0; k < visible.size(); ++k) {
        const auto &facet = facets[visible[k]];
        for (const auto &edge : Edges(facet))
            for (size_t i : neighbours.at(edge)) {
                if (selected[i] || !SamePlane(facet, facets[i])) continue;
                selected[i] = true;
                visible.push_back(i);
            }
    }
    return visible;
}

inline float Smooth(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3 - 2 * t);
}
inline float DirectVisibility(float travelled, float wallDistance) {
    return Smooth((wallDistance - travelled) / std::max(.22f, travelled * .075f));
}
} // namespace acoustic
