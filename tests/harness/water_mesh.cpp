#include "mml/water_mesh.h"
#include <cassert>
#include <cstdio>
#include <map>
#include <utility>

double Area(const std::vector<float> &m) {
    double a = 0;
    for (size_t i = 0; i < m.size(); i += 9)
        a += ((double(m[i + 3]) - m[i]) * (double(m[i + 8]) - m[i + 2]) -
              (double(m[i + 5]) - m[i + 2]) * (double(m[i + 6]) - m[i])) / 2;
    return a;
}

int main() {
    // Two unequal triangles share an oblique edge across positive and negative grid cells.
    const float tris[] = {-37, -10, -21, 49, -10, -21, 49, -10, 43,
                         -37, -10, -21, 49, -10, 43, -37, -10, 43};
    std::vector<float> mesh;
    mml::water::WaterPatches(tris, 2, mesh);
    assert(mesh.size() > 18 && mesh.size() % 9 == 0);
    assert(std::fabs(Area(mesh) - 86.0 * 64.0) < 0.01);
    for (size_t i = 0; i < mesh.size(); i += 3) {
        assert(mesh[i] >= -37 && mesh[i] <= 49);
        assert(mesh[i + 1] == -10);
        assert(mesh[i + 2] >= -21 && mesh[i + 2] <= 43);
    }
    for (size_t i = 0; i < mesh.size(); i += 9)
        for (int v = 0; v < 3; v++) {
            int w = (v + 1) % 3;
            assert(std::fabs(mesh[i + v * 3] - mesh[i + w * 3]) <= 16.0001);
            assert(std::fabs(mesh[i + v * 3 + 2] - mesh[i + w * 3 + 2]) <= 16.0001);
        }
    using Point = std::pair<float, float>;
    std::map<std::pair<Point, Point>, unsigned> edges;
    for (size_t i = 0; i < mesh.size(); i += 9) for (int v = 0; v < 3; v++) {
        int w = (v + 1) % 3;
        Point a{mesh[i + v * 3], mesh[i + v * 3 + 2]}, b{mesh[i + w * 3], mesh[i + w * 3 + 2]};
        if (b < a) std::swap(a, b);
        edges[{a, b}]++;
    }
    for (const auto &e : edges) {
        const auto &a = e.first.first, &b = e.first.second;
        bool boundary = (a.first == b.first && (a.first == -37 || a.first == 49)) ||
                        (a.second == b.second && (a.second == -21 || a.second == 43));
        assert(e.second == (boundary ? 1u : 2u));
    }
    // Opposite winding must compute bit-identical intersections along a shared edge.
    using mml::water::MeshPoint;
    std::vector<MeshPoint> a{{-37, -10, -21}, {49, -10, 43}}, b(a.rbegin(), a.rend());
    auto ca = mml::water::ClipWaterCell(a, 0, 16, true), cb = mml::water::ClipWaterCell(b, 0, 16, true);
    bool found = false;
    for (const auto &p : ca) for (const auto &q : cb)
        if (p.x == 16 && q.x == 16) { assert(p.z == q.z); found = true; }
    assert(found);
    puts("Water mesh: footprint, area, bounded patches and shared edges passed");
}
