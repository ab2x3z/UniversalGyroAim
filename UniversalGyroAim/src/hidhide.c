#include "hidhide.h"
#include <shlwapi.h>
#pragma comment(lib, "shlwapi.lib")
#include <ShlObj.h>
#include <stdio.h>

static wchar_t hid_hide_cli_path[MAX_PATH] = { 0 };

static bool ExecuteCommand(const wchar_t* command)
{
	STARTUPINFOW si;
	PROCESS_INFORMATION pi;
	DWORD exit_code = 1;

	ZeroMemory(&si, sizeof(si));
	si.cb = sizeof(si);
	ZeroMemory(&pi, sizeof(pi));

	wchar_t* cmd_mutable = _wcsdup(command);
	if (!cmd_mutable) return false;

	if (CreateProcessW(NULL, cmd_mutable, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
		WaitForSingleObject(pi.hProcess, INFINITE);
		GetExitCodeProcess(pi.hProcess, &exit_code);
		CloseHandle(pi.hProcess);
		CloseHandle(pi.hThread);
	} else {
		SDL_Log("CreateProcess failed (%lu) for command: %ls", GetLastError(), command);
	}
	free(cmd_mutable);
	return (exit_code == 0);
}

static char* ConvertSymbolicLinkToDeviceInstancePath(const char* symbolic_link)
{
	if (!symbolic_link) return NULL;
	const char* start = strstr(symbolic_link, "HID#");
	if (!start) start = strstr(symbolic_link, "USB#");
	if (!start) return NULL;

	const char* end = NULL;
	const char* current_pos = start;
	while ((current_pos = strstr(current_pos, "#{")) != NULL) {
		end = current_pos;
		current_pos++;
	}
	if (!end) return NULL;

	size_t len = end - start;
	char* instance_path = (char*)malloc(len + 1);
	if (!instance_path) return NULL;

	strncpy_s(instance_path, len + 1, start, len);
	char* first_hash = strchr(instance_path, '#');
	if (first_hash) {
		*first_hash = '\\';
		char* second_hash = strchr(first_hash + 1, '#');
		if (second_hash) *second_hash = '\\';
	}
	return instance_path;
}

static bool GetHidHideCliPath(wchar_t* cli_path, size_t cli_path_size)
{
	if (hid_hide_cli_path[0] != L'\0') {
		wcscpy_s(cli_path, cli_path_size, hid_hide_cli_path);
		return true;
	}

	const KNOWNFOLDERID* folder_ids[] = { &FOLDERID_ProgramFiles, &FOLDERID_ProgramFilesX86 };
	const wchar_t* sub_paths[] = { L"Nefarius Software Solutions\\HidHide\\x64", L"Nefarius Software Solutions\\HidHide", L"Nefarius\\HidHide", L"HidHide" };

	wchar_t* program_files_path = NULL;
	for (int i = 0; i < sizeof(folder_ids) / sizeof(folder_ids[0]); ++i) {
		if (SUCCEEDED(SHGetKnownFolderPath(folder_ids[i], 0, NULL, &program_files_path))) {
			for (int j = 0; j < sizeof(sub_paths) / sizeof(sub_paths[0]); ++j) {
				wchar_t combined_path[MAX_PATH];
				PathCombineW(combined_path, program_files_path, sub_paths[j]);
				PathCombineW(cli_path, combined_path, L"HidHideCLI.exe");
				if (GetFileAttributesW(cli_path) != INVALID_FILE_ATTRIBUTES) {
					SDL_Log("Found HidHideCLI.exe at: %ls", cli_path);
					wcscpy_s(hid_hide_cli_path, MAX_PATH, cli_path);
					CoTaskMemFree(program_files_path);
					return true;
				}
			}
			CoTaskMemFree(program_files_path);
		}
	}
	return false;
}

bool IsHidHideAvailable(void) {
	wchar_t cli_path_buffer[MAX_PATH];
	return GetHidHideCliPath(cli_path_buffer, MAX_PATH);
}

void UnhidePhysicalController(void)
{
	if (!is_controller_hidden || hidden_device_instance_path[0] == L'\0') return;
	wchar_t cli_path[MAX_PATH];
	if (!GetHidHideCliPath(cli_path, MAX_PATH)) return;

	wchar_t command[1024];
	swprintf_s(command, 1024, L"\"%s\" --dev-unhide \"%s\"", cli_path, hidden_device_instance_path);

	SDL_Log("Attempting to unhide controller...");
	if (ExecuteCommand(command)) {
		SDL_Log("Physical controller successfully unhidden.");
		is_controller_hidden = false;
		hidden_device_instance_path[0] = L'\0';
	} else {
		SDL_Log("Failed to unhide physical controller.");
	}
}

void HidePhysicalController(SDL_Gamepad* pad_to_hide)
{
	if (is_controller_hidden) return;
	if (!pad_to_hide) return;

	wchar_t cli_path[MAX_PATH];
	if (!GetHidHideCliPath(cli_path, MAX_PATH)) return;

	wchar_t command[1024];
	const char* dev_path = ConvertSymbolicLinkToDeviceInstancePath(SDL_GetGamepadPath(pad_to_hide));
	if (!dev_path) return;

	if (MultiByteToWideChar(CP_UTF8, 0, dev_path, -1, hidden_device_instance_path, MAX_PATH) == 0) {
		SDL_free((void*)dev_path);
		return;
	}

	swprintf_s(command, 1024, L"\"%s\" --dev-hide \"%s\"", cli_path, hidden_device_instance_path);
	SDL_Log("Hiding device: %s", dev_path);
	SDL_free((void*)dev_path);

	if (!ExecuteCommand(command)) {
		hidden_device_instance_path[0] = L'\0';
		return;
	}

	swprintf_s(command, 1024, L"\"%s\" --enable", cli_path);
	if (ExecuteCommand(command)) {
		is_controller_hidden = true;
		SDL_Log("Successfully hid physical controller.");
	} else {
		is_controller_hidden = true;
		SDL_Log("Failed to enable HidHide service, but device may still be hidden.");
	}
}