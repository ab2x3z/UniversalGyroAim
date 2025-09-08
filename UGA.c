#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ViGEmClient.h>

#include <math.h>
#include <stdbool.h>
#include <stdio.h>

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600 
#endif

#include <shlwapi.h>
#pragma comment(lib, "shlwapi.lib")
#include <ShlObj.h> /

#define CLAMP(v, min, max) (((v) < (min)) ? (min) : (((v) > (max)) ? (max) : (v)))

// --- Custom identifiers for our virtual controller ---
#define VIRTUAL_VENDOR_ID  0xFEED
#define VIRTUAL_PRODUCT_ID 0xBEEF

// --- Configuration file ---
#define CONFIG_FILENAME "uga_config.dat"
#define CURRENT_CONFIG_VERSION 1

// --- User configuration structure ---
typedef struct {
	SDL_GamepadButton selected_button;
	SDL_GamepadAxis selected_axis;
	float sensitivity;
	bool invert_gyro_x;
	bool invert_gyro_y;
	float anti_deathzone;
	bool always_on_gyro;
	int config_version;
	bool mouse_mode;
	float mouse_sensitivity;
} AppSettings;

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

// User configuration instance
static AppSettings settings;

// --- HidHide State ---
static bool is_controller_hidden = false;
static wchar_t hidden_device_instance_path[MAX_PATH] = { 0 };
static wchar_t hid_hide_cli_path[MAX_PATH] = { 0 };


// --- Settings Management Functions ---

static void SetDefaultSettings(void) {
	SDL_Log("Loading default settings.");
	settings.selected_button = -1;
	settings.selected_axis = -1;
	settings.sensitivity = 5.0f;
	settings.invert_gyro_x = false;
	settings.invert_gyro_y = false;
	settings.anti_deathzone = 0.0f;
	settings.always_on_gyro = false;
	settings.mouse_mode = false;
	settings.mouse_sensitivity = 100.0f;
	settings.config_version = CURRENT_CONFIG_VERSION;
}

static void SaveSettings(void) {
	FILE* file = fopen(CONFIG_FILENAME, "wb");
	if (!file) {
		SDL_Log("Error: Could not open %s for writing.", CONFIG_FILENAME);
		return;
	}

	if (fwrite(&settings, sizeof(AppSettings), 1, file) != 1) {
		SDL_Log("Error: Failed to write settings to %s.", CONFIG_FILENAME);
	}
	else {
		SDL_Log("Settings saved successfully to %s.", CONFIG_FILENAME);
	}

	fclose(file);
}

static bool LoadSettings(void) {
	FILE* file = fopen(CONFIG_FILENAME, "rb");
	if (!file) {
		SDL_Log("Info: No config file found (%s). Using defaults.", CONFIG_FILENAME);
		return false;
	}

	AppSettings loaded_settings;
	if (fread(&loaded_settings, sizeof(AppSettings), 1, file) != 1) {
		SDL_Log("Error: Failed to read settings from %s. Using defaults.", CONFIG_FILENAME);
		fclose(file);
		return false;
	}

	fclose(file);

	if (loaded_settings.config_version != CURRENT_CONFIG_VERSION) {
		SDL_Log("Warning: Config file version mismatch. Expected %d, got %d. Using defaults.", CURRENT_CONFIG_VERSION, loaded_settings.config_version);
		return false;
	}

	settings = loaded_settings;
	SDL_Log("Settings loaded successfully from %s.", CONFIG_FILENAME);
	return true;
}


// --- HidHide Helper Functions ---

/**
 * @brief Executes a command-line process silently and waits for it to complete.
 * @param command The full command line to execute.
 * @return True if the command executed and returned an exit code of 0, otherwise false.
 */
static bool ExecuteCommand(const wchar_t* command)
{
	STARTUPINFOW si;
	PROCESS_INFORMATION pi;
	DWORD exit_code = 1; // Default to error

	ZeroMemory(&si, sizeof(si));
	si.cb = sizeof(si);
	ZeroMemory(&pi, sizeof(pi));

	// CreateProcessW requires a mutable string buffer
	wchar_t* cmd_mutable = _wcsdup(command);
	if (!cmd_mutable) {
		SDL_Log("Failed to allocate memory for command.");
		return false;
	}

	// Create the process with no visible window
	if (CreateProcessW(NULL, cmd_mutable, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
		// Wait until the child process exits
		WaitForSingleObject(pi.hProcess, INFINITE);
		GetExitCodeProcess(pi.hProcess, &exit_code);

		// Close process and thread handles
		CloseHandle(pi.hProcess);
		CloseHandle(pi.hThread);
	}
	else {
		SDL_Log("CreateProcess failed (%lu) for command: %ls", GetLastError(), command);
	}

	free(cmd_mutable);

	if (exit_code != 0) {
		SDL_Log("Command returned non-zero exit code %lu.", exit_code);
	}

	return (exit_code == 0);
}

/**
 * @brief Converts an SDL Device Path (Symbolic Link) to a Windows Device Instance Path.
 * @param symbolic_link The path from SDL_GetGamepadPath.
 * @return A newly allocated char buffer with the converted path, or NULL on failure. The caller must free this buffer.
 */
static char* ConvertSymbolicLinkToDeviceInstancePath(const char* symbolic_link)
{
	if (!symbolic_link) return NULL;

	// Find the start of the relevant part (skip \\?\)
	const char* start = strstr(symbolic_link, "HID#");
	if (!start) start = strstr(symbolic_link, "USB#"); // Also handle plain USB devices if needed
	if (!start) {
		SDL_Log("Could not find start of instance path in symbolic link.");
		return NULL;
	}

	const char* end = NULL;
	const char* current_pos = start;
	while ((current_pos = strstr(current_pos, "#{")) != NULL) {
		end = current_pos;
		current_pos++;
	}

	if (!end) {
		SDL_Log("Could not find end of instance path in symbolic link.");
		return NULL;
	}

	// Allocate memory for the new string
	size_t len = end - start;
	char* instance_path = (char*)malloc(len + 1);
	if (!instance_path) {
		SDL_Log("Failed to allocate memory for instance path.");
		return NULL;
	}

	// Copy the relevant part
	strncpy_s(instance_path, len + 1, start, len);

	// Replace the first two '#' characters with '\'
	char* first_hash = strchr(instance_path, '#');
	if (first_hash) {
		*first_hash = '\\';
		char* second_hash = strchr(first_hash + 1, '#');
		if (second_hash) {
			*second_hash = '\\';
		}
	}

	return instance_path;
}

static bool GetHidHideCliPath(wchar_t* cli_path, size_t cli_path_size)
{
	// --- Check the cache first ---
	if (hid_hide_cli_path[0] != L'\0') {
		wcscpy_s(cli_path, cli_path_size, hid_hide_cli_path);
		return true;

	}

	// --- Scan common 'Program Files' directories ---

	// List of known folder IDs for Program Files
	const KNOWNFOLDERID* folder_ids[] = {
		&FOLDERID_ProgramFiles,
		&FOLDERID_ProgramFilesX86
	};

	// List of known sub-paths where HidHide might be installed
	const wchar_t* sub_paths[] = {
		L"Nefarius Software Solutions\\HidHide\\x64",
		L"Nefarius Software Solutions\\HidHide",
		L"Nefarius\\HidHide",
		L"HidHide"
	};

	wchar_t* program_files_path = NULL;
	for (int i = 0; i < sizeof(folder_ids) / sizeof(folder_ids[0]); ++i) {
		if (SUCCEEDED(SHGetKnownFolderPath(folder_ids[i], 0, NULL, &program_files_path))) {

			for (int j = 0; j < sizeof(sub_paths) / sizeof(sub_paths[0]); ++j) {
				wchar_t combined_path[MAX_PATH];
				PathCombineW(combined_path, program_files_path, sub_paths[j]);

				// Final check: does HidHideCLI.exe exist here?
				PathCombineW(cli_path, combined_path, L"HidHideCLI.exe");

				if (GetFileAttributesW(cli_path) != INVALID_FILE_ATTRIBUTES) {
					SDL_Log("Found HidHideCLI.exe at: %ls", cli_path);
					wcscpy_s(hid_hide_cli_path, MAX_PATH, cli_path); // Cache the found path
					CoTaskMemFree(program_files_path); // Free the memory
					return true; // Success!
				}
			}
			CoTaskMemFree(program_files_path); // Free the memory
		}
	}

	SDL_Log("Could not find HidHideCLI.exe in any known location.");
	return false;
}

/**
 * @brief Unhides the currently hidden physical controller using HidHideCLI.
 */
static void UnhidePhysicalController(void)
{
	if (!is_controller_hidden || hidden_device_instance_path[0] == L'\0') {
		return;
	}

	wchar_t cli_path[MAX_PATH];
	if (!GetHidHideCliPath(cli_path, MAX_PATH)) {
		SDL_Log("Cannot unhide controller: HidHideCLI not found.");
		return;
	}

	wchar_t command[1024];
	swprintf_s(command, 1024, L"\"%s\" --dev-unhide \"%s\"", cli_path, hidden_device_instance_path);

	SDL_Log("Attempting to unhide controller...");
	if (ExecuteCommand(command)) {
		SDL_Log("Physical controller successfully unhidden.");
		is_controller_hidden = false;
		hidden_device_instance_path[0] = L'\0';
	}
	else {
		SDL_Log("Failed to unhide physical controller.");
	}
}

static void HidePhysicalController(SDL_Gamepad* pad_to_hide)
{
	if (is_controller_hidden) {
		SDL_Log("Controller is already hidden.");
		return;
	}
	if (!pad_to_hide) {
		SDL_Log("Error: Cannot hide a NULL gamepad pointer.");
		return;
	}

	wchar_t cli_path[MAX_PATH];
	if (!GetHidHideCliPath(cli_path, MAX_PATH)) {
		SDL_Log("HidHide not found. Cannot hide controller.");
		return;
	}

	wchar_t command[1024];

	// --- Get the device instance path ---
	const char* dev_path = ConvertSymbolicLinkToDeviceInstancePath(SDL_GetGamepadPath(pad_to_hide));

	if (!dev_path) {
		SDL_Log("Error: SDL_GetGamepadPath failed to return a path.");
		return;
	}

	// Convert path from SDL to a wide string for the command line
	if (MultiByteToWideChar(CP_UTF8, 0, dev_path, -1, hidden_device_instance_path, MAX_PATH) == 0) {
		SDL_Log("Error: Failed to convert device path to wide string.");
		SDL_free((void*)dev_path);
		return;
	}

	// --- Send the command to hide the device ---
	swprintf_s(command, 1024, L"\"%s\" --dev-hide \"%s\"", cli_path, hidden_device_instance_path);
	SDL_Log("Hiding device: %s", dev_path);
	SDL_free((void*)dev_path);

	if (!ExecuteCommand(command)) {
		SDL_Log("Failed to hide the device. It might already be hidden.");
		hidden_device_instance_path[0] = L'\0';
		return;
	}

	// --- Enable the HidHide service to make the rule active ---
	swprintf_s(command, 1024, L"\"%s\" --enable", cli_path);
	SDL_Log("Enabling HidHide service...");
	if (ExecuteCommand(command)) {
		SDL_Log("Successfully hid physical controller.");
		is_controller_hidden = true;
	}
	else {
		SDL_Log("Failed to enable HidHide service, but device may still be hidden.");
		is_controller_hidden = true;
	}
}


// --- Drawing Helper Functions ---

void static DrawCircle(SDL_Renderer* renderer, int centreX, int centreY, int radius)
{
	const int32_t diameter = (radius * 2);

	int32_t x = (radius - 1);
	int32_t y = 0;
	int32_t tx = 1;
	int32_t ty = 1;
	int32_t error = (tx - diameter);

	while (x >= y) {
		// Each of the following renders an octant of the circle
		SDL_RenderPoint(renderer, centreX + x, centreY - y);
		SDL_RenderPoint(renderer, centreX + x, centreY + y);
		SDL_RenderPoint(renderer, centreX - x, centreY - y);
		SDL_RenderPoint(renderer, centreX - x, centreY + y);
		SDL_RenderPoint(renderer, centreX + y, centreY - x);
		SDL_RenderPoint(renderer, centreX + y, centreY + x);
		SDL_RenderPoint(renderer, centreX - y, centreY - x);
		SDL_RenderPoint(renderer, centreX - y, centreY + x);

		if (error <= 0) {
			++y;
			error += ty;
			ty += 2;
		}

		if (error > 0) {
			--x;
			tx += 2;
			error += (tx - diameter);
		}
	}
}

/**
 * @brief Draws a filled circle by rendering horizontal lines.
 */
void static DrawFilledCircle(SDL_Renderer* renderer, int x, int y, int radius)
{
	int offsetx, offsety, d;

	offsetx = 0;
	offsety = radius;
	d = radius - 1;

	while (offsety >= offsetx) {
		SDL_RenderLine(renderer, x - offsety, y + offsetx, x + offsety, y + offsetx);
		SDL_RenderLine(renderer, x - offsetx, y + offsety, x + offsetx, y + offsety);
		SDL_RenderLine(renderer, x - offsetx, y - offsety, x + offsetx, y - offsety);
		SDL_RenderLine(renderer, x - offsety, y - offsetx, x + offsety, y - offsetx);

		if (d >= 2 * offsetx) {
			d -= 2 * offsetx + 1;
			offsetx++;
		}
		else if (d < 2 * (radius - offsety)) {
			d += 2 * offsety - 1;
			offsety--;
		}
		else {
			d += 2 * (offsety - offsetx - 1);
			offsety--;
			offsetx++;
		}
	}
}

/**
 * @brief Scans for connected gamepads and opens the first valid physical one.
 */
static void find_and_open_physical_gamepad(void)
{
	if (gamepad) {
		return; // A gamepad is already open
	}

	SDL_Log("Scanning for physical controllers...");
	SDL_JoystickID* joysticks = SDL_GetGamepads(NULL);
	if (joysticks) {
		for (int i = 0; joysticks[i] != 0; ++i) {
			SDL_JoystickID instance_id = joysticks[i];
			SDL_Gamepad* temp_pad = SDL_OpenGamepad(instance_id);
			if (!temp_pad) {
				continue;
			}

			Uint16 vendor = SDL_GetGamepadVendor(temp_pad);
			Uint16 product = SDL_GetGamepadProduct(temp_pad);

			// Make sure we don't open our own virtual controller
			if (vendor == VIRTUAL_VENDOR_ID && product == VIRTUAL_PRODUCT_ID) {
				SDL_Log("Scan: Ignoring our own virtual controller.");
				SDL_CloseGamepad(temp_pad);
			}
			else {
				gamepad = temp_pad;
				gamepad_instance_id = instance_id;
				const char* name = SDL_GetGamepadName(gamepad);
				// Found a valid physical controller
				SDL_Log("Opened gamepad: %s (VID: %04X, PID: %04X)", name, vendor, product);

				HidePhysicalController(gamepad);

				if (SDL_SetGamepadSensorEnabled(gamepad, SDL_SENSOR_GYRO, true) < 0) {
					SDL_Log("Could not enable gyroscope: %s", SDL_GetError());
				}
				else {
					SDL_Log("Gyroscope enabled!");
				}
				break; // Stop after finding the first one
			}
		}
		SDL_free(joysticks);
	}
}


static bool reset_application(void)
{
	SDL_Log("--- RESETTING APPLICATION ---");

	// 1. Cleanup existing resources
	if (gamepad) {
		SDL_Log("Closing physical gamepad...");
		UnhidePhysicalController();
		SDL_SetGamepadSensorEnabled(gamepad, SDL_SENSOR_GYRO, false);
		SDL_CloseGamepad(gamepad);
		gamepad = NULL;
	}
	if (vigem_client) {
		if (x360_pad) {
			SDL_Log("Removing virtual controller...");
			vigem_target_remove(vigem_client, x360_pad);
			vigem_target_free(x360_pad);
			x360_pad = NULL;
		}
		SDL_Log("Disconnecting from ViGEmBus...");
		vigem_disconnect(vigem_client);
		vigem_free(vigem_client);
		vigem_client = NULL;
	}

	// 2. Reset state variables
	gamepad_instance_id = 0;
	gyro_data[0] = 0.0f; gyro_data[1] = 0.0f; gyro_data[2] = 0.0f;
	isAiming = false;
	SetDefaultSettings();

	// 3. Re-initialize resources
	SDL_Log("Re-initializing ViGEmBus...");
	vigem_client = vigem_alloc();
	if (vigem_client == NULL) {
		SDL_Log("FATAL: Failed to re-allocate ViGEm client during reset.");
		return false;
	}
	const VIGEM_ERROR ret = vigem_connect(vigem_client);
	if (!VIGEM_SUCCESS(ret)) {
		SDL_Log("FATAL: ViGEmBus re-connection failed: 0x%x.", ret);
		return false;
	}

	x360_pad = vigem_target_x360_alloc();
	vigem_target_set_vid(x360_pad, VIRTUAL_VENDOR_ID);
	vigem_target_set_pid(x360_pad, VIRTUAL_PRODUCT_ID);

	const VIGEM_ERROR add_ret = vigem_target_add(vigem_client, x360_pad);
	if (!VIGEM_SUCCESS(add_ret)) {
		SDL_Log("FATAL: Failed to re-add virtual X360 controller: 0x%x", add_ret);
		return false;
	}

	// 4. Actively look for an already-connected controller
	find_and_open_physical_gamepad();

	// 5. Load settings from file (or use defaults)
	if (!LoadSettings()) {
		SetDefaultSettings();
	}

	SDL_Log("--- RESET COMPLETE ---");
	return true;
}


/* This function runs once at startup. */
SDL_AppResult SDL_AppInit(void** appstate, int argc, char* argv[])
{
	SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");

	if (SDL_InitSubSystem(SDL_INIT_GAMEPAD) < 0) {
		SDL_Log("Couldn't initialize gamepad subsystem: %s", SDL_GetError());
		return SDL_APP_FAILURE;
	}

	if (!SDL_CreateWindowAndRenderer("Universal Gyro Aim", 460, 240, 0, &window, &renderer)) {
		SDL_Log("Couldn't create window and renderer: %s", SDL_GetError());
		return SDL_APP_FAILURE;
	}

	SDL_SetRenderVSync(renderer, 1);

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

	// Set a custom VID/PID to distinguish our virtual controller from real ones
	vigem_target_set_vid(x360_pad, VIRTUAL_VENDOR_ID);
	vigem_target_set_pid(x360_pad, VIRTUAL_PRODUCT_ID);

	const VIGEM_ERROR add_ret = vigem_target_add(vigem_client, x360_pad);
	if (!VIGEM_SUCCESS(add_ret)) {
		SDL_Log("Error: Failed to add virtual X360 controller: 0x%x", add_ret);
		return SDL_APP_FAILURE;
	}

	SDL_Log("App started. Virtual Xbox 360 controller is active.");

	if (!LoadSettings()) {
		SetDefaultSettings();
	}

	return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void* appstate, SDL_Event* event)
{
	switch (event->type) {
	case SDL_EVENT_QUIT:
		return SDL_APP_SUCCESS;

	case SDL_EVENT_KEY_DOWN:
		switch (event->key.key) {
		case SDLK_H:
			if (gamepad) {
				if (is_controller_hidden) {
					UnhidePhysicalController();
				}
				else {
					HidePhysicalController(gamepad);
				}
			}
			else {
				SDL_Log("No controller connected to hide/unhide.");
			}
			break;
		case SDLK_C:
			SDL_Log("Change aim button requested. Press a button or trigger on the gamepad.");
			settings.selected_button = -1;
			settings.selected_axis = -1;
			isAiming = false;
			break;
		case SDLK_T:
			settings.always_on_gyro = !settings.always_on_gyro;
			if (settings.always_on_gyro) {
				isAiming = false;
			}
			SDL_Log("Always-on gyro toggled %s.", settings.always_on_gyro ? "ON" : "OFF");
			break;
		case SDLK_M:
			settings.mouse_mode = !settings.mouse_mode;
			SDL_Log("Mouse mode toggled %s.", settings.mouse_mode ? "ON" : "OFF");
			break;
		case SDLK_I:
			settings.invert_gyro_y = !settings.invert_gyro_y;
			SDL_Log("Invert Gyro Y-Axis (Pitch) toggled %s.", settings.invert_gyro_y ? "ON" : "OFF");
			break;
		case SDLK_O:
			settings.invert_gyro_x = !settings.invert_gyro_x;
			SDL_Log("Invert Gyro X-Axis (Yaw) toggled %s.", settings.invert_gyro_x ? "ON" : "OFF");
			break;
		case SDLK_S:
			SaveSettings();
			break;
		case SDLK_R:
			if (!reset_application()) {
				return SDL_APP_SUCCESS;
			}
			break;
		case SDLK_UP:
			if (settings.mouse_mode) {
				settings.mouse_sensitivity += 10.0f;
				settings.mouse_sensitivity = CLAMP(settings.mouse_sensitivity, 10.0f, 1000.0f);
				SDL_Log("Mouse Sensitivity increased to %.1f", settings.mouse_sensitivity);
			}
			else {
				settings.sensitivity += 0.5f;
				settings.sensitivity = CLAMP(settings.sensitivity, 0.5f, 50.0f);
				SDL_Log("Joystick Sensitivity increased to %.1f", settings.sensitivity);
			}
			break;
		case SDLK_DOWN:
			if (settings.mouse_mode) {
				settings.mouse_sensitivity -= 10.0f;
				settings.mouse_sensitivity = CLAMP(settings.mouse_sensitivity, 10.0f, 1000.0f);
				SDL_Log("Mouse Sensitivity decreased to %.1f", settings.mouse_sensitivity);
			}
			else {
				settings.sensitivity -= 0.5f;
				settings.sensitivity = CLAMP(settings.sensitivity, 0.5f, 50.0f);
				SDL_Log("Joystick Sensitivity decreased to %.1f", settings.sensitivity);
			}
			break;
		case SDLK_RIGHT:
			if (!settings.mouse_mode) {
				settings.anti_deathzone += 1.0f;
				settings.anti_deathzone = CLAMP(settings.anti_deathzone, 0.0f, 90.0f);
				SDL_Log("Anti-Deadzone increased to %.0f%%", settings.anti_deathzone);
			}
			break;
		case SDLK_LEFT:
			if (!settings.mouse_mode) {
				settings.anti_deathzone -= 1.0f;
				settings.anti_deathzone = CLAMP(settings.anti_deathzone, 0.0f, 90.0f);
				SDL_Log("Anti-Deadzone decreased to %.0f%%", settings.anti_deathzone);
			}
			break;
		}
		break;

	case SDL_EVENT_GAMEPAD_ADDED:
	{
		// Open the gamepad temporarily to check its properties
		SDL_Gamepad* temp_pad = SDL_OpenGamepad(event->gdevice.which);
		if (!temp_pad) {
			break;
		}

		Uint16 vendor = SDL_GetGamepadVendor(temp_pad);
		Uint16 product = SDL_GetGamepadProduct(temp_pad);
		const char* name = SDL_GetGamepadName(temp_pad);

		// Check if the detected gamepad is our own virtual one
		if (vendor == VIRTUAL_VENDOR_ID && product == VIRTUAL_PRODUCT_ID) {
			SDL_Log("Ignoring our own virtual controller.");
			SDL_CloseGamepad(temp_pad); // Close the handle, we don't want it.
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
		}
		else {
			SDL_Log("Ignoring additional controller: %s", SDL_GetGamepadName(temp_pad));
			SDL_CloseGamepad(temp_pad);
		}
		break;
	}

	case SDL_EVENT_GAMEPAD_REMOVED:
		if (gamepad && event->gdevice.which == gamepad_instance_id) {
			SDL_Log("Gamepad disconnected: %s", SDL_GetGamepadName(gamepad));
			UnhidePhysicalController();
			SDL_SetGamepadSensorEnabled(gamepad, SDL_SENSOR_GYRO, false);
			SDL_CloseGamepad(gamepad);
			gamepad = NULL;
			// Reset aiming state on disconnect
			settings.selected_button = -1;
			settings.selected_axis = -1;
			isAiming = false;
		}
		break;

	case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
	case SDL_EVENT_GAMEPAD_BUTTON_UP:
		if (event->gbutton.which == gamepad_instance_id) {
			if (settings.selected_button == -1 && settings.selected_axis == -1 && event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
				settings.selected_button = event->gbutton.button;
				SDL_Log("Aim button set to: %s", SDL_GetGamepadStringForButton(settings.selected_button));
			}
			else if (event->gbutton.button == settings.selected_button) {
				isAiming = (event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
			}
		}
		break;

	case SDL_EVENT_GAMEPAD_AXIS_MOTION:
		if (event->gaxis.which == gamepad_instance_id) {
			if (settings.selected_button == -1 && settings.selected_axis == -1) {
				if (event->gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ||
					event->gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) {
					if (event->gaxis.value > 8000) {
						settings.selected_axis = event->gaxis.axis;
						SDL_Log("Aim trigger set to: %s", SDL_GetGamepadStringForAxis(settings.selected_axis));
					}
				}
			}
			else if (event->gaxis.axis == settings.selected_axis) {
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
	bool stick_in_use = false;

	if (gamepad) {
		// --- Passthrough physical controller state to virtual controller ---
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

		// --- Trigger Passthrough Logic ---
		report.bLeftTrigger = (SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) * 255) / 32767;
		report.bRightTrigger = (SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) * 255) / 32767;

		// --- Stick Passthrough ---
		Sint16 lx = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX);
		Sint16 ly = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTY);
		Sint16 rx = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHTX);
		Sint16 ry = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHTY);

		report.sThumbLX = lx;
		report.sThumbLY = (ly == -32768) ? 32767 : -ly;

		report.sThumbRX = rx;
		report.sThumbRY = (ry == -32768) ? 32767 : -ry;

		// --- Check for Stick Priority ---
		const float stick_deadzone = 8000.0f;
		float stick_magnitude = sqrtf((float)report.sThumbRX * report.sThumbRX + (float)report.sThumbRY * report.sThumbRY);
		if (stick_magnitude >= stick_deadzone) {
			stick_in_use = true;
		}
	}

	// --- Gyro Input Logic ---
	if ((isAiming || settings.always_on_gyro) && !stick_in_use) {
		if (settings.mouse_mode) {
			// --- MOUSE MODE ---
			float deltaX = gyro_data[1] * settings.mouse_sensitivity * (settings.invert_gyro_x ? 1.0f : -1.0f);
			float deltaY = gyro_data[0] * settings.mouse_sensitivity * (settings.invert_gyro_y ? 1.0f : -1.0f);

			if (fabsf(deltaX) > 0.01f || fabsf(deltaY) > 0.01f) {
				INPUT input = { 0 };
				input.type = INPUT_MOUSE;
				input.mi.dx = (LONG)deltaX;
				input.mi.dy = (LONG)deltaY;
				input.mi.dwFlags = MOUSEEVENTF_MOVE;
				SendInput(1, &input, sizeof(INPUT));
			}

			// Zero out the virtual stick in mouse mode to prevent interference.
			report.sThumbRX = 0;
			report.sThumbRY = 0;
		}
		else {
			// --- JOYSTICK MODE ---
			const float x_multiplier = settings.invert_gyro_x ? 10000.0f : -10000.0f;
			const float y_multiplier = settings.invert_gyro_y ? -10000.0f : 10000.0f;

			float gyro_input_x = gyro_data[1] * settings.sensitivity * x_multiplier;
			float gyro_input_y = gyro_data[0] * settings.sensitivity * y_multiplier;

			// --- Apply Anti-Deadzone ---
			if (settings.anti_deathzone > 0.0f) {
				const float max_stick_val = 32767.0f;
				float magnitude = sqrtf(gyro_input_x * gyro_input_x + gyro_input_y * gyro_input_y);

				if (magnitude > 0.01f) {
					float normalized_mag = magnitude / max_stick_val;
					if (normalized_mag <= 1.0f) {
						float dz_fraction = settings.anti_deathzone / 100.0f;
						float new_normalized_mag = dz_fraction + (1.0f - dz_fraction) * normalized_mag;
						float scale_factor = new_normalized_mag / normalized_mag;
						gyro_input_x *= scale_factor;
						gyro_input_y *= scale_factor;
					}
				}
			}

			// --- Combine Gyro and Stick Inputs ---
			float combined_x = (float)report.sThumbRX + gyro_input_x;
			float combined_y = (float)report.sThumbRY + gyro_input_y;

			report.sThumbRX = (short)CLAMP(combined_x, -32767.0f, 32767.0f);
			report.sThumbRY = (short)CLAMP(combined_y, -32767.0f, 32767.0f);
		}
	}

	if (x360_pad && vigem_client) {
		vigem_target_x360_update(vigem_client, x360_pad, report);
	}

	// --- Drawing UI ---
	SDL_SetRenderDrawColor(renderer, 25, 25, 40, 255);
	SDL_RenderClear(renderer);
	SDL_SetRenderDrawColor(renderer, 200, 200, 255, 255);

	if (!gamepad) {
		const char* message = "Waiting for physical controller...";
		int w = 0, h = 0;
		SDL_GetRenderOutputSize(renderer, &w, &h);
		float x = (w - (float)SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE * SDL_strlen(message)) / 2.0f;
		float y = (h - (float)SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE) / 2.0f;
		SDL_RenderDebugText(renderer, x, y, message);
	}
	else {
		char buffer[256];
		float y_pos = 10.0f;
		const float line_height = (float)(SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE + 4);

		const char* status_message;
		if (settings.mouse_mode) {
			if ((isAiming || settings.always_on_gyro) && !stick_in_use) {
				status_message = "Status: Aiming with Gyro (Mouse)";
			}
			else {
				status_message = "Status: OK (Mouse Mode)";
			}
		}
		else if (stick_in_use && (isAiming || settings.always_on_gyro)) {
			status_message = "Status: Stick Priority Active";
		}
		else if (settings.always_on_gyro) {
			status_message = "Status: Gyro Always ON (Joystick)";
		}
		else if (isAiming) {
			status_message = "Status: Aiming with Gyro (Joystick)";
		}
		else {
			status_message = "Status: OK (Joystick)";
		}
		SDL_RenderDebugText(renderer, 10, y_pos, status_message);
		y_pos += line_height;

		if (settings.selected_button != -1) {
			snprintf(buffer, sizeof(buffer), "Aim Button: %s", SDL_GetGamepadStringForButton(settings.selected_button));
		}
		else if (settings.selected_axis != -1) {
			snprintf(buffer, sizeof(buffer), "Aim Trigger: %s", SDL_GetGamepadStringForAxis(settings.selected_axis));
		}
		else {
			snprintf(buffer, sizeof(buffer), "Aim Button: [Press a button/trigger]");
		}
		SDL_RenderDebugText(renderer, 10, y_pos, buffer);
		y_pos += line_height;

		if (settings.mouse_mode) {
			snprintf(buffer, sizeof(buffer), "Mouse Sensitivity: %.1f", settings.mouse_sensitivity);
		}
		else {
			snprintf(buffer, sizeof(buffer), "Joystick Sensitivity: %.1f", settings.sensitivity);
		}
		SDL_RenderDebugText(renderer, 10, y_pos, buffer);
		y_pos += line_height;

		if (!settings.mouse_mode) {
			snprintf(buffer, sizeof(buffer), "Anti-Deadzone: %.0f%%", settings.anti_deathzone);
			SDL_RenderDebugText(renderer, 10, y_pos, buffer);
			y_pos += line_height;
		}

		snprintf(buffer, sizeof(buffer), "Invert Gyro -> X-Axis: %s | Y-Axis: %s",
			settings.invert_gyro_x ? "ON" : "OFF",
			settings.invert_gyro_y ? "ON" : "OFF");
		SDL_RenderDebugText(renderer, 10, y_pos, buffer);
		y_pos += line_height;

		snprintf(buffer, sizeof(buffer), "HidHide status: %s", is_controller_hidden ? "Hidden" : "Visible");
		SDL_RenderDebugText(renderer, 10, y_pos, buffer);
		y_pos += line_height * 2;


		SDL_RenderDebugText(renderer, 10, y_pos, "--- Controls ---");
		y_pos += line_height;
		SDL_RenderDebugText(renderer, 10, y_pos, " 'M' key:       Toggle Mouse/Joystick Mode");
		y_pos += line_height;
		SDL_RenderDebugText(renderer, 10, y_pos, " 'H' key:       Toggle Hiding Controller");
		y_pos += line_height;
		SDL_RenderDebugText(renderer, 10, y_pos, " 'C' key:       Change Aim Button");
		y_pos += line_height;
		SDL_RenderDebugText(renderer, 10, y_pos, " 'T' key:       Toggle Always-On Gyro");
		y_pos += line_height;
		SDL_RenderDebugText(renderer, 10, y_pos, " 'I' key:       Invert Gyro Y-Axis");
		y_pos += line_height;
		SDL_RenderDebugText(renderer, 10, y_pos, " 'O' key:       Invert Gyro X-Axis");
		y_pos += line_height;
		SDL_RenderDebugText(renderer, 10, y_pos, " Up/Down:       Adjust Sensitivity");
		y_pos += line_height;
		SDL_RenderDebugText(renderer, 10, y_pos, " Left/Right:    Adjust Anti-Deadzone");
		y_pos += line_height;
		SDL_RenderDebugText(renderer, 10, y_pos, " 'S' key:       Save Settings");
		y_pos += line_height;
		SDL_RenderDebugText(renderer, 10, y_pos, " 'R' key:       Reset Application");


		// --- Gyro Visualizer ---
		int w, h;
		SDL_GetRenderOutputSize(renderer, &w, &h);

		// Position the visualizer at the bottom right
		const int centerX = w - 80;
		const int centerY = h - 70;
		const int outerRadius = 50;
		const int innerRadius = 5;
		const float visualizerScale = 20.0f;

		// Draw the outer boundary circle
		SDL_SetRenderDrawColor(renderer, 100, 100, 120, 255);
		DrawCircle(renderer, centerX, centerY, outerRadius);

		// Draw the anti-deadzone inner circle (only in joystick mode)
		if (!settings.mouse_mode && settings.anti_deathzone > 0.0f) {
			int adzRadius = (int)(outerRadius * (settings.anti_deathzone / 100.0f));
			if (adzRadius > 0) {
				SDL_SetRenderDrawColor(renderer, 60, 60, 80, 255);
				DrawFilledCircle(renderer, centerX, centerY, adzRadius);
			}
		}

		// Calculate dot position based on raw gyro input
		const float x_multiplier = settings.invert_gyro_x ? 1.0f : -1.0f;
		const float y_multiplier = settings.invert_gyro_y ? -1.0f : 1.0f;
		float dotX_offset = gyro_data[1] * visualizerScale * x_multiplier;
		float dotY_offset = -gyro_data[0] * visualizerScale * y_multiplier;

		// Clamp the dot to be within the outer circle
		float distance = sqrtf(dotX_offset * dotX_offset + dotY_offset * dotY_offset);
		if (distance > outerRadius) {
			dotX_offset = (dotX_offset / distance) * outerRadius;
			dotY_offset = (dotY_offset / distance) * outerRadius;
		}

		int dotX = centerX + (int)dotX_offset;
		int dotY = centerY + (int)dotY_offset;

		// Change color if aiming
		if ((isAiming || settings.always_on_gyro) && !stick_in_use) {
			SDL_SetRenderDrawColor(renderer, 255, 80, 80, 255); // Red when aiming
		}
		else {
			SDL_SetRenderDrawColor(renderer, 200, 200, 255, 255); // Default color
		}

		DrawFilledCircle(renderer, dotX, dotY, innerRadius);
	}

	SDL_RenderPresent(renderer);

	return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void* appstate, SDL_AppResult result)
{
	UnhidePhysicalController();

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