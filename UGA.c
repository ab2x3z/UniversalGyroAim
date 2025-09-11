#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ViGEmClient.h>

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif

#include <shlwapi.h>
#pragma comment(lib, "shlwapi.lib")
#include <ShlObj.h>
#pragma comment(lib, "winmm.lib")

#define CLAMP(v, min, max) (((v) < (min)) ? (min) : (((v) > (max)) ? (max) : (v)))

// --- Custom identifiers for our virtual controller ---
#define VIRTUAL_VENDOR_ID  0xFEED
#define VIRTUAL_PRODUCT_ID 0xBEEF

// --- Configuration file ---
#define CONFIG_FILENAME "uga_config.dat"
#define CURRENT_CONFIG_VERSION 1

// --- Calibration Settings ---
#define CALIBRATION_SAMPLES 200
#define GYRO_STABILITY_THRESHOLD 0.1f
#define GYRO_STABILITY_DURATION_MS 3000

// --- Calibration State Machine ---
typedef enum {
	CALIBRATION_IDLE,
	CALIBRATION_WAITING_FOR_STABILITY,
	CALIBRATION_SAMPLING,
	FLICK_STICK_CALIBRATION_START,
	FLICK_STICK_CALIBRATION_TURNING,
	FLICK_STICK_CALIBRATION_ADJUST
} CalibrationState;

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
	unsigned char led_r;
	unsigned char led_g;
	unsigned char led_b;
	float gyro_calibration_offset[3]; // [0]=Pitch, [1]=Yaw, [2]=Roll
	bool flick_stick_enabled;
	bool flick_stick_calibrated;
	float flick_stick_calibration_value; // Mouse units for a 360 turn
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

// --- Driver Status ---
static bool vigem_found = false;
static bool hidhide_found = false;
static bool controller_has_led = false;


// --- HidHide State ---
static bool is_controller_hidden = false;
static wchar_t hidden_device_instance_path[MAX_PATH] = { 0 };
static wchar_t hid_hide_cli_path[MAX_PATH] = { 0 };

// --- Mouse Mode State (Shared between threads) ---
static volatile bool run_mouse_thread = false;
static HANDLE mouse_thread_handle = NULL;
static CRITICAL_SECTION data_lock;
static volatile float shared_gyro_data[3] = { 0.0f, 0.0f, 0.0f };
static volatile float shared_flick_stick_delta_x = 0.0f;
static volatile bool shared_mouse_aim_active = false;

// --- Text Input State ---
static bool is_entering_text = false;
static char hex_input_buffer[8] = { 0 };

// --- Calibration State ---
static CalibrationState calibration_state = CALIBRATION_IDLE;
static int calibration_sample_count = 0;
static float gyro_accumulator[3] = { 0.0f, 0.0f, 0.0f };
static float flick_stick_turn_remaining = 0.0f;
static Uint64 stability_timer_start_time = 0;

// --- Flick Stick State ---
static float flick_last_angle = 0.0f;
static bool is_flick_stick_active = false;

// --- Menu System State ---
typedef struct {
	const char* label;
	// `direction` is -1 for Left, 1 for Right, 0 for Enter.
	void (*execute)(int direction);
	// Fills a buffer with the current value to display.
	void (*display)(char* buffer, size_t size);
} MenuItem;

static int selected_menu_item = 0; // This is the cursor position on the VISIBLE menu
static bool is_waiting_for_aim_button = false;
static char active_menu_label[128] = { 0 };
static bool settings_are_dirty = false;


// --- Forward declarations for menu functions ---
static bool reset_application(void);
static void SaveSettings(void);
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
void execute_save_settings(int direction);
void display_save_status(char* buffer, size_t size);
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
	{ "Save Settings",         execute_save_settings,       display_save_status },
	{ "Reset Application",     execute_reset_app,           NULL }
};
static const int master_num_menu_items = sizeof(menu_items) / sizeof(MenuItem);

// --- Dynamic menu visibility state ---
static int visible_menu_map[sizeof(menu_items) / sizeof(MenuItem)]; // Maps visible index to master index
static int num_visible_menu_items = 0;


// --- Mouse Thread for High-Frequency Input ---
DWORD WINAPI MouseThread(LPVOID lpParam) {
	float accumulator_x = 0.0f;
	float accumulator_y = 0.0f;

	Uint64 perf_freq = SDL_GetPerformanceFrequency();
	Uint64 last_time = SDL_GetPerformanceCounter();

	// Request higher timer resolution
	timeBeginPeriod(1);

	while (run_mouse_thread) {
		// --- Calculate this thread's own, stable delta time ---
		Uint64 current_time = SDL_GetPerformanceCounter();
		float dt = (float)(current_time - last_time) / (float)perf_freq;
		last_time = current_time;

		// --- Read shared data ---
		EnterCriticalSection(&data_lock);
		float current_gyro_x = shared_gyro_data[0];
		float current_gyro_y = shared_gyro_data[1];
		float flick_stick_dx = shared_flick_stick_delta_x;
		shared_flick_stick_delta_x = 0.0f;
		bool is_active = shared_mouse_aim_active;
		LeaveCriticalSection(&data_lock);

		// --- Perform calculations inside this thread ---
		float deltaX = flick_stick_dx;
		float deltaY = 0.0f;

		if (is_active) { // Add gyro movement if active
			deltaX += current_gyro_y * dt * settings.mouse_sensitivity * (settings.invert_gyro_x ? 1.0f : -1.0f);
			deltaY += current_gyro_x * dt * settings.mouse_sensitivity * (settings.invert_gyro_y ? 1.0f : -1.0f);
		}
		accumulator_x += deltaX;
		accumulator_y += deltaY;

		// --- Dispatch mouse movement ---
		LONG move_x = 0;
		LONG move_y = 0;
		if (fabsf(accumulator_x) >= 1.0f) {
			move_x = (LONG)accumulator_x;
			accumulator_x -= move_x;
		}
		if (fabsf(accumulator_y) >= 1.0f) {
			move_y = (LONG)accumulator_y;
			accumulator_y -= move_y;
		}

		if (move_x != 0 || move_y != 0) {
			INPUT input = { 0 };
			input.type = INPUT_MOUSE;
			input.mi.dx = move_x;
			input.mi.dy = move_y;
			input.mi.dwFlags = MOUSEEVENTF_MOVE;
			SendInput(1, &input, sizeof(INPUT));
		}

		Sleep(1);
	}

	// Release timer resolution
	timeEndPeriod(1);
	return 0;
}


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
		settings_are_dirty = false;
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
	if (fread(&loaded_settings, sizeof(loaded_settings), 1, file) != 1) {
		fseek(file, 0, SEEK_END);
		long file_size = ftell(file);
		fclose(file);
		if (file_size < sizeof(AppSettings)) {
			SDL_Log("Warning: Config file is from an older version of the app. Using defaults.");
		}
		else {
			SDL_Log("Error: Failed to read settings from %s. Using defaults.", CONFIG_FILENAME);
		}
		return false;
	}

	fclose(file);

	if (loaded_settings.config_version != CURRENT_CONFIG_VERSION) {
		SDL_Log("Warning: Config file version mismatch. Expected %d, got %d. Using defaults.", CURRENT_CONFIG_VERSION, loaded_settings.config_version);
		return false;
	}

	if (loaded_settings.flick_stick_enabled) {
		loaded_settings.always_on_gyro = true;
	}

	settings = loaded_settings;
	settings_are_dirty = false;
	SDL_Log("Settings loaded successfully from %s.", CONFIG_FILENAME);
	return true;
}

// --- Helper Functions for LED Control ---
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

static void UpdatePhysicalControllerLED(void)
{
	if (!gamepad) {
		return;
	}
	if (SDL_SetGamepadLED(gamepad, settings.led_r, settings.led_g, settings.led_b) < 0) {
		SDL_Log("Warning: Could not set gamepad LED color: %s", SDL_GetError());
	}
	else {
		SDL_Log("Successfully set physical gamepad LED to #%02X%02X%02X", settings.led_r, settings.led_g, settings.led_b);
	}
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

				// --- Check for LED capability ---
				SDL_PropertiesID props = SDL_GetGamepadProperties(gamepad);
				controller_has_led = SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_RGB_LED_BOOLEAN, false);
				if (controller_has_led) {
					SDL_Log("Controller supports programmable LED.");
					UpdatePhysicalControllerLED();
				}
				else {
					SDL_Log("Controller does not support programmable LED.");
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
	controller_has_led = false;
	EnterCriticalSection(&data_lock);
	shared_gyro_data[0] = 0.0f; shared_gyro_data[1] = 0.0f; shared_gyro_data[2] = 0.0f;
	shared_flick_stick_delta_x = 0.0f;
	shared_mouse_aim_active = false;
	LeaveCriticalSection(&data_lock);
	gyro_data[0] = 0.0f; gyro_data[1] = 0.0f; gyro_data[2] = 0.0f;
	isAiming = false;
	calibration_state = CALIBRATION_IDLE;
	calibration_sample_count = 0;
	gyro_accumulator[0] = gyro_accumulator[1] = gyro_accumulator[2] = 0.0f;
	stability_timer_start_time = 0;
	is_flick_stick_active = false;
	flick_last_angle = 0.0f;
	SetDefaultSettings();

	// 3. Re-initialize resources
	SDL_Log("Re-initializing ViGEmBus...");
	vigem_client = vigem_alloc();
	if (vigem_client == NULL) {
		SDL_Log("FATAL: Failed to re-allocate ViGEm client during reset.");
		vigem_found = false;
		return false;
	}
	const VIGEM_ERROR ret = vigem_connect(vigem_client);
	if (!VIGEM_SUCCESS(ret)) {
		SDL_Log("FATAL: ViGEmBus re-connection failed: 0x%x.", ret);
		vigem_found = false;
		return false;
	}

	x360_pad = vigem_target_x360_alloc();
	vigem_target_set_vid(x360_pad, VIRTUAL_VENDOR_ID);
	vigem_target_set_pid(x360_pad, VIRTUAL_PRODUCT_ID);

	const VIGEM_ERROR add_ret = vigem_target_add(vigem_client, x360_pad);
	if (!VIGEM_SUCCESS(add_ret)) {
		SDL_Log("FATAL: Failed to re-add virtual X360 controller: 0x%x", add_ret);
		vigem_found = false;
		return false;
	}
	vigem_found = true;

	// 4. Actively look for an already-connected controller
	find_and_open_physical_gamepad();

	// 5. Load settings from file (or use defaults)
	if (!LoadSettings()) {
		SetDefaultSettings();
	}
	settings_are_dirty = false;

	SDL_Log("--- RESET COMPLETE ---");
	return true;
}

// --- Menu Action & Display Functions ---

void execute_mode(int direction) {
	if (direction == 0) { // Enter
		settings.mouse_mode = !settings.mouse_mode;
		settings_are_dirty = true;
		SDL_Log("Mouse mode toggled %s.", settings.mouse_mode ? "ON" : "OFF");
	}
}
void display_mode(char* buffer, size_t size) {
	snprintf(buffer, size, "%s", settings.mouse_mode ? "Mouse" : "Joystick");
}

void execute_sensitivity(int direction) {
	if (direction == 0) return; // Enter does nothing
	float step = (direction > 0) ? 1.0f : -1.0f; // Right increases, Left decreases

	if (settings.mouse_mode) {
		settings.mouse_sensitivity += step * 500.0f;
		settings.mouse_sensitivity = CLAMP(settings.mouse_sensitivity, 100.0f, 20000.0f);
		SDL_Log("Mouse Sensitivity changed to %.1f", settings.mouse_sensitivity);
	}
	else {
		settings.sensitivity += step * 0.5f;
		settings.sensitivity = CLAMP(settings.sensitivity, 0.5f, 50.0f);
		SDL_Log("Joystick Sensitivity changed to %.1f", settings.sensitivity);
	}
	settings_are_dirty = true;
}
void display_sensitivity(char* buffer, size_t size) {
	if (settings.mouse_mode) {
		snprintf(buffer, size, "%.0f", settings.mouse_sensitivity);
	}
	else {
		snprintf(buffer, size, "%.1f", settings.sensitivity);
	}
}

void execute_flick_stick(int direction) {
	if (direction == 0) {
		if (!settings.flick_stick_enabled) {
			settings.flick_stick_enabled = true;
			settings.always_on_gyro = true;
			is_flick_stick_active = false;
			flick_last_angle = 0.0f;
			settings_are_dirty = true;
			SDL_Log("Flick Stick enabled.");
		}
		else {
			settings.flick_stick_enabled = false;
			settings.always_on_gyro = false;
			settings_are_dirty = true;
			SDL_Log("Flick Stick disabled.");
		}
	}
}
void display_flick_stick(char* buffer, size_t size) {
	snprintf(buffer, size, "%s", settings.flick_stick_enabled ? "ON" : "OFF");
}

void execute_always_on(int direction) {
	if (direction == 0) {
		if (settings.flick_stick_enabled) {
			SDL_Log("Always-On Gyro is required for Flick Stick and cannot be disabled.");
		}
		else {
			settings.always_on_gyro = !settings.always_on_gyro;
			if (settings.always_on_gyro) {
				isAiming = false;
			}
			settings_are_dirty = true;
			SDL_Log("Always-on gyro toggled %s.", settings.always_on_gyro ? "ON" : "OFF");
		}
	}
}
void display_always_on(char* buffer, size_t size) {
	snprintf(buffer, size, "%s", settings.always_on_gyro ? "ON" : "OFF");
}

void execute_anti_deadzone(int direction) {
	if (direction == 0) return; // Enter does nothing
	float step = (direction > 0) ? 1.0f : -1.0f;

	settings.anti_deathzone += step * 1.0f;
	settings.anti_deathzone = CLAMP(settings.anti_deathzone, 0.0f, 90.0f);
	settings_are_dirty = true;
	SDL_Log("Anti-Deadzone changed to %.0f%%", settings.anti_deathzone);
}
void display_anti_deadzone(char* buffer, size_t size) {
	snprintf(buffer, size, "%.0f%%", settings.anti_deathzone);
}

void execute_invert_y(int direction) {
	if (direction == 0) {
		settings.invert_gyro_y = !settings.invert_gyro_y;
		settings_are_dirty = true;
		SDL_Log("Invert Gyro Y-Axis (Pitch) toggled %s.", settings.invert_gyro_y ? "ON" : "OFF");
	}
}
void display_invert_y(char* buffer, size_t size) {
	snprintf(buffer, size, "%s", settings.invert_gyro_y ? "ON" : "OFF");
}

void execute_invert_x(int direction) {
	if (direction == 0) {
		settings.invert_gyro_x = !settings.invert_gyro_x;
		settings_are_dirty = true;
		SDL_Log("Invert Gyro X-Axis (Yaw) toggled %s.", settings.invert_gyro_x ? "ON" : "OFF");
	}
}
void display_invert_x(char* buffer, size_t size) {
	snprintf(buffer, size, "%s", settings.invert_gyro_x ? "ON" : "OFF");
}

void execute_change_aim_button(int direction) {
	if (direction == 0) {
		is_waiting_for_aim_button = true;
		settings.selected_button = -1;
		settings.selected_axis = -1;
		isAiming = false;
		SDL_Log("Press a button or trigger on the gamepad to set as Aim.");
	}
}
void display_change_aim_button(char* buffer, size_t size) {
	if (is_waiting_for_aim_button) {
		snprintf(buffer, size, "[Waiting for input...]");
	}
	else if (settings.selected_button != -1) {
		snprintf(buffer, size, "%s", SDL_GetGamepadStringForButton(settings.selected_button));
	}
	else if (settings.selected_axis != -1) {
		snprintf(buffer, size, "%s", SDL_GetGamepadStringForAxis(settings.selected_axis));
	}
	else {
		snprintf(buffer, size, "[Not set]");
	}
}

void execute_calibrate_gyro(int direction) {
	if (direction == 0) {
		if (gamepad && calibration_state == CALIBRATION_IDLE) {
			calibration_state = CALIBRATION_WAITING_FOR_STABILITY;
			stability_timer_start_time = 0;
			SDL_Log("Starting gyro calibration... Waiting for controller to be still.");
		}
		else if (calibration_state != CALIBRATION_IDLE) {
			SDL_Log("Another calibration is already in progress.");
		}
		else {
			SDL_Log("Connect a controller to calibrate the gyro.");
		}
	}
}
void display_gyro_calibration(char* buffer, size_t size) {
	snprintf(buffer, size, "P:%.3f Y:%.3f",
		settings.gyro_calibration_offset[0],
		settings.gyro_calibration_offset[1]);
}
void execute_calibrate_flick(int direction) {
	if (direction == 0) {
		if (gamepad && calibration_state == CALIBRATION_IDLE) {
			calibration_state = FLICK_STICK_CALIBRATION_START;
			SDL_Log("Starting Flick Stick calibration...");
		}
		else if (calibration_state != CALIBRATION_IDLE) {
			SDL_Log("Another calibration is already in progress.");
		}
		else {
			SDL_Log("Connect a controller to calibrate Flick Stick.");
		}
	}
}
void display_flick_calibration(char* buffer, size_t size) {
	if (settings.flick_stick_calibrated) {
		snprintf(buffer, size, "%.1f", settings.flick_stick_calibration_value);
	}
	else {
		snprintf(buffer, size, "%.1f (Default)", settings.flick_stick_calibration_value);
	}
}

void execute_change_led(int direction) {
	if (direction == 0) {
		is_entering_text = true;
		hex_input_buffer[0] = '#';
		hex_input_buffer[1] = '\0';
		SDL_StartTextInput(window);
		SDL_Log("Enter a 6-digit hex color code (e.g., #0088FF) and press Enter.");
	}
}
void display_led_color(char* buffer, size_t size) {
	if (is_entering_text) {
		snprintf(buffer, size, "%s", hex_input_buffer);
	}
	else {
		snprintf(buffer, size, "#%02X%02X%02X", settings.led_r, settings.led_g, settings.led_b);
	}
}

void execute_hide_controller(int direction) {
	if (direction == 0) {
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
	}
}
void display_hide_controller(char* buffer, size_t size) {
	snprintf(buffer, size, "%s", is_controller_hidden ? "Hidden" : "Visible");
}

void execute_save_settings(int direction) {
	if (direction == 0) {
		SaveSettings();
	}
}
void display_save_status(char* buffer, size_t size) {
	const char* dirty_indicator = settings_are_dirty ? "*" : "";
	FILE* file = fopen(CONFIG_FILENAME, "rb");
	if (file) {
		fclose(file);
		char filename_base[128];
		const char* dot = strrchr(CONFIG_FILENAME, '.');

		if (dot) {
			size_t len = dot - CONFIG_FILENAME;
			strncpy_s(filename_base, sizeof(filename_base), CONFIG_FILENAME, len);
		}
		else {
			strcpy_s(filename_base, sizeof(filename_base), CONFIG_FILENAME);
		}

		snprintf(buffer, size, "%s%s", filename_base, dirty_indicator);

	}
	else {
		snprintf(buffer, size, "[No File]%s", dirty_indicator);
	}
}

void execute_reset_app(int direction) {
	if (direction == 0) {
		if (!reset_application()) {
			SDL_Event event;
			SDL_zero(event);
			event.type = SDL_EVENT_QUIT;
			SDL_PushEvent(&event);
		}
	}
}

/* This function runs once at startup. */
SDL_AppResult SDL_AppInit(void** appstate, int argc, char* argv[])
{
	SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");

	if (SDL_InitSubSystem(SDL_INIT_GAMEPAD) < 0) {
		SDL_Log("Couldn't initialize gamepad subsystem: %s", SDL_GetError());
		return SDL_APP_FAILURE;
	}

	if (!SDL_CreateWindowAndRenderer("Universal Gyro Aim", 420, 185, 0, &window, &renderer)) {
		SDL_Log("Couldn't create window and renderer: %s", SDL_GetError());
		return SDL_APP_FAILURE;
	}

	// --- Check for HidHide driver early ---
	wchar_t cli_path_buffer[MAX_PATH];
	hidhide_found = GetHidHideCliPath(cli_path_buffer, MAX_PATH);
	if (!hidhide_found) {
		SDL_Log("Warning: HidHide driver/CLI not found. Controller hiding will not be available.");
	}

	// --- Initialize ViGEmBus and check for driver ---
	vigem_client = vigem_alloc();
	if (vigem_client == NULL) {
		SDL_Log("Error: Failed to allocate ViGEm client.");
		return SDL_APP_FAILURE;
	}
	const VIGEM_ERROR ret = vigem_connect(vigem_client);
	if (ret == VIGEM_ERROR_BUS_NOT_FOUND) {
		SDL_Log("CRITICAL: ViGEmBus driver not found. The application cannot create a virtual controller.");
		vigem_found = false;
	}
	else if (!VIGEM_SUCCESS(ret)) {
		SDL_Log("Error: ViGEmBus connection failed with code: 0x%x.", ret);
		vigem_found = false;
	}
	else {
		vigem_found = true;
		SDL_Log("Successfully connected to ViGEmBus driver.");

		x360_pad = vigem_target_x360_alloc();

		// Set a custom VID/PID to distinguish our virtual controller from real ones
		vigem_target_set_vid(x360_pad, VIRTUAL_VENDOR_ID);
		vigem_target_set_pid(x360_pad, VIRTUAL_PRODUCT_ID);

		const VIGEM_ERROR add_ret = vigem_target_add(vigem_client, x360_pad);
		if (!VIGEM_SUCCESS(add_ret)) {
			SDL_Log("Error: Failed to add virtual X360 controller: 0x%x", add_ret);
			vigem_found = false; // Treat this as a failure to have a working driver.
		}
		else {
			SDL_Log("App started. Virtual Xbox 360 controller is active.");
		}
	}

	if (!LoadSettings()) {
		SetDefaultSettings();
	}

	// --- Initialize and start the mouse thread ---
	InitializeCriticalSection(&data_lock);
	run_mouse_thread = true;
	mouse_thread_handle = CreateThread(NULL, 0, MouseThread, NULL, 0, NULL);
	if (mouse_thread_handle) {
		SetThreadPriority(mouse_thread_handle, THREAD_PRIORITY_TIME_CRITICAL);
	}
	else {
		SDL_Log("FATAL: Could not create mouse thread!");
		return SDL_APP_FAILURE;
	}

	return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void* appstate, SDL_Event* event)
{
	switch (event->type) {
	case SDL_EVENT_QUIT:
		return SDL_APP_SUCCESS;

	case SDL_EVENT_KEY_DOWN:
		// --- Handle states that take priority over menu navigation ---
		if (is_entering_text) {
			if (event->key.key == SDLK_BACKSPACE && strlen(hex_input_buffer) > 1) {
				hex_input_buffer[strlen(hex_input_buffer) - 1] = '\0';
			}
			else if (event->key.key == SDLK_RETURN || event->key.key == SDLK_KP_ENTER) {
				if (ParseHexColor(hex_input_buffer, &settings.led_r, &settings.led_g, &settings.led_b)) {
					UpdatePhysicalControllerLED();
					settings_are_dirty = true;
				}
				else {
					SDL_Log("Invalid hex color format: %s", hex_input_buffer);
				}
				is_entering_text = false;
				SDL_StopTextInput(window);
			}
			else if (event->key.key == SDLK_ESCAPE) {
				is_entering_text = false;
				SDL_StopTextInput(window);
				SDL_Log("LED color change cancelled.");
			}
			break; // Don't process other keys while typing
		}
		if (is_waiting_for_aim_button) {
			if (event->key.key == SDLK_ESCAPE) {
				is_waiting_for_aim_button = false;
				SDL_Log("Aim button selection cancelled.");
			}
			break; // Ignore other keys while waiting for gamepad input
		}

		// --- Menu Navigation Logic (aware of visible items) ---
		switch (event->key.key) {
		case SDLK_UP:
			selected_menu_item--;
			if (selected_menu_item < 0) {
				selected_menu_item = num_visible_menu_items - 1;
			}
			break;

		case SDLK_DOWN:
			selected_menu_item++;
			if (selected_menu_item >= num_visible_menu_items) {
				selected_menu_item = 0;
			}
			break;

		case SDLK_LEFT:
			if (num_visible_menu_items > 0) {
				int master_index = visible_menu_map[selected_menu_item];
				strcpy_s(active_menu_label, sizeof(active_menu_label), menu_items[master_index].label);
				if (menu_items[master_index].execute) {
					menu_items[master_index].execute(-1);
				}
			}
			break;

		case SDLK_RIGHT:
			if (num_visible_menu_items > 0) {
				int master_index = visible_menu_map[selected_menu_item];
				strcpy_s(active_menu_label, sizeof(active_menu_label), menu_items[master_index].label);
				if (menu_items[master_index].execute) {
					menu_items[master_index].execute(1);
				}
			}
			break;

		case SDLK_RETURN:
		case SDLK_KP_ENTER:
			if (num_visible_menu_items > 0) {
				int master_index = visible_menu_map[selected_menu_item];
				strcpy_s(active_menu_label, sizeof(active_menu_label), menu_items[master_index].label);
				if (menu_items[master_index].execute) {
					menu_items[master_index].execute(0);
				}
			}
			break;
		}
		break;

	case SDL_EVENT_TEXT_INPUT:
		if (is_entering_text) {
			if (strlen(hex_input_buffer) < 7) {
				strcat_s(hex_input_buffer, sizeof(hex_input_buffer), event->text.text);
			}
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

			// --- Check for LED capability ---
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
		break;
	}

	case SDL_EVENT_GAMEPAD_REMOVED:
		if (gamepad && event->gdevice.which == gamepad_instance_id) {
			SDL_Log("Gamepad disconnected: %s", SDL_GetGamepadName(gamepad));
			UnhidePhysicalController();
			SDL_SetGamepadSensorEnabled(gamepad, SDL_SENSOR_GYRO, false);
			SDL_CloseGamepad(gamepad);
			gamepad = NULL;
			controller_has_led = false;
			// Reset aiming state on disconnect
			settings.selected_button = -1;
			settings.selected_axis = -1;
			isAiming = false;
			// Clear shared data for mouse thread
			EnterCriticalSection(&data_lock);
			shared_mouse_aim_active = false;
			shared_gyro_data[0] = 0.0f;
			shared_gyro_data[1] = 0.0f;
			shared_gyro_data[2] = 0.0f;
			shared_flick_stick_delta_x = 0.0f;
			LeaveCriticalSection(&data_lock);
		}
		break;

	case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
	case SDL_EVENT_GAMEPAD_BUTTON_UP:
		if (event->gbutton.which == gamepad_instance_id) {
			// --- Intercept button for aim selection ---
			if (is_waiting_for_aim_button && event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
				settings.selected_button = event->gbutton.button;
				SDL_Log("Aim button set to: %s", SDL_GetGamepadStringForButton(settings.selected_button));
				is_waiting_for_aim_button = false;
				settings_are_dirty = true;
				break;
			}

			// --- Intercept buttons for calibrations ---
			bool button_handled = false;
			if (event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
				if (calibration_state == FLICK_STICK_CALIBRATION_START) {
					if (event->gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH) { // 'A' button
						calibration_state = FLICK_STICK_CALIBRATION_TURNING;
						flick_stick_turn_remaining = settings.flick_stick_calibration_value;
						button_handled = true;
					}
					else if (event->gbutton.button == SDL_GAMEPAD_BUTTON_EAST) { // 'B' button to cancel
						calibration_state = CALIBRATION_IDLE;
						button_handled = true;
					}
				}
				else if (calibration_state == FLICK_STICK_CALIBRATION_ADJUST) {
					const float ultra_fine_adjust_amount = 1.0f;
					const float adjust_amount = 50.0f;
					const float coarse_adjust_amount = 500.0f;
					if (event->gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_UP) {
						settings.flick_stick_calibration_value += adjust_amount;
					}
					else if (event->gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_DOWN) {
						settings.flick_stick_calibration_value -= adjust_amount;
					}
					else if (event->gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_RIGHT) {
						settings.flick_stick_calibration_value += ultra_fine_adjust_amount;
					}
					else if (event->gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_LEFT) {
						settings.flick_stick_calibration_value -= ultra_fine_adjust_amount;
					}
					else if (event->gbutton.button == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) {
						settings.flick_stick_calibration_value += coarse_adjust_amount;
					}
					else if (event->gbutton.button == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) {
						settings.flick_stick_calibration_value -= coarse_adjust_amount;
					}
					else if (event->gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH) { // 'A' to re-test
						calibration_state = FLICK_STICK_CALIBRATION_TURNING;
						flick_stick_turn_remaining = settings.flick_stick_calibration_value;
					}
					else if (event->gbutton.button == SDL_GAMEPAD_BUTTON_EAST) { // 'B' to save and exit
						settings.flick_stick_calibrated = true;
						calibration_state = CALIBRATION_IDLE;
						settings_are_dirty = true;
						SDL_Log("Flick Stick calibration saved. Value: %.2f", settings.flick_stick_calibration_value);
					}
					button_handled = true;
				}
				else if (calibration_state == CALIBRATION_WAITING_FOR_STABILITY || calibration_state == CALIBRATION_SAMPLING) {
					if (event->gbutton.button == SDL_GAMEPAD_BUTTON_EAST) { // 'B' button to cancel
						calibration_state = CALIBRATION_IDLE;
						SDL_Log("Gyro calibration cancelled by user.");
						button_handled = true;
					}
				}
			}
			if (button_handled) break;

			// --- Normal button handling ---
			if (event->gbutton.button == settings.selected_button) {
				isAiming = (event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
			}
		}
		break;

	case SDL_EVENT_GAMEPAD_AXIS_MOTION:
		if (event->gaxis.which == gamepad_instance_id) {
			if (is_waiting_for_aim_button) {
				if (event->gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ||
					event->gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) {
					if (event->gaxis.value > 8000) {
						settings.selected_axis = event->gaxis.axis;
						SDL_Log("Aim trigger set to: %s", SDL_GetGamepadStringForAxis(settings.selected_axis));
						is_waiting_for_aim_button = false;
						settings_are_dirty = true;
					}
				}
				break;
			}
			else if (event->gaxis.axis == settings.selected_axis) {
				isAiming = (event->gaxis.value > 8000);
			}
		}
		break;

	case SDL_EVENT_GAMEPAD_SENSOR_UPDATE:
		if (event->gsensor.sensor == SDL_SENSOR_GYRO) {
			switch (calibration_state) {
			case CALIBRATION_IDLE:
			{
				// Normal operation: apply the offset and update state.
				float calibrated_data[3];
				calibrated_data[0] = event->gsensor.data[0] - settings.gyro_calibration_offset[0];
				calibrated_data[1] = event->gsensor.data[1] - settings.gyro_calibration_offset[1];
				calibrated_data[2] = event->gsensor.data[2] - settings.gyro_calibration_offset[2];

				// Update the shared data for the mouse thread
				EnterCriticalSection(&data_lock);
				shared_gyro_data[0] = calibrated_data[0];
				shared_gyro_data[1] = calibrated_data[1];
				shared_gyro_data[2] = calibrated_data[2];
				LeaveCriticalSection(&data_lock);

				// Also update the local copy for the UI visualizer and joystick logic
				gyro_data[0] = calibrated_data[0];
				gyro_data[1] = calibrated_data[1];
				gyro_data[2] = calibrated_data[2];
				break;
			}
			case CALIBRATION_WAITING_FOR_STABILITY:
			{
				// Check if the controller is still
				bool is_stable = fabsf(event->gsensor.data[0]) < GYRO_STABILITY_THRESHOLD &&
					fabsf(event->gsensor.data[1]) < GYRO_STABILITY_THRESHOLD &&
					fabsf(event->gsensor.data[2]) < GYRO_STABILITY_THRESHOLD;

				if (is_stable) {
					if (stability_timer_start_time == 0) {
						// Timer not started, start it now
						stability_timer_start_time = SDL_GetPerformanceCounter();
					}
					else {
						// Timer is running, check if duration has passed
						Uint64 current_time = SDL_GetPerformanceCounter();
						Uint64 elapsed_ms = (current_time - stability_timer_start_time) * 1000 / SDL_GetPerformanceFrequency();
						if (elapsed_ms >= GYRO_STABILITY_DURATION_MS) {
							// Stable for long enough, start sampling
							calibration_state = CALIBRATION_SAMPLING;
							calibration_sample_count = 0;
							gyro_accumulator[0] = 0.0f;
							gyro_accumulator[1] = 0.0f;
							gyro_accumulator[2] = 0.0f;
							SDL_Log("Controller is stable. Starting data collection...");
						}
					}
				}
				else {
					// Controller moved, reset the timer
					stability_timer_start_time = 0;
				}
				break;
			}
			case CALIBRATION_SAMPLING:
				// We are sampling: accumulate raw data and count samples.
				gyro_accumulator[0] += event->gsensor.data[0];
				gyro_accumulator[1] += event->gsensor.data[1];
				gyro_accumulator[2] += event->gsensor.data[2];
				calibration_sample_count++;
				break;
			}
		}
		break;
	}
	return SDL_APP_CONTINUE;
}

static void BuildVisibleMenu(void) {
	num_visible_menu_items = 0;
	for (int i = 0; i < master_num_menu_items; ++i) {
		bool should_show = true;
		const char* label = menu_items[i].label;

		// --- Conditions to HIDE items ---
		if (strcmp(label, "Mode") == 0 && settings.flick_stick_enabled) {
			should_show = false;
		}
		else if (strcmp(label, "Always-On Gyro") == 0 && settings.flick_stick_enabled) {
			should_show = false;
		}
		else if (strcmp(label, "Flick Stick") == 0 && !settings.mouse_mode) {
			should_show = false;
		}
		else if (strcmp(label, "Calibrate Flick Stick") == 0 && !settings.flick_stick_enabled) {
			should_show = false;
		}
		else if (strcmp(label, "Anti-Deadzone") == 0 && settings.mouse_mode) {
			should_show = false;
		}
		else if (strcmp(label, "LED Color") == 0 && !controller_has_led) {
			should_show = false;
		}

		if (should_show) {
			visible_menu_map[num_visible_menu_items] = i;
			num_visible_menu_items++;
		}
	}

	// Clamp the cursor position to the new number of visible items
	if (selected_menu_item >= num_visible_menu_items) {
		selected_menu_item = num_visible_menu_items > 0 ? num_visible_menu_items - 1 : 0;
	}
}

SDL_AppResult SDL_AppIterate(void* appstate)
{
	// --- Handle calibration completion ---
	if (calibration_state == CALIBRATION_SAMPLING && calibration_sample_count >= CALIBRATION_SAMPLES) {
		settings.gyro_calibration_offset[0] = gyro_accumulator[0] / CALIBRATION_SAMPLES;
		settings.gyro_calibration_offset[1] = gyro_accumulator[1] / CALIBRATION_SAMPLES;
		settings.gyro_calibration_offset[2] = gyro_accumulator[2] / CALIBRATION_SAMPLES;
		calibration_state = CALIBRATION_IDLE;
		settings_are_dirty = true;
		SDL_Log("Calibration complete. Offsets saved.");
		SDL_Log("-> Pitch: %.4f, Yaw: %.4f, Roll: %.4f",
			settings.gyro_calibration_offset[0],
			settings.gyro_calibration_offset[1],
			settings.gyro_calibration_offset[2]);
	}

	XUSB_REPORT report;
	XUSB_REPORT_INIT(&report);

	bool gyro_is_active = false;
	float flick_stick_output_x = 0.0f;


	// --- Handle Flick Stick Test Turn ---
	if (calibration_state == FLICK_STICK_CALIBRATION_TURNING) {
		const float TURN_SPEED_FACTOR = 0.15f; // How fast the test turn happens
		float turn_amount_this_frame = flick_stick_turn_remaining * TURN_SPEED_FACTOR;

		if (fabsf(flick_stick_turn_remaining) < 1.0f) {
			turn_amount_this_frame = flick_stick_turn_remaining;
		}

		EnterCriticalSection(&data_lock);
		shared_flick_stick_delta_x += turn_amount_this_frame;
		LeaveCriticalSection(&data_lock);

		flick_stick_turn_remaining -= turn_amount_this_frame;

		if (fabsf(flick_stick_turn_remaining) < 0.1f) {
			calibration_state = FLICK_STICK_CALIBRATION_ADJUST;
		}
	}

	if (gamepad) {
		// --- Passthrough physical controller state to virtual controller ---
		if (calibration_state == CALIBRATION_IDLE) {
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

			report.bLeftTrigger = (SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) * 255) / 32767;
			report.bRightTrigger = (SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) * 255) / 32767;

			report.sThumbLX = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX);
			Sint16 ly = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTY);
			report.sThumbLY = (ly == -32768) ? 32767 : -ly;
		}

		// --- Determine Gyro state for this frame ---
		gyro_is_active = (isAiming || settings.always_on_gyro) && (calibration_state == CALIBRATION_IDLE);

		// --- Right Stick Processing ---
		Sint16 rx = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHTX);
		Sint16 ry = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHTY);

		if (settings.flick_stick_enabled && settings.flick_stick_calibrated && calibration_state == CALIBRATION_IDLE) {
			// --- Flick Stick Logic ---
			const float FLICK_STICK_DEADZONE = 24000.0f;
			float stick_magnitude = sqrtf((float)rx * rx + (float)ry * ry);

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
				flick_stick_output_x = 0.0f;
			}

			// Pass output to the mouse thread. Gyro is handled there.
			EnterCriticalSection(&data_lock);
			shared_mouse_aim_active = gyro_is_active;
			shared_flick_stick_delta_x += flick_stick_output_x;
			LeaveCriticalSection(&data_lock);
			report.sThumbRX = 0;
			report.sThumbRY = 0;
		}
		else {
			// --- Standard Logic ---
			float stick_magnitude = sqrtf((float)rx * rx + (float)ry * ry);
			bool stick_in_use = stick_magnitude > 8000.0f;
			bool use_gyro_for_aim = gyro_is_active && !stick_in_use;

			if (settings.mouse_mode) {
				EnterCriticalSection(&data_lock);
				shared_mouse_aim_active = use_gyro_for_aim;
				LeaveCriticalSection(&data_lock);
				report.sThumbRX = 0;
				report.sThumbRY = 0;
			}
			else { // Joystick Mode
				float combined_x = (float)rx;
				float combined_y = (ry == -32768) ? 32767.f : (float)-ry;

				if (use_gyro_for_aim) {
					const float x_multiplier = settings.invert_gyro_x ? 10000.0f : -10000.0f;
					const float y_multiplier = settings.invert_gyro_y ? -10000.0f : 10000.0f;
					float gyro_input_x = gyro_data[1] * settings.sensitivity * x_multiplier;
					float gyro_input_y = gyro_data[0] * settings.sensitivity * y_multiplier;

					combined_x += gyro_input_x;
					combined_y += gyro_input_y;
				}
				report.sThumbRX = (short)CLAMP(combined_x, -32767.0f, 32767.0f);
				report.sThumbRY = (short)CLAMP(combined_y, -32767.0f, 32767.0f);
			}
		}
	}


	if (vigem_found && x360_pad && vigem_client) {
		vigem_target_x360_update(vigem_client, x360_pad, report);
	}

	// --- Drawing UI ---
	SDL_SetRenderDrawColor(renderer, 25, 25, 40, 255);
	SDL_RenderClear(renderer);

	// --- Handle Critical Errors ---
	if (!vigem_found) {
		SDL_SetRenderDrawColor(renderer, 255, 100, 100, 255);
		const float line_height = (float)(SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE + 4);
		float y_pos = 10.0f;
		SDL_RenderDebugText(renderer, 10, y_pos, "CRITICAL ERROR: Could not connect to ViGEmBus!");
		y_pos += line_height * 2;
		SDL_RenderDebugText(renderer, 10, y_pos, "The application cannot create a virtual controller.");
		y_pos += line_height;
		SDL_RenderDebugText(renderer, 10, y_pos, "Please ensure the ViGEmBus driver is installed.");
		y_pos += line_height * 2;
		SDL_RenderDebugText(renderer, 10, y_pos, "Get it from: github.com/ViGEm/ViGEmBus/releases");

		SDL_RenderPresent(renderer);
		return SDL_APP_CONTINUE;
	}

	int w = 0, h = 0;
	SDL_GetRenderOutputSize(renderer, &w, &h);
	const float line_height = (float)(SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE + 4);
	float y_pos;

	// Build the list of visible menu items for this frame
	BuildVisibleMenu();
	if (active_menu_label[0] != '\0') {
		for (int i = 0; i < num_visible_menu_items; ++i) {
			if (strcmp(menu_items[visible_menu_map[i]].label, active_menu_label) == 0) {
				selected_menu_item = i;
				break;
			}
		}
		// Clear the label so we don't do this again until the next action
		active_menu_label[0] = '\0';
	}

	// --- Handle Special UI states ---
	if (!gamepad) {
		const char* message = "Waiting for physical controller...";
		float x = (w - (float)SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE * SDL_strlen(message)) / 2.0f;
		float y = (h - (float)SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE) / 2.0f;
		SDL_SetRenderDrawColor(renderer, 200, 200, 255, 255);
		SDL_RenderDebugText(renderer, x, y, message);
	}
	else if (is_waiting_for_aim_button) {
		y_pos = (h - (float)SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE * 3) / 2.0f;
		const char* msg1 = "SET AIM BUTTON";
		const char* msg2 = "Press a button or pull a trigger on your controller.";
		const char* msg3 = "Press ESC to cancel.";
		float x1 = (w - (float)SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE * SDL_strlen(msg1)) / 2.0f;
		float x2 = (w - (float)SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE * SDL_strlen(msg2)) / 2.0f;
		float x3 = (w - (float)SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE * SDL_strlen(msg3)) / 2.0f;
		SDL_SetRenderDrawColor(renderer, 255, 255, 100, 255);
		SDL_RenderDebugText(renderer, x1, y_pos, msg1);
		y_pos += line_height * 1.5f;
		SDL_RenderDebugText(renderer, x2, y_pos, msg2);
		y_pos += line_height;
		SDL_RenderDebugText(renderer, x3, y_pos, msg3);
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
				snprintf(buffer, sizeof(buffer), "Keep still for %d more seconds...", remaining_secs);
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
			const char* msg2 = "Do not move the controller.";
			const char* msg3 = "Press (B) on controller to cancel.";
			float x1 = (w - (float)SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE * strlen(buffer)) / 2.0f;
			float x2 = (w - (float)SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE * strlen(msg2)) / 2.0f;
			float x3 = (w - (float)SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE * strlen(msg3)) / 2.0f;

			SDL_RenderDebugText(renderer, x1, y_pos, buffer);
			y_pos += line_height;
			SDL_RenderDebugText(renderer, x2, y_pos, msg2);
			y_pos += line_height * 1.5f;
			SDL_RenderDebugText(renderer, x3, y_pos, msg3);
		}
		else if (calibration_state == FLICK_STICK_CALIBRATION_START) {
			const char* msg1 = "FLICK STICK CALIBRATION";
			const char* msg2 = "Press (A) to perform a test 360 turn.";
			const char* msg3 = "Press (B) to cancel.";
			float x1 = (w - (float)SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE * strlen(msg1)) / 2.0f;
			float x2 = (w - (float)SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE * strlen(msg2)) / 2.0f;
			float x3 = (w - (float)SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE * strlen(msg3)) / 2.0f;
			SDL_RenderDebugText(renderer, x1, y_pos, msg1);
			y_pos += line_height * 1.5f;
			SDL_RenderDebugText(renderer, x2, y_pos, msg2);
			y_pos += line_height * 1.5f;
			SDL_RenderDebugText(renderer, x3, y_pos, msg3);
		}
		else if (calibration_state == FLICK_STICK_CALIBRATION_TURNING) {
			const char* msg1 = "TURNING...";
			float x1 = (w - (float)SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE * strlen(msg1)) / 2.0f;
			SDL_RenderDebugText(renderer, x1, y_pos, msg1);
		}
		else if (calibration_state == FLICK_STICK_CALIBRATION_ADJUST) {
			const char* msg1 = "ADJUST CALIBRATION";
			snprintf(buffer, sizeof(buffer), "Current Value: %.1f", settings.flick_stick_calibration_value);
			const char* msg2 = "D-Pad U/D: Fine Tune (+/- 50)";
			const char* msg5 = "D-Pad L/R: Ultra-Fine Tune (+/- 1)";
			const char* msg3 = "Shoulders: Coarse Tune (+/- 500)";
			const char* msg4 = "Press (A) to re-test. Press (B) to save.";
			float x1 = (w - (float)SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE * strlen(msg1)) / 2.0f;
			float x_buf = (w - (float)SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE * strlen(buffer)) / 2.0f;
			float x2 = (w - (float)SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE * strlen(msg2)) / 2.0f;
			float x5 = (w - (float)SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE * strlen(msg5)) / 2.0f;
			float x3 = (w - (float)SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE * strlen(msg3)) / 2.0f;
			float x4 = (w - (float)SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE * strlen(msg4)) / 2.0f;

			SDL_RenderDebugText(renderer, x1, y_pos, msg1);
			y_pos += line_height;
			SDL_RenderDebugText(renderer, x_buf, y_pos, buffer);
			y_pos += line_height * 1.5f;
			SDL_RenderDebugText(renderer, x2, y_pos, msg2);
			y_pos += line_height;
			SDL_RenderDebugText(renderer, x5, y_pos, msg5);
			y_pos += line_height;
			SDL_RenderDebugText(renderer, x3, y_pos, msg3);
			y_pos += line_height;
			SDL_RenderDebugText(renderer, x4, y_pos, msg4);
		}
	}
	else {
		// --- Draw the Dynamic Menu ---
		y_pos = 10.0f;
		float x_label = 5.0f;
		float x_value = 200.0f;
		char label_buffer[128];
		char value_buffer[128];

		for (int i = 0; i < num_visible_menu_items; ++i) {
			int real_index = visible_menu_map[i];
			bool is_selected = (i == selected_menu_item);

			if (is_selected) {
				SDL_SetRenderDrawColor(renderer, 255, 255, 100, 255);
			}
			else {
				SDL_SetRenderDrawColor(renderer, 200, 200, 255, 255);
			}

			// Prepare and draw the label
			snprintf(label_buffer, sizeof(label_buffer), "%s%s",
				is_selected ? ">" : " ",
				menu_items[real_index].label);
			SDL_RenderDebugText(renderer, x_label, y_pos, label_buffer);

			// Prepare and draw the value, if a display function exists
			if (menu_items[real_index].display) {
				menu_items[real_index].display(value_buffer, sizeof(value_buffer));
				SDL_RenderDebugText(renderer, x_value, y_pos, value_buffer);
			}

			y_pos += line_height * 1.2f;
		}


		// --- Gyro Visualizer ---
		const int centerX = w - 55;
		const int centerY = 55;
		const int outerRadius = 50;
		const int innerRadius = 5;
		const float visualizerScale = 20.0f;

		if (settings.flick_stick_enabled) {
			SDL_SetRenderDrawColor(renderer, 255, 80, 80, 255);
		}
		else {
			SDL_SetRenderDrawColor(renderer, 100, 100, 120, 255);
		}
		DrawCircle(renderer, centerX, centerY, outerRadius);

		if (!settings.mouse_mode && settings.anti_deathzone > 0.0f) {
			int adzRadius = (int)(outerRadius * (settings.anti_deathzone / 100.0f));
			if (adzRadius > 0) {
				SDL_SetRenderDrawColor(renderer, 60, 60, 80, 255);
				DrawFilledCircle(renderer, centerX, centerY, adzRadius);
			}
		}

		const float x_multiplier = settings.invert_gyro_x ? 1.0f : -1.0f;
		const float y_multiplier = settings.invert_gyro_y ? -1.0f : 1.0f;
		float dotX_offset = gyro_data[1] * visualizerScale * x_multiplier;
		float dotY_offset = -gyro_data[0] * visualizerScale * y_multiplier;

		float distance = sqrtf(dotX_offset * dotX_offset + dotY_offset * dotY_offset);
		if (distance > outerRadius) {
			dotX_offset = (dotX_offset / distance) * outerRadius;
			dotY_offset = (dotY_offset / distance) * outerRadius;
		}

		int dotX = centerX + (int)dotX_offset;
		int dotY = centerY + (int)dotY_offset;

		if (gyro_is_active) {
			SDL_SetRenderDrawColor(renderer, 255, 80, 80, 255);
		}
		else {
			SDL_SetRenderDrawColor(renderer, 200, 200, 255, 255);
		}

		DrawFilledCircle(renderer, dotX, dotY, innerRadius);
	}

	SDL_RenderPresent(renderer);

	return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void* appstate, SDL_AppResult result)
{
	// --- Stop and clean up the mouse thread ---
	if (mouse_thread_handle) {
		run_mouse_thread = false;
		WaitForSingleObject(mouse_thread_handle, INFINITE);
		CloseHandle(mouse_thread_handle);
	}
	DeleteCriticalSection(&data_lock);


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