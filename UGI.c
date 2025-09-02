#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ViGEmClient.h>

#include <math.h>
#include <stdbool.h>
#include <stdio.h> // For snprintf

#define CLAMP(v, min, max) (((v) < (min)) ? (min) : (((v) > (max)) ? (max) : (v)))

// --- Global State ---
static SDL_Window* window = NULL;
static SDL_Renderer* renderer = NULL;
static SDL_Gamepad* gamepad = NULL;
static SDL_JoystickID gamepad_instance_id = 0;

static PVIGEM_CLIENT vigem_client = NULL;
static PVIGEM_TARGET x360_pad = NULL;

// Gyro and Aiming state
static float gyro_data[3] = { 0.0f, 0.0f, 0.0f };
static bool isAiming = false;

// User configuration
static SDL_GamepadButton selected_button = -1;
static SDL_GamepadAxis selected_axis = -1;
static float sensitivity_x = -5.0f;
static float sensitivity_y = 5.0f;

/* This function runs once at startup. */
SDL_AppResult SDL_AppInit(void** appstate, int argc, char* argv[])
{
	SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");

	if (SDL_InitSubSystem(SDL_INIT_GAMEPAD) < 0) {
		SDL_Log("Couldn't initialize gamepad subsystem: %s", SDL_GetError());
		return SDL_APP_FAILURE;
	}

	if (!SDL_CreateWindowAndRenderer("Universal Gyro Input", 500, 250, SDL_WINDOW_RESIZABLE, &window, &renderer)) {
		SDL_Log("Couldn't create window and renderer: %s", SDL_GetError());
		return SDL_APP_FAILURE;
	}

	vigem_client = vigem_alloc();
	if (vigem_client == NULL) {
		SDL_Log("Error: Failed to allocate ViGEm client.");
		return SDL_APP_FAILURE;
	}
	const VIGEM_ERROR ret = vigem_connect(vigem_client);
	if (!VIGEM_SUCCESS(ret)) {
		SDL_Log("Error: ViGEmBus connection failed: 0x%x. Is the driver installed?", ret);
		return SDL_APP_FAILURE;
	}

	x360_pad = vigem_target_x360_alloc();
	const VIGEM_ERROR add_ret = vigem_target_add(vigem_client, x360_pad);
	if (!VIGEM_SUCCESS(add_ret)) {
		SDL_Log("Error: Failed to add virtual X360 controller: 0x%x", add_ret);
		return SDL_APP_FAILURE;
	}

	SDL_Log("App started. Virtual Xbox 360 controller is active.");
	return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void* appstate, SDL_Event* event)
{
	switch (event->type) {
	case SDL_EVENT_QUIT:
		return SDL_APP_SUCCESS;

	case SDL_EVENT_KEY_DOWN:
		switch (event->key.key) {
		case SDLK_C:
			SDL_Log("Change aim button requested. Press a button or trigger on the gamepad.");
			selected_button = -1;
			selected_axis = -1;
			isAiming = false;
			break;
		case SDLK_UP:
			sensitivity_y += 0.5f;
			sensitivity_x -= 0.5f;
			SDL_Log("Sensitivity increased to X: %.2f, Y: %.2f", sensitivity_x, sensitivity_y);
			break;
		case SDLK_DOWN:
			sensitivity_y -= 0.5f;
			sensitivity_x += 0.5f;
			SDL_Log("Sensitivity decreased to X: %.2f, Y: %.2f", sensitivity_x, sensitivity_y);
			break;
		case SDLK_Q:
		case SDLK_ESCAPE:
			return SDL_APP_SUCCESS;
		}
		break;

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
	case SDL_EVENT_GAMEPAD_BUTTON_UP:
		if (event->gbutton.which == gamepad_instance_id) {
			if (selected_button == -1 && selected_axis == -1 && event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
				selected_button = event->gbutton.button;
				SDL_Log("Aim button set to: %s", SDL_GetGamepadStringForButton(selected_button));
			}
			else if (event->gbutton.button == selected_button) {
				isAiming = (event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
			}
		}
		break;

	case SDL_EVENT_GAMEPAD_AXIS_MOTION:
		if (event->gaxis.which == gamepad_instance_id) {
			if (selected_button == -1 && selected_axis == -1) {
				if (event->gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ||
					event->gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) {
					if (event->gaxis.value > 8000) {
						selected_axis = event->gaxis.axis;
						SDL_Log("Aim trigger set to: %s", SDL_GetGamepadStringForAxis(selected_axis));
					}
				}
			}
			else if (event->gaxis.axis == selected_axis) {
				isAiming = (event->gaxis.value > 8000);
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

SDL_AppResult SDL_AppIterate(void* appstate)
{
	XUSB_REPORT report;
	XUSB_REPORT_INIT(&report);

	if (gamepad) {
		// --- Passthrough physical controller state to virtual controller ---
		// Buttons
		if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_SOUTH)) report.wButtons |= XUSB_GAMEPAD_A;
		if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_EAST)) report.wButtons |= XUSB_GAMEPAD_B;
		if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_WEST)) report.wButtons |= XUSB_GAMEPAD_X;
		if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_NORTH)) report.wButtons |= XUSB_GAMEPAD_Y;
		if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)) report.wButtons |= XUSB_GAMEPAD_LEFT_SHOULDER;
		if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)) report.wButtons |= XUSB_GAMEPAD_RIGHT_SHOULDER;
		if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_BACK)) report.wButtons |= XUSB_GAMEPAD_BACK;
		if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_START)) report.wButtons |= XUSB_GAMEPAD_START;
		if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_LEFT_STICK)) report.wButtons |= XUSB_GAMEPAD_LEFT_THUMB;
		if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_RIGHT_STICK)) report.wButtons |= XUSB_GAMEPAD_RIGHT_THUMB;
		if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_UP)) report.wButtons |= XUSB_GAMEPAD_DPAD_UP;
		if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_DOWN)) report.wButtons |= XUSB_GAMEPAD_DPAD_DOWN;
		if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_LEFT)) report.wButtons |= XUSB_GAMEPAD_DPAD_LEFT;
		if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) report.wButtons |= XUSB_GAMEPAD_DPAD_RIGHT;
		if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_GUIDE)) report.wButtons |= XUSB_GAMEPAD_GUIDE;

		// Triggers (SDL is -32768 to 32767, XUSB is 0 to 255)
		report.bLeftTrigger = (SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) + 32768) / 257;
		report.bRightTrigger = (SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) + 32768) / 257;

		// Sticks
		report.sThumbLX = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX);
		report.sThumbLY = -SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTY);
		report.sThumbRX = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHTX);
		report.sThumbRY = -SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHTY);
	}

	if (isAiming) {
		const float FACTOR = 10000.0f;
		const float DEAD_ZONE = 0.05f;

		float stick_input_x = 0.0f;
		float stick_input_y = 0.0f;

		if (fabsf(gyro_data[1]) > DEAD_ZONE) {
			stick_input_x = gyro_data[1] * sensitivity_x * FACTOR;
		}
		if (fabsf(gyro_data[0]) > DEAD_ZONE) {
			stick_input_y = gyro_data[0] * sensitivity_y * FACTOR;
		}

		report.sThumbRX = (short)CLAMP(stick_input_x, -32767.0f, 32767.0f);
		report.sThumbRY = (short)CLAMP(stick_input_y, -32767.0f, 32767.0f);
	}

	if (x360_pad && vigem_client) {
		vigem_target_x360_update(vigem_client, x360_pad, report);
	}

	// --- Drawing UI ---
	char buffer[256];
	SDL_SetRenderDrawColor(renderer, 25, 25, 40, 255);
	SDL_RenderClear(renderer);
	SDL_SetRenderDrawColor(renderer, 200, 200, 255, 255);

	float y_pos = 10.0f;
	const float line_height = (float)(SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE + 4);

	// Status message
	const char* status_message = "Status: OK";
	if (!x360_pad) {
		status_message = "Error: Virtual controller failed to start.";
	}
	else if (!gamepad) {
		status_message = "Status: Waiting for physical controller...";
	}
	else if (isAiming) {
		status_message = "Status: Aiming with Gyro...";
	}
	SDL_RenderDebugText(renderer, 10, y_pos, status_message);
	y_pos += line_height * 2;

	// Aim button info
	if (selected_button != -1) {
		snprintf(buffer, sizeof(buffer), "Aim Button: %s", SDL_GetGamepadStringForButton(selected_button));
	}
	else if (selected_axis != -1) {
		snprintf(buffer, sizeof(buffer), "Aim Trigger: %s", SDL_GetGamepadStringForAxis(selected_axis));
	}
	else {
		snprintf(buffer, sizeof(buffer), "Aim Button: [Press a button/trigger to set]");
	}
	SDL_RenderDebugText(renderer, 10, y_pos, buffer);
	y_pos += line_height;

	// Sensitivity info
	snprintf(buffer, sizeof(buffer), "Sensitivity: %.1f", sensitivity_y); // Show one value for simplicity
	SDL_RenderDebugText(renderer, 10, y_pos, buffer);
	y_pos += line_height * 2;

	// Instructions
	SDL_RenderDebugText(renderer, 10, y_pos, "--- Controls ---");
	y_pos += line_height;
	SDL_RenderDebugText(renderer, 10, y_pos, " 'C' key:          Change Aim Button");
	y_pos += line_height;
	SDL_RenderDebugText(renderer, 10, y_pos, " Up/Down Arrows:   Adjust Sensitivity");
	y_pos += line_height;
	SDL_RenderDebugText(renderer, 10, y_pos, " 'Esc' or 'Q' key: Quit");

	SDL_RenderPresent(renderer);

	SDL_Delay(1);
	return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void* appstate, SDL_AppResult result)
{
	if (gamepad) {
		SDL_CloseGamepad(gamepad);
	}
	if (vigem_client) {
		if (x360_pad) {
			vigem_target_remove(vigem_client, x360_pad);
			vigem_target_free(x360_pad);
		}
		vigem_disconnect(vigem_client);
		vigem_free(vigem_client);
	}
}