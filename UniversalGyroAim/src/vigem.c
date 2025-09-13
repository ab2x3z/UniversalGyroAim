#include "vigem.h"

bool Vigem_Init(void) {
	vigem_client = vigem_alloc();
	if (vigem_client == NULL) {
		SDL_Log("Error: Failed to allocate ViGEm client.");
		vigem_found = false;
		return false;
	}
	const VIGEM_ERROR ret = vigem_connect(vigem_client);
	if (!VIGEM_SUCCESS(ret)) {
		SDL_Log("ViGEmBus connection failed: 0x%x. Is the driver installed?", ret);
		vigem_found = false;
		return false;
	}

	SDL_Log("Successfully connected to ViGEmBus driver.");
	x360_pad = vigem_target_x360_alloc();
	vigem_target_set_vid(x360_pad, VIRTUAL_VENDOR_ID);
	vigem_target_set_pid(x360_pad, VIRTUAL_PRODUCT_ID);

	const VIGEM_ERROR add_ret = vigem_target_add(vigem_client, x360_pad);
	if (!VIGEM_SUCCESS(add_ret)) {
		SDL_Log("Error: Failed to add virtual X360 controller: 0x%x", add_ret);
		vigem_found = false;
		return false;
	}

	SDL_Log("Virtual Xbox 360 controller is active.");
	vigem_found = true;
	return true;
}

void Vigem_Shutdown(void) {
	if (vigem_client) {
		if (x360_pad) {
			vigem_target_remove(vigem_client, x360_pad);
			vigem_target_free(x360_pad);
			x360_pad = NULL;
		}
		vigem_disconnect(vigem_client);
		vigem_free(vigem_client);
		vigem_client = NULL;
	}
}

void Vigem_Update(XUSB_REPORT report) {
	if (vigem_found && x360_pad && vigem_client) {
		vigem_target_x360_update(vigem_client, x360_pad, report);
	}
}