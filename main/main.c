#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_sntp.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "driver/spi_master.h"
#include "driver/mcpwm.h"
#include "soc/mcpwm_periph.h"

#define WIFI_SSID      	"{your WI-FI's SSID}"
#define WIFI_PASS      	"{your WI-FI's password}"
#define MAXIMUM_RETRY  	10
#define TIMEZONE 	  	"{your timezone}"

//TAG used for debug printouts
static const char *TAG = "Nixie Clock";

//wifi intialization stuff (same as in the basic station demo)
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
static int s_retry_num = 0;
static EventGroupHandle_t s_wifi_event_group;

//Nixie connections
#define NIXIEPINS_CLK 	23
#define NIXIEPINS_PWM 	22
#define NIXIEPINS_MOSI 	21

//initialization handler prototypes
void NIXIE_init();
void WIFI_init();
void SNTP_init();

//Nixie display function prototypes
void NIXIE_writeSR(uint32_t data, spi_device_handle_t devHandle);
void NIXIE_showNumber(BaseType_t hours, BaseType_t minutes, uint32_t dots, spi_device_handle_t devHandle);

//Nixie number to IO lookup tables
#define NIXIE_DOT_TOP		0x4000
#define NIXIE_DOT_BOTTOM	0x8000
#define NIXIE_DOT_BOTH		0xc000
#define NIXIE_DOT_NONE		0x0000
uint32_t NIXIE_NL_h10[] = {0x8, 0x4, 0x2};
uint32_t NIXIE_NL_h1[] 	= {0x200, 0x80, 0x40, 0x20, 0x10, 0x100, 0x2000, 0x1000, 0x800, 0x400};
uint32_t NIXIE_NL_m10[] = {0x200000, 0x100000, 0x80000, 0x40000, 0x20000, 0x10000};
uint32_t NIXIE_NL_m1[] 	= {0x80000000, 0x20000000, 0x10000000, 0x4000000, 0x1000000, 0x800000, 0x400000, 0x2000000, 0x8000000, 0x40000000};

void app_main(void){
    //Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    //Initialize app
    NIXIE_init();
    WIFI_init();
    SNTP_init();
}

void clockTask(void * params){
	//the NIXIE_init() passes us the spi handle through the params pointer.
	//We need to cast it back to "spi_device_handle_t" to use it
	spi_device_handle_t spi = *((spi_device_handle_t *)params);

	//create the variables that will hold the time
	time_t now;
	struct tm timeinfo;

	while(1){
		//get the current time
		time(&now);
		localtime_r(&now, &timeinfo);

		//show the number on the display
		NIXIE_showNumber(timeinfo.tm_hour, timeinfo.tm_min, (timeinfo.tm_sec & 1) ? NIXIE_DOT_BOTH : NIXIE_DOT_NONE, spi);

		//wait for 100ms until updating the time again
		vTaskDelay(100 / portTICK_PERIOD_MS);
	}
}

void NIXIE_init(){
	//Initialize the SPI bus
	spi_device_handle_t spi;
	spi_bus_config_t buscfg={
		.mosi_io_num	=	NIXIEPINS_MOSI,
		.sclk_io_num	=	NIXIEPINS_CLK,
		.miso_io_num 	= 	-1,
		.quadwp_io_num	=	-1,
		.quadhd_io_num	=	-1,
		.max_transfer_sz=	4
	};
	spi_device_interface_config_t devcfg={
		.clock_speed_hz	=	1000000,
		.mode			=	2,	//we need mode 2 (inverted clock) since the clock gets inverted by the level shifter circuit
		.command_bits 	= 	0,
		.address_bits 	= 	0,
		.spics_io_num	=	-1,
		.queue_size		=	1,
	};
	esp_err_t ret = spi_bus_initialize(HSPI_HOST, &buscfg, 0);
	ESP_ERROR_CHECK(ret);
	ret=spi_bus_add_device(HSPI_HOST, &devcfg, &spi);
	ESP_ERROR_CHECK(ret);

	//Initialize the PWM module that will dim the display, the frequency can't be above 1kHz because the tubes would'n light up uniformly
    mcpwm_gpio_init(MCPWM_UNIT_0, MCPWM0B, NIXIEPINS_PWM);
    mcpwm_config_t pwm_config = {.frequency = 440, .cmpr_b = 0, .counter_mode = MCPWM_UP_COUNTER, .duty_mode = MCPWM_DUTY_MODE_0};
    mcpwm_init(MCPWM_UNIT_0, MCPWM_TIMER_0, &pwm_config);
    mcpwm_set_duty(MCPWM_UNIT_0, MCPWM_TIMER_0, MCPWM_OPR_B, 75.0);

    //Start the task that will update the display
    xTaskCreate(clockTask, "clock Task", 2048, &spi, 10, NULL);
}

void NIXIE_showNumber(BaseType_t hours, BaseType_t minutes, uint32_t dots, spi_device_handle_t devHandle){
	//calculate the individual numbers that need to be displayed
	BaseType_t h10 = hours / 10;	BaseType_t h1 = hours % 10;
	BaseType_t m10 = minutes / 10;	BaseType_t m1 = minutes % 10;

	//generate the data to write to the SR by or'ing the data with the bit-mask of the corresponding number
	uint32_t srData = 0;
	if(h10 < 3) 	srData |= NIXIE_NL_h10[h10];
	if(h1 < 10) 	srData |= NIXIE_NL_h1[h1];
	if(m10 < 6) 	srData |= NIXIE_NL_m10[m10];
	if(m1 < 10) 	srData |= NIXIE_NL_m1[m1];

	//or the value with the settings for the dots
	srData |= dots;

	//send the data to the display
	NIXIE_writeSR(~srData, devHandle);
}

void NIXIE_writeSR(uint32_t data, spi_device_handle_t devHandle){
	//swap the byte order to match what the shift register expects
	data = __builtin_bswap32(data);

	//prepare the transaction
    spi_transaction_t transaction = {.length = 32, .tx_buffer=&data};

    //send the data
	spi_device_polling_transmit(devHandle, &transaction);
}

//Wifi init code from the station demo
static void event_handler(void* arg, esp_event_base_t event_base,int32_t event_id, void* event_data){
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG,"connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void WIFI_init(void){
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            /* Setting a password implies station will connect to all security modes including WEP/WPA.
             * However these modes are deprecated and not advisable to be used. Incase your Access point
             * doesn't support WPA2, these mode can be enabled by commenting below line */
	     .threshold.authmode = WIFI_AUTH_WPA2_PSK,

            .pmf_cfg = {
                .capable = true,
                .required = false
            },
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start() );

    ESP_LOGI(TAG, "wifi_init_sta finished.");

    /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
     * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
     * happened. */
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to ap SSID:%s password:%s", WIFI_SSID, WIFI_PASS);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s, password:%s", WIFI_SSID, WIFI_PASS);
    } else {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }

    /* The event will not be processed after unregister */
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip));
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id));
    vEventGroupDelete(s_wifi_event_group);
}

//NTP synchronisation callback. Just print that we got the newest time
void SNTP_notify(struct timeval *tv){
	ESP_LOGI(TAG, "Time was synchronized successfully!");
}

void SNTP_init(void){
    ESP_LOGI(TAG, "Initializing SNTP");
	setenv("TZ", TIMEZONE, 1);
	tzset();
    sntp_set_time_sync_notification_cb(SNTP_notify);
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "pool.ntp.org");
    sntp_init();
}
