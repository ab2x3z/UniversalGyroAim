#ifndef VIGEM_H
#define VIGEM_H

#include "state.h"

bool Vigem_Init(void);
void Vigem_Shutdown(void);
void Vigem_Update(XUSB_REPORT report);

#endif