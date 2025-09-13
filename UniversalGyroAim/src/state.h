#ifndef STATE_H
#define STATE_H

#include <SDL3/SDL.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ViGEmClient.h>

#include <stdbool.h>

// --- Custom identifiers for our virtual controller ---
#define VIRTUAL_VENDOR_ID  0xFEED
#define VIRTUAL_PRODUCT_ID 0xBEEF

// --- Configuration files ---
#define PROFILES_DIRECTORY "UGA_profiles"
#define DEFAULT_PROFILE_FILENAME "default.ini"
#define CURRENT_CONFIG_VERSION 1

// --- Calibration Settings ---
#define CALIBRATION_SAMPLES 200
#define GYRO_STABILITY_THRESHOLD 0.1f
#define GYRO_STABILITY_DURATION_MS 3000

#define MOUSE_INPUT_BATCH_SIZE 64
#define CLAMP(v, min, max) (((v) < (min)) ? (min) : (((v) > (max)) ? (max) : (v)))

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

// --- Menu System Structure ---
typedef struct {
	const char* label;
	void (*execute)(int direction);
	void (*display)(char* buffer, size_t size);
} MenuItem;

// --- Global Application State (declared extern) ---
extern SDL_Window* window;
extern SDL_Renderer* renderer;
extern SDL_Gamepad* gamepad;
extern SDL_JoystickID gamepad_instance_id;
extern bool is_window_focused;
extern bool force_one_render;
extern bool isAiming;
extern AppSettings settings;
extern bool settings_are_dirty;
extern char current_profile_name[64];
extern bool controller_has_led;
extern CalibrationState calibration_state;
extern int calibration_sample_count;
extern float gyro_accumulator[3];
extern float flick_stick_turn_remaining;
extern Uint64 stability_timer_start_time;
extern float flick_last_angle;
extern bool is_flick_stick_active;

// --- Driver/Library State ---
extern PVIGEM_CLIENT vigem_client;
extern PVIGEM_TARGET x360_pad;
extern bool vigem_found;
extern bool is_controller_hidden;
extern wchar_t hidden_device_instance_path[MAX_PATH];

// --- Gyro Data ---
extern float gyro_data[3];

// --- Mouse Thread State ---
extern volatile bool run_mouse_thread;
extern HANDLE mouse_thread_handle;
extern CRITICAL_SECTION data_lock;
extern volatile float shared_gyro_data[3];
extern volatile float shared_flick_stick_delta_x;
extern volatile bool shared_mouse_aim_active;

// --- UI State ---
extern bool is_entering_text;
extern char hex_input_buffer[8];
extern bool is_entering_save_filename;
extern char filename_input_buffer[64];
extern bool is_choosing_profile;
extern char** profile_filenames;
extern int num_profiles;
extern int selected_profile_index;
extern int selected_menu_item;
extern bool is_waiting_for_aim_button;
extern char active_menu_label[128];
extern int visible_menu_map[];
extern int num_visible_menu_items;

#endif