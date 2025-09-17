#include "input.h"
#include "hidhide.h"
#include "config.h"
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void Input_HandleGamepadAdded(SDL_Event* event)
{
	SDL_Gamepad* temp_pad = SDL_OpenGamepad(event->gdevice.which);
	if (!temp_pad) return;

	Uint16 vendor = SDL_GetGamepadVendor(temp_pad);
	Uint16 product = SDL_GetGamepadProduct(temp_pad);
	const char* name = SDL_GetGamepadName(temp_pad);

	if (vendor == VIRTUAL_VENDOR_ID && product == VIRTUAL_PRODUCT_ID) {
		SDL_Log("Ignoring our own virtual controller.");
		SDL_CloseGamepad(temp_pad);
	}
	else if (!gamepad) {
		gamepad = temp_pad;
		gamepad_instance_id = event->gdevice.which;
		SDL_Log("Opened gamepad: %s (VID: %04X, PID: %04X)", name, vendor, product);

		HidePhysicalController(gamepad);
		if (SDL_SetGamepadSensorEnabled(gamepad, SDL_SENSOR_GYRO, true) < 0) {
			SDL_Log("Could not enable gyroscope: %s", SDL_GetError());
		}
		else {
			SDL_Log("Gyroscope enabled!");
		}

		SDL_PropertiesID props = SDL_GetGamepadProperties(gamepad);
		controller_has_led = SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_RGB_LED_BOOLEAN, false);
		if (controller_has_led) {
			SDL_Log("Controller supports programmable LED.");
			UpdatePhysicalControllerLED();
		}
		else {
			SDL_Log("Controller does not support programmable LED.");
		}
	}
	else {
		SDL_Log("Ignoring additional controller: %s", SDL_GetGamepadName(temp_pad));
		SDL_CloseGamepad(temp_pad);
	}
	force_one_render = true;
}

void Input_HandleGamepadRemoved(SDL_Event* event)
{
	if (gamepad && event->gdevice.which == gamepad_instance_id) {
		SDL_Log("Gamepad disconnected: %s", SDL_GetGamepadName(gamepad));
		UnhidePhysicalController();
		SDL_SetGamepadSensorEnabled(gamepad, SDL_SENSOR_GYRO, false);
		SDL_CloseGamepad(gamepad);
		gamepad = NULL;
		controller_has_led = false;
		force_one_render = true;
		settings.selected_button = -1;
		settings.selected_axis = -1;
		isAiming = false;

		EnterCriticalSection(&data_lock);
		shared_mouse_aim_active = false;
		shared_gyro_data[0] = 0.0f;
		shared_gyro_data[1] = 0.0f;
		shared_gyro_data[2] = 0.0f;
		shared_flick_stick_delta_x = 0.0f;
		LeaveCriticalSection(&data_lock);
	}
}

void Input_HandleGamepadButton(SDL_Event* event)
{
	if (event->gbutton.which != gamepad_instance_id) return;

	if (is_waiting_for_aim_button && event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
		settings.selected_button = event->gbutton.button;
		SDL_Log("Aim button set to: %s", SDL_GetGamepadStringForButton(settings.selected_button));
		is_waiting_for_aim_button = false;
		settings_are_dirty = true;
		return;
	}

	bool button_handled = false;
	if (event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
		if (calibration_state == FLICK_STICK_CALIBRATION_START) {
			if (event->gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH) {
				calibration_state = FLICK_STICK_CALIBRATION_TURNING;
				flick_stick_turn_remaining = settings.flick_stick_calibration_value;
				button_handled = true;
			}
			else if (event->gbutton.button == SDL_GAMEPAD_BUTTON_EAST) {
				calibration_state = CALIBRATION_IDLE;
				force_one_render = true;
				button_handled = true;
			}
		}
		else if (calibration_state == FLICK_STICK_CALIBRATION_ADJUST) {
			if (event->gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_UP) settings.flick_stick_calibration_value += 50.0f;
			else if (event->gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_DOWN) settings.flick_stick_calibration_value -= 50.0f;
			else if (event->gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_RIGHT) settings.flick_stick_calibration_value += 1.0f;
			else if (event->gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_LEFT) settings.flick_stick_calibration_value -= 1.0f;
			else if (event->gbutton.button == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) settings.flick_stick_calibration_value += 500.0f;
			else if (event->gbutton.button == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) settings.flick_stick_calibration_value -= 500.0f;
			else if (event->gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH) {
				calibration_state = FLICK_STICK_CALIBRATION_TURNING;
				flick_stick_turn_remaining = settings.flick_stick_calibration_value;
			}
			else if (event->gbutton.button == SDL_GAMEPAD_BUTTON_EAST) {
				settings.flick_stick_calibrated = true;
				calibration_state = CALIBRATION_IDLE;
				force_one_render = true;
				settings_are_dirty = true;
				SDL_Log("Flick Stick calibration saved. Value: %.2f", settings.flick_stick_calibration_value);
			}
			button_handled = true;
		}
		else if (calibration_state == CALIBRATION_WAITING_FOR_STABILITY || calibration_state == CALIBRATION_SAMPLING) {
			if (event->gbutton.button == SDL_GAMEPAD_BUTTON_EAST) {
				calibration_state = CALIBRATION_IDLE;
				stability_timer_start_time = 0;
				calibration_sample_count = 0;
				gyro_accumulator[0] = 0.0f; gyro_accumulator[1] = 0.0f; gyro_accumulator[2] = 0.0f;
				force_one_render = true;
				SDL_Log("Gyro calibration cancelled by user.");
				button_handled = true;
			}
		}
	}
	if (button_handled) return;

	if (event->gbutton.button == settings.selected_button) {
		isAiming = (event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
	}
}

void Input_HandleGamepadAxis(SDL_Event* event)
{
	if (event->gaxis.which != gamepad_instance_id) return;

	if (is_waiting_for_aim_button) {
		if ((event->gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || event->gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) && event->gaxis.value > 8000) {
			settings.selected_axis = event->gaxis.axis;
			SDL_Log("Aim trigger set to: %s", SDL_GetGamepadStringForAxis(settings.selected_axis));
			is_waiting_for_aim_button = false;
			settings_are_dirty = true;
		}
		return;
	}

	if (event->gaxis.axis == settings.selected_axis) {
		isAiming = (event->gaxis.value > 8000);
	}
}

void Input_HandleGamepadSensor(SDL_Event* event)
{
	if (event->gsensor.sensor != SDL_SENSOR_GYRO) return;

	switch (calibration_state) {
	case CALIBRATION_IDLE:
	{
		float calibrated_data[3];
		calibrated_data[0] = event->gsensor.data[0] - settings.gyro_calibration_offset[0];
		calibrated_data[1] = event->gsensor.data[1] - settings.gyro_calibration_offset[1];
		calibrated_data[2] = event->gsensor.data[2] - settings.gyro_calibration_offset[2];

		EnterCriticalSection(&data_lock);
		shared_gyro_data[0] = calibrated_data[0];
		shared_gyro_data[1] = calibrated_data[1];
		shared_gyro_data[2] = calibrated_data[2];
		LeaveCriticalSection(&data_lock);

		gyro_data[0] = calibrated_data[0];
		gyro_data[1] = calibrated_data[1];
		gyro_data[2] = calibrated_data[2];
		break;
	}
	case CALIBRATION_WAITING_FOR_STABILITY:
	{
		bool is_stable = fabsf(event->gsensor.data[0]) < GYRO_STABILITY_THRESHOLD &&
			fabsf(event->gsensor.data[1]) < GYRO_STABILITY_THRESHOLD &&
			fabsf(event->gsensor.data[2]) < GYRO_STABILITY_THRESHOLD;
		if (is_stable) {
			if (stability_timer_start_time == 0) {
				stability_timer_start_time = SDL_GetPerformanceCounter();
			}
			else {
				Uint64 elapsed_ms = (SDL_GetPerformanceCounter() - stability_timer_start_time) * 1000 / SDL_GetPerformanceFrequency();
				if (elapsed_ms >= GYRO_STABILITY_DURATION_MS) {
					calibration_state = CALIBRATION_SAMPLING;
					calibration_sample_count = 0;
					gyro_accumulator[0] = 0.0f; gyro_accumulator[1] = 0.0f; gyro_accumulator[2] = 0.0f;
					SDL_Log("Controller is stable. Starting data collection...");
				}
			}
		}
		else {
			stability_timer_start_time = 0;
		}
		break;
	}
	case CALIBRATION_SAMPLING:
		gyro_accumulator[0] += event->gsensor.data[0];
		gyro_accumulator[1] += event->gsensor.data[1];
		gyro_accumulator[2] += event->gsensor.data[2];
		calibration_sample_count++;
		break;
	default: break;
	}
}

void Input_UpdateCalibrationState(void)
{
	if (calibration_state == CALIBRATION_SAMPLING && calibration_sample_count >= CALIBRATION_SAMPLES) {
		settings.gyro_calibration_offset[0] = gyro_accumulator[0] / CALIBRATION_SAMPLES;
		settings.gyro_calibration_offset[1] = gyro_accumulator[1] / CALIBRATION_SAMPLES;
		settings.gyro_calibration_offset[2] = gyro_accumulator[2] / CALIBRATION_SAMPLES;
		calibration_state = CALIBRATION_IDLE;
		force_one_render = true;
		settings_are_dirty = true;
		SDL_Log("Calibration complete. Offsets saved -> Pitch: %.4f, Yaw: %.4f, Roll: %.4f",
			settings.gyro_calibration_offset[0], settings.gyro_calibration_offset[1], settings.gyro_calibration_offset[2]);
	}

	if (calibration_state == FLICK_STICK_CALIBRATION_TURNING) {
		const float TURN_SPEED_FACTOR = 0.15f;
		float turn_amount = flick_stick_turn_remaining * TURN_SPEED_FACTOR;
		if (fabsf(flick_stick_turn_remaining) < 1.0f) turn_amount = flick_stick_turn_remaining;

		EnterCriticalSection(&data_lock);
		shared_flick_stick_delta_x += turn_amount;
		LeaveCriticalSection(&data_lock);

		flick_stick_turn_remaining -= turn_amount;
		if (fabsf(flick_stick_turn_remaining) < 0.1f) {
			calibration_state = FLICK_STICK_CALIBRATION_ADJUST;
		}
	}
}

void Input_ProcessAndPassthrough(XUSB_REPORT* report)
{
	if (!gamepad) return;

	if (calibration_state == CALIBRATION_IDLE) {
		if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_SOUTH)) report->wButtons |= XUSB_GAMEPAD_A;
		if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_EAST)) report->wButtons |= XUSB_GAMEPAD_B;
		if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_WEST)) report->wButtons |= XUSB_GAMEPAD_X;
		if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_NORTH)) report->wButtons |= XUSB_GAMEPAD_Y;
		if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)) report->wButtons |= XUSB_GAMEPAD_LEFT_SHOULDER;
		if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)) report->wButtons |= XUSB_GAMEPAD_RIGHT_SHOULDER;
		if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_BACK)) report->wButtons |= XUSB_GAMEPAD_BACK;
		if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_START)) report->wButtons |= XUSB_GAMEPAD_START;
		if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_LEFT_STICK)) report->wButtons |= XUSB_GAMEPAD_LEFT_THUMB;
		if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_RIGHT_STICK)) report->wButtons |= XUSB_GAMEPAD_RIGHT_THUMB;
		if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_UP)) report->wButtons |= XUSB_GAMEPAD_DPAD_UP;
		if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_DOWN)) report->wButtons |= XUSB_GAMEPAD_DPAD_DOWN;
		if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_LEFT)) report->wButtons |= XUSB_GAMEPAD_DPAD_LEFT;
		if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) report->wButtons |= XUSB_GAMEPAD_DPAD_RIGHT;
		if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_GUIDE)) report->wButtons |= XUSB_GAMEPAD_GUIDE;

		report->bLeftTrigger = (SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) * 255) / 32767;
		report->bRightTrigger = (SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) * 255) / 32767;

		report->sThumbLX = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX);
		Sint16 ly = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTY);
		report->sThumbLY = (ly == -32768) ? 32767 : -ly;
	}

	bool gyro_is_active = (isAiming || settings.always_on_gyro) && (calibration_state == CALIBRATION_IDLE);
	Sint16 rx = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHTX);
	Sint16 ry = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHTY);

	if (settings.flick_stick_enabled) {
		const float FLICK_STICK_DEADZONE = 28000.0f;
		float stick_magnitude = sqrtf((float)rx * rx + (float)ry * ry);
		float flick_stick_output_x = 0.0f;

		if (stick_magnitude > FLICK_STICK_DEADZONE) {
			float current_angle = atan2f((float)-ry, (float)rx);
			if (!is_flick_stick_active) {
				is_flick_stick_active = true;
				float flick_angle = current_angle - ((float)M_PI / 2.0f);
				while (flick_angle <= -(float)M_PI) flick_angle += (2.0f * (float)M_PI);
				while (flick_angle > (float)M_PI) flick_angle -= (2.0f * (float)M_PI);
				flick_stick_output_x = -(flick_angle / (float)M_PI) * (settings.flick_stick_calibration_value / 2.0f);
			}
			else {
				float delta_angle = current_angle - flick_last_angle;
				if (delta_angle > M_PI) delta_angle -= (2.0f * (float)M_PI);
				if (delta_angle < -M_PI) delta_angle += (2.0f * (float)M_PI);
				flick_stick_output_x = -(delta_angle / (2.0f * (float)M_PI)) * settings.flick_stick_calibration_value;
			}
			flick_last_angle = current_angle;
		}
		else {
			is_flick_stick_active = false;
		}

		EnterCriticalSection(&data_lock);
		shared_mouse_aim_active = gyro_is_active;
		shared_flick_stick_delta_x += flick_stick_output_x;
		LeaveCriticalSection(&data_lock);
		report->sThumbRX = 0; report->sThumbRY = 0;
	}
	else { // Standard logic
		bool stick_in_use = sqrtf((float)rx * rx + (float)ry * ry) > 8000.0f;
		bool use_gyro_for_aim = gyro_is_active && !stick_in_use;

		report->sThumbRX = rx;
		report->sThumbRY = (ry == -32768) ? 32767 : -ry;

		if (settings.mouse_mode) {
			EnterCriticalSection(&data_lock);
			shared_mouse_aim_active = use_gyro_for_aim;
			LeaveCriticalSection(&data_lock);
		}
		else { // Joystick Mode
			EnterCriticalSection(&data_lock);
			shared_mouse_aim_active = false;
			LeaveCriticalSection(&data_lock);
			if (use_gyro_for_aim) {
				const float x_mult = settings.invert_gyro_x ? 10000.0f : -10000.0f;
				const float y_mult = settings.invert_gyro_y ? -10000.0f : 10000.0f;
				float combined_x = (float)rx + (gyro_data[1] * settings.sensitivity * x_mult);
				float combined_y = ((ry == -32768) ? 32767.f : (float)-ry) + (gyro_data[0] * settings.sensitivity * y_mult);
				report->sThumbRX = (short)CLAMP(combined_x, -32767.0f, 32767.0f);
				report->sThumbRY = (short)CLAMP(combined_y, -32767.0f, 32767.0f);
			}
		}
	}
}