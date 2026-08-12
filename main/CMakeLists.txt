# main 组件构建配置
# 注册所有源文件和头文件目录

idf_component_register(
    SRCS
        "main.c"
        "button.c"
        "pwm_drv.c"
        "storage.c"
        "indicator.c"
        "timer.c"
        "state_machine.c"
        "mqtt_client.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        nvs_flash
        esp_timer
        esp_wifi
        mqtt
        driver
        json
)
