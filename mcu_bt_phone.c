#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_event.h"

// Classic Bluetooth
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_bt_device.h"
#include "esp_mac.h"

// HFP AG (手机端)
#include "esp_hf_ag_api.h"

#define TAG "BT_PHONE"

// ========== 引脚定义 ==========
// LED指示灯
#define LED_R GPIO_NUM_18  // 红灯 - 通话中
#define LED_G GPIO_NUM_5   // 绿灯 - 已连接
#define LED_B GPIO_NUM_17  // 蓝灯 - 待机

// 按键
#define BOOT_KEY GPIO_NUM_0        // 开关蓝牙
#define CALL_KEY GPIO_NUM_23       // 模拟来电按键

// ========== 全局变量 ==========
static char my_mac_id[8];
static char bt_name[32];
static bool bt_on = false;
static int led_mode = 0;

// HFP连接状态
static bool hfp_connected = false;
static esp_bd_addr_t connected_device = {0};

// 呼叫状态
typedef enum {
    CALL_STATE_IDLE,        // 空闲
    CALL_STATE_INCOMING,    // 来电中
    CALL_STATE_ACTIVE,      // 通话中
    CALL_STATE_DIALING,     // 拨号中
} call_state_t;

static call_state_t current_call_state = CALL_STATE_IDLE;
static char current_phone_number[32] = "";
static TimerHandle_t ring_timer = NULL;
static TimerHandle_t out_alert_timer = NULL;
static TimerHandle_t out_answer_timer = NULL;
static SemaphoreHandle_t state_mutex = NULL;

static inline void state_lock(void)
{
    if (state_mutex != NULL)
    {
        xSemaphoreTake(state_mutex, portMAX_DELAY);
    }
}

static inline void state_unlock(void)
{
    if (state_mutex != NULL)
    {
        xSemaphoreGive(state_mutex);
    }
}

static inline call_state_t get_call_state(void)
{
    call_state_t s;
    state_lock();
    s = current_call_state;
    state_unlock();
    return s;
}

static inline void set_call_state(call_state_t s)
{
    state_lock();
    current_call_state = s;
    state_unlock();
}

/* ===================== LED控制 ===================== */

static inline void led_off(void)
{
    gpio_set_level(LED_R, 1);
    gpio_set_level(LED_G, 1);
    gpio_set_level(LED_B, 1);
}

static inline void led_red(void)
{
    gpio_set_level(LED_R, 0);
    gpio_set_level(LED_G, 1);
    gpio_set_level(LED_B, 1);
}

static inline void led_green(void)
{
    gpio_set_level(LED_R, 1);
    gpio_set_level(LED_G, 0);
    gpio_set_level(LED_B, 1);
}

static inline void led_blue(void)
{
    gpio_set_level(LED_R, 1);
    gpio_set_level(LED_G, 1);
    gpio_set_level(LED_B, 0);
}

static void led_task(void *arg)
{
    while (1)
    {
        if (led_mode == 1)
        {
            // 待机 - 蓝灯慢闪
            led_blue();
            vTaskDelay(pdMS_TO_TICKS(500));
            led_off();
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        else if (led_mode == 2)
        {
            // 已连接 - 绿灯常亮
            led_green();
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        else if (led_mode == 3)
        {
            // 来电 - 绿灯快闪
            led_green();
            vTaskDelay(pdMS_TO_TICKS(200));
            led_off();
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        else if (led_mode == 4)
        {
            // 通话中 - 红灯常亮
            led_red();
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        else if (led_mode == 5)
        {
            // 错误 - 红灯快闪
            led_red();
            vTaskDelay(pdMS_TO_TICKS(100));
            led_off();
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        else
        {
            led_off();
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

/* ===================== 呼叫管理 ===================== */

static void report_incoming_ring_indicators(void)
{
    esp_hf_ag_ciev_report(connected_device, ESP_HF_IND_TYPE_SERVICE, 1);  // service=1
    esp_hf_ag_ciev_report(connected_device, ESP_HF_IND_TYPE_SIGNAL, 5);   // signal=5
    // 来电状态：无活动通话，但处于来电建立中
    esp_hf_ag_ciev_report(connected_device, ESP_HF_IND_TYPE_CALL, 0);
    esp_hf_ag_ciev_report(connected_device, ESP_HF_IND_TYPE_CALLSETUP, ESP_HF_CALL_SETUP_STATUS_INCOMING);

    // 很多车机会依赖RING/+CLIP触发来电界面
    esp_hf_ag_unknown_at_send(connected_device, "RING");
    if (current_phone_number[0] != '\0')
    {
        char clip[64];
        snprintf(clip, sizeof(clip), "+CLIP: \"%s\",129", current_phone_number);
        esp_hf_ag_unknown_at_send(connected_device, clip);
    }
}

// 定时发送RING
static void ring_timer_callback(TimerHandle_t xTimer)
{
    if (get_call_state() == CALL_STATE_INCOMING && hfp_connected)
    {
        ESP_LOGI(TAG, "🔔 发送RING...");
        report_incoming_ring_indicators();
    }
}

static void outgoing_answer_timer_callback(TimerHandle_t xTimer)
{
    if (get_call_state() != CALL_STATE_DIALING || !hfp_connected)
    {
        return;
    }

    ESP_LOGI(TAG, "✅ 对方已接听");
    set_call_state(CALL_STATE_ACTIVE);
    led_mode = 4; // 红灯常亮

    esp_hf_ag_out_call(
        connected_device,
        1,                                // num_active=1
        0,
        ESP_HF_CALL_STATUS_CALL_IN_PROGRESS,
        ESP_HF_CALL_SETUP_STATUS_IDLE,
        current_phone_number,
        ESP_HF_CALL_ADDR_TYPE_UNKNOWN
    );

    esp_hf_ag_ciev_report(connected_device, ESP_HF_IND_TYPE_CALL, 1);
    esp_hf_ag_ciev_report(connected_device, ESP_HF_IND_TYPE_CALLSETUP, 0);
    esp_hf_ag_audio_connect(connected_device);
}

static void outgoing_alert_timer_callback(TimerHandle_t xTimer)
{
    if (get_call_state() != CALL_STATE_DIALING || !hfp_connected)
    {
        return;
    }

    esp_hf_ag_ciev_report(connected_device, ESP_HF_IND_TYPE_CALLSETUP, ESP_HF_CALL_SETUP_STATUS_OUTGOING_ALERTING);
    ESP_LOGI(TAG, "📞 对方振铃中...");
    if (out_answer_timer != NULL)
    {
        xTimerStart(out_answer_timer, 0);
    }
}

// 模拟来电
void simulate_incoming_call(const char *phone_number)
{
    if (!hfp_connected)
    {
        ESP_LOGW(TAG, "❌ HFP未连接，无法模拟来电");
        return;
    }

    if (get_call_state() != CALL_STATE_IDLE)
    {
        ESP_LOGW(TAG, "❌ 当前有通话，无法模拟新来电");
        return;
    }

    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "📞 ========== 模拟来电 ==========");
    ESP_LOGI(TAG, "📞 来电号码: %s", phone_number);
    ESP_LOGI(TAG, "📞 ===============================");

    // 保存电话号码
    strncpy(current_phone_number, phone_number, sizeof(current_phone_number) - 1);
    set_call_state(CALL_STATE_INCOMING);

    // 更新LED
    led_mode = 3; // 绿灯快闪

    // 发送状态指示 - 来电中
    // 使用ciev_report发送单独的指示器
    report_incoming_ring_indicators();
    // 启动RING定时器（每3秒发送一次）
    if (ring_timer == NULL)
    {
        ring_timer = xTimerCreate("ring", pdMS_TO_TICKS(3000), pdTRUE, NULL, ring_timer_callback);
    }
    xTimerStart(ring_timer, 0);

    ESP_LOGI(TAG, "💡 等待车机接听/拒接...");
}

// 接听来电
void handle_call_answer(void)
{
    if (get_call_state() != CALL_STATE_INCOMING)
    {
        ESP_LOGW(TAG, "❌ 当前无来电，无法接听");
        return;
    }

    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "✅ ========== 接听来电 ==========");
    ESP_LOGI(TAG, "✅ 电话号码: %s", current_phone_number);
    ESP_LOGI(TAG, "✅ ===============================");

    // 停止RING
    if (ring_timer != NULL)
    {
        xTimerStop(ring_timer, 0);
    }

    set_call_state(CALL_STATE_ACTIVE);
    led_mode = 4; // 红灯常亮

    // 先同步CIEV状态，再发送接听应答，提升车机状态机兼容性
    esp_hf_ag_ciev_report(connected_device, ESP_HF_IND_TYPE_CALL, 1);         // call=1 (有活动呼叫)
    esp_hf_ag_ciev_report(connected_device, ESP_HF_IND_TYPE_CALLSETUP, 0);    // callsetup=0 (无呼叫建立中)

    // 发送接听应答
    esp_hf_ag_answer_call(
        connected_device,
        1,                                    // num_active=1 (1个活动呼叫)
        0,                                    // num_held=0
        ESP_HF_CALL_STATUS_CALL_IN_PROGRESS,  // call_state
        ESP_HF_CALL_SETUP_STATUS_IDLE,
        current_phone_number,
        ESP_HF_CALL_ADDR_TYPE_UNKNOWN
    );

    // 建立SCO音频连接
    esp_hf_ag_audio_connect(connected_device);

    ESP_LOGI(TAG, "🎙️ 通话已建立，音频连接中...");
}

// 拒接来电
void handle_call_reject(void)
{
    if (get_call_state() != CALL_STATE_INCOMING)
    {
        ESP_LOGW(TAG, "❌ 当前无来电，无法拒接");
        return;
    }

    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "❌ ========== 拒接来电 ==========");
    ESP_LOGI(TAG, "❌ 电话号码: %s", current_phone_number);
    ESP_LOGI(TAG, "❌ ===============================");

    // 停止RING
    if (ring_timer != NULL)
    {
        xTimerStop(ring_timer, 0);
    }

    set_call_state(CALL_STATE_IDLE);
    led_mode = 2; // 绿灯常亮

    // 发送拒接应答
    esp_hf_ag_reject_call(
        connected_device,
        0,                                    // num_active=0
        0,                                    // num_held=0
        ESP_HF_CALL_STATUS_NO_CALLS,
        ESP_HF_CALL_SETUP_STATUS_IDLE,
        current_phone_number,
        ESP_HF_CALL_ADDR_TYPE_UNKNOWN
    );

    // 显式同步空闲状态，提升部分车机界面收敛速度
    esp_hf_ag_ciev_report(connected_device, ESP_HF_IND_TYPE_CALL, 0);
    esp_hf_ag_ciev_report(connected_device, ESP_HF_IND_TYPE_CALLSETUP, 0);

    memset(current_phone_number, 0, sizeof(current_phone_number));
    ESP_LOGI(TAG, "📵 来电已拒绝");
}

// 挂断电话
void handle_call_hangup(void)
{
    if (get_call_state() != CALL_STATE_ACTIVE)
    {
        ESP_LOGW(TAG, "❌ 当前无通话，无法挂断");
        return;
    }

    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "📴 ========== 结束通话 ==========");
    ESP_LOGI(TAG, "📴 电话号码: %s", current_phone_number);
    ESP_LOGI(TAG, "📴 ===============================");

    set_call_state(CALL_STATE_IDLE);
    led_mode = 2; // 绿灯常亮

    // 发送挂断应答
    esp_hf_ag_end_call(
        connected_device,
        0,                                    // num_active=0
        0,                                    // num_held=0
        ESP_HF_CALL_STATUS_NO_CALLS,
        ESP_HF_CALL_SETUP_STATUS_IDLE,
        current_phone_number,
        ESP_HF_CALL_ADDR_TYPE_UNKNOWN
    );

    // 显式同步空闲状态，避免车机界面残留在通话页
    esp_hf_ag_ciev_report(connected_device, ESP_HF_IND_TYPE_CALL, 0);
    esp_hf_ag_ciev_report(connected_device, ESP_HF_IND_TYPE_CALLSETUP, 0);

    // 断开SCO音频
    esp_hf_ag_audio_disconnect(connected_device);

    memset(current_phone_number, 0, sizeof(current_phone_number));
    ESP_LOGI(TAG, "📵 通话已结束");
}

// 外拨电话
void handle_call_dial(const char *number)
{
    if (!hfp_connected)
    {
        ESP_LOGW(TAG, "❌ HFP未连接，无法拨号");
        return;
    }

    if (get_call_state() != CALL_STATE_IDLE)
    {
        ESP_LOGW(TAG, "❌ 当前有通话，无法拨号");
        return;
    }

    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "📞 ========== 外拨电话 ==========");
    ESP_LOGI(TAG, "📞 拨号: %s", number);
    ESP_LOGI(TAG, "📞 ===============================");

    strncpy(current_phone_number, number, sizeof(current_phone_number) - 1);
    set_call_state(CALL_STATE_DIALING);
    led_mode = 3; // 绿灯快闪

    // 发送外拨应答
    esp_hf_ag_out_call(
        connected_device,
        0,                                    // num_active=0
        0,                                    // num_held=0
        ESP_HF_CALL_STATUS_NO_CALLS,
        ESP_HF_CALL_SETUP_STATUS_OUTGOING_DIALING,
        (char *)number,
        ESP_HF_CALL_ADDR_TYPE_UNKNOWN
    );

    // 发送callsetup=2 (外拨中)
    esp_hf_ag_ciev_report(connected_device, ESP_HF_IND_TYPE_CALLSETUP, ESP_HF_CALL_SETUP_STATUS_OUTGOING_DIALING);

    if (out_alert_timer == NULL)
    {
        out_alert_timer = xTimerCreate("out_alert", pdMS_TO_TICKS(300), pdFALSE, NULL, outgoing_alert_timer_callback);
    }
    if (out_answer_timer == NULL)
    {
        out_answer_timer = xTimerCreate("out_answer", pdMS_TO_TICKS(2000), pdFALSE, NULL, outgoing_answer_timer_callback);
    }
    if (out_alert_timer != NULL)
    {
        xTimerStart(out_alert_timer, 0);
    }
}

/* ===================== HFP AG事件回调 ===================== */

static void hfp_ag_callback(esp_hf_cb_event_t event, esp_hf_cb_param_t *param)
{
    switch (event)
    {
    case ESP_HF_CONNECTION_STATE_EVT:
    {
        uint8_t *bda = param->conn_stat.remote_bda;
        ESP_LOGI(TAG, "HFP连接状态: %s [%02X:%02X:%02X:%02X:%02X:%02X]",
                 (param->conn_stat.state == ESP_HF_CONNECTION_STATE_CONNECTED) ? "已连接" : "已断开",
                 bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);

        if (param->conn_stat.state == ESP_HF_CONNECTION_STATE_CONNECTED
#ifdef ESP_HF_CONNECTION_STATE_SLC_CONNECTED
            || param->conn_stat.state == ESP_HF_CONNECTION_STATE_SLC_CONNECTED
#endif
        )
        {
            hfp_connected = true;
            memcpy(connected_device, bda, 6);
            led_mode = 2; // 绿灯常亮
            esp_hf_ag_bsir(connected_device, ESP_HF_IN_BAND_RINGTONE_NOT_PROVIDED);

            ESP_LOGI(TAG, "");
            ESP_LOGI(TAG, "🎉 HFP连接成功！");
            ESP_LOGI(TAG, "💡 按CALL_KEY (GPIO23) 模拟来电");
            ESP_LOGI(TAG, "");
        }
        else
        {
            hfp_connected = false;
            memset(connected_device, 0, 6);
            set_call_state(CALL_STATE_IDLE);
            led_mode = 1; // 蓝灯慢闪

            // 停止RING
            if (ring_timer != NULL)
            {
                xTimerStop(ring_timer, 0);
            }
            if (out_alert_timer != NULL)
            {
                xTimerStop(out_alert_timer, 0);
            }
            if (out_answer_timer != NULL)
            {
                xTimerStop(out_answer_timer, 0);
            }
        }
        break;
    }

    case ESP_HF_AUDIO_STATE_EVT:
        ESP_LOGI(TAG, "HFP音频状态: %s",
                 (param->audio_stat.state == ESP_HF_AUDIO_STATE_CONNECTED) ? "已连接" : "已断开");
        break;

    case ESP_HF_ATA_RESPONSE_EVT:
        // 车机按下了"接听"按钮
        ESP_LOGI(TAG, "🎯 车机发送接听命令");
        handle_call_answer();
        break;

    case ESP_HF_CHUP_RESPONSE_EVT:
        // 车机按下了"挂断/拒接"按钮
        ESP_LOGI(TAG, "🎯 车机发送挂断命令");
        if (get_call_state() == CALL_STATE_INCOMING)
        {
            handle_call_reject();
        }
        else if (get_call_state() == CALL_STATE_ACTIVE)
        {
            handle_call_hangup();
        }
        break;

    case ESP_HF_DIAL_EVT:
        // 车机发起拨号
        ESP_LOGI(TAG, "🎯 车机发起拨号");
        if (param->out_call.num_or_loc)
        {
            handle_call_dial(param->out_call.num_or_loc);
        }
        break;

    case ESP_HF_VOLUME_CONTROL_EVT:
        ESP_LOGI(TAG, "音量控制: type=%d, volume=%d",
                 param->volume_control.type, param->volume_control.volume);
        break;

    case ESP_HF_BVRA_RESPONSE_EVT:
        ESP_LOGI(TAG, "语音识别: %s",
                 param->vra_rep.value ? "启用" : "禁用");
        break;

    default:
        ESP_LOGD(TAG, "HFP未处理事件: %d", event);
        break;
    }
}

/* ===================== GAP事件回调 ===================== */

static void bt_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    switch (event)
    {
    case ESP_BT_GAP_AUTH_CMPL_EVT:
    {
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS)
        {
            ESP_LOGI(TAG, "✓ 配对成功");
        }
        else
        {
            ESP_LOGE(TAG, "✗ 配对失败: %d", param->auth_cmpl.stat);
        }
        break;
    }
    case ESP_BT_GAP_MODE_CHG_EVT:
        ESP_LOGI(TAG, "GAP模式变化: %d", param->mode_chg.mode);
        break;

    default:
        ESP_LOGD(TAG, "GAP未处理事件: %d", event);
        break;
    }
}

/* ===================== 蓝牙初始化 ===================== */

static esp_err_t bt_init(void)
{
    esp_err_t ret;

    // 初始化蓝牙控制器
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ret = esp_bt_controller_init(&bt_cfg);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "蓝牙控制器初始化失败: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "蓝牙控制器启用失败: %s", esp_err_to_name(ret));
        return ret;
    }

    // 初始化Bluedroid
    ret = esp_bluedroid_init();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Bluedroid初始化失败: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_bluedroid_enable();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Bluedroid启用失败: %s", esp_err_to_name(ret));
        return ret;
    }

    // 注册GAP回调
    esp_bt_gap_register_callback(bt_gap_cb);

    // 设置蓝牙设备名称（使用新API）
    esp_bt_gap_set_device_name(bt_name);

    // 设置可发现和可连接
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);

    // 设置PIN码
    esp_bt_pin_type_t pin_type = ESP_BT_PIN_TYPE_FIXED;
    esp_bt_pin_code_t pin_code = {'1', '2', '3', '4'};
    esp_bt_gap_set_pin(pin_type, 4, pin_code);

    // 初始化HFP AG
    ret = esp_hf_ag_register_callback(hfp_ag_callback);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "HFP AG回调注册失败: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_hf_ag_init();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "HFP AG初始化失败: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "✓ 蓝牙手机模拟器初始化成功");
    return ESP_OK;
}

static void bt_deinit(void)
{
    // 关闭HFP AG
    esp_hf_ag_deinit();

    // 关闭Bluedroid
    esp_bluedroid_disable();
    esp_bluedroid_deinit();

    // 关闭蓝牙控制器
    esp_bt_controller_disable();
    esp_bt_controller_deinit();

    hfp_connected = false;
    set_call_state(CALL_STATE_IDLE);

    ESP_LOGI(TAG, "蓝牙已关闭");
}

/* ===================== 按键任务 ===================== */

static void button_task(void *arg)
{
    int last_boot = 1;
    int last_call = 1;

    while (1)
    {
        int now_boot = gpio_get_level(BOOT_KEY);
        int now_call = gpio_get_level(CALL_KEY);

        // ========== BOOT按键 - 开关蓝牙 ==========
        if (last_boot == 1 && now_boot == 0)
        {
            vTaskDelay(pdMS_TO_TICKS(50)); // 消抖

            if (gpio_get_level(BOOT_KEY) == 0)
            {
                if (!bt_on)
                {
                    ESP_LOGI(TAG, "👆 启动蓝牙手机模拟器");
                    led_mode = 0;

                    esp_err_t ret = bt_init();
                    if (ret == ESP_OK)
                    {
                        bt_on = true;
                        led_mode = 1; // 蓝灯慢闪（待机）

                        ESP_LOGI(TAG, "");
                        ESP_LOGI(TAG, "🎉 蓝牙手机模拟器启动成功");
                        ESP_LOGI(TAG, "📱 请在车机上搜索并连接: %s", bt_name);
                        ESP_LOGI(TAG, "");
                    }
                    else
                    {
                        ESP_LOGE(TAG, "✗ 蓝牙启动失败: %s", esp_err_to_name(ret));
                        led_mode = 5; // 红灯快闪
                        vTaskDelay(pdMS_TO_TICKS(2000));
                        led_mode = 0;
                    }
                }
                else
                {
                    ESP_LOGI(TAG, "👆 关闭蓝牙手机模拟器");
                    bt_deinit();
                    bt_on = false;
                    led_mode = 0;
                }

                // 等待按键释放
                while (gpio_get_level(BOOT_KEY) == 0)
                {
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
                vTaskDelay(pdMS_TO_TICKS(50));
            }
        }

        // ========== CALL按键 - 模拟来电 ==========
        if (last_call == 1 && now_call == 0)
        {
            vTaskDelay(pdMS_TO_TICKS(50)); // 消抖

            if (gpio_get_level(CALL_KEY) == 0)
            {
                ESP_LOGI(TAG, "👆 触发模拟来电");
                simulate_incoming_call("13800138000");

                // 等待按键释放
                while (gpio_get_level(CALL_KEY) == 0)
                {
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
                vTaskDelay(pdMS_TO_TICKS(50));
            }
        }

        last_boot = now_boot;
        last_call = now_call;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* ===================== 主函数 ===================== */

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    state_mutex = xSemaphoreCreateMutex();
    if (state_mutex == NULL)
    {
        ESP_LOGE(TAG, "状态互斥锁创建失败");
        return;
    }

    // 读取MAC地址生成唯一标识
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    snprintf(my_mac_id, sizeof(my_mac_id), "%02X%02X", mac[4], mac[5]);
    snprintf(bt_name, sizeof(bt_name), "BT_Phone_%s", my_mac_id);

    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  蓝牙手机模拟器 (HFP AG) [%s]", my_mac_id);
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  MAC: %02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "  设备名: %s", bt_name);
    ESP_LOGI(TAG, "  PIN码: 1234");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "");

    // 初始化LED
    gpio_reset_pin(LED_R);
    gpio_reset_pin(LED_G);
    gpio_reset_pin(LED_B);
    gpio_set_direction(LED_R, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_G, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_B, GPIO_MODE_OUTPUT);
    led_off();

    // 初始化按键
    gpio_config_t boot_conf = {
        .pin_bit_mask = (1ULL << BOOT_KEY),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&boot_conf);

    gpio_config_t call_conf = {
        .pin_bit_mask = (1ULL << CALL_KEY),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&call_conf);

    // 创建任务
    xTaskCreate(led_task, "led", 2048, NULL, 5, NULL);
    xTaskCreate(button_task, "button", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "💡 系统就绪");
    ESP_LOGI(TAG, "💡 按BOOT键 (GPIO0) 启动蓝牙");
    ESP_LOGI(TAG, "💡 按CALL键 (GPIO23) 模拟来电");
    ESP_LOGI(TAG, "");
}
