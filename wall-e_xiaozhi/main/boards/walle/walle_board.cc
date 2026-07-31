/**
 * Wall-E voice robot board (xiaozhi-esp32 port)
 *
 * Single-MCU design on ESP32-S3 N16R8 (third-party Wall-E kit, see
 * hardware/另一套硬件方案.md):
 *   - Voice: INMP441 mic + PCM5102 DAC (NoAudioCodecSimplex)
 *   - Eyes:  eye display is on the ESP32-S3-CAM module (confirmed), NOT on
 *            this MCU - the GC9A01 1.28" round 240x240 code path (LVGL via
 *            SpiLcdDisplay) is kept but disabled (EYE_DISPLAY_ENABLED=0)
 *   - Status: ST7789 1.3" 240x240 on its own SPI bus (walle_status_display)
 *   - Motion: TB6612 motors + LU9685/PCA9685 servos (walle_motion)
 *   - Buttons: BOOT (chat/config), volume +/- and long-press restart
 *   - Network: Wi-Fi first, ML307A 4G fallback on connect timeout
 *     (DualNetworkBoard; double-click BOOT to switch manually)
 *
 * Reference boards: bread-compact-wifi (audio/buttons),
 * spotpear/sp-esp32-s3-1.28-box (GC9A01 wiring), electron-bot (robot style).
 */

#include "dual_network_board.h"
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
#include "walle_cam_viewer.h"

#include <esp_log.h>
#include <esp_system.h>
#include <driver/spi_master.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_gc9a01.h>

#define TAG "WalleBoard"

// walle_serial.cc: USB serial protocol task (Pi fallback + debug)
extern void WalleSerialStart();


class WalleBoard : public DualNetworkBoard {
private:
    Display* display_ = nullptr;
    Button boot_button_;
    Button volume_up_button_;
    Button volume_down_button_;
    Button reset_button_;
    NetworkEventCallback app_callback_;
    // Distinguishes a user-requested Wi-Fi config mode from the connect-timeout
    // path (which triggers the automatic 4G fallback instead).
    bool manual_config_request_ = false;

#if EYE_DISPLAY_ENABLED
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
#endif  // EYE_DISPLAY_ENABLED

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            // While a camera preview/replay is on the eye display, BOOT
            // stops it instead of toggling the chat state.
            auto& viewer = WalleCamViewer::GetInstance();
            if (viewer.IsBusy()) {
                viewer.StopPlayback();
                return;
            }
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                if (GetNetworkType() == NetworkType::WIFI) {
                    manual_config_request_ = true;
                    static_cast<WifiBoard&>(GetCurrentBoard()).EnterWifiConfigMode();
                    return;
                }
            }
            app.ToggleChatState();
        });
        // Double-click in starting/configuring state: manually switch Wi-Fi <-> 4G
        boot_button_.OnDoubleClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting || app.GetDeviceState() == kDeviceStateWifiConfiguring) {
                manual_config_request_ = true;
                SwitchNetworkType();
            }
        });

        // Volume +/- (same behaviour as bread-compact-wifi)
        volume_up_button_.OnClick([this]() {
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() + 10;
            if (volume > 100) {
                volume = 100;
            }
            codec->SetOutputVolume(volume);
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
        });
        volume_up_button_.OnLongPress([this]() {
            GetAudioCodec()->SetOutputVolume(100);
            GetDisplay()->ShowNotification(Lang::Strings::MAX_VOLUME);
        });
        volume_down_button_.OnClick([this]() {
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() - 10;
            if (volume < 0) {
                volume = 0;
            }
            codec->SetOutputVolume(volume);
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
        });
        volume_down_button_.OnLongPress([this]() {
            GetAudioCodec()->SetOutputVolume(0);
            GetDisplay()->ShowNotification(Lang::Strings::MUTED);
        });

        // Dedicated long-press restart button
        reset_button_.OnLongPress([]() {
            esp_restart();
        });
    }

public:
    WalleBoard() : DualNetworkBoard(ML307_TX_PIN, ML307_RX_PIN, ML307_DTR_PIN, /*default_net_type=*/0),
        boot_button_(BOOT_BUTTON_GPIO),
        volume_up_button_(VOLUME_UP_BUTTON_GPIO),
        volume_down_button_(VOLUME_DOWN_BUTTON_GPIO),
        reset_button_(RESET_BUTTON_GPIO) {
#if EYE_DISPLAY_ENABLED
        InitializeSpi();
        InitializeDisplay();
#else
        // Eye-display wiring unconfirmed (conflicts with the new hardware
        // pin map) - run without it until config.h is updated.
        ESP_LOGW(TAG, "Eye display disabled (EYE_DISPLAY_ENABLED=0)");
        display_ = new NoDisplay();
#endif
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
        // Secondary ST7789 status screen on its own SPI bus. When the eye
        // display is disabled it initialises the LVGL port itself.
        if (WalleStatusDisplay::GetInstance().Init() != ESP_OK) {
            ESP_LOGE(TAG, "Status display failed to start - continuing without it");
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

    // Wrap the application callback: a Wi-Fi connect timeout normally enters
    // the AP config mode; with WIFI_AUTO_FALLBACK_4G it switches to 4G instead
    // (unless the user explicitly requested config mode via the BOOT button).
    virtual void SetNetworkEventCallback(NetworkEventCallback callback) override {
        app_callback_ = std::move(callback);
        GetCurrentBoard().SetNetworkEventCallback([this](NetworkEvent event, const std::string& data) {
#if WIFI_AUTO_FALLBACK_4G
            if (event == NetworkEvent::WifiConfigModeEnter
                && GetNetworkType() == NetworkType::WIFI
                && !manual_config_request_) {
                ESP_LOGW(TAG, "Wi-Fi connect timeout, falling back to 4G");
                SwitchNetworkType();  // saves ML307 as default and reboots
                return;
            }
#endif
            if (app_callback_) {
                app_callback_(event, data);
            }
        });
    }

    virtual Backlight* GetBacklight() override {
#if EYE_DISPLAY_ENABLED
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
#else
        return nullptr;
#endif
    }
};

DECLARE_BOARD(WalleBoard);
