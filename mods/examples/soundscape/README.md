# Soundscape

Optional 3D sound visualization for Mafia 1.0 and 1.2. Shift+F10 toggles visibility.
Options > Mods > Soundscape controls the waves, reflections, room display, labels and speed.
Build mod_soundscape, deploy soundscape, then load the mod. The repository defaults to disabled.

- Cyan spherical fronts travel from native spatial sources. The mod reads a 40 ms RMS window from
  supported PCM WAV assets at the native playback cursor. The source gain and audio master
  determine the emitted level. The actual DirectSound buffer gain drives the listener meter.
  Loudness changes both the brightness and the maximum reach. Continuous sounds emit pulses
  on an illustrative cadence, and live playback timestamps detect source restarts. The direct
  front fades smoothly where the first wall intercepts it.
- Amber pulses and paths follow up to two reflections against native static collision.
  They appear in the scene and the inset. The display uses slowed propagation and uniform reflection loss.
- Violet surfaces start with collision triangles found by 264 probes within 30 m of the rendered
  camera. Adjacent coplanar collision triangles extend those surfaces, with only exterior edges
  and creases outlined. The cutaway inset shows the sampled span. The percentage counts ray hits.
- When EAX is active, early and late levels are displayed separately and the graph shows the
  recent late envelope. The model uses the native gain, delay, decay and echo settings. Its
  time history is independent of the illustrative wave speed. This estimates the wet effect.

A camera override can place the geometry probe away from the native audio listener. Short sounds
between polls and unsupported PCM formats can be absent from the visualization. Hide, mission
change and unload clear visual history. The mod only reads audio state.

Rendering uses the shared C++ SDK legacy_mesh header and the borrowed Direct3D 8 device. It
restores state after drawing and uses the same path on native D3D8 and D3D8 wrappers.

Validation on 2026-11-14 covered 18 offline checks and 22 live checks per build. Both builds
showed the expected eightfold source-amplitude change and zero level at mute. The owner copy
rendered on native D3D8 after hot load with the same 112-byte EAX packet. Lab preview receipts
for the prison corridor are in build/soundscape-continuity-20261114. The lab audio provider did
not expose active EAX. The owner copy reported the corridor's EAX decay and delay after hot load.
