#include "state.h"

// --- Global Application State ---
SDL_Window* window = NULL;
SDL_Renderer* renderer = NULL;
SDL_Gamepad* gamepad = NULL;
SDL_JoystickID gamepad_instance_id = 0;
bool is_window_focused = true;
bool force_one_render = false;
bool isAiming = false;
AppSettings settings;
bool settings_are_dirty = false;
char current_profile_name[64] = DEFAULT_PROFILE_FILENAME;
bool controller_has_led = false;
CalibrationState calibration_state = CALIBRATION_IDLE;
int calibration_sample_count = 0;
float gyro_accumulator[3] = { 0.0f, 0.0f, 0.0f };
float flick_stick_turn_remaining = 0.0f;
Uint64 stability_timer_start_time = 0;
float flick_last_angle = 0.0f;
bool is_flick_stick_active = false;

// --- Driver/Library State ---
PVIGEM_CLIENT vigem_client = NULL;
PVIGEM_TARGET x360_pad = NULL;
bool vigem_found = false;
bool is_controller_hidden = false;
wchar_t hidden_device_instance_path[MAX_PATH] = { 0 };

// --- Gyro Data ---
float gyro_data[3] = { 0.0f, 0.0f, 0.0f };

// --- Mouse Thread State ---
volatile bool run_mouse_thread = false;
HANDLE mouse_thread_handle = NULL;
CRITICAL_SECTION data_lock;
volatile float shared_gyro_data[3] = { 0.0f, 0.0f, 0.0f };
volatile float shared_flick_stick_delta_x = 0.0f;
volatile bool shared_mouse_aim_active = false;

// --- UI State ---
bool is_entering_text = false;
char hex_input_buffer[8] = { 0 };
bool is_entering_save_filename = false;
char filename_input_buffer[64] = { 0 };
bool is_choosing_profile = false;
char** profile_filenames = NULL;
int num_profiles = 0;
int selected_profile_index = 0;
int selected_menu_item = 0;
bool is_waiting_for_aim_button = false;
char active_menu_label[128] = { 0 };
int visible_menu_map[32]; // Max menu items
int num_visible_menu_items = 0;