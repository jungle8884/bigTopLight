/**
 * state_machine.c — 状态机模块实现 (指示灯/定时器已拆分到独立模块)
 *
 * ═══ 数据流 ═══
 *   button_task ──┐
 *   mqtt_client ──┼──→ [事件队列] ──→ lamp_task ──→ PWM / indicator / storage / MQTT
 *   timer.c     ──┘
 *
 * ═══ 状态模型 ═══
 *   SYSTEM_OFF ←── 开关键 ──→ NORMAL ←── 任意其他键 ──→ READING
 *                                ↕
 *                             NIGHT (30min 超时 → OFF)
 *
 * ═══ 按键映射 (NORMAL 状态) ═══
 *   开关键短按     → 系统开/关
 *   上灯键短按     → 上灯开/关
 *   上灯键长按     → 上灯无级调光 (往复扫掠)
 *   上灯键双击     → 上灯最暗↔最亮
 *   下灯键同理
 *   阅读键短按     → 阅读模式 (上下灯 100%)
 *   夜灯键短按     → 夜灯模式 (上灯 5%, 下灯灭, 30min 定时)
 */
#include "state_machine.h"
#include "indicator.h"
#include "timer.h"
#include "pwm_drv.h"
#include "storage.h"
#include "esp_log.h"

static const char *TAG = "STATE";

/* ── MQTT 发布函数 (在 mqtt_client.c 中实现) ── */
extern void mqtt_publish_state(const lamp_status_t *status, system_state_t state);

/* ═══════════════════════════════════════════════════
 * 内部状态
 * ═══════════════════════════════════════════════════ */

static system_state_t s_state = STATE_OFF;     /* 当前系统状态 */
static lamp_status_t  s_status = {0};           /* 当前灯状态 (开关+亮度) */
static lamp_status_t  s_snapshot = {0};         /* 模式切换前的快照 (用于恢复) */
static QueueHandle_t  s_event_queue = NULL;     /* 统一事件队列 */

/* 长按调光状态 */
static lamp_id_t s_dim_lamp = LAMP_COUNT;       /* 正在调光的灯 */
static int8_t     s_dim_dir  = 1;               /* 调光方向: 1=递增, -1=递减 */

/* ═══════════════════════════════════════════════════
 * 调光步进 (由 timer.c dim 定时器回调直接调用)
 * ═══════════════════════════════════════════════════ */

void state_machine_dim_step(void)
{
    if (s_dim_lamp >= LAMP_COUNT) return;

    uint8_t *bright = (s_dim_lamp == LAMP_UPPER)
                      ? &s_status.bright_upper
                      : &s_status.bright_lower;

    /* 往复扫掠: 到达上限反向递减, 到达下限反向递增 */
    if (s_dim_dir > 0) {
        *bright += DIM_STEP_PCT;
        if (*bright >= MAX_BRIGHTNESS) {
            *bright = MAX_BRIGHTNESS;
            s_dim_dir = -1;  /* 反向 → 递减 */
        }
    } else {
        *bright -= DIM_STEP_PCT;
        if (*bright <= MIN_BRIGHTNESS) {
            *bright = MIN_BRIGHTNESS;
            s_dim_dir = 1;   /* 反向 → 递增 */
        }
    }

    /* 立即更新 PWM (无渐变, 直接设置占空比) */
    pwm_set_duty_immediate(s_dim_lamp, *bright);
}

/* ═══════════════════════════════════════════════════
 * 状态转移辅助函数
 * ═══════════════════════════════════════════════════ */

/* 更新指示灯 + 发布 MQTT 状态 */
static void notify_change(void)
{
    indicator_update(s_state, &s_status);
    mqtt_publish_state(&s_status, s_state);
}

/* 开机: 恢复记忆亮度, 缓起亮灯 */
static void do_power_on(void)
{
    s_state = STATE_NORMAL;
    s_status.switch_upper = true;
    s_status.switch_lower = true;

    /* 两路灯同时缓起到记忆亮度 */
    pwm_set_brightness(LAMP_UPPER, s_status.bright_upper, FADE_IN_TIME_MS);
    pwm_set_brightness(LAMP_LOWER, s_status.bright_lower, FADE_IN_TIME_MS);

    ESP_LOGI(TAG, "POWER ON: upper=%d%% lower=%d%%",
             s_status.bright_upper, s_status.bright_lower);
    notify_change();
}

/* 关机: 缓灭, 保存状态 */
static void do_power_off(void)
{
    pwm_turn_off(LAMP_UPPER, FADE_OUT_TIME_MS);
    pwm_turn_off(LAMP_LOWER, FADE_OUT_TIME_MS);

    s_status.switch_upper = false;
    s_status.switch_lower = false;
    s_state = STATE_OFF;

    /* 保存当前亮度 + 开关状态到 NVS */
    storage_save_brightness(LAMP_UPPER, s_status.bright_upper);
    storage_save_brightness(LAMP_LOWER, s_status.bright_lower);
    storage_save_switch_state(false, false);

    ESP_LOGI(TAG, "POWER OFF");
    notify_change();
}

/* 进入阅读模式: 上下灯均 100% */
static void do_enter_reading(void)
{
    /* 保存当前状态快照 (用于退出时恢复) */
    s_snapshot = s_status;

    /* 上下灯均设为 100%, 带过渡渐变 */
    s_status.switch_upper = true;
    s_status.switch_lower = true;
    s_status.bright_upper = MAX_BRIGHTNESS;
    s_status.bright_lower = MAX_BRIGHTNESS;
    pwm_set_brightness(LAMP_UPPER, MAX_BRIGHTNESS, MODE_FADE_TIME_MS);
    pwm_set_brightness(LAMP_LOWER, MAX_BRIGHTNESS, MODE_FADE_TIME_MS);

    s_state = STATE_READING;
    ESP_LOGI(TAG, "ENTER READING MODE");
    notify_change();
}

/* 进入夜灯模式: 上灯 5%, 下灯灭, 启动 30min 定时 */
static void do_enter_night(void)
{
    /* 保存当前状态快照 */
    s_snapshot = s_status;

    /* 上灯设为 5%, 下灯关闭 */
    s_status.switch_upper = true;
    s_status.switch_lower = false;
    s_status.bright_upper = NIGHT_BRIGHTNESS;
    pwm_set_brightness(LAMP_UPPER, NIGHT_BRIGHTNESS, MODE_FADE_TIME_MS);
    pwm_turn_off(LAMP_LOWER, MODE_FADE_TIME_MS);

    s_state = STATE_NIGHT;

    /* 启动 30 分钟单次定时器 */
    timer_start_night();

    ESP_LOGI(TAG, "ENTER NIGHT MODE (30min timeout)");
    notify_change();
}

/* 退出特殊模式 (阅读/夜灯): 恢复快照状态 */
static void do_exit_special(void)
{
    /* 停止夜灯定时器 (如果是从夜灯模式退出) */
    if (s_state == STATE_NIGHT) {
        timer_stop_night();
    }

    /* 恢复快照中的状态 */
    s_status = s_snapshot; // s_snapshot 是在进入特殊模式时保存的完整灯状态副本（结构体赋值）

    /* 根据快照恢复 PWM 输出 */
    if (s_status.switch_upper) {
        pwm_set_brightness(LAMP_UPPER, s_status.bright_upper, MODE_FADE_TIME_MS);
    } else {
        pwm_turn_off(LAMP_UPPER, MODE_FADE_TIME_MS);
    }
    if (s_status.switch_lower) {
        pwm_set_brightness(LAMP_LOWER, s_status.bright_lower, MODE_FADE_TIME_MS);
    } else {
        pwm_turn_off(LAMP_LOWER, MODE_FADE_TIME_MS);
    }
    // 状态设为 NORMAL，然后 notify_change() 同步更新指示灯和 MQTT 状态回传。
    s_state = STATE_NORMAL;
    ESP_LOGI(TAG, "EXIT SPECIAL MODE → NORMAL");
    notify_change();
}

/* ═══════════════════════════════════════════════════
 * 按键事件处理
 * ═══════════════════════════════════════════════════ */

/* 处理上灯/下灯按键 (短按/长按/双击) 
* 这是单灯按键操作处理函数，处理上灯/下灯键的短按、长按、双击三种交互，实现开关切换、无级调光、亮度跳转三个功能。
*/
static void handle_lamp_button(lamp_id_t lamp, button_event_t evt)
{
    bool     *sw     = (lamp == LAMP_UPPER) ? &s_status.switch_upper : &s_status.switch_lower;
    uint8_t  *bright = (lamp == LAMP_UPPER) ? &s_status.bright_upper  : &s_status.bright_lower;

    switch (evt) {

        case BTN_EVT_SHORT_PRESS:
            /* 短按: 切换灯开/关 */
            if (*sw) {
                *sw = false;
                pwm_turn_off(lamp, FADE_OUT_TIME_MS); // 开→关：缓灭
            } else {
                *sw = true;
                pwm_set_brightness(lamp, *bright, FADE_IN_TIME_MS); // 关→开：缓起到记忆亮度
            }
            ESP_LOGI(TAG, "lamp %d short: sw=%d", lamp, *sw);
            notify_change();
            break;

        case BTN_EVT_LONG_PRESS_START:
            /* 长按开始: 仅在灯亮时启动调光 */
            if (*sw) { 
                s_dim_lamp = lamp;
                s_dim_dir = 1;   /* 从递增开始 */
                timer_start_dim();  /* 启动周期性调光定时器 (20ms) */
                ESP_LOGI(TAG, "lamp %d dimming START", lamp);
            }
            break;

        case BTN_EVT_LONG_PRESS_RELEASE:
            /* 长按释放: 停止调光, 保存亮度 */
            if (s_dim_lamp < LAMP_COUNT) {
                timer_stop_dim();
                storage_save_brightness(s_dim_lamp, *bright);
                ESP_LOGI(TAG, "lamp %d dimming STOP, bright=%d%%", s_dim_lamp, *bright);
                s_dim_lamp = LAMP_COUNT;
                notify_change();
            }
            break;

        case BTN_EVT_DOUBLE_CLICK:
            /* 双击: 切换最暗/最亮 (仅在灯亮时) */
            if (*sw) {
                if (*bright <= MIN_BRIGHTNESS) {
                    *bright = MAX_BRIGHTNESS;
                } else {
                    *bright = MIN_BRIGHTNESS;
                }
                pwm_set_brightness(lamp, *bright, DOUBLE_CLICK_FADE_MS);
                storage_save_brightness(lamp, *bright);
                ESP_LOGI(TAG, "lamp %d double: bright=%d%%", lamp, *bright);
                notify_change();
            }
            break;
        /**
         * -------------------------------- 交互设计意图 ---------------------------------
         * 用户操作        短按          长按(持续)              释放           双击
            │              │              │                     │              │
            ▼              ▼              ▼                     ▼              ▼
            按键状态机    SHORT_PRESS   LONG_PRESS_START    LONG_PRESS_RELEASE  DOUBLE_CLICK
            │              │              │                     │              │
            ▼              ▼              ▼                     ▼              ▼
            handle_lamp   切换开关     启动调光定时器        停止+存储亮度     亮度跳变
            button           │         (20ms周期扫掠)            │              │
                            │              │                     │              │
                            ▼              ▼                     ▼              ▼
                        PWM缓起/灭   brightness往复变化      亮度固定        PWM快速渐变
                                    ↑ 5% ─→ 100% ─→ 5%
         * ──────────────────────────────────────────────────────────────────────────────
        */
        default:
            break;
    }
}

/* 主按键事件分发 (根据当前状态) */
static void handle_button_event(button_id_t btn, button_event_t evt)
{
    /* ── 阅读模式: 仅夜灯键可切换, 其余键退出 ── */
    if (s_state == STATE_READING) {
        if (btn == BTN_READING) return;                /* 重复按无响应 */
        if (btn == BTN_NIGHT && evt == BTN_EVT_SHORT_PRESS) { // BTN_NIGHT + 短按: 切换到夜灯模式
            do_exit_special();  /* 退出阅读 */
            do_enter_night();   /* 进入夜灯 */
            return;
        }
        do_exit_special();  /* 其他键: 退出阅读模式 */
        return; // 设计意图：阅读模式下大部分键变成"退出键"，只有夜灯键可以切换到夜灯模式，避免误操作。
    }

    /* ── 夜灯模式: 仅阅读键可切换, 其余键退出 ── */
    if (s_state == STATE_NIGHT) {
        if (btn == BTN_NIGHT) return;                   /* 重复按无响应 */
        if (btn == BTN_READING && evt == BTN_EVT_SHORT_PRESS) { // BTN_READING + 短按: 切换到阅读模式
            do_exit_special();  /* 退出夜灯 */
            do_enter_reading(); /* 进入阅读 */
            return;
        }
        do_exit_special();  /* 其他键: 退出夜灯模式 */
        return; // 设计意图：夜灯模式下大部分键变成"退出键"，只有阅读键可以切换到阅读模式，避免误操作。
    }

    /* ── OFF 状态: 仅开关键响应 ── */
    if (s_state == STATE_OFF) {
        if (btn == BTN_POWER && evt == BTN_EVT_SHORT_PRESS) { // BTN_POWER + 短按: 开机
            do_power_on();
        }
        return; // 关机状态下只有开关键能唤醒，其余键无响应，避免误开机
    }

    /* ── NORMAL 状态: 全部按键有效 ── */
    switch (btn) {

        case BTN_POWER:
            if (evt == BTN_EVT_SHORT_PRESS) {
                do_power_off(); // 短按: 关机
            }
            break;

        case BTN_UPPER:
            handle_lamp_button(LAMP_UPPER, evt); // 处理上灯按键事件
            break;

        case BTN_LOWER:
            handle_lamp_button(LAMP_LOWER, evt); // 处理下灯按键事件
            break;

        case BTN_READING:
            if (evt == BTN_EVT_SHORT_PRESS) {
                do_enter_reading(); // 短按: 进入阅读模式
            }
            break;

        case BTN_NIGHT:
            if (evt == BTN_EVT_SHORT_PRESS) {
                do_enter_night(); // 短按: 进入夜灯模式
            }
            break;

        default:
            break;
    }
}

/* ═══════════════════════════════════════════════════
 * MQTT 远程命令处理
    参数	                     含义
    cmd	    命令类型（CMD_POWER_TOGGLE / CMD_LAMP_TOGGLE / CMD_SET_BRIGHTNESS / CMD_SET_MODE）
    lamp	目标灯（LAMP_UPPER / LAMP_LOWER），仅对单灯操作有效
    value	命令参数（开关 0/1、亮度 0~100、模式 0/1/2）
 * ═══════════════════════════════════════════════════ */

static void handle_command(command_id_t cmd, lamp_id_t lamp, int32_t value)
{
    switch (cmd) {

        case CMD_POWER_TOGGLE:
            /* 总开关: OFF → ON, 其他 → OFF */
            if (s_state == STATE_OFF) {
                do_power_on(); // OFF → NORMAL，两灯缓起到记忆亮度
            } else {
                do_power_off(); // 任意状态 → OFF，缓灭 + 保存状态到 NVS
            }
            break;

        case CMD_LAMP_TOGGLE:
            /* 单灯开关: value=1 开, value=0 关 */
            if (s_state == STATE_OFF) {
                do_power_on();  /* 系统关时先开机 */
            }
            if (s_state == STATE_NORMAL) {
                bool *sw = (lamp == LAMP_UPPER) ? &s_status.switch_upper : &s_status.switch_lower; // 指向 switch_upper 或 switch_lower
                uint8_t *br = (lamp == LAMP_UPPER) ? &s_status.bright_upper : &s_status.bright_lower; // 指向 bright_upper 或 bright_lower
                *sw = (value != 0);
                if (*sw) {
                    pwm_set_brightness(lamp, *br, FADE_IN_TIME_MS); // 开：缓起
                } else {
                    pwm_turn_off(lamp, FADE_OUT_TIME_MS); // 关：缓灭
                }
                notify_change();
            }
            /**
             * 关键设计：
                系统关时先开机：云端发 switch_upper=1 而系统处于 OFF，会自动先开机再开灯
                仅在 NORMAL 模式生效：阅读/夜灯模式下忽略单灯开关，避免破坏模式一致性
            */
            break;

        case CMD_SET_BRIGHTNESS:
            /* 设置亮度: value=0~100, 仅在正常模式生效 */
            if (s_state == STATE_NORMAL) {
                uint8_t b = (uint8_t)value;
                if (b > 100) b = 100;
                // 更新状态 + PWM（仅灯开着时才驱动 PWM）
                if (lamp == LAMP_UPPER) {
                    s_status.bright_upper = b;
                    if (s_status.switch_upper) {
                        pwm_set_brightness(LAMP_UPPER, b, MODE_FADE_TIME_MS);
                    }
                } else {
                    s_status.bright_lower = b;
                    if (s_status.switch_lower) {
                        pwm_set_brightness(LAMP_LOWER, b, MODE_FADE_TIME_MS);
                    }
                }
                storage_save_brightness(lamp, b);
                notify_change();
            }
            /**
             * 关键设计：
                灯关时只存不亮：更新亮度值到 s_status 并存入 NVS，但不驱动 PWM，下次开灯时使用新亮度
                立即存储：亮度变更实时写入 NVS，断电不丢失
            */
            break;

        case CMD_SET_MODE:
            /* 设置工作模式: 0=normal, 1=reading, 2=night */
            if (s_state == STATE_OFF) { // 关机状态不允许切模式
                ESP_LOGW(TAG, "cannot set mode while OFF");
                break;
            }
            switch (value) {
                case 0:  /* normal */
                    if (s_state != STATE_NORMAL) {
                        do_exit_special(); // 退出阅读/夜灯模式，恢复快照状态
                    }
                break;
                case 1:  /* reading */
                    if (s_state != STATE_READING) {
                        if (s_state != STATE_NORMAL) do_exit_special(); // 先退出夜灯模式，恢复快照到 NORMAL
                        do_enter_reading(); //  再从 NORMAL 进入阅读模式，保存新快照
                    }
                break;
                case 2:  /* night */
                    if (s_state != STATE_NIGHT) {
                        if (s_state != STATE_NORMAL) do_exit_special(); // 先退出阅读，恢复快照到 NORMAL
                        do_enter_night(); // 再从 NORMAL 进入夜灯，保存新快照
                    }
                break;
            }
            /**
             * 关键设计：
                OFF 状态拒绝切换：必须先开机
                特殊模式互斥：从阅读切夜灯（或反向），必须先 do_exit_special() 恢复快照，再进入新模式
                do_exit_special() 恢复进入特殊模式前保存的 s_snapshot，确保状态一致性
                do_enter_night() 自动启动 30 分钟定时器，超时后自动关机
            */
            break;
        default:
            break;
    }
    /**
     * handle_command()
        ├── do_power_on()       → STATE_NORMAL, 两灯缓起, notify
        ├── do_power_off()      → STATE_OFF, 缓灭, 存 NVS, notify
        ├── do_enter_reading()  → 保存快照, 两灯 100%, STATE_READING, notify
        ├── do_enter_night()    → 保存快照, 上灯 5% 下灯灭, 启动 30min 定时, STATE_NIGHT, notify
        ├── do_exit_special()   → 停定时器, 恢复快照, STATE_NORMAL, notify
        ├── pwm_set_brightness() / pwm_turn_off()   → 直接驱动硬件
        ├── storage_save_brightness()                → 持久化
        └── notify_change()     → 更新指示灯 + MQTT 状态回传
    */
}

/* ═══════════════════════════════════════════════════
 * 定时器事件处理
 * ═══════════════════════════════════════════════════ */

static void handle_timer(timer_id_t timer_id)
{
    switch (timer_id) {

        case TIMER_NIGHT_LAMP:
            /* 夜灯 30 分钟超时 → 自动关灯 */
            ESP_LOGI(TAG, "night lamp timeout → power off");
            pwm_turn_off(LAMP_UPPER, FADE_OUT_TIME_MS);
            pwm_turn_off(LAMP_LOWER, FADE_OUT_TIME_MS);
            s_status.switch_upper = false;
            s_status.switch_lower = false;
            s_state = STATE_OFF;
            storage_save_switch_state(false, false);
            notify_change();
            break;

        default:
            break;
    }
}

/* ═══════════════════════════════════════════════════
 * 状态机主任务 (lamp_task)
 * ═══════════════════════════════════════════════════ */
/**
 * @brief 状态机主任务 — 统一事件消费者
 *
 * 阻塞等待 s_event_queue 中的消息, 根据消息类型分发到对应处理函数:
 *   - MSG_BUTTON  → handle_button_event()  按键事件
 *   - MSG_COMMAND → handle_command()        远程命令
 *   - MSG_TIMER   → handle_timer()          定时器事件
 *
 * 所有事件串行处理, 天然避免并发竞争, 无需加锁。
 *
 * @param arg 未使用 (FreeRTOS 任务入口签名要求)
 */
static void lamp_task(void *arg)
{
    ESP_LOGI(TAG, "lamp_task started (state=%d)", s_state);

    system_msg_t msg;

    while (1) {
        /* 永久阻塞等待, 无事件时不消耗 CPU */
        if (xQueueReceive(s_event_queue, &msg, portMAX_DELAY) != pdPASS) {
            continue;
        }
        /* 按消息类型分发到对应处理函数 */
        switch (msg.type) {

            case MSG_BUTTON:
                ESP_LOGD(TAG, "BTN: id=%d evt=%d state=%d",
                        msg.btn_id, msg.btn_evt, s_state);
                handle_button_event(msg.btn_id, msg.btn_evt);
                break;

            case MSG_COMMAND:
                ESP_LOGD(TAG, "CMD: id=%d lamp=%d val=%d state=%d",
                        msg.cmd_id, msg.lamp_id, (int)msg.value, s_state);
                handle_command(msg.cmd_id, msg.lamp_id, msg.value);
                break;

            case MSG_TIMER:
                ESP_LOGD(TAG, "TIMER: id=%d state=%d", msg.timer_id, s_state);
                handle_timer(msg.timer_id);
                break;

            default:
                break;
        }
    }
}

/* ═══════════════════════════════════════════════════
 * 公共接口实现
 * ═══════════════════════════════════════════════════ */

void state_machine_init(void)
{
    /* 1. 创建事件队列 (容量 32 条消息) */
    s_event_queue = xQueueCreate(32, sizeof(system_msg_t));

    /* 2. 从 NVS 加载保存的亮度/开关状态 */
    storage_load(&s_status);

    /* 3. 初始化 PWM 驱动 (两路 LEDC, 初始占空比 0) */
    pwm_init();

    /* 4. 初始化指示灯 GPIO */
    indicator_init();

    /* 5. 上电指示灯全亮 1 秒后熄灭 */
    indicator_power_on_sequence();

    /* 6. 初始化定时器 (调光步进 + 夜灯超时) */
    timer_init();

    /* 初始状态: 全关 */
    s_state = STATE_OFF;
    s_status.switch_upper = false;
    s_status.switch_lower = false;

    ESP_LOGI(TAG, "state machine initialized");
}

void state_machine_start_task(void)
{
    /* 状态机任务优先级 7 (高, 优先处理用户输入) */
    xTaskCreate(lamp_task, "lamp_task", 4096, NULL, 7, NULL);
}

QueueHandle_t state_machine_get_event_queue(void)
{
    return s_event_queue;
}

/**
 * @brief 向状态机统一事件队列投递消息 (非阻塞)
 *
 * 系统中三个生产者 (button_task / mqtt_client / timer) 均通过此函数
 * 向 lamp_task 消费者投递事件, 是生产者-消费者架构的核心管道。
 *
 * @param msg 指向要投递的消息 (值拷贝入队, 调用方可安全释放)
 * @note  超时为 0, 队列满时立即返回不阻塞, 保证发送方不被卡住
 */
void state_machine_send_event(system_msg_t *msg)
{
    /* 队列未初始化时跳过, 防止空指针崩溃 */
    if (s_event_queue) {
        /* 非阻塞入队: 超时=0, 满则丢弃 */
        xQueueSend(s_event_queue, msg, 0);
    }
}

system_state_t state_machine_get_state(void)
{
    return s_state;
}

void state_machine_get_status(lamp_status_t *status)
{
    if (status) *status = s_status;
}
