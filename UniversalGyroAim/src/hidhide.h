#ifndef HIDHIDE_H
#define HIDHIDE_H

#include "state.h"

void HidePhysicalController(SDL_Gamepad* pad_to_hide);
void UnhidePhysicalController(void);
bool IsHidHideAvailable(void);

#endif