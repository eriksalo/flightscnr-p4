#pragma once

namespace ui {

/** Draw the sleep screen (black, dim drifting message) and reset the drift. */
void sleepScreenEnter();

/** Move the message to its next spot when due; call each loop while asleep. */
void sleepScreenTick();

}  // namespace ui
