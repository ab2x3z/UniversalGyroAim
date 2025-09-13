#include "app.h"
#include "vigem.h"
#include "config.h"
#include "hidhide.h"
#include "input.h"

void App_FindAndOpenPhysicalGamepad(void)
{
	if (gamepad) return;

	SDL_JoystickID* joysticks = SDL_GetGamepads(NULL);
	if (joysticks) {
		for (int i = 0; joysticks[i] != 0; ++i) {
			SDL_Event event;
			event.type = SDL_EVENT_GAMEPAD_ADDED;
			event.gdevice.which = joysticks[i];
			Input_HandleGamepadAdded(&event);
			if (gamepad) break; // Found one
		}
		SDL_free(joysticks);
	}
}

bool App_Reset(void)
{
	SDL_Log("--- RESETTING APPLICATION ---");

	if (gamepad) {
		UnhidePhysicalController();
		SDL_SetGamepadSensorEnabled(gamepad, SDL_SENSOR_GYRO, false);
		SDL_CloseGamepad(gamepad);
		gamepad = NULL;
	}
	Vigem_Shutdown();

	gamepad_instance_id = 0;
	controller_has_led = false;
	calibration_state = CALIBRATION_IDLE;
	SetDefaultSettings();

	if (!Vigem_Init()) {
		SDL_Log("FATAL: Failed to re-initialize ViGEmBus during reset.");
		return false;
	}

	App_FindAndOpenPhysicalGamepad();
	
	if (!LoadSettings(DEFAULT_PROFILE_FILENAME)) {
		SetDefaultSettings();
	}
	settings_are_dirty = false;

	SDL_Log("--- RESET COMPLETE ---");
	return true;
}