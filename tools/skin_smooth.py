"""skin_smooth.py: a skin file's weights spread over more bones (docs/MODDING.md "People's skinning").

    python tools/skin_smooth.py <in.skin.json> <out.skin.json> [--rings 2] [--max 4] [--drop-rest]

The input is a skin file with its rest mesh (`skin.export` writes one: LS3D's own skin, two bones a vertex at most).
Each LOD's weights are smoothed over the mesh: every vertex's bone weights become the average of its own and its
neighbours' (the vertices it shares a triangle with), `--rings` times, so a joint's seam turns into a band where three
or four bones blend (an elbow, a knee, a shoulder bend without the hard crease). Vertices at the same place (a seam of
the uv map splits one point into several) are taken as one, so the seam stays closed. The `--max` strongest bones of
each vertex are kept (8 at most), their weights summing to 1. Vertices weighted by no bone (`influences` []) stay so.

The weights are an example of what a modder's own export gives (Blender's weights painted by hand): a test of the
loader's skinning with more than two bones a vertex, made from the game's own models without shipping their data.
"""
import argparse
import json
import sys


def smooth_lod(lod, rings, keep):
    rest = lod.get('rest')
    if not rest or 'positions' not in rest or 'triangles' not in rest:
        sys.exit('the input has no rest mesh (skin.export writes it with "rest": true)')
    pos, tris, inf = rest['positions'], rest['triangles'], lod['influences']
    n = len(inf)
    # one node a place (the uv map's seams split points)
    key = {}
    node = [0] * n
    for v in range(n):
        k = tuple(round(c, 5) for c in pos[v * 3:v * 3 + 3])
        node[v] = key.setdefault(k, len(key))
    nodes = len(key)
    nbr = [set() for _ in range(nodes)]
    for t in range(0, len(tris), 3):
        a, b, c = (node[tris[t + i]] for i in range(3))
        nbr[a].update((b, c)); nbr[b].update((a, c)); nbr[c].update((a, b))
    for s in nbr:
        s.discard(-1)
    # each node's weights: a dict bone -> weight (the average of its vertices'), rigid nodes left out
    w = [None] * nodes
    count = [0] * nodes
    for v in range(n):
        p = inf[v]
        if not p:
            continue
        d = w[node[v]] or {}
        for i in range(0, len(p), 2):
            d[p[i]] = d.get(p[i], 0.0) + p[i + 1]
        w[node[v]] = d
        count[node[v]] += 1
    for i in range(nodes):
        if w[i]:
            w[i] = {b: x / count[i] for b, x in w[i].items()}
    for _ in range(rings):
        nw = [None] * nodes
        for i in range(nodes):
            if not w[i]:
                continue
            acc = dict(w[i])
            m = 1
            for j in nbr[i]:
                if not w[j]:
                    continue
                for b, x in w[j].items():
                    acc[b] = acc.get(b, 0.0) + x
                m += 1
            nw[i] = {b: x / m for b, x in acc.items()}
        w = nw
    out = []
    most = 0
    for v in range(n):
        d = w[node[v]]
        if not inf[v] or not d:
            out.append([])
            continue
        top = sorted(d.items(), key=lambda kv: -kv[1])[:keep]
        s = sum(x for _, x in top)
        pairs = []
        for b, x in top:
            pairs += [b, round(x / s, 6)]
        most = max(most, len(top))
        out.append(pairs)
    lod['influences'] = out
    return most


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('src')
    ap.add_argument('dst')
    ap.add_argument('--rings', type=int, default=2)
    ap.add_argument('--max', type=int, default=4)
    ap.add_argument('--drop-rest', action='store_true', help='leave the rest mesh out of the output')
    a = ap.parse_args()
    keep = max(1, min(8, a.max))
    data = json.load(open(a.src, encoding='utf-8'))
    for m in data['meshes']:
        for i, lod in enumerate(m['lods']):
            most = smooth_lod(lod, a.rings, keep)
            blended = sum(1 for p in lod['influences'] if len(p) > 4)
            print(f"{m['mesh']} LOD {i}: {lod['vertices']} vertices, up to {most} bones a vertex, {blended} with more than two")
            if a.drop_rest:
                lod.pop('rest', None)
    json.dump(data, open(a.dst, 'w', encoding='utf-8'), separators=(',', ':'))


if __name__ == '__main__':
    main()
