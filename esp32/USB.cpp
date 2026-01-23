#include <array>
#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"

#include "config.h"

#ifdef USB_HOST
#include "usb/usb_host.h"

#include "usb/hid_host.h"
#include "usb/hid_usage_keyboard.h"
#include "usb/hid_usage_mouse.h"

#include "Scancode.h"

static uint8_t last_keys[HID_KEYBOARD_KEY_MAX]{0, 0, 0, 0, 0};
static uint8_t last_key_mod = 0;

// TODO: these are duplicated with pico
static const ATScancode scancode_map[]{
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,

    ATScancode::A,
    ATScancode::B,
    ATScancode::C,
    ATScancode::D,
    ATScancode::E,
    ATScancode::F,
    ATScancode::G,
    ATScancode::H,
    ATScancode::I,
    ATScancode::J,
    ATScancode::K,
    ATScancode::L,
    ATScancode::M,
    ATScancode::N,
    ATScancode::O,
    ATScancode::P,
    ATScancode::Q,
    ATScancode::R,
    ATScancode::S,
    ATScancode::T,
    ATScancode::U,
    ATScancode::V,
    ATScancode::W,
    ATScancode::X,
    ATScancode::Y,
    ATScancode::Z,
    
    ATScancode::_1,
    ATScancode::_2,
    ATScancode::_3,
    ATScancode::_4,
    ATScancode::_5,
    ATScancode::_6,
    ATScancode::_7,
    ATScancode::_8,
    ATScancode::_9,
    ATScancode::_0,

    ATScancode::Return,
    ATScancode::Escape,
    ATScancode::Backspace,
    ATScancode::Tab,
    ATScancode::Space,

    ATScancode::Minus,
    ATScancode::Equals,
    ATScancode::LeftBracket,
    ATScancode::RightBracket,
    ATScancode::Backslash,
    ATScancode::Backslash, // same key
    ATScancode::Semicolon,
    ATScancode::Apostrophe,
    ATScancode::Grave,
    ATScancode::Comma,
    ATScancode::Period,
    ATScancode::Slash,

    ATScancode::CapsLock,

    ATScancode::F1,
    ATScancode::F2,
    ATScancode::F3,
    ATScancode::F4,
    ATScancode::F5,
    ATScancode::F6,
    ATScancode::F7,
    ATScancode::F8,
    ATScancode::F9,
    ATScancode::F10,
    ATScancode::F11,
    ATScancode::F12,

    ATScancode::Invalid, // PrintScreen
    ATScancode::ScrollLock,
    ATScancode::Invalid, // Pause
    ATScancode::Insert,
    
    ATScancode::Home,
    ATScancode::PageUp,
    ATScancode::Delete,
    ATScancode::End,
    ATScancode::PageDown,
    ATScancode::Right,
    ATScancode::Left,
    ATScancode::Down,
    ATScancode::Up,

    ATScancode::NumLock,

    ATScancode::KPDivide,
    ATScancode::KPMultiply,
    ATScancode::KPMinus,
    ATScancode::KPPlus,
    ATScancode::KPEnter,
    ATScancode::KP1,
    ATScancode::KP2,
    ATScancode::KP3,
    ATScancode::KP4,
    ATScancode::KP5,
    ATScancode::KP6,
    ATScancode::KP7,
    ATScancode::KP8,
    ATScancode::KP9,
    ATScancode::KP0,
    ATScancode::KPPeriod,

    ATScancode::NonUSBackslash,

    ATScancode::Application,
    ATScancode::Power,

    ATScancode::KPEquals,

    // F13-F24
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,

    // no mapping
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,
    ATScancode::Invalid,

    ATScancode::KPComma,
    ATScancode::Invalid,

    ATScancode::International1,
    ATScancode::International2,
    ATScancode::International3,
    ATScancode::International4,
    ATScancode::International5,
    ATScancode::International6,
    ATScancode::Invalid, // ...7
    ATScancode::Invalid, // ...8
    ATScancode::Invalid, // ...9
    ATScancode::Lang1,
    ATScancode::Lang2,
    ATScancode::Lang3,
    ATScancode::Lang4,
    ATScancode::Lang5,

    // ... some media keys
};

static const ATScancode mod_map[]
{
    ATScancode::LeftCtrl,
    ATScancode::LeftShift,
    ATScancode::LeftAlt,
    ATScancode::LeftGUI,
    ATScancode::RightCtrl,
    ATScancode::RightShift,
    ATScancode::RightAlt,
    ATScancode::RightGUI,
};

void update_key_state(ATScancode code, bool state);
void update_mouse_state(int8_t x, int8_t y, bool left, bool right);


static void usb_host_task(void *arg)
{
    // host init
    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .root_port_unpowered = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
        .enum_filter_cb = nullptr,
        .fifo_settings_custom = {},
        .peripheral_map = 0
    };

    ESP_ERROR_CHECK(usb_host_install(&host_config));

    // notify main task
    xTaskNotifyGive((TaskHandle_t)arg);

    // handle events
    while(true)
    {
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
    }
}

static void hid_host_interface_callback(hid_host_device_handle_t hid_device_handle, const hid_host_interface_event_t event, void *arg)
{
    hid_host_dev_params_t dev_params;
    ESP_ERROR_CHECK(hid_host_device_get_params(hid_device_handle, &dev_params));

    switch(event)
    {
        case HID_HOST_INTERFACE_EVENT_INPUT_REPORT:
        {
            uint8_t data[64] = { 0 };
            size_t data_length = 0;

            ESP_ERROR_CHECK(hid_host_device_get_raw_input_report_data(hid_device_handle, data, 64, &data_length));

            if(dev_params.proto == HID_PROTOCOL_KEYBOARD)
            {
                hid_keyboard_input_report_boot_t *keyboard_report = (hid_keyboard_input_report_boot_t *)data;

                // check for new keys down
                for(int i = 0; i < HID_KEYBOARD_KEY_MAX && keyboard_report->key[i]; i++)
                {
                    auto key = keyboard_report->key[i];
                    bool found = false;
                    for(int j = 0; j < 6 && last_keys[j] && !found; j++)
                        found = last_keys[j] == key;

                    if(found)
                        continue;

                    if(key < std::size(scancode_map) && scancode_map[key] != ATScancode::Invalid)
                        update_key_state(scancode_map[key], true);
                }

                // do the reverse and check for released keys
                for(int i = 0; i < HID_KEYBOARD_KEY_MAX && last_keys[i]; i++)
                {
                    auto key = last_keys[i];
                    bool found = false;
                    for(int j = 0; j < 6 && keyboard_report->key[j] && !found; j++)
                        found = keyboard_report->key[j] == key;

                    if(found)
                        continue;

                    if(key < std::size(scancode_map) && scancode_map[key] != ATScancode::Invalid)
                        update_key_state(scancode_map[key], false);
                }

                // ...and mods
                auto changed_mods = last_key_mod ^ keyboard_report->modifier.val;
                auto pressed_mods = changed_mods & keyboard_report->modifier.val;
                auto released_mods = changed_mods ^ pressed_mods;
                
                for(int i = 0; i < 8; i++)
                {
                    if(pressed_mods & (1 << i))
                        update_key_state(mod_map[i], true);
                    else if(released_mods & (1 << i))
                        update_key_state(mod_map[i], false);
                }

                memcpy(last_keys, keyboard_report->key, HID_KEYBOARD_KEY_MAX);
                last_key_mod = keyboard_report->modifier.val;
            }
            else if(dev_params.proto == HID_PROTOCOL_MOUSE)
            {
                hid_mouse_input_report_boot_t *mouse_report = (hid_mouse_input_report_boot_t *)data;

                update_mouse_state(mouse_report->x_displacement, mouse_report->y_displacement, mouse_report->buttons.button1, mouse_report->buttons.button2);
            }
            break;
        }

        case HID_HOST_INTERFACE_EVENT_DISCONNECTED:
            ESP_ERROR_CHECK(hid_host_device_close(hid_device_handle));
            break;
    }
}

static void hid_host_device_callback(hid_host_device_handle_t hid_device_handle, const hid_host_driver_event_t event, void *arg)
{
    hid_host_dev_params_t dev_params;
    ESP_ERROR_CHECK(hid_host_device_get_params(hid_device_handle, &dev_params));

    switch(event)
    {
        case HID_HOST_DRIVER_EVENT_CONNECTED:
        {
            // open any keyboards/mice
            if(dev_params.proto == HID_PROTOCOL_KEYBOARD || dev_params.proto == HID_PROTOCOL_MOUSE)
            {
                const hid_host_device_config_t dev_config = {
                    .callback = hid_host_interface_callback,
                    .callback_arg = NULL
                };

                ESP_ERROR_CHECK(hid_host_device_open(hid_device_handle, &dev_config));

                // set boot protocol
                if(HID_SUBCLASS_BOOT_INTERFACE == dev_params.sub_class)
                {
                    // FIXME: this is failing with ESP_ERR_INVALID_ARG
                    /*ESP_ERROR_CHECK*/(hid_class_request_set_protocol(hid_device_handle, HID_REPORT_PROTOCOL_BOOT));
                    if(HID_PROTOCOL_KEYBOARD == dev_params.proto)
                        /*ESP_ERROR_CHECK*/(hid_class_request_set_idle(hid_device_handle, 0, 0));
                }

                ESP_ERROR_CHECK(hid_host_device_start(hid_device_handle));
            }
            break;
        }
    }
}
#endif

void init_usb()
{
#ifdef USB_HOST
    xTaskCreatePinnedToCore(usb_host_task, "usb_host", 3072, xTaskGetCurrentTaskHandle(), 2, nullptr, 0);

    // wait for task
    ulTaskNotifyTake(false, portMAX_DELAY);

    // init HID driver
    const hid_host_driver_config_t hid_host_driver_config = {
        .create_background_task = true,
        .task_priority = 5,
        .stack_size = 4096,
        .core_id = 0,
        .callback = hid_host_device_callback,
        .callback_arg = NULL
    };

    ESP_ERROR_CHECK(hid_host_install(&hid_host_driver_config));
#endif
}