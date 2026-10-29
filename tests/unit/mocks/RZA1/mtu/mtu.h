#pragma once
#include <cstdint>
extern volatile uint16_t usbTestTimer;
static volatile uint16_t* const TCNT[] = {&usbTestTimer};
