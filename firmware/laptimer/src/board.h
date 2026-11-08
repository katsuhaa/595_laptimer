// board.h - pins and fixed hardware facts of the Waveshare ESP32-S3-Touch-AMOLED-2.41 as used by 595_laptimer.
// Sources: Waveshare wiki "ESP32-S3-Touch-AMOLED-2.41" (interface table) and the official demo package
// ESP32-S3-Touch-AMOLED-2.41-Demo.zip (Arduino 09_LVGL_Test, 07_EX_GPIO; ESP-IDF 10_FactoryProgram sdkconfig).
// Pins marked PLAN are the project's proposal (プロジェクト計画 §3.2) and are confirmed at F1 with the silkscreen.
#pragma once
#include <stdint.h>

namespace board {

// ---- AMOLED (RM690B0, QSPI). Native 450x600 portrait; used rotated to 600x450 (MADCTL 0x30, rows +16)
constexpr int LCD_CS = 9;
constexpr int LCD_SCK = 10;
constexpr int LCD_D0 = 11;
constexpr int LCD_D1 = 12;
constexpr int LCD_D2 = 13;
constexpr int LCD_D3 = 14;
constexpr int LCD_RST = 21;
// AMOLED_TE = EXIO0, AMOLED_EN = EXIO1 on the TCA9554 (I2C 0x20). The demo never touches the expander.
constexpr int LCD_W = 600;
constexpr int LCD_H = 450;
constexpr int LCD_ROW_OFFSET = 16;  // panel memory offset in the rotated orientation (demo flush: y + 16)

// ---- I2C bus shared by touch (FT6336, 0x38), IMU (QMI8658), RTC (PCF85063, 0x51), IO expander (TCA9554, 0x20)
constexpr int I2C_SDA = 47;
constexpr int I2C_SCL = 48;
constexpr int TP_RST = 3;
constexpr uint8_t TCA9554_ADDR = 0x20;  // EXIO0 TE, EXIO1 AMOLED_EN, EXIO2 TP_INT, EXIO3 IMU_INT2, EXIO4 IMU_INT1/RTC_INT

// ---- XTSD (SD NAND, SPI mode) on the TF-slot nets, wired from the 34-pin header (slot left empty)
constexpr int SD_CS = 2;
constexpr int SD_SCK = 4;
constexpr int SD_MOSI = 5;
constexpr int SD_MISO = 6;

// ---- GNSS Matek M9N-5883 (NEO-M9N), UART1. PLAN
constexpr int GNSS_RX = 38;  // <- M9N TX
constexpr int GNSS_TX = 39;  // -> M9N RX

// ---- CAN reader box (XIAO ESP32C6 c6_can_link 2.x, meter_link v2), UART2 receive only. PLAN
constexpr int LINK_RX = 40;  // <- XIAO "D6" (DATA of the JST-SM 4P, as on the round gauge)

// ---- Not used: battery (GPIO15 Key_BAT, GPIO16 BAT_Control, GPIO17 BAT_ADC), USB (19/20), strapping (0, 45, 46).
// Free on the header: GPIO1, 7, 8, 18, 41, 42, EXIO5-7.

}  // namespace board
