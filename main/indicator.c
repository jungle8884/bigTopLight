/**
 * indicator.c — 指示灯控制模块实现
 *
 * 5 个 LED 通过 GPIO 直接驱动 (低电平有效或高电平有效, 视硬件设计)
 */
#include "indicator.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "INDIC";

/* 指示灯 GPIO 引脚表 */
static const int led_gpios[] = {
    GPIO_LED_POWER, GPIO_LED_UPPER, GPIO_LED_LOWER,
    GPIO_LED_READING, GPIO_LED_NIGHT
};

void indicator_init(void)
{
    uint64_t pin_mask = 0;
    for (int i = 0; i < 5; i++) {
        pin_mask |= (1ULL << led_gpios[i]);
    }

    gpio_config_t io_conf = {
        .pin_bit_mask  = pin_mask,
        .mode          = GPIO_MODE_OUTPUT,
        .pull_up_en    = GPIO_PULLUP_DISABLE,
        .pull_down_en  = GPIO_PULLDOWN_DISABLE,
        .intr_type     = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    /* 初始全部熄灭 */
    for (int i = 0; i < 5; i++) {
        gpio_set_level(led_gpios[i], 0);
    }
}

void indicator_power_on_sequence(void)
{
    /* 上电时所有指示灯亮 1 秒后熄灭 */
    for (int i = 0; i < 5; i++) {
        gpio_set_level(led_gpios[i], 1);
    }
    vTaskDelay(pdMS_TO_TICKS(POWER_ON_INDICATOR_MS));
    for (int i = 0; i < 5; i++) {
        gpio_set_level(led_gpios[i], 0);
    }
    ESP_LOGI(TAG, "power-on indicator sequence done");
}

void indicator_update(system_state_t state, const lamp_status_t *status)
{
    /* 开关键指示灯: 系统非 OFF 时亮 */
    gpio_set_level(GPIO_LED_POWER,   state != STATE_OFF);
    /* 上灯指示灯: 上灯亮时亮 */
    gpio_set_level(GPIO_LED_UPPER,   status->switch_upper);
    /* 下灯指示灯: 下灯亮时亮 */
    gpio_set_level(GPIO_LED_LOWER,   status->switch_lower);
    /* 阅读指示灯: 阅读模式时亮 */
    gpio_set_level(GPIO_LED_READING, state == STATE_READING);
    /* 夜灯指示灯: 夜灯模式时亮 */
    gpio_set_level(GPIO_LED_NIGHT,   state == STATE_NIGHT);
}
