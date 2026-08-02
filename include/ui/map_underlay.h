#pragma once

class PlaneGfx;

namespace ui::map_underlay {

/** True when the compiled map extract covers the current radar center. */
bool available();

/** Shaded-relief terrain, water and major roads/rivers for the current range
 *  and facing, drawn beneath the grid. No-op when available() is false. Heavy
 *  (per-pixel terrain pass) — call only from background-sprite rebuilds. */
void draw(PlaneGfx& gfx);

}  // namespace ui::map_underlay
