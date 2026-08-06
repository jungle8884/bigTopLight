#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h" // 引入日志库

#define TAG "app_main"

void app_main(void)
{
    ESP_LOGI(TAG, "Hello world!"); // 打印日志信息
}
