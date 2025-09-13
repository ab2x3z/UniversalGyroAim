#include "ui.h"
#include "app.h"
#include "config.h"
#include "hidhide.h"
#include <shlwapi.h>
#pragma comment(lib, "shlwapi.lib")
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

// --- Forward declarations for menu functions ---
void execute_mode(int direction);
void display_mode(char* buffer, size_t size);
void execute_sensitivity(int direction);
void display_sensitivity(char* buffer, size_t size);
void execute_flick_stick(int direction);
void display_flick_stick(char* buffer, size_t size);
void execute_always_on(int direction);
void display_always_on(char* buffer, size_t size);
void execute_anti_deadzone(int direction);
void display_anti_deadzone(char* buffer, size_t size);
void execute_invert_y(int direction);
void display_invert_y(char* buffer, size_t size);
void execute_invert_x(int direction);
void display_invert_x(char* buffer, size_t size);
void execute_change_aim_button(int direction);
void display_change_aim_button(char* buffer, size_t size);
void execute_calibrate_gyro(int direction);
void display_gyro_calibration(char* buffer, size_t size);
void execute_calibrate_flick(int direction);
void display_flick_calibration(char* buffer, size_t size);
void execute_change_led(int direction);
void display_led_color(char* buffer, size_t size);
void execute_hide_controller(int direction);
void display_hide_controller(char* buffer, size_t size);
void execute_load_profile(int direction);
void display_profile_count(char* buffer, size_t size);
void execute_save_profile(int direction);
void display_current_profile(char* buffer, size_t size);
void execute_reset_app(int direction);

// --- Menu Definition (Master List) ---
static MenuItem menu_items[] = {
	{ "Mode",                  execute_mode,                display_mode },
	{ "Sensitivity",           execute_sensitivity,         display_sensitivity },
	{ "Always-On Gyro",        execute_always_on,           display_always_on },
	{ "Flick Stick",           execute_flick_stick,         display_flick_stick },
	{ "Anti-Deadzone",         execute_anti_deadzone,       display_anti_deadzone },
	{ "Invert Gyro Y",         execute_invert_y,            display_invert_y },
	{ "Invert Gyro X",         execute_invert_x,            display_invert_x },
	{ "Aim Button",            execute_change_aim_button,   display_change_aim_button },
	{ "Calibrate Gyro",        execute_calibrate_gyro,      display_gyro_calibration },
	{ "Calibrate Flick Stick", execute_calibrate_flick,     display_flick_calibration },
	{ "LED Color",             execute_change_led,          display_led_color },
	{ "Hide Controller",       execute_hide_controller,     display_hide_controller },
	{ "Load Profile",          execute_load_profile,        display_profile_count  },
	{ "Save Profile",          execute_save_profile,        display_current_profile },
	{ "Reset Application",     execute_reset_app,           NULL }
};
static const int master_num_menu_items = sizeof(menu_items) / sizeof(MenuItem);


// --- Profile Scanning Helpers ---
static void FreeProfileList() {
	if (profile_filenames) {
		for (int i = 0; i < num_profiles; ++i) free(profile_filenames[i]);
		free(profile_filenames);
		profile_filenames = NULL;
	}
	num_profiles = 0;
}

static void GetProfilesDir(char* buffer, size_t size) {
	GetModuleFileNameA(NULL, buffer, (DWORD)size);
	PathRemoveFileSpecA(buffer);
	PathAppendA(buffer, PROFILES_DIRECTORY);
}

static void ScanForProfiles() {
	FreeProfileList();
	char search_path[MAX_PATH];
	GetProfilesDir(search_path, MAX_PATH);
	PathAppendA(search_path, "*.ini");

	WIN32_FIND_DATAA find_data;
	HANDLE find_handle = FindFirstFileA(search_path, &find_data);
	if (find_handle == INVALID_HANDLE_VALUE) return;

	char** temp_list = NULL;
	int capacity = 0;
	do {
		if (!(find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
			if (num_profiles >= capacity) {
				capacity = (capacity == 0) ? 8 : capacity * 2;
				temp_list = (char**)realloc(profile_filenames, capacity * sizeof(char*));
				if (!temp_list) { FindClose(find_handle); FreeProfileList(); return; }
				profile_filenames = temp_list;
			}
			profile_filenames[num_profiles] = _strdup(find_data.cFileName);
			num_profiles++;
		}
	} while (FindNextFileA(find_handle, &find_data) != 0);
	FindClose(find_handle);
}

// --- Menu Functions ---
void execute_mode(int d) { if (d == 0) { settings.mouse_mode = !settings.mouse_mode; settings_are_dirty = true; } }
void display_mode(char* b, size_t s) { snprintf(b, s, "%s", settings.mouse_mode ? "Mouse" : "Joystick"); }
void execute_sensitivity(int d) {
	if (d == 0) return;
	if (settings.mouse_mode) {
		settings.mouse_sensitivity += (float)d * 500.0f;
		settings.mouse_sensitivity = CLAMP(settings.mouse_sensitivity, 100.0f, 20000.0f);
	}
	else {
		settings.sensitivity += (float)d * 0.5f;
		settings.sensitivity = CLAMP(settings.sensitivity, 0.5f, 50.0f);
	}
	settings_are_dirty = true;
}
void display_sensitivity(char* b, size_t s) {
	if (settings.mouse_mode) snprintf(b, s, "%.0f", settings.mouse_sensitivity);
	else snprintf(b, s, "%.1f", settings.sensitivity);
}
void execute_flick_stick(int d) {
	if (d == 0) {
		settings.flick_stick_enabled = !settings.flick_stick_enabled;
		settings.always_on_gyro = settings.flick_stick_enabled;
		is_flick_stick_active = false;
		flick_last_angle = 0.0f;
		settings_are_dirty = true;
	}
}
void display_flick_stick(char* b, size_t s) { snprintf(b, s, "%s", settings.flick_stick_enabled ? "ON" : "OFF"); }
void execute_always_on(int d) {
	if (d == 0 && !settings.flick_stick_enabled) {
		settings.always_on_gyro = !settings.always_on_gyro;
		if (settings.always_on_gyro) isAiming = false;
		settings_are_dirty = true;
	}
}
void display_always_on(char* b, size_t s) { snprintf(b, s, "%s", settings.always_on_gyro ? "ON" : "OFF"); }
void execute_anti_deadzone(int d) {
	if (d == 0) return;
	settings.anti_deathzone += (float)d * 1.0f;
	settings.anti_deathzone = CLAMP(settings.anti_deathzone, 0.0f, 90.0f);
	settings_are_dirty = true;
}
void display_anti_deadzone(char* b, size_t s) { snprintf(b, s, "%.0f%%", settings.anti_deathzone); }
void execute_invert_y(int d) { if (d == 0) { settings.invert_gyro_y = !settings.invert_gyro_y; settings_are_dirty = true; } }
void display_invert_y(char* b, size_t s) { snprintf(b, s, "%s", settings.invert_gyro_y ? "ON" : "OFF"); }
void execute_invert_x(int d) { if (d == 0) { settings.invert_gyro_x = !settings.invert_gyro_x; settings_are_dirty = true; } }
void display_invert_x(char* b, size_t s) { snprintf(b, s, "%s", settings.invert_gyro_x ? "ON" : "OFF"); }
void execute_change_aim_button(int d) { if (d == 0) { is_waiting_for_aim_button = true; settings.selected_button = -1; settings.selected_axis = -1; isAiming = false; } }
void display_change_aim_button(char* b, size_t s) {
	if (is_waiting_for_aim_button) snprintf(b, s, "[Waiting for input...]");
	else if (settings.selected_button != -1) snprintf(b, s, "%s", SDL_GetGamepadStringForButton(settings.selected_button));
	else if (settings.selected_axis != -1) snprintf(b, s, "%s", SDL_GetGamepadStringForAxis(settings.selected_axis));
	else snprintf(b, s, "[Not set]");
}
void execute_calibrate_gyro(int d) {
	if (d == 0 && gamepad && calibration_state == CALIBRATION_IDLE) {
		calibration_state = CALIBRATION_WAITING_FOR_STABILITY;
		stability_timer_start_time = 0;
	}
}
void display_gyro_calibration(char* b, size_t s) { snprintf(b, s, "P:%.3f Y:%.3f", settings.gyro_calibration_offset[0], settings.gyro_calibration_offset[1]); }
void execute_calibrate_flick(int d) {
	if (d == 0 && gamepad && calibration_state == CALIBRATION_IDLE) {
		calibration_state = FLICK_STICK_CALIBRATION_START;
	}
}
void display_flick_calibration(char* b, size_t s) { snprintf(b, s, "%.1f%s", settings.flick_stick_calibration_value, settings.flick_stick_calibrated ? "" : " (Default)"); }
void execute_change_led(int d) {
	if (d == 0) {
		is_entering_text = true;
		hex_input_buffer[0] = '#'; hex_input_buffer[1] = '\0';
		SDL_StartTextInput(window);
	}
}
void display_led_color(char* b, size_t s) {
	if (is_entering_text) snprintf(b, s, "%s", hex_input_buffer);
	else snprintf(b, s, "#%02X%02X%02X", settings.led_r, settings.led_g, settings.led_b);
}
void execute_hide_controller(int d) {
	if (d == 0 && gamepad) {
		if (is_controller_hidden) UnhidePhysicalController();
		else HidePhysicalController(gamepad);
	}
}
void display_hide_controller(char* b, size_t s) { snprintf(b, s, "%s", is_controller_hidden ? "Hidden" : "Visible"); }
void execute_load_profile(int d) {
	if (d == 0) {
		ScanForProfiles();
		if (num_profiles > 0) {
			is_choosing_profile = true;
			selected_profile_index = 0;
		}
	}
}
void display_profile_count(char* b, size_t s) {
	char search_path[MAX_PATH];
	GetProfilesDir(search_path, MAX_PATH);
	PathAppendA(search_path, "*.ini");
	WIN32_FIND_DATAA fd;
	HANDLE hFind = FindFirstFileA(search_path, &fd);
	int count = 0;
	if (hFind != INVALID_HANDLE_VALUE) {
		do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) count++; } while (FindNextFileA(hFind, &fd) != 0);
		FindClose(hFind);
	}
	snprintf(b, s, "[%d]", count);
}
void execute_save_profile(int d) {
	if (d == 0) {
		is_entering_save_filename = true;
		strcpy_s(filename_input_buffer, sizeof(filename_input_buffer), current_profile_name);
		SDL_StartTextInput(window);
	}
}
void display_current_profile(char* b, size_t s) { snprintf(b, s, "%s%s", current_profile_name, settings_are_dirty ? "*" : ""); }
void execute_reset_app(int d) { if (d == 0) App_Reset(); }

// --- Drawing Helpers ---
static void DrawFilledCircle(SDL_Renderer* renderer, int x, int y, int radius)
{
	int offsetx, offsety, d;
	offsetx = 0; offsety = radius; d = radius - 1;
	while (offsety >= offsetx) {
		SDL_RenderLine(renderer, x - offsety, y + offsetx, x + offsety, y + offsetx);
		SDL_RenderLine(renderer, x - offsetx, y + offsety, x + offsetx, y + offsety);
		SDL_RenderLine(renderer, x - offsetx, y - offsety, x + offsetx, y - offsety);
		SDL_RenderLine(renderer, x - offsety, y - offsetx, x + offsety, y - offsetx);
		if (d >= 2 * offsetx) {
			d -= 2 * offsetx + 1; offsetx++;
		}
		else if (d < 2 * (radius - offsety)) {
			d += 2 * offsety - 1; offsety--;
		}
		else { d += 2 * (offsety - offsetx - 1); offsety--; offsetx++; }
	}
}
static void DrawCircle(SDL_Renderer* renderer, int centreX, int centreY, int radius) {
	const int32_t diameter = (radius * 2);
	int32_t x = (radius - 1); int32_t y = 0; int32_t tx = 1; int32_t ty = 1; int32_t error = (tx - diameter);
	while (x >= y) {
		SDL_RenderPoint(renderer, centreX + x, centreY - y); SDL_RenderPoint(renderer, centreX + x, centreY + y);
		SDL_RenderPoint(renderer, centreX - x, centreY - y); SDL_RenderPoint(renderer, centreX - x, centreY + y);
		SDL_RenderPoint(renderer, centreX + y, centreY - x); SDL_RenderPoint(renderer, centreX + y, centreY + x);
		SDL_RenderPoint(renderer, centreX - y, centreY - x); SDL_RenderPoint(renderer, centreX - y, centreY + x);
		if (error <= 0) { ++y; error += ty; ty += 2; }
		if (error > 0) { --x; tx += 2; error += (tx - diameter); }
	}
}

// --- Event Handlers ---
void UI_HandleKeyEvent(SDL_Event* event) {
	if (is_entering_save_filename) {
		if (event->key.key == SDLK_BACKSPACE && strlen(filename_input_buffer) > 0) {
			filename_input_buffer[strlen(filename_input_buffer) - 1] = '\0';
		}
		else if (event->key.key == SDLK_RETURN || event->key.key == SDLK_KP_ENTER) {
			if (strlen(filename_input_buffer) > 0) SaveSettings(filename_input_buffer);
			is_entering_save_filename = false; SDL_StopTextInput(window);
		}
		else if (event->key.key == SDLK_ESCAPE) {
			is_entering_save_filename = false; SDL_StopTextInput(window);
		}
		return;
	}
	if (is_choosing_profile) {
		switch (event->key.key) {
		case SDLK_UP: selected_profile_index = (selected_profile_index - 1 + num_profiles) % num_profiles; break;
		case SDLK_DOWN: selected_profile_index = (selected_profile_index + 1) % num_profiles; break;
		case SDLK_RETURN: case SDLK_KP_ENTER:
			if (num_profiles > 0) { LoadSettings(profile_filenames[selected_profile_index]); UpdatePhysicalControllerLED(); }
		case SDLK_ESCAPE: is_choosing_profile = false; FreeProfileList(); break;
		}
		return;
	}
	if (is_entering_text) {
		if (event->key.key == SDLK_BACKSPACE && strlen(hex_input_buffer) > 1) {
			hex_input_buffer[strlen(hex_input_buffer) - 1] = '\0';
		}
		else if (event->key.key == SDLK_RETURN || event->key.key == SDLK_KP_ENTER) {
			unsigned char r, g, b;
			if (sscanf_s(hex_input_buffer, "#%02hhx%02hhx%02hhx", &r, &g, &b) == 3) {
				settings.led_r = r; settings.led_g = g; settings.led_b = b;
				UpdatePhysicalControllerLED(); settings_are_dirty = true;
			}
			is_entering_text = false; SDL_StopTextInput(window);
		}
		else if (event->key.key == SDLK_ESCAPE) {
			is_entering_text = false; SDL_StopTextInput(window);
		}
		return;
	}
	if (is_waiting_for_aim_button) {
		if (event->key.key == SDLK_ESCAPE) is_waiting_for_aim_button = false;
		return;
	}

	int direction = 0; // Stays 0 for Enter
	switch (event->key.key) {
	case SDLK_UP: selected_menu_item = (selected_menu_item - 1 + num_visible_menu_items) % num_visible_menu_items; return;
	case SDLK_DOWN: selected_menu_item = (selected_menu_item + 1) % num_visible_menu_items; return;
	case SDLK_LEFT: direction = -1; break;
	case SDLK_RIGHT: direction = 1; break;
	case SDLK_RETURN: case SDLK_KP_ENTER: direction = 0; break;
	default: return; // No relevant key, do nothing
	}

	if (num_visible_menu_items > 0) {
		int master_index = visible_menu_map[selected_menu_item];
		strcpy_s(active_menu_label, sizeof(active_menu_label), menu_items[master_index].label);
		if (menu_items[master_index].execute) menu_items[master_index].execute(direction);
	}
}

void UI_HandleTextInputEvent(SDL_Event* event) {
	if (is_entering_text) {
		if (strlen(hex_input_buffer) < 7) strcat_s(hex_input_buffer, sizeof(hex_input_buffer), event->text.text);
	}
	else if (is_entering_save_filename) {
		if (strlen(filename_input_buffer) < sizeof(filename_input_buffer) - 1) {
			for (char* c = event->text.text; *c; c++) {
				if (isalnum((unsigned char)*c) || *c == '_' || *c == '-' || *c == ' ') {
					strncat_s(filename_input_buffer, sizeof(filename_input_buffer), c, 1);
				}
			}
		}
	}
}

static void BuildVisibleMenu(void) {
	num_visible_menu_items = 0;
	for (int i = 0; i < master_num_menu_items; ++i) {
		bool show = true;
		const char* label = menu_items[i].label;
		if ((strcmp(label, "Mode") == 0 || strcmp(label, "Always-On Gyro") == 0) && settings.flick_stick_enabled) show = false;
		else if (strcmp(label, "Flick Stick") == 0 && !settings.mouse_mode) show = false;
		else if (strcmp(label, "Calibrate Flick Stick") == 0 && !settings.flick_stick_enabled) show = false;
		else if (strcmp(label, "Anti-Deadzone") == 0 && settings.mouse_mode) show = false;
		else if (strcmp(label, "LED Color") == 0 && !controller_has_led) show = false;
		if (show) visible_menu_map[num_visible_menu_items++] = i;
	}
	if (selected_menu_item >= num_visible_menu_items) selected_menu_item = num_visible_menu_items > 0 ? num_visible_menu_items - 1 : 0;
}

static void RenderStatusMessage(const char* msg1, const char* msg2, const char* msg3) {
	int w, h;
	SDL_GetRenderOutputSize(renderer, &w, &h);
	float y = (h - (float)SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE * 4) / 2.0f;
	float line_height = (float)(SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE + 4);

	if (msg1) {
		float x = (w - (float)SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE * strlen(msg1)) / 2.0f;
		SDL_RenderDebugText(renderer, x, y, msg1);
		y += line_height * 1.5f;
	}
	if (msg2) {
		float x = (w - (float)SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE * strlen(msg2)) / 2.0f;
		SDL_RenderDebugText(renderer, x, y, msg2);
		y += line_height;
	}
	if (msg3) {
		float x = (w - (float)SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE * strlen(msg3)) / 2.0f;
		SDL_RenderDebugText(renderer, x, y, msg3);
	}
}

void UI_Render(void) {
	if (!is_window_focused && calibration_state == CALIBRATION_IDLE && !force_one_render) {
		SDL_Delay(1);
		return;
	}
	force_one_render = false;

	SDL_SetRenderDrawColor(renderer, 25, 25, 40, 255);
	SDL_RenderClear(renderer);

	int w, h;
	SDL_GetRenderOutputSize(renderer, &w, &h);
	const float line_height = (float)(SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE + 4);
	float y_pos;

	if (!vigem_found) {
		SDL_SetRenderDrawColor(renderer, 255, 100, 100, 255);
		RenderStatusMessage("CRITICAL ERROR: ViGEmBus driver not found!", "Please install it from:", "github.com/ViGEm/ViGEmBus/releases");
	}
	else if (!gamepad) {
		SDL_SetRenderDrawColor(renderer, 200, 200, 255, 255);
		RenderStatusMessage(NULL, "Waiting for physical controller...", NULL);
	}
	else if (is_waiting_for_aim_button) {
		SDL_SetRenderDrawColor(renderer, 255, 255, 100, 255);
		RenderStatusMessage("SET AIM BUTTON", "Press a button or pull a trigger.", "Press ESC to cancel.");
	}
	else if (is_entering_save_filename) {
		SDL_SetRenderDrawColor(renderer, 255, 255, 100, 255);
		char buffer[128];
		snprintf(buffer, sizeof(buffer), "%s_", filename_input_buffer);
		RenderStatusMessage("SAVE PROFILE", buffer, "Press ESC to cancel.");
	}
	else if (is_choosing_profile) {
		y_pos = 10.0f;
		const char* title = "LOAD PROFILE (ENTER to select, ESC to cancel)";
		float x_title = (w - (float)SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE * strlen(title)) / 2.0f;
		SDL_SetRenderDrawColor(renderer, 255, 255, 100, 255);
		SDL_RenderDebugText(renderer, x_title, y_pos, title);
		y_pos += line_height * 2.0f;

		for (int i = 0; i < num_profiles; ++i) {
			bool is_selected = (i == selected_profile_index);
			if (is_selected) { SDL_SetRenderDrawColor(renderer, 255, 255, 100, 255); }
			else { SDL_SetRenderDrawColor(renderer, 200, 200, 255, 255); }
			char display_buffer[128];
			snprintf(display_buffer, sizeof(display_buffer), "%s %s", is_selected ? ">" : " ", profile_filenames[i]);
			SDL_RenderDebugText(renderer, 20, y_pos, display_buffer);
			y_pos += line_height;
		}
	}
	else if (calibration_state != CALIBRATION_IDLE) {
		y_pos = (h - (float)SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE * 7) / 2.0f;
		char buffer[128];
		SDL_SetRenderDrawColor(renderer, 0, 128, 255, 255);

		if (calibration_state == CALIBRATION_WAITING_FOR_STABILITY) {
			const char* msg1 = "GYRO CALIBRATION: WAITING FOR STABILITY...";
			const char* msg2 = "Press (B) on controller to cancel.";
			float x1 = (w - (float)SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE * strlen(msg1)) / 2.0f;
			float x_cancel = (w - (float)SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE * strlen(msg2)) / 2.0f;
			SDL_RenderDebugText(renderer, x1, y_pos, msg1);
			y_pos += line_height;

			if (stability_timer_start_time > 0) {
				Uint64 elapsed_ms = (SDL_GetPerformanceCounter() - stability_timer_start_time) * 1000 / SDL_GetPerformanceFrequency();
				int remaining_secs = (int)((GYRO_STABILITY_DURATION_MS - elapsed_ms) / 1000) + 1;
				snprintf(buffer, sizeof(buffer), "Keep still for %d more seconds...", remaining_secs > 0 ? remaining_secs : 0);
			}
			else {
				snprintf(buffer, sizeof(buffer), "Place controller on a flat surface.");
			}
			float x2 = (w - (float)SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE * strlen(buffer)) / 2.0f;
			SDL_RenderDebugText(renderer, x2, y_pos, buffer);
			y_pos += line_height * 1.5f;
			SDL_RenderDebugText(renderer, x_cancel, y_pos, msg2);
		}
		else if (calibration_state == CALIBRATION_SAMPLING) {
			snprintf(buffer, sizeof(buffer), "GYRO CALIBRATION: SAMPLING... (%d / %d)", calibration_sample_count, CALIBRATION_SAMPLES);
			RenderStatusMessage(buffer, "Do not move the controller.", "Press (B) on controller to cancel.");
		}
		else if (calibration_state == FLICK_STICK_CALIBRATION_START) {
			RenderStatusMessage("FLICK STICK CALIBRATION", "Press (A) to perform a test 360 turn.", "Press (B) to cancel.");
		}
		else if (calibration_state == FLICK_STICK_CALIBRATION_TURNING) {
			RenderStatusMessage(NULL, "TURNING...", NULL);
		}
		else if (calibration_state == FLICK_STICK_CALIBRATION_ADJUST) {
			const char* msg1 = "ADJUST CALIBRATION";
			snprintf(buffer, sizeof(buffer), "Current Value: %.1f", settings.flick_stick_calibration_value);
			const char* msg2 = "D-Pad U/D: Fine Tune (+/- 50)";
			const char* msg3 = "D-Pad L/R: Ultra-Fine Tune (+/- 1)";
			const char* msg4 = "Shoulders: Coarse Tune (+/- 500)";
			const char* msg5 = "Press (A) to re-test. Press (B) to save.";

			y_pos = (h - (line_height * 7)) / 2.0f;

			SDL_RenderDebugText(renderer, (w - (float)SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE * strlen(msg1)) / 2.0f, y_pos, msg1);
			y_pos += line_height * 1.5f;
			SDL_RenderDebugText(renderer, (w - (float)SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE * strlen(buffer)) / 2.0f, y_pos, buffer);
			y_pos += line_height * 1.5f;
			SDL_RenderDebugText(renderer, (w - (float)SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE * strlen(msg2)) / 2.0f, y_pos, msg2);
			y_pos += line_height;
			SDL_RenderDebugText(renderer, (w - (float)SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE * strlen(msg3)) / 2.0f, y_pos, msg3);
			y_pos += line_height;
			SDL_RenderDebugText(renderer, (w - (float)SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE * strlen(msg4)) / 2.0f, y_pos, msg4);
			y_pos += line_height * 1.5f;
			SDL_RenderDebugText(renderer, (w - (float)SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE * strlen(msg5)) / 2.0f, y_pos, msg5);
		}
	}
	else {
		// --- Draw Main Menu ---
		BuildVisibleMenu();
		if (active_menu_label[0] != '\0') {
			for (int i = 0; i < num_visible_menu_items; ++i) {
				if (strcmp(menu_items[visible_menu_map[i]].label, active_menu_label) == 0) {
					selected_menu_item = i; break;
				}
			}
			active_menu_label[0] = '\0';
		}

		y_pos = 10.0f;
		char label_buf[128], value_buf[128];
		for (int i = 0; i < num_visible_menu_items; ++i) {
			int real_idx = visible_menu_map[i];
			SDL_SetRenderDrawColor(renderer, (i == selected_menu_item) ? 255 : 200, (i == selected_menu_item) ? 255 : 200, (i == selected_menu_item) ? 100 : 255, 255);
			snprintf(label_buf, sizeof(label_buf), "%s%s", (i == selected_menu_item) ? ">" : " ", menu_items[real_idx].label);
			SDL_RenderDebugText(renderer, 5.0f, y_pos, label_buf);
			if (menu_items[real_idx].display) {
				menu_items[real_idx].display(value_buf, sizeof(value_buf));
				SDL_RenderDebugText(renderer, 200.0f, y_pos, value_buf);
			}
			y_pos += line_height * 1.2f;
		}

		// --- Gyro Visualizer ---
		const int cX = w - 55, cY = 55, oR = 50, iR = 5;
		SDL_SetRenderDrawColor(renderer, settings.flick_stick_enabled ? 255 : 100, 80, 80, 255);
		DrawCircle(renderer, cX, cY, oR);
		if (!settings.mouse_mode && settings.anti_deathzone > 0.0f) {
			SDL_SetRenderDrawColor(renderer, 60, 60, 80, 255);
			DrawFilledCircle(renderer, cX, cY, (int)(oR * (settings.anti_deathzone / 100.0f)));
		}
		float dx = gyro_data[1] * 20.f * (settings.invert_gyro_x ? 1.f : -1.f);
		float dy = -gyro_data[0] * 20.f * (settings.invert_gyro_y ? -1.f : 1.f);
		float dist = sqrtf(dx * dx + dy * dy);
		if (dist > oR) { dx = (dx / dist) * oR; dy = (dy / dist) * oR; }
		bool gyro_is_active = isAiming || settings.always_on_gyro;
		SDL_SetRenderDrawColor(renderer, gyro_is_active ? 255 : 200, gyro_is_active ? 80 : 200, gyro_is_active ? 80 : 255, 255);
		DrawFilledCircle(renderer, cX + (int)dx, cY + (int)dy, iR);
	}

	SDL_RenderPresent(renderer);
}