/**
 * pwm_drv.c — LEDC PWM 驱动模块实现
 *
 * 使用 ESP-IDF LEDC 外设:
 *   - ledc_timer_config: 配置 PWM 定时器 (频率 + 分辨率)
 *   - ledc_channel_config: 配置 PWM 通道 (GPIO + 定时器)
 *   - ledc_set_fade_with_time + ledc_fade_start: 硬件渐变
 *   - ledc_set_duty_and_update: 直接设置占空比
 */
#include "pwm_drv.h"
#include "esp_log.h"
#include "driver/ledc.h"

static const char *TAG = "PWM";

/* 通道映射: LAMP_UPPER → LEDC_CHANNEL_0, LAMP_LOWER → LEDC_CHANNEL_1 */
static const ledc_channel_t lamp_channels[LAMP_COUNT] = {
    LEDC_CHANNEL_0,   /* 上灯 */
    LEDC_CHANNEL_1,   /* 下灯 */
};

static const gpio_num_t lamp_gpios[LAMP_COUNT] = {
    GPIO_PWM_UPPER,
    GPIO_PWM_LOWER,
};

/* 当前逻辑亮度 (0~100), 用于查询 */
static uint8_t current_brightness[LAMP_COUNT] = {0, 0};

/* ── 亮度百分比转占空比 ── */
static inline uint32_t brightness_to_duty(uint8_t brightness)
{
    if (brightness > 100) brightness = 100;
    /* 0~100 → 0~1023 */
    return (uint32_t)brightness * 1023 / 100;
}

/* ═══════════════════════════════════════════════════
 * 公共接口实现
 * ═══════════════════════════════════════════════════ */

void pwm_init(void)
{
    /* 1. 配置 LEDC 定时器 */
    ledc_timer_config_t timer_cfg = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .timer_num       = LEDC_TIMER_0,
        .duty_resolution = PWM_RESOLUTION_BIT,   /* 10bit */
        .freq_hz         = PWM_FREQUENCY_HZ,      /* 1kHz */
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer_cfg));

    /* 2. 配置 2 个 PWM 通道 */
    for (int i = 0; i < LAMP_COUNT; i++) {
        ledc_channel_config_t ch_cfg = {
            .channel    = lamp_channels[i],
            .gpio_num   = lamp_gpios[i],
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .timer_sel  = LEDC_TIMER_0,
            .duty       = 0,           /* 初始占空比 0 (灯灭) */
            .hpoint     = 0,
            .flags      = { .output_invert = 0 },
        };
        ESP_ERROR_CHECK(ledc_channel_config(&ch_cfg));
    }

    /* 3. 安装硬件渐变功能 */
    ESP_ERROR_CHECK(ledc_fade_func_install(0));

    ESP_LOGI(TAG, "LEDC initialized: %dHz, %dbit, %d channels",
             PWM_FREQUENCY_HZ, PWM_RESOLUTION_BIT, LAMP_COUNT);
}

void pwm_set_brightness(lamp_id_t lamp, uint8_t brightness, uint32_t fade_ms)
{
    if (lamp >= LAMP_COUNT) return;
    if (brightness > 100) brightness = 100;

    uint32_t target_duty = brightness_to_duty(brightness);
    current_brightness[lamp] = brightness;

    if (fade_ms > 0) {
        /* 硬件渐变: 在 fade_ms 时间内从当前占空比渐变到目标值 */
        ledc_set_fade_with_time(LEDC_LOW_SPEED_MODE,
                                lamp_channels[lamp],
                                target_duty,
                                fade_ms);
        ledc_fade_start(LEDC_LOW_SPEED_MODE,
                         lamp_channels[lamp],
                         LEDC_FADE_NO_WAIT);
    } else {
        /* 无渐变, 直接设置 */
        ledc_set_duty(LEDC_LOW_SPEED_MODE, lamp_channels[lamp], target_duty);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, lamp_channels[lamp]);
    }

    ESP_LOGD(TAG, "lamp %u brightness=%u%% duty=%u fade=%ums",
         (unsigned)lamp, (unsigned)brightness, (unsigned)target_duty, (unsigned)fade_ms);
}

void pwm_turn_off(lamp_id_t lamp, uint32_t fade_ms)
{
    pwm_set_brightness(lamp, 0, fade_ms);
}

void pwm_set_duty_immediate(lamp_id_t lamp, uint8_t brightness)
{
    if (lamp >= LAMP_COUNT) return;
    if (brightness > 100) brightness = 100;

    uint32_t duty = brightness_to_duty(brightness);
    current_brightness[lamp] = brightness;

    /* 原子性更新占空比 (无渐变) */
    ledc_set_duty_and_update(LEDC_LOW_SPEED_MODE,
                             lamp_channels[lamp],
                             duty, 0);
}

uint8_t pwm_get_brightness(lamp_id_t lamp)
{
    if (lamp >= LAMP_COUNT) return 0;
    return current_brightness[lamp];
}
