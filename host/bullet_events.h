#pragma once

struct V3;

namespace host {
// Report the final ray result once, at the outer hook which sees the game's bullet caller.
void ReportBulletRay(const void *primitive, const V3 *from, const V3 *delta, const V3 *hit, const V3 *normal);
}
