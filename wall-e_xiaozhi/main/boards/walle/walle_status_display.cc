/**
 * WALL-E STATUS DISPLAY (ST7789 1.3" 240x240)
 *
 * @file    walle_status_display.cc
 * @brief   Implementation, see walle_status_display.h
 *
 * The GC9A01 eye display is created first by the board and initialises
 * the esp_lvgl_port task; this class only adds a second lv_display to
 * the same port. All LVGL access happens under lvgl_port_lock().
 */

#include "walle_status_display.h"
#include "walle_motion.h"
#include "config.h"

#include "application.h"
#include "device_state_machine.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <esp_lvgl_port.h>
#include <driver/spi_master.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_st7789.h>
#include <wifi_manager.h>
#include <lvgl.h>

#define TAG "WalleStatus"


struct WalleStatusDisplay::Impl {
    esp_lcd_panel_io_handle_t io = nullptr;
    esp_lcd_panel_handle_t panel = nullptr;
    lv_display_t* disp = nullptr;
    esp_timer_handle_t refresh_timer = nullptr;

    lv_obj_t* battery_label = nullptr;
    lv_obj_t* wifi_label = nullptr;
    lv_obj_t* state_label = nullptr;
    lv_obj_t* mode_label = nullptr;
};


WalleStatusDisplay& WalleStatusDisplay::GetInstance() {
    static WalleStatusDisplay instance;
    return instance;
}


esp_err_t WalleStatusDisplay::Init() {
    impl_ = new Impl();
    if (!impl_) return ESP_ERR_NO_MEM;

    // Panel IO on the shared SPI bus (initialised with the GC9A01)
    esp_lcd_panel_io_spi_config_t io_cfg = {};
    io_cfg.cs_gpio_num = STATUS_SPI_CS_PIN;
    io_cfg.dc_gpio_num = STATUS_SPI_DC_PIN;
    io_cfg.spi_mode = 0;
    io_cfg.pclk_hz = DISPLAY_SPI_SCLK_HZ;
    io_cfg.trans_queue_depth = 10;
    io_cfg.lcd_cmd_bits = 8;
    io_cfg.lcd_param_bits = 8;
    esp_err_t ret = esp_lcd_new_panel_io_spi(SPI3_HOST, &io_cfg, &impl_->io);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ST7789 panel IO failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Reset pin is shared with the GC9A01 and already toggled - skip it here
    esp_lcd_panel_dev_config_t panel_cfg = {};
    panel_cfg.reset_gpio_num = GPIO_NUM_NC;
    panel_cfg.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR;
    panel_cfg.bits_per_pixel = 16;
    ret = esp_lcd_new_panel_st7789(impl_->io, &panel_cfg, &impl_->panel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ST7789 panel create failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_ERROR_CHECK(esp_lcd_panel_reset(impl_->panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(impl_->panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(impl_->panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(impl_->panel, STATUS_DISPLAY_MIRROR_X, STATUS_DISPLAY_MIRROR_Y));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(impl_->panel, true));

    // Add as a second LVGL display on the existing esp_lvgl_port task
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = impl_->io,
        .panel_handle = impl_->panel,
        .control_handle = nullptr,
        .buffer_size = STATUS_DISPLAY_WIDTH * 20,
        .double_buffer = false,
        .trans_size = 0,
        .hres = STATUS_DISPLAY_WIDTH,
        .vres = STATUS_DISPLAY_HEIGHT,
        .monochrome = false,
        .rotation = {
            .swap_xy = STATUS_DISPLAY_SWAP_XY,
            .mirror_x = STATUS_DISPLAY_MIRROR_X,
            .mirror_y = STATUS_DISPLAY_MIRROR_Y,
        },
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_dma = 1,
            .buff_spiram = 0,
            .sw_rotate = 0,
            .swap_bytes = 1,
            .full_refresh = 0,
            .direct_mode = 0,
        },
    };

    impl_->disp = lvgl_port_add_disp(&disp_cfg);
    if (impl_->disp == nullptr) {
        ESP_LOGE(TAG, "Failed to add status display to LVGL port");
        return ESP_FAIL;
    }
    if (STATUS_DISPLAY_OFFSET_X != 0 || STATUS_DISPLAY_OFFSET_Y != 0) {
        lv_display_set_offset(impl_->disp, STATUS_DISPLAY_OFFSET_X, STATUS_DISPLAY_OFFSET_Y);
    }

    CreateUi();

    // 1s refresh timer
    const esp_timer_create_args_t timer_args = {
        .callback = [](void* arg) { ((WalleStatusDisplay*)arg)->Refresh(); },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "walle_status",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &impl_->refresh_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(impl_->refresh_timer, 1000 * 1000));

    ESP_LOGI(TAG, "ST7789 status display initialised");
    return ESP_OK;
}


static lv_obj_t* AddRow(lv_obj_t* parent, lv_obj_t* prev, lv_color_t color) {
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_text(label, "-");
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_style_text_font(label, LV_FONT_DEFAULT, 0);
    if (prev) lv_obj_align_to(label, prev, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 12);
    return label;
}


void WalleStatusDisplay::CreateUi() {
    lvgl_port_lock(0);

    lv_obj_t* screen = lv_display_get_screen_active(impl_->disp);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x101820), 0);
    lv_obj_set_style_text_color(screen, lv_color_hex(0xE0E0E0), 0);

    lv_obj_t* title = lv_label_create(screen);
    lv_label_set_text(title, "WALL-E");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFC832), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);

    impl_->battery_label = AddRow(screen, title, lv_color_hex(0xE0E0E0));
    impl_->wifi_label = AddRow(screen, impl_->battery_label, lv_color_hex(0xE0E0E0));
    impl_->state_label = AddRow(screen, impl_->wifi_label, lv_color_hex(0xE0E0E0));
    impl_->mode_label = AddRow(screen, impl_->state_label, lv_color_hex(0xE0E0E0));

    lvgl_port_unlock();
}


void WalleStatusDisplay::Refresh() {
    if (!impl_ || !impl_->disp) return;

    auto& motion = WalleMotion::GetInstance();
    auto& wifi = WifiManager::GetInstance();

    int battery = motion.battery_level();
    const char* state_name = DeviceStateMachine::GetStateName(
        Application::GetInstance().GetDeviceState());

    char buf[96];

    lvgl_port_lock(0);

    if (battery >= 0) snprintf(buf, sizeof(buf), "Battery: %d%%", battery);
    else              snprintf(buf, sizeof(buf), "Battery: --");
    lv_label_set_text(impl_->battery_label, buf);

    if (wifi.IsConnected()) {
        snprintf(buf, sizeof(buf), "WiFi: %s", wifi.GetIpAddress().c_str());
    } else {
        snprintf(buf, sizeof(buf), "WiFi: connecting...");
    }
    lv_label_set_text(impl_->wifi_label, buf);

    snprintf(buf, sizeof(buf), "State: %s", state_name);
    lv_label_set_text(impl_->state_label, buf);

    snprintf(buf, sizeof(buf), "Auto: %s", motion.auto_mode() ? "ON" : "OFF");
    lv_label_set_text(impl_->mode_label, buf);

    lvgl_port_unlock();
}
