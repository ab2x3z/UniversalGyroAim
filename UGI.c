#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <math.h>
#include <stdbool.h>

static SDL_Window* window = NULL;
static SDL_Renderer* renderer = NULL;

static SDL_Gamepad* gamepad = NULL;
static SDL_JoystickID gamepad_instance_id = 0;

// We will now track the global mouse position, not a window-relative one.
static float mouse_x = 0.0f;
static float mouse_y = 0.0f;
static float gyro_data[3] = { 0.0f, 0.0f, 0.0f };
static bool isAiming = false;
static SDL_GamepadButton selected_button = -1;
static SDL_GamepadAxis selected_axis = -1;

/* This function runs once at startup. */
SDL_AppResult SDL_AppInit(void** appstate, int argc, char* argv[])
{
	SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");

	/* Initialize the gamepad subsystem */
	if (SDL_InitSubSystem(SDL_INIT_GAMEPAD) < 0) {
		SDL_Log("Couldn't initialize gamepad subsystem: %s", SDL_GetError());
		return SDL_APP_FAILURE;
	}

	// This lets you focus on other applications while this one runs in the background.
	if (!SDL_CreateWindowAndRenderer("Global Gyro Aim", 400, 150, SDL_WINDOW_RESIZABLE, &window, &renderer)) {
		SDL_Log("Couldn't create window and renderer: %s", SDL_GetError());
		return SDL_APP_FAILURE;
	}

	/* Get the initial global mouse position when the app starts. */
	SDL_GetGlobalMouseState(&mouse_x, &mouse_y);
	SDL_Log("App started. Global Gyro Aim is running in the background.");

	return SDL_APP_CONTINUE;
}

/* This function runs when a new event (mouse input, keypresses, etc) occurs. */
SDL_AppResult SDL_AppEvent(void* appstate, SDL_Event* event)
{
	switch (event->type) {
	case SDL_EVENT_KEY_DOWN:
	case SDL_EVENT_QUIT:
		return SDL_APP_SUCCESS;

	case SDL_EVENT_GAMEPAD_ADDED:
		if (!gamepad) {
			gamepad = SDL_OpenGamepad(event->gdevice.which);
			if (gamepad) {
				gamepad_instance_id = event->gdevice.which;
				SDL_Log("Opened gamepad: %s", SDL_GetGamepadName(gamepad));

				if (SDL_SetGamepadSensorEnabled(gamepad, SDL_SENSOR_GYRO, true) < 0) {
					SDL_Log("Could not enable gyroscope: %s", SDL_GetError());
				}
				else {
					SDL_Log("Gyroscope enabled!");
				}
			}
			else {
				SDL_Log("Couldn't open gamepad %d", event->gdevice.which);
			}
		}
		break;

	case SDL_EVENT_GAMEPAD_REMOVED:
		if (gamepad && event->gdevice.which == gamepad_instance_id) {
			SDL_Log("Gamepad disconnected: %s", SDL_GetGamepadName(gamepad));
			SDL_SetGamepadSensorEnabled(gamepad, SDL_SENSOR_GYRO, false);
			SDL_CloseGamepad(gamepad);
			gamepad = NULL;
		}
		break;

	case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
		if (event->gbutton.which == gamepad_instance_id) {
			if (selected_button == -1 && selected_axis == -1) {
				selected_button = event->gbutton.button;
			} else if (event->gbutton.button == selected_button) {
				isAiming = true;
				SDL_GetGlobalMouseState(&mouse_x, &mouse_y);
			}
		}
		break;

	case SDL_EVENT_GAMEPAD_BUTTON_UP:
		if (event->gbutton.which == gamepad_instance_id) {
			if (event->gbutton.button == selected_button) {
				isAiming = false;
			}
		}
		break;

	case SDL_EVENT_GAMEPAD_AXIS_MOTION:
		if (event->gaxis.which == gamepad_instance_id) {
			if (selected_button == -1 && selected_axis == -1) {
				if (event->gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ||
					event->gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) {
					selected_axis = event->gaxis.axis;
				}
			}
			else if (event->gaxis.axis == selected_axis) {
				if (event->gaxis.value > 8000) {
					if (!isAiming) {
						isAiming = true;
						SDL_GetGlobalMouseState(&mouse_x, &mouse_y);
					}
				}
				else {
					if (isAiming) {
						isAiming = false;
					}
				}
			}
		}
		break;

	case SDL_EVENT_GAMEPAD_SENSOR_UPDATE:
		if (event->gsensor.sensor == SDL_SENSOR_GYRO) {
			gyro_data[0] = event->gsensor.data[0];
			gyro_data[1] = event->gsensor.data[1];
			gyro_data[2] = event->gsensor.data[2];
		}
		break;
	}

	return SDL_APP_CONTINUE;
}

/* This function runs once per frame, and is the heart of the program. */
SDL_AppResult SDL_AppIterate(void* appstate)
{
	const char* message = "Controller connected. Minimize this window.";
	if (!gamepad) {
		message = "Waiting for controller...";
	}
	else if (isAiming) {
		message = "Aiming...";
	}
	else if (selected_button == -1 && selected_axis == -1) {
		message = "Press a button to select it for aiming.";
	}
	else {
		message = "Waiting for aim...";
	}

	const float SENSITIVITY = -0.5f;
	const float DEAD_ZONE = 0.1f;

	if (isAiming) {
		float delta_x = 0.0f;
		float delta_y = 0.0f;

		/* Map gyro axes to mouse movement */
		if (fabsf(gyro_data[1]) > DEAD_ZONE) {
			delta_x = gyro_data[1] * SENSITIVITY;
		}
		if (fabsf(gyro_data[0]) > DEAD_ZONE) {
			delta_y = gyro_data[0] * SENSITIVITY;
		}

		/* Update our high-precision mouse coordinates */
		mouse_x += delta_x;
		mouse_y += delta_y;

		SDL_WarpMouseGlobal(mouse_x, mouse_y);
	}


	/* --- Drawing Code (for status display) --- */
	int w = 0, h = 0;
	SDL_GetRenderOutputSize(renderer, &w, &h);

	float x = (w - SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE * SDL_strlen(message)) / 2;
	float y = (h - SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE) / 2;

	SDL_SetRenderDrawColor(renderer, 25, 25, 40, 255);
	SDL_RenderClear(renderer);
	SDL_SetRenderDrawColor(renderer, 200, 200, 255, 255);
	SDL_RenderDebugText(renderer, x, y, message);
	SDL_RenderPresent(renderer);

	// A small delay to prevent the app from using 100% CPU when idle.
	SDL_Delay(1);

	return SDL_APP_CONTINUE;
}

/* This function runs once at shutdown. */
void SDL_AppQuit(void* appstate, SDL_AppResult result)
{
	if (gamepad) {
		SDL_CloseGamepad(gamepad);
	}
}