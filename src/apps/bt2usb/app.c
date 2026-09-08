// app.c - BT2USB App Entry Point
// Bluetooth to USB HID gamepad adapter for Pico W
//
// Uses Pico W's built-in CYW43 Bluetooth to receive controllers,
// outputs as USB HID device.

#include "app.h"
#include "core/router/router.h"
#include "core/services/players/manager.h"
#include "core/services/players/feedback.h"
#include "core/services/button/button.h"
#include "core/input_interface.h"
#include "core/output_interface.h"
#include "usb/usbd/usbd.h"
#include "bt/transport/bt_transport.h"
#include "bt/btstack/btstack_host.h"
#include "core/services/leds/leds.h"
#include "core/buttons.h"

#include "tusb.h"
#include "platform/platform.h"
#include <stdio.h>

#ifdef BTSTACK_USE_ESP32
#include "driver/gpio.h"
extern const bt_transport_t bt_transport_esp32;

/* ============================================================================
 * CUSTOM: PC POWER BUTTON + PLED SENSOR DEBUG
 *
 * PC817 #1: ESP32 presses motherboard PWR_SW
 *   GPIO4 -- 390 ohm resistor -- PC817 #1 pin 1
 *   GND ------------------------ PC817 #1 pin 2
 *   PC817 #1 pin 3/4 ---------- motherboard PWR_SW pair
 *
 * PC817 #2: motherboard PWR_LED senses PC power state
 *   PLED+ -- 390 ohm -- 390 ohm -- 390 ohm -- PC817 #2 pin 1
 *   PLED- ---------------------------------------- PC817 #2 pin 2
 *   PC817 #2 pin 4 ------------------------------- GPIO5
 *   PC817 #2 pin 3 ------------------------------- ESP32 GND
 *
 * GPIO5 uses an internal pull-up:
 *   GPIO5 LOW  = PC817 #2 on = PLED active = PC considered ON
 *   GPIO5 HIGH = PC817 #2 off = PLED inactive = PC considered OFF
 *
 * LED debugger:
 *   Yellow steady       = PC considered ON
 *   Dim red steady      = PC considered OFF
 *   Purple blink x3     = controller connected while PC considered OFF
 *   Bright red blink x3 = Guide/Xbox + A + B manual press
 *
 * Safety:
 *   Automatic PWR_SW pressing is deliberately OFF in this debug build.
 * ========================================================================== */

#define PWR_PULSE_GPIO             GPIO_NUM_4
#define PC_ON_SENSE_GPIO           GPIO_NUM_5

#define PWR_PULSE_DURATION_MS      300
#define PWR_TRIGGER_COOLDOWN_MS    3000
#define PWR_FLASH_STEP_MS          160

#define PWR_DEBUG_COMBO_MASK       (JP_BUTTON_A1 | JP_BUTTON_B1 | JP_BUTTON_B2)

typedef enum {
    PWR_FLASH_NONE = 0,
    PWR_FLASH_CONTROLLER_PURPLE,
    PWR_FLASH_MANUAL_RED,
} pwr_flash_mode_t;

static bool pwr_pulse_active = false;
static uint32_t pwr_pulse_started_ms = 0;
static uint32_t pwr_last_trigger_ms = 0;

static bool pwr_debug_combo_was_held = false;

static pwr_flash_mode_t pwr_flash_mode = PWR_FLASH_NONE;
static bool pwr_flash_led_on = false;
static uint8_t pwr_flash_count_done = 0;
static uint8_t pwr_flash_target_count = 0;
static uint32_t pwr_flash_last_ms = 0;

static void power_button_init(void)
{
    gpio_config_t pwr_cfg = {
        .pin_bit_mask = (1ULL << PWR_PULSE_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    gpio_config_t sense_cfg = {
        .pin_bit_mask = (1ULL << PC_ON_SENSE_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_ERROR_CHECK(gpio_config(&pwr_cfg));
    ESP_ERROR_CHECK(gpio_set_level(PWR_PULSE_GPIO, 0));

    ESP_ERROR_CHECK(gpio_config(&sense_cfg));
}

static bool pc_is_on(void)
{
    return gpio_get_level(PC_ON_SENSE_GPIO) == 0;
}

static void power_button_start_flash(pwr_flash_mode_t mode)
{
    pwr_flash_mode = mode;
    pwr_flash_led_on = false;
    pwr_flash_count_done = 0;
    pwr_flash_target_count = 3;
    pwr_flash_last_ms = platform_time_ms();
}

static void power_button_press(pwr_flash_mode_t flash_mode)
{
    uint32_t now = platform_time_ms();

    if (pwr_pulse_active) {
        return;
    }

    if ((now - pwr_last_trigger_ms) < PWR_TRIGGER_COOLDOWN_MS) {
        return;
    }

    gpio_set_level(PWR_PULSE_GPIO, 1);

    pwr_pulse_active = true;
    pwr_pulse_started_ms = now;
    pwr_last_trigger_ms = now;

    power_button_start_flash(flash_mode);
}

static void power_button_task(void)
{
    if (!pwr_pulse_active) {
        return;
    }

    if ((platform_time_ms() - pwr_pulse_started_ms) >=
        PWR_PULSE_DURATION_MS) {
        gpio_set_level(PWR_PULSE_GPIO, 0);
        pwr_pulse_active = false;
    }
}

static void power_button_debug_combo_task(void)
{
    uint32_t buttons = 0;

    if (playersCount > 0 && players[0].dev_addr >= 0) {
        const input_event_t* ev =
            router_get_output(OUTPUT_TARGET_USB_DEVICE, 0);

        if (ev) {
            buttons = ev->buttons;
        }
    }

    bool combo_held =
        (buttons & PWR_DEBUG_COMBO_MASK) == PWR_DEBUG_COMBO_MASK;

    if (combo_held && !pwr_debug_combo_was_held) {
        power_button_press(PWR_FLASH_MANUAL_RED);
    }

    pwr_debug_combo_was_held = combo_held;
}

static void power_button_on_controller_connection(void)
{
    /*
     * PC PLED active:
     * - PC is on or sleeping.
     * - Do not touch PWR_SW.
     * - Keep the persistent yellow state LED.
     */
    if (pc_is_on()) {
        return;
    }

    /*
     * PC PLED inactive:
     * - PC is considered off.
     * - Pulse GPIO4 through PC817 #1 for 300 ms.
     * - Show three purple flashes.
     */
    power_button_press(PWR_FLASH_CONTROLLER_PURPLE);
}

static void power_button_flash_task(void)
{
    uint32_t now = platform_time_ms();

    if (pwr_flash_mode == PWR_FLASH_NONE) {
        return;
    }

    if ((now - pwr_flash_last_ms) < PWR_FLASH_STEP_MS) {
        return;
    }

    pwr_flash_last_ms = now;
    pwr_flash_led_on = !pwr_flash_led_on;

    if (pwr_flash_led_on) {
        if (pwr_flash_mode == PWR_FLASH_CONTROLLER_PURPLE) {
            leds_set_color(120, 0, 255);
        } else {
            leds_set_color(255, 0, 0);
        }
        return;
    }

    leds_set_color(0, 0, 0);
    pwr_flash_count_done++;

    if (pwr_flash_count_done >= pwr_flash_target_count) {
        pwr_flash_mode = PWR_FLASH_NONE;
    }
}

static void power_button_state_led_task(void)
{
    /*
     * Do not overwrite a temporary flash sequence.
     * Persistent colour represents current PLED sensor state.
     */
    if (pwr_flash_mode != PWR_FLASH_NONE) {
        return;
    }

    if (pc_is_on()) {
        leds_set_color(255, 180, 0);  // Yellow: PLED sensed active.
    } else {
        leds_set_color(20, 0, 0);     // Very dim red: PLED sensed inactive.
    }
}

/* End custom PC power-button + sensor debug block. */

// Status LED GPIO — board-specific defaults
// Feather ESP32-S3: GPIO 13 (red LED, active high). GPIO 21 is NeoPixel power!
// Seeed XIAO ESP32-S3: GPIO 21 (active low)
#ifndef STATUS_LED_GPIO
  #ifdef BOARD_FEATHER_ESP32S3
    #define STATUS_LED_GPIO 13
  #else
    #define STATUS_LED_GPIO 21
  #endif
#endif

#ifndef STATUS_LED_ACTIVE_LOW
  #ifdef BOARD_FEATHER_ESP32S3
    #define STATUS_LED_ACTIVE_LOW 0
  #else
    #define STATUS_LED_ACTIVE_LOW 1
  #endif
#endif

#elif defined(BTSTACK_USE_NRF)
extern const bt_transport_t bt_transport_nrf;
// nRF: LED status handled by ws2812_nrf.c (RGB LEDs driven via neopixel API)
#ifdef OLED_I2C_DISPLAY
#include "core/services/display/display.h"
#include "core/input_event.h"
#include "core/buttons.h"
#endif
#else
#include "pico/cyw43_arch.h"
extern const bt_transport_t bt_transport_cyw43;
#endif

// ============================================================================
// USB BUS SUSPEND / RESUME
// ============================================================================
// When the USB host (e.g. PS3) enters sleep, the bus suspends (no SOF >3 ms).
// We drop the active BT link so the bridged controller (e.g. DS4) auto-sleeps
// instead of staying powered forever — the PS3 keeps VBUS hot during sleep so
// nothing else would tell the controller to power down. On resume, the existing
// scan loop reconnects when the controller advertises again.

static volatile bool usb_bus_suspended = false;
static bool          usb_bus_suspended_seen = false;   // mirror for edge detect

void tud_suspend_cb(bool remote_wakeup_en)
{
    (void)remote_wakeup_en;
    usb_bus_suspended = true;
}

void tud_resume_cb(void)
{
    usb_bus_suspended = false;
}

// Called from app_task; performs the actual BT disconnect on a suspend edge so
// btstack APIs run from the main-loop context they expect (not the USB IRQ).
static void usb_suspend_check(void)
{
    bool now = usb_bus_suspended;
    if (now == usb_bus_suspended_seen) return;
    usb_bus_suspended_seen = now;
    if (now) {
        printf("[app:bt2usb] USB bus suspended -> disconnecting BT to let controller sleep\n");
        btstack_host_disconnect_all_devices();
    } else {
        printf("[app:bt2usb] USB bus resumed -> scan/reconnect will pick the controller back up when it advertises\n");
    }
}

// PS3 "Turn off controller" override. The PS3 fires this when the user
// selects Settings -> Accessory Settings -> Turn off controller while we
// are in PS3 output mode. The wired DS3 surface can't actually disappear
// (we stay plugged in), so propagate the intent to the BT side -- a real
// DS4/DS3 bridged over Bluetooth loses its host and auto-sleeps within a
// minute. User presses the controller's PS button to wake + re-pair.
void app_on_console_shutdown(void)
{
    printf("[app:bt2usb] Console shutdown -> disconnecting bridged BT controller\n");
    btstack_host_disconnect_all_devices();
}

// ============================================================================
// LED STATUS
// ============================================================================

static uint32_t led_last_toggle = 0;
static bool led_state = false;

// Update LED based on connection status
// - Blink (0.8s): No device connected (scanning, connecting, or idle)
// - Solid on: Device connected
static void platform_led_set(bool on)
{
#ifdef BTSTACK_USE_ESP32
    gpio_set_level(STATUS_LED_GPIO, (on ^ STATUS_LED_ACTIVE_LOW) ? 1 : 0);
#elif defined(BTSTACK_USE_NRF)
    // No-op: RGB LEDs driven by ws2812_nrf.c via neopixel API
    (void)on;
#else
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, on ? 1 : 0);
#endif
}

static void led_status_update(void)
{
    uint32_t now = platform_time_ms();

    // Actively scanning (button press / pairing window): FAST blink so the
    // press visibly took — even while other devices stay connected.
    if (btstack_host_is_scanning()) {
        if (now - led_last_toggle >= 110) {
            led_state = !led_state;
            platform_led_set(led_state);
            led_last_toggle = now;
        }
        return;
    }

    if (btstack_classic_get_connection_count() > 0) {
        // Device connected - solid on
        if (!led_state) {
            platform_led_set(true);
            led_state = true;
        }
    } else {
        // No device connected - blink
        if (now - led_last_toggle >= 400) {
            led_state = !led_state;
            platform_led_set(led_state);
            led_last_toggle = now;
        }
    }
}

static void power_button_pc_shutdown_task(void)
{
    static bool previous_pc_on = false;
    static bool initial_state_seen = false;

    bool pc_on = pc_is_on();

    /*
     * Do nothing on initial firmware boot. This prevents an immediate
     * disconnect if the ESP itself starts while the PC is already off.
     */
    if (!initial_state_seen) {
        previous_pc_on = pc_on;
        initial_state_seen = true;
        return;
    }

    /*
     * Detect only a real PC-on -> PC-off transition:
     * PLED was active, then went inactive.
     */
    if (previous_pc_on && !pc_on) {
        btstack_host_disconnect_all_devices();
    }

    previous_pc_on = pc_on;
}

// ============================================================================
// BUTTON EVENT HANDLER
// ============================================================================

static void on_button_event(button_event_t event)
{
    switch (event) {
        case BUTTON_EVENT_CLICK:
            // Start/extend 60-second BT scan for additional devices
            printf("[app:bt2usb] Starting BT scan (60s)...\n");
            btstack_host_start_timed_scan(60000);
            break;

        case BUTTON_EVENT_DOUBLE_CLICK: {
            // Double-click to cycle USB output mode
            printf("[app:bt2usb] Double-click - switching USB output mode...\n");
            tud_task_ext(1, false);
            platform_sleep_ms(50);
            tud_task_ext(1, false);

            usb_output_mode_t next = usbd_get_next_mode();
            printf("[app:bt2usb] Switching to %s\n", usbd_get_mode_name(next));
            usbd_set_mode(next);
            break;
        }

        case BUTTON_EVENT_TRIPLE_CLICK:
            // Triple-click to reset to default HID mode
            printf("[app:bt2usb] Triple-click - resetting to HID mode...\n");
            if (!usbd_reset_to_hid()) {
                printf("[app:bt2usb] Already in HID mode\n");
            }
            break;

        case BUTTON_EVENT_HOLD:
            // Long press to disconnect all devices and clear all bonds
            printf("[app:bt2usb] Disconnecting all devices and clearing bonds...\n");
            btstack_host_disconnect_all_devices();
            btstack_host_delete_all_bonds();
            break;

        default:
            break;
    }
}

// ============================================================================
// APP INPUT INTERFACES
// ============================================================================

// BT2USB has no InputInterface - BT transport handles input internally
// via bthid drivers that call router_submit_input()

const InputInterface** app_get_input_interfaces(uint8_t* count)
{
    *count = 0;
    return NULL;
}

// ============================================================================
// APP OUTPUT INTERFACES
// ============================================================================

static const OutputInterface* output_interfaces[] = {
    &usbd_output_interface,
};

const OutputInterface** app_get_output_interfaces(uint8_t* count)
{
    *count = sizeof(output_interfaces) / sizeof(output_interfaces[0]);
    return output_interfaces;
}

// ============================================================================
// OLED DISPLAY (XIAO Expansion Board - SSD1306 128x64 I2C)
// ============================================================================

#ifdef OLED_I2C_DISPLAY

// Arrow characters for display (1=up, 2=down, 3=left, 4=right)
#define ARROW_UP    "\x01"
#define ARROW_DOWN  "\x02"
#define ARROW_LEFT  "\x03"
#define ARROW_RIGHT "\x04"

typedef struct {
    uint32_t mask;
    const char* name;
} button_name_t;

static const button_name_t button_names[] = {
    { JP_BUTTON_DU, ARROW_UP },
    { JP_BUTTON_DR, ARROW_RIGHT },
    { JP_BUTTON_DD, ARROW_DOWN },
    { JP_BUTTON_DL, ARROW_LEFT },
    { JP_BUTTON_B1, "B1" },
    { JP_BUTTON_B2, "B2" },
    { JP_BUTTON_B3, "B3" },
    { JP_BUTTON_B4, "B4" },
    { JP_BUTTON_L1, "L1" },
    { JP_BUTTON_R1, "R1" },
    { JP_BUTTON_L2, "L2" },
    { JP_BUTTON_R2, "R2" },
    { JP_BUTTON_S1, "S1" },
    { JP_BUTTON_S2, "S2" },
    { JP_BUTTON_L3, "L3" },
    { JP_BUTTON_R3, "R3" },
    { JP_BUTTON_A1, "A1" },
    { 0, NULL }
};

static const char* transport_str(input_transport_t t) {
    switch (t) {
        case INPUT_TRANSPORT_BT_CLASSIC: return "BT";
        case INPUT_TRANSPORT_BT_BLE:     return "BLE";
        default:                         return "?";
    }
}

static void oled_init(void) {
    display_i2c_config_t cfg = {
        .i2c_inst = 0,
        .pin_sda  = 0,
        .pin_scl  = 0,
        .addr     = 0x3C,
    };
#ifdef BOARD_FEATHER_NRF52840
    display_init_i2c(&cfg);
    printf("[app:bt2usb] OLED display initialized (SH1107 I2C)\n");
#else
    display_init_ssd1306_i2c(&cfg);
    printf("[app:bt2usb] OLED display initialized (SSD1306 XIAO Expansion Board)\n");
#endif
}

static input_event_t oled_cached_event;
static bool oled_has_event = false;

static void oled_update_display(void) {
    static uint32_t last_update = 0;
    static uint32_t last_buttons = 0;
    uint32_t now = platform_time_ms();

    if (playersCount > 0 && players[0].dev_addr >= 0) {
        const input_event_t* ev =
            router_get_output(OUTPUT_TARGET_USB_DEVICE, 0);
        if (ev) {
            oled_cached_event = *ev;
            oled_has_event = true;
        }
    }

    uint32_t buttons = oled_has_event ? oled_cached_event.buttons : 0;
    uint32_t newly_pressed = ~last_buttons & buttons;
    last_buttons = buttons;
    for (int i = 0; button_names[i].name != NULL; i++) {
        if (newly_pressed & button_names[i].mask) {
            display_marquee_add(button_names[i].name);
        }
    }

    if (now - last_update < 50) return;
    last_update = now;

    display_clear();

    usb_output_mode_t mode = usbd_get_mode();
    display_text_large(0, 0, usbd_get_mode_name(mode));

    display_hline(0, 17, DISPLAY_WIDTH);

    if (playersCount > 0 && players[0].dev_addr >= 0) {
        const char* name = get_player_name(0);
        if (name) {
            display_text(0, 20, name);
        }

        char info[22];
        snprintf(info, sizeof(info), "%s dev:%d P%d/%d",
                 transport_str(players[0].transport),
                 players[0].dev_addr,
                 players[0].player_number, playersCount);
        display_text(0, 30, info);

        if (oled_has_event) {
            char line[22];
            snprintf(line, sizeof(line), "L:%02X,%02X R:%02X,%02X T:%02X,%02X",
                     oled_cached_event.analog[ANALOG_LX],
                     oled_cached_event.analog[ANALOG_LY],
                     oled_cached_event.analog[ANALOG_RX],
                     oled_cached_event.analog[ANALOG_RY],
                     oled_cached_event.analog[ANALOG_L2],
                     oled_cached_event.analog[ANALOG_R2]);
            display_text(0, 40, line);
        }
    } else {
        display_text(0, 28, "No controller");
    }

    display_marquee_tick();
    display_marquee_render(52);

    display_update();
}

#endif // OLED_I2C_DISPLAY

// ============================================================================
// APP INITIALIZATION
// ============================================================================

void app_init(void)
{
    printf("[app:bt2usb] Initializing BT2USB v%s\n", JOYPAD_VERSION);

#ifdef BTSTACK_USE_ESP32
    printf("[app:bt2usb] ESP32-S3 BLE -> USB HID\n");

    gpio_config_t led_cfg = {
        .pin_bit_mask = (1ULL << STATUS_LED_GPIO),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&led_cfg);
    gpio_set_level(STATUS_LED_GPIO, STATUS_LED_ACTIVE_LOW ? 1 : 0);

    power_button_init();

#elif defined(BTSTACK_USE_NRF)
#ifdef BOARD_FEATHER_NRF52840
    printf("[app:bt2usb] Adafruit Feather nRF52840 Express BLE -> USB HID\n");
#else
    printf("[app:bt2usb] Seeed XIAO nRF52840 BLE -> USB HID\n");
#endif
#ifdef OLED_I2C_DISPLAY
    oled_init();
#endif
#else
    printf("[app:bt2usb] Pico W built-in Bluetooth -> USB HID\n");
#endif

    button_init();
    button_set_callback(on_button_event);

    router_config_t router_cfg = {
        .mode = ROUTING_MODE,
        .merge_mode = MERGE_MODE,
        .max_players_per_output = {
            [OUTPUT_TARGET_USB_DEVICE] = USB_OUTPUT_PORTS,
        },
        .merge_all_inputs = true,
        .transform_flags = TRANSFORM_FLAGS,
    };
    router_init(&router_cfg);

    router_add_route(INPUT_SOURCE_BLE_CENTRAL, OUTPUT_TARGET_USB_DEVICE, 0);

    player_config_t player_cfg = {
        .slot_mode = PLAYER_SLOT_MODE,
        .max_slots = MAX_PLAYER_SLOTS,
        .auto_assign_on_press = AUTO_ASSIGN_ON_PRESS,
    };
    players_init_with_config(&player_cfg);

    printf("[app:bt2usb] Initializing Bluetooth...\n");
#ifdef BTSTACK_USE_ESP32
    bt_init(&bt_transport_esp32);
#elif defined(BTSTACK_USE_NRF)
    bt_init(&bt_transport_nrf);
#else
    bt_init(&bt_transport_cyw43);
#endif

    printf("[app:bt2usb] Initialization complete\n");
    printf("[app:bt2usb]   Routing: Bluetooth -> USB Device (HID Gamepad)\n");
    printf("[app:bt2usb]   Player slots: %d\n", MAX_PLAYER_SLOTS);
    printf("[app:bt2usb]   Click BOOTSEL for 60s BT scan\n");
    printf("[app:bt2usb]   Hold BOOTSEL to disconnect all + clear bonds\n");
    printf("[app:bt2usb]   Double-click BOOTSEL to switch USB mode\n");
}

// ============================================================================
// APP TASK (Called from main loop)
// ============================================================================

void app_task(void)
{
    usb_suspend_check();

    button_task();

    static usb_output_mode_t last_led_mode = USB_OUTPUT_MODE_COUNT;
    usb_output_mode_t mode = usbd_get_mode();
    if (mode != last_led_mode) {
        uint8_t r, g, b;
        usbd_get_mode_color(mode, &r, &g, &b);
        leds_set_color(r, g, b);
        last_led_mode = mode;
    }

    bt_task();

    leds_set_connected_devices(btstack_classic_get_connection_count());
    led_status_update();

    if (usbd_output_interface.get_feedback) {
        output_feedback_t fb;
        if (usbd_output_interface.get_feedback(&fb)) {
            for (int i = 0; i < playersCount; i++) {
                feedback_set_rumble(i, fb.rumble_left, fb.rumble_right);
                if (fb.led_player > 0) {
                    feedback_set_led_player(i, fb.led_player);
                }
                if (fb.led_r || fb.led_g || fb.led_b) {
                    feedback_set_led_rgb(i, fb.led_r, fb.led_g, fb.led_b);
                }
            }
        }
    }

#ifdef BTSTACK_USE_ESP32
    /*
     * Detect controller connection edge for purple debug feedback.
     * This diagnostic build does NOT automatically pulse GPIO4 here.
     */
    static int last_bt_connection_count = 0;
    int bt_connection_count = btstack_classic_get_connection_count();

    if (bt_connection_count > 0 && last_bt_connection_count == 0) {
        power_button_on_controller_connection();
    }

    last_bt_connection_count = bt_connection_count;

    power_button_debug_combo_task();
    power_button_task();
    power_button_flash_task();
    power_button_state_led_task();
    power_button_pc_shutdown_task();
#endif

#ifdef OLED_I2C_DISPLAY
    oled_update_display();
#endif
}