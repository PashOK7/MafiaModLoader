// Water drawn by a graphics pass (mml.h gfx_pass; docs/MODDING.md "The game's own geometry hidden or replaced"): the
// parts any water needs, as mods/examples/river_water draws the city's river. Include it after mml/gfx/frame.hlsli.
//
// The pieces, each a function of what the pass has (no registers of its own):
// - WaterRipples: the ripples' slopes filtered over a pixel (mml/gfx/water_waves.hlsli makes them; mml/water.h the
//   spectrum): their mean is the normal, their variance the roughness of what the pixel cannot draw;
// - WaterFollowReflection: a planar reflection (a scene view mirrored in the water's plane) read where the reflected
//   ray meets what it reflects, three steps through the reflection's depth;
// - WaterStreakedReflection: that reflection blurred by the roughness, up and down whole and sideways by the cosine
//   (the streaks of real water; the hardware's anisotropy, asked for footprints 8 to 30 times taller than wide,
//   blurred both ways);
// - WaterGlitter: the sun on the ripples' facets (Cox and Munk), the sun's disc and the pixel's angle included;
// - WaterBody: the bed seen through the water, absorbed down to it and back (Beer-Lambert), the light the water scatters;
// - WaterSunScatter: the sun's light scattered once inside the water towards the eye (the body's glow against the sun,
//   lit through the crests of the waves), from the refracted rays and the particles' forward phase function;
// - WaterSunSeen: the sun the sky shows (its lens flare), where its reflection and its glow belong; the world is lit
//   and shadowed from g_sun, elsewhere in the sky (frame.hlsli);
// - WaterGameLight, WaterSunColour: the game's light (display terms, its sun brighter than 1: a multiplier of its
//   textures) as light to shade with (Lin() of the raw values made water two to three times too bright);
// - WaterFoam, WaterSky, WaterFresnel.
// Light is mixed in linear terms (frame.hlsli Lin, Enc).

// Schlick's Fresnel for water (0.02 head on).
float WaterFresnel(float cosTheta) { return 0.02 + 0.98 * pow(1 - saturate(cosTheta), 5); }

// The sun the sky shows (its lens flare), else the light's (frame.hlsli g_sunSeen).
float3 WaterSunSeen() { return g_sunSeen.w > 0.5 ? g_sunSeen.xyz : g_sun.xyz; }

// The ripples at a point: the cascades of a slopes texture array (x, z slope and their squares) and of a shape one
// (the height, the choppy displacement's dDx/dx, dDz/dz, dDx/dz), both mipmapped (mml/gfx/water_waves.hlsli), each of
// size 1 / invSize[c] metres; `rest`: the slope variance no cascade draws (an axis); `strength` scales the waves;
// `chop` the displacement towards the crests (0 none, 1 a Gerstner wave's). The surface is shaded where it lies:
// its normal is the displaced surface's, from its two tangents (1 - chop dDx/dx, dh/dx, -chop dDx/dz) and (-chop
// dDx/dz, dh/dz, 1 - chop dDz/dz), whose cross product's y is the Jacobian.
struct WaterSurface {
    float2 slope;     // the mean slope over the pixel of the surface as displaced (dh/dx, dh/dz)
    float2 var;       // the variance of the slopes it averages, each axis (with `rest`)
    float3 normal;
    float height;     // the mean height over the pixel above the water's level (m)
    float jacobian;   // 1 flat; under 1 squeezed at a crest; 0 and under folded over (a breaking crest)
};
WaterSurface WaterRipples(Texture2DArray<float4> slopes, Texture2DArray<float4> shape, SamplerState wrap, float2 xz,
                          float3 invSize, float rest, float strength, float chop) {
    WaterSurface w;
    float2 sl = 0, var = 0;
    float4 sh = 0;
    [unroll] for (int c = 0; c < 3; c++) {
        float3 at = float3(xz * invSize[c], c);
        float4 s = slopes.Sample(wrap, at);
        sl += s.xy;
        var += max(s.zw - s.xy * s.xy, 0);
        sh += shape.Sample(wrap, at);
    }
    sl *= strength;
    sh *= strength;
    float3 tx = float3(1 - chop * sh.y, sl.x, -chop * sh.w);
    float3 tz = float3(-chop * sh.w, sl.y, 1 - chop * sh.z);
    float3 n = cross(tz, tx);
    w.jacobian = n.y;
    n.y = max(n.y, 0.05);   // (folded over: taken as steep)
    w.normal = normalize(n);
    w.slope = -n.xz / n.y;
    // the slopes it averages are steepened as the crest is squeezed
    float squeeze = 1 / max(w.jacobian, 0.3);
    w.var = (var + rest) * strength * strength * squeeze * squeeze;
    w.height = sh.x;
    return w;
}

// Pixels a radian about the screen's centre (the frame's projection).
float WaterPixelsPerRadian() { return g_size.y * g_proj.y * 0.5; }

// A world point in a reflection's picture (its view x projection, row vectors: MmlGfxView::view x proj).
float2 WaterReflectionUv(float3 p, float4x4 viewProj) {
    float4 c = mul(float4(p, 1), viewProj);
    return c.xy / c.w * float2(0.5, -0.5) + 0.5;
}

// How far a reflection's pixel is from the line past which it shows nothing the water reflects (the sine of its
// ray's elevation: > 0 it does). The reflection's camera is under the water looking up through it: above-water things
// lie along its rays going up; along the others it drew only the sky's backdrop, which its clip plane does not cut
// (and would put a band of sky under every wall at the waterline if a blur reached it). invViewProj: the reflection's
// view x projection inverted (row vectors).
float WaterReflectionRise(float2 uv, float4x4 invViewProj) {
    float2 ndc = float2(uv.x * 2 - 1, 1 - uv.y * 2);
    float4 a = mul(float4(ndc, 0, 1), invViewProj), b = mul(float4(ndc, 1, 1), invViewProj);
    return normalize(b.xyz / b.w - a.xyz / a.w).y;
}

// uv moved off the reflection's line to `margin` pixels from it at least, along `up` (one of the reflection's pixels
// along the screen's vertical), away from the line: the clip plane leaves the last row of a thing's foot at the line
// covered in part, the sky's backdrop showing through; a pixel and a half off it lies the foot's first whole row.
static const float kWaterLineMargin = 1.5;
float2 WaterOffLine(float2 uv, float2 up, float4x4 invViewProj, float pxPerRadian) {
    float rise = WaterReflectionRise(uv, invViewProj);
    float2 away = WaterReflectionRise(uv + up, invViewProj) >= rise ? up : -up;
    float toLine = rise * pxPerRadian;
    return toLine < kWaterLineMargin ? uv + away * (kWaterLineMargin - toLine) : uv;
}

// Where the reflected ray r from the water's point p meets what the reflection drew: `depthRow` is the reflection
// view's column 2 (a point's linear depth in it: dot(float4(p, 1), depthRow)), `depth` its linear depth. Start from
// the flat water's place (WaterReflectionUv(p)); `t` is how far along r it met something (3000: the sky).
// At a thing's foot the reflection has holes: the clip plane cuts the thing at the water, and a pixel there whose
// centre passes under the cut foot shows the sky's backdrop although most of it is the thing (the game's models end
// just under the water: the city's rocks at -10.00, the river at -9.99). A water point at the foot finds that sky,
// its reflection a light dashed line along the waterline. So a read that finds the sky or something beyond the foot
// looks into the first three texels further from the cut (`up`: one of the reflection's pixels down the screen, invViewProj, pxPerRadian as
// WaterStreakedReflection's), and takes what stands there if it stands at the water point itself (the ray would
// rise less than a few pixels before it): the foot the hole hid. Real sky has no such thing beside it.
struct WaterHit {
    float2 uv;
    float t;
    bool sky;
    bool contact;    // a surface within the cut's pixel footprint: do not filter its colour with the sky beside it
};
float2 WaterAwayFromCut(float2 uv, float2 up, float4x4 invViewProj) {
    return WaterReflectionRise(uv + up, invViewProj) >= WaterReflectionRise(uv, invViewProj) ? up : -up;
}
// uv, or the foot beside it when uv is a hole at a thing's cut foot (see WaterFollowReflection); d its depth
float2 WaterFillFoot(float2 uv, float2 away, Texture2D<float> depth, float zp, float rz, float tNear, out float d) {
    d = depth.SampleLevel(g_point, saturate(uv), 0);
    if (!IsSky(d) && (d - zp) / rz <= tNear) return uv;
    // Raster coverage and the colour filter's footprint need not cross the cut at the same texel centre. The
    // old single 1.5-texel probe missed holes as the camera rose, and accepted background geometry as a valid foot.
    [unroll] for (int j = 1; j <= 3; j++) {
        float2 beside = uv + away * j;
        float d2 = depth.SampleLevel(g_point, saturate(beside), 0);
        if (!IsSky(d2) && (d2 - zp) / rz <= tNear) {
            d = d2;
            return beside;
        }
    }
    return uv;
}
WaterHit WaterFollowReflection(float3 p, float3 r, float2 uvFlat, float4x4 viewProj, float4 depthRow,
                               Texture2D<float> depth, float2 up, float4x4 invViewProj, float pxPerRadian) {
    WaterHit h;
    float zp = dot(float4(p, 1), depthRow);
    float rz = max(dot(r, depthRow.xyz), 1e-3);
    float2 away = WaterAwayFromCut(uvFlat, up, invViewProj);
    // how far along r a thing may stand and still be within a few pixels of the cut: a pixel at the water point is
    // zp / pxPerRadian metres high, and the ray rises r.y a metre
    float tNear = 3 * kWaterLineMargin * zp / (pxPerRadian * max(r.y, 0.01)) + 0.05;
    h.uv = uvFlat;
    float d = 0;
    h.t = 0;
    [unroll] for (int k = 0; k < 3; k++) {
        float2 at = WaterFillFoot(h.uv, away, depth, zp, rz, tNear, d);
        h.t = IsSky(d) ? 3000 : max((d - zp) / rz, 0);
        // (a foot found beside a hole: stay on it; the ray's point would lead back into the hole)
        h.uv = any(at != h.uv) ? at : WaterReflectionUv(p + r * h.t, viewProj);
    }
    h.uv = WaterFillFoot(h.uv, away, depth, zp, rz, tNear, d);
    h.sky = IsSky(d);
    h.t = h.sky ? 3000 : max((d - zp) / rz, 0);
    h.contact = !h.sky && h.t <= tNear;
    return h;
}

// The reflected rays' spread seen from the eye, in pixels (a Gaussian's deviation): 2 sigma of the slopes, as far
// behind the surface as the reflected thing lies (t) against the eye's distance.
float WaterReflectionSpread(float2 var, float t, float dist) {
    return 2 * sqrt(0.5 * (var.x + var.y)) * t / (dist + t) * WaterPixelsPerRadian();
}

// Interleaved gradient noise (Jimenez 2014): a pixel's own offset, -0.5 .. 0.5.
float WaterNoise(float2 px) { return frac(52.9829189 * frac(dot(px, float2(0.06711056, 0.00583715)))) - 0.5; }

// The reflection (with mips) read at uv, blurred `spread` pixels up and down and spread * cosTheta sideways: eight
// taps along `up` (one pixel along the screen's vertical in the reflection's uv: ddy of the flat water's uv,
// normalised and times 1 / height), each from the mip as wide as the sideways spread, shifted by the pixel's noise.
// Only what the water reflects is mixed: the reflection's line (WaterReflectionRise) is the clip plane's, where the
// last row of a wall's or a rock's foot is covered only in part and the sky's backdrop shows through, so uv is read
// at least a pixel and a half off it (a thing at the waterline: its foot's first whole row), a tap within a pixel of
// it or past it is left out and the mip kept narrower than the way to it. `pxPerRadian`: the reflection's pixels a
// radian (WaterPixelsPerRadian at the frame's size).
float3 WaterStreakedReflection(Texture2D<float4> refl, SamplerState clampS, float2 uv, float2 up, float spread,
                               float cosTheta, float2 px, float4x4 invViewProj, float pxPerRadian) {
    uv = WaterOffLine(uv, up, invViewProj, pxPerRadian);
    float toLine = max(WaterReflectionRise(uv, invViewProj) * pxPerRadian, kWaterLineMargin);   // pixels to the line
    float lod = log2(clamp(min(spread * cosTheta, toLine - 1), 1, 4096));
    float jitter = WaterNoise(px);
    float3 sum = 0;
    float wsum = 0;
    [unroll] for (int j = 0; j < 8; j++) {
        float o = (j - 3.5 + jitter) / 1.75;   // -2 .. 2 deviations
        float2 at = uv + up * (o * spread);
        float wj = exp(-0.5 * o * o) * saturate(WaterReflectionRise(at, invViewProj) * pxPerRadian - 1);
        sum += wj * refl.SampleLevel(clampS, at, lod).rgb;
        wsum += wj;
    }
    return wsum > 1e-4 ? sum / wsum : refl.SampleLevel(clampS, uv, 0).rgb;
}

// The sky where no reflection was drawn: the horizon's fog colour into a bluer zenith (display space).
float3 WaterSky(float3 r) {
    float3 horizon = g_fogColor.w > 0.5 ? g_fogColor.rgb : float3(0.62, 0.68, 0.74);
    return lerp(horizon, horizon * float3(0.62, 0.76, 1.0), sqrt(saturate(r.y)));
}

// The sun's colour for a highlight (linear): its hue half kept, its brightness up to 1.
float3 WaterSunColour() {
    float m = max(max(g_sunColor.r, g_sunColor.g), max(g_sunColor.b, 1e-3));
    return Lin(lerp(1, g_sunColor.rgb / m, 0.5) * saturate(m));
}

// The light a horizontal thing is lit by as the game lights its own (ambient + the sun, display terms, clipped),
// linear: a colour given "in the full sun" is that colour in the sun, bluer and darker in a shadow (0..1).
float3 WaterGameLight(float shadow) {
    return Lin(saturate(g_ambient.rgb + g_sunColor.rgb * (saturate(g_sun.y) * shadow * 0.5)));
}

// What a white surface facing the sun sends back (linear), the sun's irradiance over pi as the game lights its own
// (display terms, clipped): the scale of the sun's light for the water's body.
float3 WaterSunWhite() { return Lin(saturate(g_sunColor.rgb * 0.5)); }

// The sun on the ripples (Cox and Munk): the share of the facets turned to send the sun (towards l: WaterSunSeen) to
// the eye v, from their slopes' distribution about the mean slope (var, each axis), the sun's disc (0.27 degrees) and
// the pixel's own angle added. Times the sun's colour and whether the reflected ray meets the sky.
float WaterGlitter(float3 v, float3 l, float2 slope, float2 var) {
    if (l.y <= 0) return 0;
    float3 h = normalize(l + v);
    float2 need = -h.xz / h.y - slope;
    float pixelAngle = 1 / WaterPixelsPerRadian();
    float2 s2 = var + 6e-6 + 0.25 * pixelAngle * pixelAngle;
    float pdf = exp(-0.5 * (need.x * need.x / s2.x + need.y * need.y / s2.y)) / (6.2831853 * sqrt(s2.x * s2.y));
    float h2 = h.y * h.y;
    return WaterFresnel(dot(h, v)) * pdf / (4 * h2 * h2 * max(v.y, 0.02));
}

// The water's body: the bed (linear) seen through `path` metres of water, lit through `down` metres above it,
// absorbed by `absorb` a metre; the light `scatter` (of white light) the water sends back, lit by `light`.
float3 WaterBody(float3 bed, float path, float down, float3 absorb, float3 scatter, float3 light) {
    float3 through = exp(-absorb * path);
    return bed * through * exp(-absorb * down) + scatter * light * (1 - through);
}

// Henyey and Greenstein's phase function (per steradian): g the forward asymmetry (a river's particles scatter about
// 0.9 forward once; a few scatterings broaden it, 0.6 to 0.8).
float WaterPhase(float cosPsi, float g) {
    float d = 1 + g * g - 2 * g * cosPsi;
    return (1 - g * g) / (12.5663706 * d * sqrt(d));
}

// The sun's light scattered once inside the water towards the eye (linear, in WaterSunWhite's units: times pi the
// sun's irradiance). Water's optics: `absorb` and `scatter` per metre (`scatter`: the particles', grey in a river),
// `g` their phase's asymmetry. v towards the eye, n the facet the eye sees, `slope` its slope, l towards the sun
// (WaterSunSeen), `depth` the water under the surface along the vertical (m, 1e3 open), `height` the surface's height
// above its level at this pixel and `crest` the crests' height (2 deviations of the heights: m), `crests` a strength.
//
// Two ways in. The layer: the sun refracted through the mean surface (it enters over a wide area), scattered at every
// depth down to the bed, out through the facet the eye sees: the exact single scattering of a flat slab (the beam
// and the way out attenuated, Fresnel's transmission both ways, radiance divided by n^2 coming out). The crest: on a
// face turned away from the sun under a crest (in the crest's own shadow once it is steeper than the sun is high),
// the sun enters the crest's face turned to it (the same slope mirrored along the sun's way) and is scattered inside
// the crest, the forward share of it ((1 + g) / 2) out of the face the eye sees, diffusely: the glow of a wave's crest
// against the sun, green with what the crossing absorbs. The crest's width at this height (its slopes and its height
// above this point) sets how much is scattered (1 - e^-bd) and absorbed (e^-ad/2). Small ripples' crests are thin
// and scatter little: their glow is faint, as on real calm water; big steep waves' crests glow.
float3 WaterSunScatter(float3 v, float3 n, float2 slope, float3 l, float depth, float3 absorb, float3 scatter, float g,
                       float height, float crest, float crests) {
    if (l.y <= 0.01) return 0;
    const float eta = 1 / 1.333;
    float3 extinct = absorb + scatter;
    float3 down = refract(-v, n, eta);   // the eye's ray in the water
    float3 up = -down;                   // the scattered light's way out
    float muV = max(-down.y, 0.05);
    float tOut = (1 - WaterFresnel(saturate(dot(n, v)))) / (1.333 * 1.333);

    // the layer, lit through the mean surface
    float3 ts = refract(-l, float3(0, 1, 0), eta);
    float muS = max(-ts.y, 0.05);
    float tIn = 1 - WaterFresnel(l.y);
    float3 k = extinct * (1 / muS + 1 / muV);
    float3 layer = scatter * WaterPhase(dot(ts, up), g) * (1 - exp(-k * depth)) / (extinct * (1 + muV / muS));
    float3 lit = layer * (3.14159265 * l.y / muS * tIn);

    // the crest, lit through its face turned to the sun
    float2 sunH = normalize(l.xz + 1e-6);
    float m = dot(slope, sunH);                      // > 0: this face is turned away from the sun
    if (m > 1e-3 && crests > 0) {
        float2 fSlope = slope - 2 * m * sunH;        // the crest's other face
        float3 nf = normalize(float3(-fSlope.x, 1, -fSlope.y));
        float cf = dot(nf, l);
        if (cf > 0.01) {
            float across = 2 * max(crest - height, 0) / m;   // the crest's width at this height (m)
            float3 glow = (1 + g) * 0.5 * (1 - exp(-scatter * across)) * exp(-absorb * across * 0.5);
            lit += crests * cf * (1 - WaterFresnel(cf)) * glow * saturate(height / max(crest, 1e-4) + 1);   // (above the troughs)
        }
    }
    return lit * tOut;
}

// What the eye sees through a wave's crest (linear light): on a face turned away from the sun (towards the eye)
// under a crest, the eye's ray refracted in may cross the crest and come out of its face turned to the sun, into the
// sky: the sky and the sun's disc seen through the water, green with what the crossing absorbs (the bright crests of
// steep waves against a low sun). The crest a wedge: this face's slope `slope`, its other face the same slope
// mirrored along the sun's way (l: WaterSunSeen), its top `crest` above the water's level, this point `height` above
// it; `var` the facets' slope variance (the sun's disc spread by them). Fresnel's transmission at the way out only
// (the way in is the body's share, 1 - Fresnel, where it is added). 0 where the ray goes down into the body (a face
// gentler than about 25 degrees: calm water's, nearly always) or is reflected back inside.
float3 WaterThroughCrest(float3 v, float3 n, float2 slope, float3 l, float height, float crest, float3 extinct, float2 var) {
    float2 sunH = normalize(l.xz + 1e-6);
    float m = dot(slope, sunH);   // this face rises towards the sun
    if (m <= 0.05 || crest <= 0 || l.y <= 0) return 0;
    float3 t = refract(-v, n, 1 / 1.333);
    float ts = dot(t.xz, sunH);
    float den = t.y + m * ts;
    if (den <= 1e-3) return 0;                       // the ray runs down into the body
    float u = 2 * max(crest - height, 0) / den;      // the way across to the other face (m)
    if (height + t.y * u < -crest) return 0;         // (out below the troughs: into the body)
    float2 fSlope = slope - 2 * m * sunH;
    float3 nf = normalize(float3(-fSlope.x, 1, -fSlope.y));
    float3 o = refract(t, -nf, 1.333);
    if (dot(o, o) < 0.5 || o.y <= 0) return 0;      // reflected back inside, or out towards the water
    float leave = 1 - WaterFresnel(saturate(dot(o, nf)));
    float s2 = 0.5 * (var.x + var.y) + 2e-5;         // the directions' spread (radians^2), the sun's disc included
    float sun = exp(-(1 - dot(o, l)) / s2) / (2 * s2);
    return leave * exp(-extinct * u) * (Lin(WaterSky(o)) + WaterSunWhite() * sun);
}

float WaterHash(float2 p) { return frac(sin(dot(p, float2(127.1, 311.7))) * 43758.5453); }
float WaterValueNoise(float2 p) {
    float2 i = floor(p), f = frac(p);
    f = f * f * (3 - 2 * f);
    return lerp(lerp(WaterHash(i), WaterHash(i + float2(1, 0)), f.x),
                lerp(WaterHash(i + float2(0, 1)), WaterHash(i + 1), f.x), f.y);
}

// Foam where waves wash against a wall, a rock or a post: `under`, the depth of what is seen under the surface (m);
// `crest` the waves' crest height (m, 2 deviations of the heights). Waves make it, not the wall: its band is a few
// crest heights wide and it grows with the waves (full at 5 cm crests), so calm water has none at all (it had a
// band 45 cm deep whatever the waves, a bright wedge along every wall and hull: 2026-10-29). Broken up by drifting
// noise, bent by the slopes. 0..1.
float WaterFoam(float2 xz, float2 slope, float under, float crest, float time) {
    float2 q = xz * 1.3 + slope * 4;
    float grain = WaterValueNoise(q + float2(time * 0.11, time * 0.07)) * 0.65 +
                  WaterValueNoise(q * 3.1 - float2(time * 0.17, -time * 0.13)) * 0.35;
    float edge = 1 - smoothstep(0, 3 * crest + 1e-3, under);
    return saturate((edge - grain * 0.85) * 3) * saturate(crest / 0.05);
}

// Whitecaps: where the choppy displacement folds the surface over (its Jacobian under 0.3: a breaking crest), broken
// up by drifting noise. 0..1.
float WaterWhitecaps(float jacobian, float2 xz, float time) {
    float grain = WaterValueNoise(xz * 2.3 + float2(time * 0.21, time * 0.13));
    return saturate((0.3 - jacobian) * 3 - grain * 0.5);
}
