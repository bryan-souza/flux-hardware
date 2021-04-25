#include <stdio.h>
#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>

#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_event.h>
#include <nvs_flash.h>

// Wi-Fi related
#include <wifi_provisioning/manager.h>
#include <wifi_provisioning/scheme_ble.h>

// Display related
#include <driver/gpio.h>
#include <driver/spi_master.h>
#include <u8g2.h>
#include <sdkconfig.h>
#include <u8g2_esp32_hal.h>

// Sensor counter
#include <freertos/queue.h>
#include <freertos/portmacro.h>
#include <driver/periph_ctrl.h>
#include <driver/gpio.h>
#include <driver/pcnt.h>

// Display pins
#define PIN_SDA 21
#define PIN_SCL 22

// Counter
#define PCNT_TEST_UNIT      PCNT_UNIT_0
#define PCNT_H_LIM_VAL      256
#define PCNT_L_LIM_VAL      0
#define PCNT_INPUT_SIG_IO   4 // GPIO PORT 4
#define PCNT_INPUT_CTRL_IO  5 // CONTROL GPIO HIGH => CNTR++, LOW => CNTR--

xQueueHandle pcnt_evt_queue; // Queue to handle pulse counter events
pcnt_isr_handle_t user_isr_handle = NULL;

typedef struct {
    int unit;
    uint32_t status;
} pcnt_evt_t;

static const char *TAG = "app";

/* Signal Wi-Fi events on this event-group */
const int WIFI_CONNECTED_EVENT = BIT0;
static EventGroupHandle_t wifi_event_group;

static void pcnt_init(void)
{
    pcnt_config_t pcnt_config = {
        // Set GPIO input and control ports
        .pulse_gpio_num = PCNT_INPUT_SIG_IO,
        .ctrl_gpio_num = PCNT_INPUT_CTRL_IO,
        .channel = PCNT_CHANNEL_0,
        .unit = PCNT_TEST_UNIT,
        // What to do on the positive / negative edge of pulse input?
        .pos_mode = PCNT_COUNT_INC, // Increase the counter
        .neg_mode = PCNT_COUNT_DIS, // Keep the counter as is
        // What to do when control input is low / high?
        .lctrl_mode = PCNT_MODE_REVERSE, // Reverse counting direction ( ++ => --)
        .hctrl_mode = PCNT_MODE_KEEP, // Keep the counting mode as is
        // Set the min / max values to watch
        .counter_h_lim = PCNT_H_LIM_VAL,
        .counter_l_lim = PCNT_L_LIM_VAL
    };

    // Initialize PCNT unit
    pcnt_unit_config(&pcnt_config);

    // Initialize PCNT counter
    pcnt_counter_pause(PCNT_TEST_UNIT);
    pcnt_counter_clear(PCNT_TEST_UNIT);
    
    // Start counting
    pcnt_counter_resume(PCNT_TEST_UNIT);
}

/* Event handler for catching system events */
static void event_handler(void* arg, esp_event_base_t event_base,
                          int event_id, void* event_data)
{
    if (event_base == WIFI_PROV_EVENT) {
        switch (event_id) {
            case WIFI_PROV_START:
                ESP_LOGI(TAG, "Provisioning started");
                break;
            case WIFI_PROV_CRED_RECV: {
                wifi_sta_config_t *wifi_sta_cfg = (wifi_sta_config_t *)event_data;
                ESP_LOGI(TAG, "Received Wi-Fi credentials"
                         "\n\tSSID     : %s\n\tPassword : %s",
                         (const char *) wifi_sta_cfg->ssid,
                         (const char *) wifi_sta_cfg->password);
                break;
            }
            case WIFI_PROV_CRED_FAIL: {
                wifi_prov_sta_fail_reason_t *reason = (wifi_prov_sta_fail_reason_t *)event_data;
                ESP_LOGE(TAG, "Provisioning failed!\n\tReason : %s"
                         "\n\tPlease reset to factory and retry provisioning",
                         (*reason == WIFI_PROV_STA_AUTH_ERROR) ?
                         "Wi-Fi station authentication failed" : "Wi-Fi access-point not found");
                break;
            }
            case WIFI_PROV_CRED_SUCCESS:
                ESP_LOGI(TAG, "Provisioning successful");
                break;
            case WIFI_PROV_END:
                /* De-initialize manager once provisioning is finished */
                wifi_prov_mgr_deinit();
                break;
            default:
                break;
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Connected with IP Address:" IPSTR, IP2STR(&event->ip_info.ip));
        /* Signal main application to continue execution */
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_EVENT);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "Disconnected. Connecting to the AP again...");
        esp_wifi_connect();
    }
}

static void wifi_init_sta(void)
{
    /* Start Wi-Fi in station mode */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
}

static void get_device_service_name(char *service_name, size_t max)
{
    uint8_t eth_mac[6];
    const char *ssid_prefix = "PROV_";
    esp_wifi_get_mac(WIFI_IF_STA, eth_mac);
    snprintf(service_name, max, "%s%02X%02X%02X",
             ssid_prefix, eth_mac[3], eth_mac[4], eth_mac[5]);
}

u8g2_t display_init(void) 
{
    /* Initialize the display */
    u8g2_esp32_hal_t u8g2_esp32_hal = U8G2_ESP32_HAL_DEFAULT;
	u8g2_esp32_hal.sda   = PIN_SDA;
	u8g2_esp32_hal.scl  = PIN_SCL;
	u8g2_esp32_hal_init(u8g2_esp32_hal);

	u8g2_t display; // a structure which will contain all the data for one display
	u8g2_Setup_ssd1306_i2c_128x64_vcomh0_f(
		&display,
		U8G2_R0,
		u8g2_esp32_i2c_byte_cb,
		u8g2_esp32_gpio_and_delay_cb);  // init u8g2 structure
	u8x8_SetI2CAddress(&display.u8x8,0x78);

	u8g2_InitDisplay(&display); // send init sequence to the display, display is in sleep mode after this,
	u8g2_SetPowerSave(&display, 0); // wake up display
	u8g2_ClearBuffer(&display); // Clear command buffer
    u8g2_SetFont(&display, u8g2_font_6x12_mf);
    u8g2_DrawUTF8(&display, 0, 10, "Volume    :    0 L");
    u8g2_DrawUTF8(&display, 0, 20, "Vazão     :    0 L/min");
    u8g2_DrawBox(&display, 0, 30, 63, 5);
    u8g2_DrawUTF8(&display, 0, 35, "Vol. Total:    0 m³");
    u8g2_SendBuffer(&display); // Clear screen
    
    return display;
}

static void update_values(u8g2_t display, float flux, float vol, float total_vol)
{       
    char buffer[128];
    snprintf(buffer, 128, "Volume    : %.4f", vol);

    // Update visor values
    u8g2_ClearBuffer(&display); // Clear command buffer
    u8g2_DrawUTF8(&display, 0, 10, buffer);

    snprintf(buffer, 128, "Vazão     : %.4f", flux);
    u8g2_DrawUTF8(&display, 0, 20, buffer);
    
    // u8g2_DrawBox(&display, 0, 30, 63, 5);

    snprintf(buffer, 128, "Vol. Total: %.4f", total_vol);
    u8g2_DrawUTF8(&display, 0, 35, buffer);
    u8g2_SendBuffer(&display); // Clear screen
}


void app_main(void)
{
    /* Initialize NVS partition */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* NVS partition was truncated
         * and needs to be erased */
        ESP_ERROR_CHECK(nvs_flash_erase());

        /* Retry nvs_flash_init */
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    /* Uncomment this section to wipe
       Wi-Fi last used credentials*/
    // ESP_ERROR_CHECK(nvs_flash_erase());
    // ESP_ERROR_CHECK(nvs_flash_init());

    /* Initialize TCP/IP */
    ESP_ERROR_CHECK(esp_netif_init());

    /* Initialize the event loop */
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_event_group = xEventGroupCreate();

    /* Register our event handler for Wi-Fi, IP and Provisioning related events */
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

    /* Initialize Wi-Fi including netif with default config */
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* Configuration for the provisioning manager */
    wifi_prov_mgr_config_t config = {
        .scheme = wifi_prov_scheme_ble,
        .scheme_event_handler = WIFI_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM
    };

    /* Initialize provisioning manager with the
     * configuration parameters set above */
    ESP_ERROR_CHECK(wifi_prov_mgr_init(config));

    bool provisioned = false;
    /* Let's find out if the device is provisioned */
    ESP_ERROR_CHECK(wifi_prov_mgr_is_provisioned(&provisioned));

    /* If device is not yet provisioned start provisioning service */
    if (!provisioned) {
        ESP_LOGI(TAG, "Starting provisioning");

        /* What is the Device Service Name that we want
         * This translates to :
         *     - Wi-Fi SSID when scheme is wifi_prov_scheme_softap
         *     - device name when scheme is wifi_prov_scheme_ble
         */
        char service_name[12];
        get_device_service_name(service_name, sizeof(service_name));

        /* What is the security level that we want (0 or 1):
         *      - WIFI_PROV_SECURITY_0 is simply plain text communication.
         *      - WIFI_PROV_SECURITY_1 is secure communication which consists of secure handshake
         *          using X25519 key exchange and proof of possession (pop) and AES-CTR
         *          for encryption/decryption of messages.
         */
        wifi_prov_security_t security = WIFI_PROV_SECURITY_1;

        /* Do we want a proof-of-possession (ignored if Security 0 is selected):
         *      - this should be a string with length > 0
         *      - NULL if not used
         */
        const char *pop = "abcd1234";

        /* What is the service key (could be NULL)
         * This translates to :
         *     - Wi-Fi password when scheme is wifi_prov_scheme_softap
         *     - simply ignored when scheme is wifi_prov_scheme_ble
         */
        const char *service_key = NULL;

        uint8_t custom_service_uuid[] = {
            /* LSB <---------------------------------------
             * ---------------------------------------> MSB */
            0xb4, 0xdf, 0x5a, 0x1c, 0x3f, 0x6b, 0xf4, 0xbf,
            0xea, 0x4a, 0x82, 0x03, 0x04, 0x90, 0x1a, 0x02,
        };
        wifi_prov_scheme_ble_set_service_uuid(custom_service_uuid);
        
        /* Start provisioning service */
        ESP_ERROR_CHECK(wifi_prov_mgr_start_provisioning(security, pop, service_name, service_key));

    } else {
        ESP_LOGI(TAG, "Already provisioned, starting Wi-Fi STA");

        /* We don't need the manager as device is already provisioned,
         * so let's release it's resources */
        wifi_prov_mgr_deinit();

        /* Start Wi-Fi station */
        wifi_init_sta();
    }

    /* Wait for Wi-Fi connection */
    xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_EVENT, false, true, portMAX_DELAY);

    // Display
    u8g2_t display = display_init();    

    // PCNT event queue
    pcnt_evt_queue = xQueueCreate(10, sizeof(pcnt_evt_t));
    pcnt_init();

    int16_t count = 0;
    float total_vol = 0;
    pcnt_evt_t evt;
    portBASE_TYPE res;

    while(1)
    {
        // Wait for event information
        res = xQueueReceive(pcnt_evt_queue, &evt, 1000 / portTICK_PERIOD_MS);
        pcnt_get_counter_value(PCNT_TEST_UNIT, &count);
        printf("Current counter value: %d\n", count);

        float flux = (count / 4.5); // Vazão em L/min
        // float flux = (count / 4.5) * 60000; // Vazão em m³/s
        float vol = flux / 60; // Volume do ultimo segundo em L
        total_vol += vol;

        update_values(display, flux, vol, total_vol);

        pcnt_counter_clear(PCNT_TEST_UNIT); // Reset the counter
    }

}