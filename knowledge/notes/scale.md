# The size things are drawn at (read 2026-10-23 in both builds' LS3DF; seen in both labs)

What `actor.scale` (mml.h actor_scale, host/scale.cpp) rests on.

- **code** An I3D_frame carries a scale at `+0x90` (three floats, x y z). `I3D_frame::UpdateMatrix` (1.2 ls3d+1A6E0,
  1.0 +1A4A0, the same code) builds the local matrix `+0x50`: with `+0xAC` bit 0x10 clear, its rows from the
  rotation (`+0x9C`, w x y z) and each row times its scale (flags |= 0x14); with 0x10 set and 0x4 clear, the rows
  as they are times the scale (the rotation remade from them when 0x8 is clear; flags |= 0xC); with 0x10 and 0x4
  set, nothing. So 0x4 means "the scale is in the rows". The translation (`+0x80`) is never scaled.
- **code** `UpdateWMatrixProc` (1.2 ls3d+1C7D0): the same local step, then the world matrix (`+0x10`) is the local
  times the parent's world (the parent's first while its 0x20 is clear), `+0x114` the world's mean scale
  ((sx + sy + sz) times the parent's, times a third), and 0x20 set. `I3D_frame::Update` (vtable +0x18; 1.2
  ls3d+1CA60) passes 0x40000000 ("changed") down as each child's 0x2000 with its 0x20 and 0x100 cleared.
- **code** The frame's hierarchy box is at `+0xB0` (min) and `+0xBC` (max), in the frame's own space (the taxi's:
  x -0.93..0.93, y -0.30..2.06, z -2.34..2.47: its origin 0.3 m over the road), valid while 0x100 is set.
- **game** The scripts have it too: `FRM_SETSCALE`, `FRM_GETSCALE`, `FRM_GETWORLDSCALE`; Game.exe's inline
  `I3D_Frame::SetScale` (1.2 0x47A750) writes `+0x90` and the flags as props.cpp's WriteLocal does.
- **game** A scale written into an actor's model frame (the taxi, 2, 1.2 lab) holds: the car's own SetPos/SetRot
  keep `+0x90`, every frame's matrix is built with it, and the car is drawn at twice its size while it drives. But
  the game reads those matrices back: the owner watching saw the car drive as another car (its wheels' frames, the
  matrices the vehicle code takes from its model, were scaled with it; which reads exactly was not traced). So a
  scale in the frame is not a drawing alone.
- **game** The loader's way: the scale lives only around the world's `Render` (render.cpp: after the mods'
  RENDER_BEGIN, before the shadow map and the scene views; given back right after the world is drawn). The local
  matrix is kept, its rows scaled, its translation moved so the pivot (the box's bottom under its middle by default)
  stays put, `+0x90` set (for `+0x114`), the flags 0x10 | 0x4 kept so UpdateMatrix leaves the rows alone, 0x20 and
  0x100 cleared, 0x40000000 set, and `Update` run; the kept matrix, scale and flags put back the same way. An A/B
  from one restore point (1.2 lab, Accelerate 3 s): (-1985.51, -4.805, 18.913) at its own size, (-1985.67, -4.803,
  18.916) drawn at 2: the same drive within the input's timing.
- **game** The bridge's thread may read a frame while it is drawn (mem_read during Render saw the train's world
  matrix with its y row 2.5 long): a reader on another thread may see the scaled matrix; the game's tick never does.
- **game** Actor types seen in the city freeride (1.2): 2 the player, 4 cars, 8 trams (`salina*`) and the elevated
  trains (`metro*`), 5, 6, 12, 18, 20, 25, 35 (scenery and managers). An elevated train far from the player was not
  found drawn where its actor stood (it may be drawn only near the player); not looked into.
- **code** A train's other cars (both builds, `C_railway::GetVagon` 1.2 0x496E10, 1.0 0x4E82D0, the same code): car
  0 is the actor's model (+0x68); the others are model frames of their own in the sector, named `<name>_vagon<i>`
  (C_railway::GameInit), not children of the first: +0xC4 points to their frames, +0x1A8 counts them (metro6 in the
  1.2 lab: one, `metro6_vagon0`, 12.5 m behind). actor_scale scales them all (game.h kRailwayVagons).
- **code** The traffic's cars (both builds; game_build.h trafficElements, 1.2 0x63D848, 1.0 0x6561A0, checked by
  tools/check_builds.py in C_traffic_car::ChcesMeSejmout): a vector of element pointers, an element's +0 byte set
  while it is out, its +0xC a C_car (type 4, frame `traffic_element`, `save_excluded`), parked cars among them: 13 in
  the city freeride. The game's actors' lists (C_mission +0x38, C_game +0x124) leave them out, so world_near now adds
  them (game.h ListTrafficCars). The owner of the collision's class-0x100 boxes is the pedestrians' system
  (C_don_ocas, type 19), not a person.
