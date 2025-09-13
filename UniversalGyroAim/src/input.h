#ifndef INPUT_H
#define INPUT_H

#include "state.h"

void Input_HandleGamepadAdded(SDL_Event* event);
void Input_HandleGamepadRemoved(SDL_Event* event);
void Input_HandleGamepadButton(SDL_Event* event);
void Input_HandleGamepadAxis(SDL_Event* event);
void Input_HandleGamepadSensor(SDL_Event* event);
void Input_UpdateCalibrationState(void);
void Input_ProcessAndPassthrough(XUSB_REPORT* report);

#endif