// (MML: third_party/dxwrapper/MmlManaged.h, copied into dxwrapper's d3d9 folder by patch.py)
//
// D3DPOOL_MANAGED for a Direct3D 9Ex device, as a driver keeps it, without a second copy of every texture in the
// process. Direct3D 9Ex has no managed pool: dxwrapper's D3d9to9Ex made each managed texture a D3DUSAGE_DYNAMIC one in
// the default pool, which the driver (and DXVK) keeps in memory the CPU can reach: inside the game's 4 GB. A player's
// Mafia with an 8.4 GB texture pack kept 3.2 GB of them so and died loading the freeride (2026-10-22).
//
// Here a managed 2D texture is a default-pool texture without DYNAMIC (in video memory only). Locking one of its
// levels gives the game a copy in system memory (a one-level D3DPOOL_SYSTEMMEM texture), filled from the video memory
// when the level holds something (StretchRect into a render target, GetRenderTargetData); unlocking it sends the
// written rectangle back (UpdateSurface). The copies stay while they are used and are let go least recently used
// first once they pass Budget() bytes, so a level the game locks again and again (LS3D rebuilds some mip chains every
// frame) keeps its copy and reads nothing back. A format the device cannot read back that way (no render target of
// it, or no StretchRect from textures: DXT, some 16-bit ones) keeps its copies as long as its texture lives: managed
// as before for those, which the measured games hold little of. Cube and volume textures, and textures with
// AUTOGENMIPMAP, keep dxwrapper's DYNAMIC conversion.
#pragma once
#include <list>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace MmlManaged {

// Bytes of system-memory copies kept for readable formats: 256 MB, or the environment's MML_MANAGED_BUDGET_MB (a
// test reads back at every lock with 0; a player short of memory may give less)
inline size_t Budget()
{
	static const size_t budget = [] {
		char text[32] = {};
		DWORD n = GetEnvironmentVariableA("MML_MANAGED_BUDGET_MB", text, sizeof(text));
		return (n > 0 && n < sizeof(text)) ? (size_t)strtoul(text, nullptr, 10) << 20 : (size_t)256 << 20;
	}();
	return budget;
}

struct Level {
	UINT width = 0, height = 0;
	size_t bytes = 0;
	IDirect3DTexture9* copy = nullptr;    // the system-memory copy (one level), or null
	IDirect3DSurface9* copySurface = nullptr;
	bool gpuHasData = false;   // something was written to the video memory's level
	bool copyStale = false;    // the video memory was written past the copy (UpdateSurface, StretchRect, ColorFill)
	bool locked = false, readOnly = false, whole = true, dc = false;
	RECT rect = {};
	bool inLru = false;
	std::list<std::pair<IDirect3DTexture9*, UINT>>::iterator lru;
};
struct Texture {
	IDirect3DDevice9* device = nullptr;   // the real device (not held: the texture holds it)
	D3DFORMAT format = D3DFMT_UNKNOWN;
	bool readable = false;     // its levels can be read back from the video memory
	std::vector<Level> levels;
	std::vector<IDirect3DSurface9*> surfaces;   // its levels' surfaces seen (keys of g_surfaces)
};

inline std::recursive_mutex g_lock;
inline std::unordered_map<IDirect3DTexture9*, Texture> g_textures;
inline std::unordered_map<IDirect3DSurface9*, std::pair<IDirect3DTexture9*, UINT>> g_surfaces;
inline std::list<std::pair<IDirect3DTexture9*, UINT>> g_lru;   // readable levels with a copy, least recent first
inline size_t g_copyBytes = 0, g_keptBytes = 0;                // copies of readable formats; of the others
inline std::unordered_map<UINT, IDirect3DSurface9*> g_readbackTargets;   // render targets for read-backs, by size
inline std::unordered_map<DWORD, bool> g_formatReadable;
inline unsigned long long g_readBacks = 0, g_letGo = 0, g_lostContent = 0;
inline DWORD g_lastReport = 0;

inline size_t LevelBytes(D3DFORMAT f, UINT w, UINT h)
{
	switch ((DWORD)f)
	{
	case D3DFMT_DXT1: return (size_t)((w + 3) / 4) * ((h + 3) / 4) * 8;
	case D3DFMT_DXT2: case D3DFMT_DXT3: case D3DFMT_DXT4: case D3DFMT_DXT5: return (size_t)((w + 3) / 4) * ((h + 3) / 4) * 16;
	case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: case D3DFMT_A8B8G8R8: case D3DFMT_X8B8G8R8: case D3DFMT_A2R10G10B10:
	case D3DFMT_G16R16: case D3DFMT_R32F: return (size_t)w * h * 4;
	case D3DFMT_A8: case D3DFMT_L8: case D3DFMT_P8: return (size_t)w * h;
	case D3DFMT_R8G8B8: return (size_t)w * h * 3;
	default: return (size_t)w * h * 2;
	}
}

// The formats managed this way (the others, and AUTOGENMIPMAP, keep DYNAMIC).
inline bool Wanted(DWORD usage)
{
	return !(usage & D3DUSAGE_AUTOGENMIPMAP);
}

// Whether the device can read a level of this format back: StretchRect from a texture into a render target of it
inline bool FormatReadable(IDirect3DDevice9* device, D3DFORMAT format)
{
	auto it = g_formatReadable.find((DWORD)format);
	if (it != g_formatReadable.end()) return it->second;
	bool ok = false;
	D3DCAPS9 caps = {};
	if (SUCCEEDED(device->GetDeviceCaps(&caps)) && (caps.DevCaps2 & D3DDEVCAPS2_CAN_STRETCHRECT_FROM_TEXTURES))
	{
		IDirect3DSurface9* rt = nullptr;
		ok = SUCCEEDED(device->CreateRenderTarget(4, 4, format, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, nullptr)) && rt;
		if (rt) rt->Release();
	}
	g_formatReadable[(DWORD)format] = ok;
	Logging::Log() << "MML managed textures: format " << (DWORD)format << (ok ? " read back from the video memory when needed"
		: " not readable back: its copies kept as long as its texture");
	return ok;
}

inline void Report()
{
	DWORD now = GetTickCount();
	if (now - g_lastReport < 30000) return;
	g_lastReport = now;
	Logging::Log() << "MML managed textures: " << g_textures.size() << " in video memory only; copies " << (g_copyBytes >> 20)
		<< " MB (budget " << (Budget() >> 20) << " MB) and " << (g_keptBytes >> 20) << " MB kept (unreadable formats); "
		<< g_readBacks << " levels read back, " << g_letGo << " copies let go, " << g_lostContent << " contents lost";
}

inline void Register(IDirect3DTexture9* texture, D3DFORMAT format)
{
	if (!texture) return;
	std::lock_guard<std::recursive_mutex> lock(g_lock);
	Texture t;
	texture->GetDevice(&t.device);
	if (t.device) t.device->Release();   // (the texture holds it)
	t.format = format;
	t.readable = t.device && FormatReadable(t.device, format);
	t.levels.resize(texture->GetLevelCount());
	for (UINT i = 0; i < t.levels.size(); i++)
	{
		D3DSURFACE_DESC d = {};
		texture->GetLevelDesc(i, &d);
		t.levels[i].width = d.Width;
		t.levels[i].height = d.Height;
		t.levels[i].bytes = LevelBytes(format, d.Width, d.Height);
	}
	if (g_textures.empty()) Logging::Log() << "MML managed textures: on (Direct3D 9Ex: managed textures in video memory, "
		"copies in system memory only while used)";
	g_textures[texture] = std::move(t);
}

inline bool Has(IDirect3DTexture9* texture)
{
	if (g_textures.empty()) return false;
	std::lock_guard<std::recursive_mutex> lock(g_lock);
	return g_textures.count(texture) != 0;
}

inline void DropCopy(Texture& t, Level& l)
{
	if (!l.copy) return;
	if (l.copySurface) l.copySurface->Release();
	l.copy->Release();
	l.copy = nullptr;
	l.copySurface = nullptr;
	if (l.inLru) { g_lru.erase(l.lru); l.inLru = false; g_copyBytes -= l.bytes; }
	else if (!t.readable) g_keptBytes -= l.bytes;
}

inline void ReleaseTargets()
{
	for (auto& kv : g_readbackTargets) kv.second->Release();
	g_readbackTargets.clear();
}

// The copies past the budget let go, least recently used first (a locked level keeps its copy)
inline void Enforce()
{
	for (auto it = g_lru.begin(); g_copyBytes > Budget() && it != g_lru.end();)
	{
		auto key = *it++;
		Texture& t = g_textures[key.first];
		Level& l = t.levels[key.second];
		if (l.locked) continue;
		DropCopy(t, l);
		g_letGo++;
	}
}

inline void Touch(Texture& t, Level& l, IDirect3DTexture9* texture, UINT level)
{
	if (!t.readable) return;
	if (l.inLru) g_lru.splice(g_lru.end(), g_lru, l.lru);
	else
	{
		l.lru = g_lru.insert(g_lru.end(), { texture, level });
		l.inLru = true;
		g_copyBytes += l.bytes;
	}
}

// The level's content from the video memory into its copy
inline bool ReadBack(Texture& t, Level& l, IDirect3DTexture9* texture, UINT level)
{
	if (!t.readable) return false;
	UINT key = (l.width << 16) ^ l.height ^ ((UINT)t.format << 24);
	IDirect3DSurface9*& rt = g_readbackTargets[key];
	if (!rt && FAILED(t.device->CreateRenderTarget(l.width, l.height, t.format, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, nullptr))) { rt = nullptr; return false; }
	IDirect3DSurface9* src = nullptr;
	if (FAILED(texture->GetSurfaceLevel(level, &src)) || !src) return false;
	bool ok = SUCCEEDED(t.device->StretchRect(src, nullptr, rt, nullptr, D3DTEXF_NONE)) &&
		SUCCEEDED(t.device->GetRenderTargetData(rt, l.copySurface));
	src->Release();
	if (g_readbackTargets.size() > 8)   // (a few sizes kept; the rest made again when needed)
	{
		for (auto it = g_readbackTargets.begin(); it != g_readbackTargets.end();)
		{
			if (it->first == key) { ++it; continue; }
			it->second->Release();
			it = g_readbackTargets.erase(it);
		}
	}
	if (ok) g_readBacks++;
	return ok;
}

inline HRESULT LockLevel(IDirect3DTexture9* texture, UINT level, D3DLOCKED_RECT* out, const RECT* rect, DWORD flags)
{
	std::lock_guard<std::recursive_mutex> lock(g_lock);
	auto it = g_textures.find(texture);
	if (it == g_textures.end() || level >= it->second.levels.size() || !out) return D3DERR_INVALIDCALL;
	Texture& t = it->second;
	Level& l = t.levels[level];
	if (l.locked) return D3DERR_INVALIDCALL;
	bool discard = (flags & D3DLOCK_DISCARD) && !rect;
	if (l.copy && l.copyStale && !discard)
	{
		if (!ReadBack(t, l, texture, level)) { g_lostContent++; LOG_LIMIT(20, "MML managed textures: a level written by the GPU could not be read back"); }
		l.copyStale = false;
	}
	if (!l.copy)
	{
		if (FAILED(t.device->CreateTexture(l.width, l.height, 1, 0, t.format, D3DPOOL_SYSTEMMEM, &l.copy, nullptr)) || !l.copy)
		{
			l.copy = nullptr;
			return D3DERR_OUTOFVIDEOMEMORY;
		}
		l.copy->GetSurfaceLevel(0, &l.copySurface);
		if (!t.readable) g_keptBytes += l.bytes;
		if (l.gpuHasData && !discard && !ReadBack(t, l, texture, level))
		{
			g_lostContent++;
			LOG_LIMIT(20, "MML managed textures: a level's content could not be read back (format " << (DWORD)t.format << ")");
		}
		l.copyStale = false;
	}
	HRESULT hr = l.copySurface->LockRect(out, rect, flags & (D3DLOCK_READONLY | D3DLOCK_NOSYSLOCK | D3DLOCK_NO_DIRTY_UPDATE));
	if (FAILED(hr)) return hr;
	l.locked = true;
	l.readOnly = (flags & D3DLOCK_READONLY) != 0;
	l.whole = rect == nullptr;
	if (rect) l.rect = *rect;
	Touch(t, l, texture, level);
	return hr;
}

// The copy's written part to the video memory
inline void Send(Texture& t, Level& l, IDirect3DTexture9* texture, UINT level, bool whole, const RECT& rect)
{
	IDirect3DSurface9* dst = nullptr;
	if (SUCCEEDED(texture->GetSurfaceLevel(level, &dst)) && dst)
	{
		POINT at = { rect.left, rect.top };
		t.device->UpdateSurface(l.copySurface, whole ? nullptr : &rect, dst, whole ? nullptr : &at);
		dst->Release();
	}
	l.gpuHasData = true;
}

inline HRESULT UnlockLevel(IDirect3DTexture9* texture, UINT level)
{
	std::lock_guard<std::recursive_mutex> lock(g_lock);
	auto it = g_textures.find(texture);
	if (it == g_textures.end() || level >= it->second.levels.size()) return D3DERR_INVALIDCALL;
	Texture& t = it->second;
	Level& l = t.levels[level];
	if (!l.locked || !l.copySurface) return D3DERR_INVALIDCALL;
	HRESULT hr = l.copySurface->UnlockRect();
	l.locked = false;
	if (!l.readOnly) Send(t, l, texture, level, l.whole, l.rect);
	Enforce();
	Report();
	return hr;
}

// A device context on a level (fonts drawn by GDI): the copy's, sent back at its release
inline HRESULT GetLevelDC(IDirect3DTexture9* texture, UINT level, HDC* dc)
{
	D3DLOCKED_RECT unused = {};
	std::lock_guard<std::recursive_mutex> lock(g_lock);
	HRESULT hr = LockLevel(texture, level, &unused, nullptr, 0);
	if (FAILED(hr)) return hr;
	Level& l = g_textures[texture].levels[level];
	l.copySurface->UnlockRect();
	hr = l.copySurface->GetDC(dc);
	if (FAILED(hr)) { l.locked = false; return hr; }
	l.dc = true;
	return hr;
}
inline HRESULT ReleaseLevelDC(IDirect3DTexture9* texture, UINT level, HDC dc)
{
	std::lock_guard<std::recursive_mutex> lock(g_lock);
	auto it = g_textures.find(texture);
	if (it == g_textures.end() || level >= it->second.levels.size()) return D3DERR_INVALIDCALL;
	Texture& t = it->second;
	Level& l = t.levels[level];
	if (!l.dc || !l.copySurface) return D3DERR_INVALIDCALL;
	HRESULT hr = l.copySurface->ReleaseDC(dc);
	l.dc = false;
	l.locked = false;
	Send(t, l, texture, level, true, l.rect);
	Enforce();
	return hr;
}

// Which emulated texture's level a real surface is (null when none)
inline bool LevelOf(IDirect3DSurface9* surface, IDirect3DTexture9** texture, UINT* level)
{
	if (g_textures.empty() || !surface) return false;
	std::lock_guard<std::recursive_mutex> lock(g_lock);
	auto it = g_surfaces.find(surface);
	if (it != g_surfaces.end()) { *texture = it->second.first; *level = it->second.second; return true; }
	IDirect3DTexture9* container = nullptr;
	if (FAILED(surface->GetContainer(IID_IDirect3DTexture9, (void**)&container)) || !container) return false;
	container->Release();   // (the surface holds it)
	auto tex = g_textures.find(container);
	if (tex == g_textures.end()) return false;
	for (UINT i = 0; i < tex->second.levels.size(); i++)
	{
		IDirect3DSurface9* s = nullptr;
		if (FAILED(container->GetSurfaceLevel(i, &s)) || !s) continue;
		s->Release();
		if (s != surface) continue;
		g_surfaces[surface] = { container, i };
		tex->second.surfaces.push_back(surface);
		*texture = container;
		*level = i;
		return true;
	}
	return false;
}

// The GPU wrote a level (UpdateSurface, StretchRect, ColorFill into it; UpdateTexture: every level): its copy is
// out of date, read again at its next lock
inline void GpuWrote(IDirect3DTexture9* texture, UINT level)
{
	std::lock_guard<std::recursive_mutex> lock(g_lock);
	auto it = g_textures.find(texture);
	if (it == g_textures.end() || level >= it->second.levels.size()) return;
	Level& l = it->second.levels[level];
	l.gpuHasData = true;
	if (l.copy && !l.locked) l.copyStale = true;
}
inline void GpuWroteSurface(IDirect3DSurface9* surface)
{
	IDirect3DTexture9* texture = nullptr;
	UINT level = 0;
	if (LevelOf(surface, &texture, &level)) GpuWrote(texture, level);
}
inline void GpuWroteTexture(IDirect3DBaseTexture9* base)
{
	if (g_textures.empty() || !base || base->GetType() != D3DRTYPE_TEXTURE) return;
	std::lock_guard<std::recursive_mutex> lock(g_lock);
	IDirect3DTexture9* texture = static_cast<IDirect3DTexture9*>(base);
	auto it = g_textures.find(texture);
	if (it == g_textures.end()) return;
	for (UINT i = 0; i < it->second.levels.size(); i++) GpuWrote(texture, i);
}

// The texture's last reference went: its copies with it
inline void Forget(IDirect3DTexture9* texture)
{
	if (g_textures.empty()) return;
	std::lock_guard<std::recursive_mutex> lock(g_lock);
	auto it = g_textures.find(texture);
	if (it == g_textures.end()) return;
	for (Level& l : it->second.levels) DropCopy(it->second, l);
	for (IDirect3DSurface9* s : it->second.surfaces) g_surfaces.erase(s);
	g_textures.erase(it);
	if (g_textures.empty()) ReleaseTargets();   // (nothing of ours keeps the device then)
}

} // namespace MmlManaged
