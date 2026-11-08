// rtc.h - PCF85063 real-time clock (I2C 0x51) on the shared bus of touch.h, kept in UTC.
//
// Registers and procedure of the Waveshare demo (02_I2C_PCF85063 pcf85063.cpp): 7 time registers from 0x04
// (seconds, minutes, hours, days, weekdays, months, years) in BCD, read and written as one burst; years 2000-2099.
// Validity = bit 7 of the seconds register (the demo's "flag_vl"; on the PCF85063 it is the oscillator-stop flag OS):
// set = the clock stopped since it was last set, so the time is not trusted. Writing the time clears it.
#pragma once
#include <stdint.h>

namespace hw {

bool rtcBegin();  // reads the time registers once (demo pcf85063_init); true = the chip answered

// UTC seconds since 1970-01-01. true only if the chip answered, the oscillator-stop flag is clear and the registers
// hold a valid date; *utc is not changed otherwise. oscStopped (optional) = the flag was set.
bool rtcGet(uint32_t* utc, bool* oscStopped = nullptr);

// Sets the clock (e.g. from the GNSS time). 2000-01-01 <= utc < 2100-01-01. Reads back and checks (+-2 s).
bool rtcSet(uint32_t utc);

}  // namespace hw
