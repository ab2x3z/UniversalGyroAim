#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL_main.h>

#include "state.h"
#include "app.h"
#include "config.h"
#include "hidhide.h"
#include "vigem.h"
#include "mouse.h"
#include "input.h"
#include "ui.h"

SDL_AppResult SDL_AppInit(void** appstate, int argc, char* argv[])
{
	SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
	if (SDL_InitSubSystem(SDL_INIT_GAMEPAD) < 0) return SDL_APP_FAILURE;
	if (!SDL_CreateWindowAndRenderer("Universal Gyro Aim", 420, 195, 0, &window, &renderer)) return SDL_APP_FAILURE;

	if (!IsHidHideAvailable()) {
		SDL_Log("Warning: HidHide driver/CLI not found. Controller hiding will not be available.");
	}

	if (!Vigem_Init()) {
		// UI will show error message, but we can continue to allow debugging.
	}

	if (!LoadSettings(DEFAULT_PROFILE_FILENAME)) {
		SetDefaultSettings();
		SaveSettings(DEFAULT_PROFILE_FILENAME);
	}
	
	if (!Mouse_StartThread()) {
		return SDL_APP_FAILURE;
	}

	return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void* appstate, SDL_Event* event)
{
	switch (event->type) {
		case SDL_EVENT_QUIT: return SDL_APP_SUCCESS;
		case SDL_EVENT_WINDOW_FOCUS_GAINED: is_window_focused = true; break;
		case SDL_EVENT_WINDOW_FOCUS_LOST: is_window_focused = false; break;
		case SDL_EVENT_KEY_DOWN: UI_HandleKeyEvent(event); break;
		case SDL_EVENT_TEXT_INPUT: UI_HandleTextInputEvent(event); break;
		case SDL_EVENT_GAMEPAD_ADDED: Input_HandleGamepadAdded(event); break;
		case SDL_EVENT_GAMEPAD_REMOVED: Input_HandleGamepadRemoved(event); break;
		case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
		case SDL_EVENT_GAMEPAD_BUTTON_UP: Input_HandleGamepadButton(event); break;
		case SDL_EVENT_GAMEPAD_AXIS_MOTION: Input_HandleGamepadAxis(event); break;
		case SDL_EVENT_GAMEPAD_SENSOR_UPDATE: Input_HandleGamepadSensor(event); break;
	}
	return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void* appstate)
{
	Input_UpdateCalibrationState();
	
	XUSB_REPORT report = { 0 };
	Input_ProcessAndPassthrough(&report);
	
	Vigem_Update(report);

	UI_Render();
	
	return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void* appstate, SDL_AppResult result)
{
	Mouse_StopThread();
	UnhidePhysicalController();
	Vigem_Shutdown();

	if (gamepad) {
		SDL_CloseGamepad(gamepad);
	}
	SDL_DestroyRenderer(renderer);
	SDL_DestroyWindow(window);
	SDL_Quit();
}