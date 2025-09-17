#include "config.h"
#include <shlwapi.h>
#pragma comment(lib, "shlwapi.lib")
#include <ShlObj.h>
#include <direct.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

static bool GetProfilesDir(char* path_buffer, size_t buffer_size)
{
	if (GetModuleFileNameA(NULL, path_buffer, (DWORD)buffer_size) == 0) {
		return false;
	}
	PathRemoveFileSpecA(path_buffer);

	if (PathAppendA(path_buffer, PROFILES_DIRECTORY) == FALSE) {
		return false;
	}
	_mkdir(path_buffer);

	return true;
}

void SetDefaultSettings(void) {
	SDL_Log("Loading default settings.");
	settings.selected_button = -1;
	settings.selected_axis = -1;
	settings.sensitivity = 5.0f;
	settings.invert_gyro_x = false;
	settings.invert_gyro_y = false;
	settings.anti_deathzone = 0.0f;
	settings.always_on_gyro = false;
	settings.mouse_mode = false;
	settings.mouse_sensitivity = 5000.0f;
	settings.config_version = CURRENT_CONFIG_VERSION;
	settings.led_r = 48;
	settings.led_g = 48;
	settings.led_b = 48;
	settings.gyro_calibration_offset[0] = 0.0f;
	settings.gyro_calibration_offset[1] = 0.0f;
	settings.gyro_calibration_offset[2] = 0.0f;
	settings.flick_stick_enabled = false;
	settings.flick_stick_calibrated = false;
	settings.flick_stick_calibration_value = 12000.0f;
}

static SDL_GamepadButton GamepadButtonFromString(const char* str) {
	if (!str) return SDL_GAMEPAD_BUTTON_INVALID;
	for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT; ++i) {
		const char* btn_str = SDL_GetGamepadStringForButton((SDL_GamepadButton)i);
		if (btn_str && _stricmp(str, btn_str) == 0) {
			return (SDL_GamepadButton)i;
		}
	}
	return SDL_GAMEPAD_BUTTON_INVALID;
}

static SDL_GamepadAxis GamepadAxisFromString(const char* str) {
	if (!str) return SDL_GAMEPAD_AXIS_INVALID;
	for (int i = 0; i < SDL_GAMEPAD_AXIS_COUNT; ++i) {
		const char* axis_str = SDL_GetGamepadStringForAxis((SDL_GamepadAxis)i);
		if (axis_str && _stricmp(str, axis_str) == 0) {
			return (SDL_GamepadAxis)i;
		}
	}
	return SDL_GAMEPAD_AXIS_INVALID;
}

static bool ParseHexColor(const char* hex_string, unsigned char* r, unsigned char* g, unsigned char* b)
{
	if (!hex_string || !r || !g || !b) {
		return false;
	}
	const char* ptr = hex_string;
	if (*ptr == '#') {
		ptr++;
	}
	if (strlen(ptr) != 6) {
		return false;
	}
	int result = sscanf_s(ptr, "%2hhx%2hhx%2hhx", r, g, b);
	return (result == 3);
}

void SaveSettings(const char* profile_name) {
	char dir_path[MAX_PATH];
	if (!GetProfilesDir(dir_path, MAX_PATH)) {
		SDL_Log("Error: Could not determine profiles directory path.");
		return;
	}

	char full_path[MAX_PATH];
	PathCombineA(full_path, dir_path, profile_name);
	if (!PathMatchSpecA(full_path, "*.ini")) {
		strcat_s(full_path, MAX_PATH, ".ini");
	}

	FILE* file;
	if (fopen_s(&file, full_path, "w") != 0 || !file) {
		SDL_Log("Error: Could not open %s for writing.", full_path);
		return;
	}

	fprintf(file, "# Universal Gyro Aim Profile: %s\n", profile_name);
	fprintf(file, "config_version = %d\n\n", CURRENT_CONFIG_VERSION);
	fprintf(file, "mouse_mode = %s\n", settings.mouse_mode ? "true" : "false");
	fprintf(file, "sensitivity = %f\n", settings.sensitivity);
	fprintf(file, "mouse_sensitivity = %f\n", settings.mouse_sensitivity);
	fprintf(file, "always_on_gyro = %s\n", settings.always_on_gyro ? "true" : "false");
	fprintf(file, "invert_gyro_x = %s\n", settings.invert_gyro_x ? "true" : "false");
	fprintf(file, "invert_gyro_y = %s\n", settings.invert_gyro_y ? "true" : "false");
	fprintf(file, "anti_deadzone = %f\n", settings.anti_deathzone);
	if (settings.selected_button != -1) {
		fprintf(file, "aim_input_type = button\n");
		fprintf(file, "aim_input_value = %s\n", SDL_GetGamepadStringForButton(settings.selected_button));
	}
	else if (settings.selected_axis != -1) {
		fprintf(file, "aim_input_type = axis\n");
		fprintf(file, "aim_input_value = %s\n", SDL_GetGamepadStringForAxis(settings.selected_axis));
	}
	else {
		fprintf(file, "aim_input_type = none\n");
	}
	fprintf(file, "led_color = #%02X%02X%02X\n", settings.led_r, settings.led_g, settings.led_b);
	fprintf(file, "gyro_offset_pitch = %f\n", settings.gyro_calibration_offset[0]);
	fprintf(file, "gyro_offset_yaw = %f\n", settings.gyro_calibration_offset[1]);
	fprintf(file, "gyro_offset_roll = %f\n", settings.gyro_calibration_offset[2]);
	fprintf(file, "flick_stick_enabled = %s\n", settings.flick_stick_enabled ? "true" : "false");
	fprintf(file, "flick_stick_calibrated = %s\n", settings.flick_stick_calibrated ? "true" : "false");
	fprintf(file, "flick_stick_value = %f\n", settings.flick_stick_calibration_value);

	fclose(file);
	settings_are_dirty = false;
	strcpy_s(current_profile_name, sizeof(current_profile_name), profile_name);
	SDL_Log("Settings saved successfully to %s.", full_path);
}

bool LoadSettings(const char* profile_name) {
	char dir_path[MAX_PATH];
	if (!GetProfilesDir(dir_path, MAX_PATH)) {
		SDL_Log("Error: Could not determine profiles directory path for loading.");
		return false;
	}

	char full_path[MAX_PATH];
	PathCombineA(full_path, dir_path, profile_name);
	if (!PathMatchSpecA(full_path, "*.ini")) {
		strcat_s(full_path, MAX_PATH, ".ini");
	}

	FILE* file;
	if (fopen_s(&file, full_path, "r") != 0 || !file) {
		SDL_Log("Info: No profile file found (%s).", full_path);
		return false;
	}

	SetDefaultSettings();

	char line[256], key[64], value[192], aim_type[32] = "none";
	while (fgets(line, sizeof(line), file)) {
		if (line[0] == '#' || line[0] == '\n' || line[0] == '\r' || line[0] == '[') continue;
		if (sscanf_s(line, " %63[^= \t] = %191[^\n\r]", key, (unsigned)_countof(key), value, (unsigned)_countof(value)) != 2) continue;

		char* end = value + strlen(value) - 1;
		while (end > value && isspace((unsigned char)*end)) end--;
		*(end + 1) = 0;

		if (_stricmp(key, "config_version") == 0) {
			if (atoi(value) != CURRENT_CONFIG_VERSION) SDL_Log("Warning: Profile version mismatch in %s.", profile_name);
		}
		else if (_stricmp(key, "mouse_mode") == 0) {
			settings.mouse_mode = (_stricmp(value, "true") == 0);
		}
		else if (_stricmp(key, "sensitivity") == 0) {
			settings.sensitivity = (float)atof(value);
		}
		else if (_stricmp(key, "mouse_sensitivity") == 0) {
			settings.mouse_sensitivity = (float)atof(value);
		}
		else if (_stricmp(key, "always_on_gyro") == 0) {
			settings.always_on_gyro = (_stricmp(value, "true") == 0);
		}
		else if (_stricmp(key, "invert_gyro_x") == 0) {
			settings.invert_gyro_x = (_stricmp(value, "true") == 0);
		}
		else if (_stricmp(key, "invert_gyro_y") == 0) {
			settings.invert_gyro_y = (_stricmp(value, "true") == 0);
		}
		else if (_stricmp(key, "anti_deadzone") == 0) {
			settings.anti_deathzone = (float)atof(value);
		}
		else if (_stricmp(key, "aim_input_type") == 0) {
			strcpy_s(aim_type, sizeof(aim_type), value);
		}
		else if (_stricmp(key, "aim_input_value") == 0) {
			if (_stricmp(aim_type, "button") == 0) {
				settings.selected_button = GamepadButtonFromString(value); settings.selected_axis = -1;
			}
			else if (_stricmp(aim_type, "axis") == 0) {
				settings.selected_axis = GamepadAxisFromString(value); settings.selected_button = -1;
			}
		}
		else if (_stricmp(key, "led_color") == 0) {
			ParseHexColor(value, &settings.led_r, &settings.led_g, &settings.led_b);
		}
		else if (_stricmp(key, "gyro_offset_pitch") == 0) {
			settings.gyro_calibration_offset[0] = (float)atof(value);
		}
		else if (_stricmp(key, "gyro_offset_yaw") == 0) {
			settings.gyro_calibration_offset[1] = (float)atof(value);
		}
		else if (_stricmp(key, "gyro_offset_roll") == 0) {
			settings.gyro_calibration_offset[2] = (float)atof(value);
		}
		else if (_stricmp(key, "flick_stick_enabled") == 0) {
			settings.flick_stick_enabled = (_stricmp(value, "true") == 0);
		}
		else if (_stricmp(key, "flick_stick_calibrated") == 0) {
			settings.flick_stick_calibrated = (_stricmp(value, "true") == 0);
		}
		else if (_stricmp(key, "flick_stick_value") == 0) {
			settings.flick_stick_calibration_value = (float)atof(value);
		}
	}
	fclose(file);

	if (settings.flick_stick_enabled) settings.always_on_gyro = true;

	settings_are_dirty = false;
	char profile_name_no_ext[64];
	strcpy_s(profile_name_no_ext, sizeof(profile_name_no_ext), profile_name);
	PathRemoveExtensionA(profile_name_no_ext);
	strcpy_s(current_profile_name, sizeof(current_profile_name), profile_name_no_ext);
	SDL_Log("Settings loaded successfully from %s.", full_path);
	return true;
}

void UpdatePhysicalControllerLED(void)
{
	if (!gamepad) return;
	if (SDL_SetGamepadLED(gamepad, settings.led_r, settings.led_g, settings.led_b) < 0) {
		SDL_Log("Warning: Could not set gamepad LED color: %s", SDL_GetError());
	}
	else {
		SDL_Log("Successfully set physical gamepad LED to #%02X%02X%02X", settings.led_r, settings.led_g, settings.led_b);
	}
}