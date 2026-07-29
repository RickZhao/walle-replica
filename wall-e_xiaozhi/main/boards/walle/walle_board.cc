/**
 * Wall-E voice robot board (xiaozhi-esp32 port)
 *
 * Single-MCU design on ESP32-S3 N16R8:
 *   - Voice: INMP441 mic + PCM5102 DAC (NoAudioCodecSimplex)
 *   - Eyes:  single GC9A01 1.28" round 240x240 display (LVGL via SpiLcdDisplay)
 *   - Motion: TB6612 motors + LU9685/PCA9685 servos (walle_motion, stage 2)
 *
 * Reference boards: bread-compact-wifi (audio/buttons),
 * spotpear/sp-esp32-s3-1.28-box (GC9A01 wiring), electron-bot (robot style).
 */

#include "wifi_board.h"
#include "codecs/no_audio_codec.h"
#include "display/lcd_display.h"
#include "system_reset.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "backlight.h"
#include "led/single_led.h"
#include "assets/lang_config.h"
#include "walle_motion.h"
#include "walle_status_display.h"
#include "walle_web_server.h"

#include <esp_log.h>
#include <driver/spi_master.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_gc9a01.h>

#define TAG "WalleBoard"

// walle_serial.cc: USB serial protocol task (Pi fallback + debug)
extern void WalleSerialStart();


class WalleBoard : public WifiBoard {
private:
    Display* display_ = nullptr;
    Button boot_button_;

    void InitializeSpi() {
        ESP_LOGI(TAG, "Initialize SPI bus");
        spi_bus_config_t buscfg = GC9A01_PANEL_BUS_SPI_CONFIG(DISPLAY_SPI_SCLK_PIN, DISPLAY_SPI_MOSI_PIN,
                                    DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t));
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeDisplay() {
        ESP_LOGI(TAG, "Init GC9A01 eye display");

        esp_lcd_panel_io_handle_t io_handle = nullptr;
        esp_lcd_panel_io_spi_config_t io_config = GC9A01_PANEL_IO_SPI_CONFIG(DISPLAY_SPI_CS_PIN, DISPLAY_SPI_DC_PIN, 0, nullptr);
        io_config.pclk_hz = DISPLAY_SPI_SCLK_HZ;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &io_handle));

        esp_lcd_panel_handle_t panel_handle = nullptr;
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_SPI_RESET_PIN;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR;
        panel_config.bits_per_pixel = 16;

        ESP_ERROR_CHECK(esp_lcd_new_panel_gc9a01(io_handle, &panel_config, &panel_handle));
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
        if (esp_lcd_panel_init(panel_handle) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize GC9A01 display");
            display_ = new NoDisplay();
            return;
        }
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, true));
        ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y));
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

        display_ = new SpiLcdDisplay(io_handle, panel_handle,
                                     DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                     DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
                                     DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
    }

public:
    WalleBoard() : boot_button_(BOOT_BUTTON_GPIO) {
        InitializeSpi();
        InitializeDisplay();
        InitializeButtons();

        // Motion core (servos/motors/animations), MCP tools and the
        // USB serial protocol task
        if (WalleMotion::GetInstance().Init() != ESP_OK) {
            ESP_LOGE(TAG, "Motion core failed to start - continuing without it");
        } else {
            WalleMotion::GetInstance().RegisterMcpTools();
        }
        WalleSerialStart();

#if STATUS_DISPLAY_ENABLED
        // Secondary ST7789 status screen (needs the LVGL port already
        // initialised by the main display above)
        if (display_ != nullptr) {
            if (WalleStatusDisplay::GetInstance().Init() != ESP_OK) {
                ESP_LOGE(TAG, "Status display failed to start - continuing without it");
            }
        }
#endif

#if WEB_SERVER_ENABLED
        // HTTP control panel (API-compatible with the Pi Flask version)
        WalleWebServerStart();
#endif
    }

    virtual AudioCodec* GetAudioCodec() override {
        static NoAudioCodecSimplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT,
            AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }
};

DECLARE_BOARD(WalleBoard);
