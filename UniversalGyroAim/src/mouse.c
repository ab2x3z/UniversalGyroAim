#include "mouse.h"
#pragma comment(lib, "winmm.lib")

DWORD WINAPI MouseThread(LPVOID lpParam) {
	float accumulator_x = 0.0f;
	float accumulator_y = 0.0f;

	Uint64 perf_freq = SDL_GetPerformanceFrequency();
	Uint64 last_time = SDL_GetPerformanceCounter();

	timeBeginPeriod(1);

	while (run_mouse_thread) {
		Uint64 current_time = SDL_GetPerformanceCounter();
		float dt = (float)(current_time - last_time) / (float)perf_freq;
		last_time = current_time;

		EnterCriticalSection(&data_lock);
		float current_gyro_x = shared_gyro_data[0];
		float current_gyro_y = shared_gyro_data[1];
		float flick_stick_dx = shared_flick_stick_delta_x;
		shared_flick_stick_delta_x = 0.0f;
		bool is_active = shared_mouse_aim_active;
		LeaveCriticalSection(&data_lock);

		float deltaX = flick_stick_dx;
		float deltaY = 0.0f;

		if (is_active) {
			deltaX += current_gyro_y * dt * settings.mouse_sensitivity * (settings.invert_gyro_x ? 1.0f : -1.0f);
			deltaY += current_gyro_x * dt * settings.mouse_sensitivity * (settings.invert_gyro_y ? 1.0f : -1.0f);
		}
		accumulator_x += deltaX;
		accumulator_y += deltaY;

		LONG move_x = 0, move_y = 0;
		if (fabsf(accumulator_x) >= 1.0f) {
			move_x = (LONG)accumulator_x; accumulator_x -= move_x;
		}
		if (fabsf(accumulator_y) >= 1.0f) {
			move_y = (LONG)accumulator_y; accumulator_y -= move_y;
		}

		if (move_x != 0 || move_y != 0) {
			INPUT inputs[MOUSE_INPUT_BATCH_SIZE] = { 0 };
			int batch_count = 0;
			long x_rem = move_x, y_rem = move_y;

			while (x_rem != 0 || y_rem != 0) {
				long dx = (labs(x_rem) > labs(y_rem)) ? ((x_rem > 0) ? 1 : -1) : 0;
				long dy = (labs(y_rem) >= labs(x_rem)) ? ((y_rem > 0) ? 1 : -1) : 0;
				x_rem -= dx; y_rem -= dy;

				inputs[batch_count].type = INPUT_MOUSE;
				inputs[batch_count].mi.dx = dx;
				inputs[batch_count].mi.dy = dy;
				inputs[batch_count].mi.dwFlags = MOUSEEVENTF_MOVE;
				batch_count++;

				if (batch_count == MOUSE_INPUT_BATCH_SIZE) {
					SendInput(batch_count, inputs, sizeof(INPUT)); batch_count = 0;
				}
			}
			if (batch_count > 0) SendInput(batch_count, inputs, sizeof(INPUT));
		}
		Sleep(1);
	}

	timeEndPeriod(1);
	return 0;
}

bool Mouse_StartThread(void) {
	InitializeCriticalSection(&data_lock);
	run_mouse_thread = true;
	mouse_thread_handle = CreateThread(NULL, 0, MouseThread, NULL, 0, NULL);
	if (mouse_thread_handle) {
		SetThreadPriority(mouse_thread_handle, THREAD_PRIORITY_TIME_CRITICAL);
		return true;
	}
	SDL_Log("FATAL: Could not create mouse thread!");
	return false;
}

void Mouse_StopThread(void) {
	if (mouse_thread_handle) {
		run_mouse_thread = false;
		WaitForSingleObject(mouse_thread_handle, INFINITE);
		CloseHandle(mouse_thread_handle);
		mouse_thread_handle = NULL;
	}
	DeleteCriticalSection(&data_lock);
}