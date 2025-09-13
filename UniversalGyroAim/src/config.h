#ifndef CONFIG_H
#define CONFIG_H

#include "state.h"

void SetDefaultSettings(void);
void SaveSettings(const char* profile_name);
bool LoadSettings(const char* profile_name);
void UpdatePhysicalControllerLED(void);

#endif