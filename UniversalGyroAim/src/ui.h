#ifndef UI_H
#define UI_H

#include "state.h"

void UI_HandleKeyEvent(SDL_Event* event);
void UI_HandleTextInputEvent(SDL_Event* event);
void UI_Render(void);

#endif