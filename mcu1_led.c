#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"

// Classic Bluetooth
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_bt_device.h"
#include "esp_mac.h"

// 音乐和电话控制
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"
#include "esp_hf_client_api.h"

#define TAG "MCU"

// ========== 引脚定义 ==========
// 左旋码
#define BCD1_1 GPIO_NUM_35 // BIT1（之前错写成34）
#define BCD1_2 GPIO_NUM_34 // BIT2（之前错写成35）
#define BCD1_4 GPIO_NUM_33 // BIT4（之前错写成32）
#define BCD1_8 GPIO_NUM_32 // BIT8（之前错写成33）

// 右旋码
#define BCD2_1 GPIO_NUM_26 // BIT1（之前错写成25）
#define BCD2_2 GPIO_NUM_25 // BIT2（之前错写成26）
#define BCD2_4 GPIO_NUM_14 // BIT4（之前错写成27）
#define BCD2_8 GPIO_NUM_27 // BIT8（之前错写成14）
#define LED_R GPIO_NUM_18
#define LED_G GPIO_NUM_17
#define LED_B GPIO_NUM_16
#define BOOT_KEY GPIO_NUM_0

// ========== 全局变量 ==========
static char my_mac_id[8];
static char wifi_ssid[32];
static char bt_name[32];
static bool radio_on = false;
static int led_mode = 0;
static bool bt_connected = false;
static bool avrcp_connected = false;
static bool hfp_connected = false;
static uint8_t remote_bda[6] = {0};

// ========== 音乐元数据 ==========
typedef struct
{
    char title[64];
    char artist[64];
    char album[64];
    uint32_t total_tracks;
    uint32_t track_number;
    uint32_t playing_time; // 毫秒
} music_metadata_t;

// 模拟的音乐播放列表
static music_metadata_t playlist[] = {
    {"夜曲", "周杰伦", "十一月的萧邦", 12, 1, 223000},
    {"七里香", "周杰伦", "七里香", 10, 2, 299000},
    {"青花瓷", "周杰伦", "我很忙", 10, 3, 230000},
    {"稻香", "周杰伦", "魔杰座", 11, 4, 223000},
    {"告白气球", "周杰伦", "周杰伦的床边故事", 10, 5, 210000}};

static uint8_t current_track = 0;
static uint8_t playback_status = ESP_AVRC_PLAYBACK_STOPPED; // 停止、播放、暂停

// ========== 通讯录数据 ==========
typedef struct
{
    char name[32];
    char number[20];
} contact_t;

// 模拟的通讯录（可以自定义修改）
static contact_t phonebook[] = {
    {"张三", "13800138000"},
    {"李四", "13900139000"},
    {"王五", "13700137000"},
    {"赵六", "13600136000"},
    {"孙七", "13500135000"}};

static const uint8_t phonebook_size = sizeof(phonebook) / sizeof(contact_t);

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
            // 运行 - 绿灯慢闪（连接后常亮）
            if (bt_connected)
            {
                led_green();
                vTaskDelay(pdMS_TO_TICKS(500));
            }
            else
            {
                led_green();
                vTaskDelay(pdMS_TO_TICKS(500));
                led_off();
                vTaskDelay(pdMS_TO_TICKS(500));
            }
        }
        else if (led_mode == 3)
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

/* ===================== 旋码读取 ===================== */

static int read_bcd(gpio_num_t bit1, gpio_num_t bit2, gpio_num_t bit4, gpio_num_t bit8)
{
    int val = 0;
    val |= (gpio_get_level(bit1) == 0) ? 1 : 0;
    val |= (gpio_get_level(bit2) == 0) ? 2 : 0;
    val |= (gpio_get_level(bit4) == 0) ? 4 : 0;
    val |= (gpio_get_level(bit8) == 0) ? 8 : 0;
    return val;
}

/* ===================== 音乐控制函数 ===================== */

void music_play_pause(void)
{
    if (!avrcp_connected)
    {
        ESP_LOGW(TAG, "⚠️ AVRCP未连接，无法控制音乐");
        return;
    }
    // 先发PAUSE命令
    esp_avrc_ct_send_passthrough_cmd(0, ESP_AVRC_PT_CMD_PAUSE, ESP_AVRC_PT_CMD_STATE_PRESSED);
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_avrc_ct_send_passthrough_cmd(0, ESP_AVRC_PT_CMD_PAUSE, ESP_AVRC_PT_CMD_STATE_RELEASED);
    vTaskDelay(pdMS_TO_TICKS(200));
    // 再发PLAY命令（组合使用，提高兼容性）
    esp_avrc_ct_send_passthrough_cmd(0, ESP_AVRC_PT_CMD_PLAY, ESP_AVRC_PT_CMD_STATE_PRESSED);
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_avrc_ct_send_passthrough_cmd(0, ESP_AVRC_PT_CMD_PLAY, ESP_AVRC_PT_CMD_STATE_RELEASED);

    // 切换播放状态
    if (playback_status == ESP_AVRC_PLAYBACK_PLAYING)
    {
        playback_status = ESP_AVRC_PLAYBACK_PAUSED;
    }
    else
    {
        playback_status = ESP_AVRC_PLAYBACK_PLAYING;
    }

    ESP_LOGI(TAG, "🎵 播放/暂停");
}

void music_next(void)
{
    if (!avrcp_connected)
    {
        ESP_LOGW(TAG, "⚠️ AVRCP未连接，无法控制音乐");
        return;
    }
    esp_avrc_ct_send_passthrough_cmd(0, ESP_AVRC_PT_CMD_FORWARD, ESP_AVRC_PT_CMD_STATE_PRESSED);
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_avrc_ct_send_passthrough_cmd(0, ESP_AVRC_PT_CMD_FORWARD, ESP_AVRC_PT_CMD_STATE_RELEASED);

    // 切换到下一首
    current_track = (current_track + 1) % (sizeof(playlist) / sizeof(music_metadata_t));
    ESP_LOGI(TAG, "🎵 下一曲: %s - %s", playlist[current_track].artist, playlist[current_track].title);
}

void music_previous(void)
{
    if (!avrcp_connected)
    {
        ESP_LOGW(TAG, "⚠️ AVRCP未连接，无法控制音乐");
        return;
    }
    esp_avrc_ct_send_passthrough_cmd(0, ESP_AVRC_PT_CMD_BACKWARD, ESP_AVRC_PT_CMD_STATE_PRESSED);
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_avrc_ct_send_passthrough_cmd(0, ESP_AVRC_PT_CMD_BACKWARD, ESP_AVRC_PT_CMD_STATE_RELEASED);

    // 切换到上一首
    if (current_track == 0)
    {
        current_track = (sizeof(playlist) / sizeof(music_metadata_t)) - 1;
    }
    else
    {
        current_track--;
    }
    ESP_LOGI(TAG, "🎵 上一曲: %s - %s", playlist[current_track].artist, playlist[current_track].title);
}

void volume_up(void)
{
    if (!avrcp_connected)
    {
        ESP_LOGW(TAG, "⚠️ AVRCP未连接，无法控制音量");
        return;
    }
    esp_avrc_ct_send_passthrough_cmd(0, ESP_AVRC_PT_CMD_VOL_UP, ESP_AVRC_PT_CMD_STATE_PRESSED);
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_avrc_ct_send_passthrough_cmd(0, ESP_AVRC_PT_CMD_VOL_UP, ESP_AVRC_PT_CMD_STATE_RELEASED);
    ESP_LOGI(TAG, "🔊 音量+");
}

void volume_down(void)
{
    if (!avrcp_connected)
    {
        ESP_LOGW(TAG, "⚠️ AVRCP未连接，无法控制音量");
        return;
    }
    esp_avrc_ct_send_passthrough_cmd(0, ESP_AVRC_PT_CMD_VOL_DOWN, ESP_AVRC_PT_CMD_STATE_PRESSED);
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_avrc_ct_send_passthrough_cmd(0, ESP_AVRC_PT_CMD_VOL_DOWN, ESP_AVRC_PT_CMD_STATE_RELEASED);
    ESP_LOGI(TAG, "🔉 音量-");
}

/* ===================== 电话控制函数 ===================== */

void call_answer(void)
{
    if (!bt_connected)
    {
        ESP_LOGW(TAG, "⚠️ 蓝牙未连接");
        return;
    }
    esp_hf_client_answer_call();
    ESP_LOGI(TAG, "📞 接听");
}

void call_hangup(void)
{
    if (!bt_connected)
    {
        ESP_LOGW(TAG, "⚠️ 蓝牙未连接");
        return;
    }
    esp_hf_client_reject_call();
    ESP_LOGI(TAG, "📞 挂断");
}

void call_redial(void)
{
    if (!bt_connected)
    {
        ESP_LOGW(TAG, "⚠️ 蓝牙未连接");
        return;
    }
    esp_hf_client_dial_memory(1);
    ESP_LOGI(TAG, "📞 重拨");
}

/* ===================== 蓝牙回调 ===================== */

static void bt_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    ESP_LOGI(TAG, "🔵 GAP事件: %d", event);

    switch (event)
    {
    case ESP_BT_GAP_AUTH_CMPL_EVT:
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS)
        {
            ESP_LOGI(TAG, "✓ 认证成功: %s", param->auth_cmpl.device_name);
            ESP_LOGI(TAG, "  设备地址: %02x:%02x:%02x:%02x:%02x:%02x",
                     param->auth_cmpl.bda[0], param->auth_cmpl.bda[1],
                     param->auth_cmpl.bda[2], param->auth_cmpl.bda[3],
                     param->auth_cmpl.bda[4], param->auth_cmpl.bda[5]);
        }
        else
        {
            ESP_LOGW(TAG, "✗ 认证失败: status=%d", param->auth_cmpl.stat);
        }
        break;

    case ESP_BT_GAP_PIN_REQ_EVT:
    {
        esp_bt_pin_code_t pin = {'1', '2', '3', '4'};
        esp_bt_gap_pin_reply(param->pin_req.bda, true, 4, pin);
        ESP_LOGI(TAG, "📌 PIN请求，使用PIN: 1234");
        break;
    }

    case ESP_BT_GAP_CFM_REQ_EVT:
        ESP_LOGI(TAG, "🔐 配对确认请求: %" PRIu32, param->cfm_req.num_val);
        esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
        break;

    case ESP_BT_GAP_KEY_NOTIF_EVT:
        ESP_LOGI(TAG, "🔑 配对密钥通知: %" PRIu32, param->key_notif.passkey);
        break;

    case ESP_BT_GAP_KEY_REQ_EVT:
        ESP_LOGI(TAG, "🔑 配对密钥请求");
        break;

    case ESP_BT_GAP_MODE_CHG_EVT:
        ESP_LOGI(TAG, "📡 模式变更: mode=%d", param->mode_chg.mode);
        break;

    default:
        ESP_LOGD(TAG, "GAP事件: %d", event);
        break;
    }
}

static void a2dp_callback(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    if (event == ESP_A2D_CONNECTION_STATE_EVT)
    {
        if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED)
        {
            memcpy(remote_bda, param->conn_stat.remote_bda, 6);
            bt_connected = true;
            ESP_LOGI(TAG, "✓ A2DP已连接");
            ESP_LOGI(TAG, "→ 等待AVRCP连接...");

            // ===== 主动发起HFP连接 =====
            ESP_LOGI(TAG, "→ 正在主动连接HFP...");
            esp_err_t ret = esp_hf_client_connect(remote_bda);
            if (ret == ESP_OK)
            {
                ESP_LOGI(TAG, "✓ HFP连接请求已发送");
            }
            else
            {
                ESP_LOGW(TAG, "✗ HFP连接请求失败: %s", esp_err_to_name(ret));
            }
        }
        else
        {
            bt_connected = false;
            avrcp_connected = false;
            ESP_LOGI(TAG, "A2DP已断开");
        }
    }
}

static void avrc_ct_callback(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param)
{
    ESP_LOGI(TAG, "🎵 AVRCP CT事件: %d", event);

    switch (event)
    {
    case ESP_AVRC_CT_CONNECTION_STATE_EVT:
        ESP_LOGI(TAG, "🎵 AVRCP CT连接状态: %d", param->conn_stat.connected);
        if (param->conn_stat.connected)
        {
            avrcp_connected = true;
            ESP_LOGI(TAG, "========================================");
            ESP_LOGI(TAG, "  ✓✓✓ AVRCP已连接！");
            ESP_LOGI(TAG, "  🎵 现在可以控制音乐了！");
            ESP_LOGI(TAG, "  设备地址: %02x:%02x:%02x:%02x:%02x:%02x",
                     param->conn_stat.remote_bda[0], param->conn_stat.remote_bda[1],
                     param->conn_stat.remote_bda[2], param->conn_stat.remote_bda[3],
                     param->conn_stat.remote_bda[4], param->conn_stat.remote_bda[5]);
            ESP_LOGI(TAG, "========================================");
            esp_avrc_ct_send_get_rn_capabilities_cmd(0);
        }
        else
        {
            avrcp_connected = false;
            ESP_LOGW(TAG, "🎵 AVRCP已断开");
        }
        break;

    case ESP_AVRC_CT_PASSTHROUGH_RSP_EVT:
        ESP_LOGI(TAG, "✓ 控制成功: key=%d, state=%d",
                 param->psth_rsp.key_code, param->psth_rsp.key_state);
        break;

    case ESP_AVRC_CT_METADATA_RSP_EVT:
        ESP_LOGI(TAG, "📀 收到元数据响应 (attr_id=0x%x, attr_text=%s)",
                 param->meta_rsp.attr_id,
                 param->meta_rsp.attr_text ? (char *)param->meta_rsp.attr_text : "NULL");
        break;

    case ESP_AVRC_CT_REMOTE_FEATURES_EVT:
        ESP_LOGI(TAG, "设备特性: 0x%" PRIx32, (uint32_t)param->rmt_feats.feat_mask);
        if (param->rmt_feats.feat_mask & ESP_AVRC_FEAT_RCTG)
        {
            ESP_LOGI(TAG, "  ✓ 支持远程控制");
        }
        else
        {
            ESP_LOGW(TAG, "  ⚠️ 设备可能不支持远程控制");
        }
        break;

    case ESP_AVRC_CT_GET_RN_CAPABILITIES_RSP_EVT:
        ESP_LOGI(TAG, "📋 设备通知能力: count=%d",
                 param->get_rn_caps_rsp.cap_count);
        break;

    default:
        ESP_LOGD(TAG, "AVRCP CT事件: %d", event);
        break;
    }
}

// AVRCP Target回调 - 简化版本（ESP-IDF 5.4支持有限）
static void avrc_tg_callback(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param)
{
    ESP_LOGI(TAG, "🎵 AVRCP TG事件: %d", event);

    switch (event)
    {
    case ESP_AVRC_TG_CONNECTION_STATE_EVT:
        ESP_LOGI(TAG, "🎵 AVRCP TG连接状态: %d", param->conn_stat.connected);
        if (param->conn_stat.connected)
        {
            ESP_LOGI(TAG, "  ✓ AVRCP TG已连接");
        }
        else
        {
            ESP_LOGI(TAG, "  AVRCP TG已断开");
        }
        break;

    case ESP_AVRC_TG_SET_PLAYER_APP_VALUE_EVT:
        ESP_LOGI(TAG, "🎵 车机设置播放器参数");
        break;

    case ESP_AVRC_TG_SET_ABSOLUTE_VOLUME_CMD_EVT:
        ESP_LOGI(TAG, "🔊 车机设置音量: %d", param->set_abs_vol.volume);
        break;

    case ESP_AVRC_TG_REGISTER_NOTIFICATION_EVT:
        ESP_LOGI(TAG, "📋 车机注册通知: event_id=%d", param->reg_ntf.event_id);
        break;

    case ESP_AVRC_TG_REMOTE_FEATURES_EVT:
        ESP_LOGI(TAG, "📋 远程设备特性: 0x%" PRIx32, (uint32_t)param->rmt_feats.feat_mask);
        break;

    default:
        ESP_LOGD(TAG, "AVRCP TG事件: %d", event);
        break;
    }
}

/*
   注意：ESP-IDF 5.4的AVRCP Target API功能有限
   无法主动推送元数据给车机，车机需要通过其他方式获取
   这里保留数据结构供将来使用
*/

// HFP Client回调函数
static void hfp_client_callback(esp_hf_client_cb_event_t event, esp_hf_client_cb_param_t *param)
{
    ESP_LOGI(TAG, "📞 HFP事件: %d", event);

    switch (event)
    {
    case ESP_HF_CLIENT_CONNECTION_STATE_EVT:
        ESP_LOGI(TAG, "📞 HFP连接状态变化: %d", param->conn_stat.state);
        if (param->conn_stat.state == ESP_HF_CLIENT_CONNECTION_STATE_CONNECTED)
        {
            hfp_connected = true;
            ESP_LOGI(TAG, "========================================");
            ESP_LOGI(TAG, "  ✓✓✓ HFP已连接！");
            ESP_LOGI(TAG, "  📞 电话功能已启用");
            ESP_LOGI(TAG, "  设备地址: %02x:%02x:%02x:%02x:%02x:%02x",
                     param->conn_stat.remote_bda[0], param->conn_stat.remote_bda[1],
                     param->conn_stat.remote_bda[2], param->conn_stat.remote_bda[3],
                     param->conn_stat.remote_bda[4], param->conn_stat.remote_bda[5]);
            ESP_LOGI(TAG, "========================================");
        }
        else if (param->conn_stat.state == ESP_HF_CLIENT_CONNECTION_STATE_DISCONNECTED)
        {
            hfp_connected = false;
            ESP_LOGI(TAG, "📞 HFP已断开");
        }
        else if (param->conn_stat.state == ESP_HF_CLIENT_CONNECTION_STATE_CONNECTING)
        {
            ESP_LOGI(TAG, "📞 HFP正在连接...");
        }
        else if (param->conn_stat.state == ESP_HF_CLIENT_CONNECTION_STATE_DISCONNECTING)
        {
            ESP_LOGI(TAG, "📞 HFP正在断开...");
        }
        else if (param->conn_stat.state == ESP_HF_CLIENT_CONNECTION_STATE_SLC_CONNECTED)
        {
            ESP_LOGI(TAG, "📞 HFP服务级连接已建立 (SLC)");

            // ===== 关键：SLC建立后，主动上报设备信息 =====
            vTaskDelay(pdMS_TO_TICKS(500)); // 等待车机稳定

            // 1. 查询并设置本机号码
            ESP_LOGI(TAG, "📱 请求获取本机号码...");
            esp_hf_client_query_current_calls();

            vTaskDelay(pdMS_TO_TICKS(200));

            // 2. 查询运营商信息
            ESP_LOGI(TAG, "📡 请求获取运营商信息...");
            esp_hf_client_query_current_operator_name();

            vTaskDelay(pdMS_TO_TICKS(200));

            // 3. 发送信号和电池状态指示
            ESP_LOGI(TAG, "📊 更新设备状态指示器...");
            // 注意：这些在ESP-IDF中可能需要通过AT命令发送
        }
        break;

    case ESP_HF_CLIENT_AUDIO_STATE_EVT:
        ESP_LOGI(TAG, "📞 音频状态: %s",
                 param->audio_stat.state == ESP_HF_CLIENT_AUDIO_STATE_CONNECTED ? "已连接" : param->audio_stat.state == ESP_HF_CLIENT_AUDIO_STATE_CONNECTING ? "正在连接"
                                                                                         : param->audio_stat.state == ESP_HF_CLIENT_AUDIO_STATE_DISCONNECTED ? "已断开"
                                                                                                                                                             : "未知");
        break;

    case ESP_HF_CLIENT_BVRA_EVT:
        ESP_LOGI(TAG, "📞 语音识别状态: %d", param->bvra.value);
        break;

    case ESP_HF_CLIENT_CIND_SERVICE_AVAILABILITY_EVT:
        ESP_LOGI(TAG, "📞 服务可用性: %d", param->service_availability.status);
        break;

    case ESP_HF_CLIENT_CIND_ROAMING_STATUS_EVT:
        ESP_LOGI(TAG, "📞 漫游状态: %d", param->roaming.status);
        break;

    case ESP_HF_CLIENT_CIND_SIGNAL_STRENGTH_EVT:
        ESP_LOGI(TAG, "📞 信号强度: %d", param->signal_strength.value);
        break;

    case ESP_HF_CLIENT_CIND_BATTERY_LEVEL_EVT:
        ESP_LOGI(TAG, "📞 电池电量: %d", param->battery_level.value);
        break;

    case ESP_HF_CLIENT_CIND_CALL_EVT:
        ESP_LOGI(TAG, "📞 通话状态: %d", param->call.status);
        break;

    case ESP_HF_CLIENT_CIND_CALL_SETUP_EVT:
        ESP_LOGI(TAG, "📞 通话建立状态: %d", param->call_setup.status);
        break;

    case ESP_HF_CLIENT_CIND_CALL_HELD_EVT:
        ESP_LOGI(TAG, "📞 通话保持状态: %d", param->call_held.status);
        break;

    case ESP_HF_CLIENT_RING_IND_EVT:
        ESP_LOGI(TAG, "📞📞📞 来电！");
        break;

    case ESP_HF_CLIENT_CLIP_EVT:
        ESP_LOGI(TAG, "📞 来电号码: %s", param->clip.number);
        break;

    case ESP_HF_CLIENT_CLCC_EVT:
        ESP_LOGI(TAG, "📞 通话列表: idx=%d, dir=%d, status=%d, mpty=%d",
                 param->clcc.idx, param->clcc.dir, param->clcc.status,
                 param->clcc.mpty);
        if (param->clcc.number)
        {
            ESP_LOGI(TAG, "📞 号码: %s", param->clcc.number);
        }
        break;

    case ESP_HF_CLIENT_CNUM_EVT:
        ESP_LOGI(TAG, "📱 本机号码: %s (类型: %d)",
                 param->cnum.number, param->cnum.type);
        // 车机可能用这个来确认是手机设备
        break;

    case ESP_HF_CLIENT_COPS_CURRENT_OPERATOR_EVT:
        ESP_LOGI(TAG, "📡 运营商: %s", param->cops.name);
        // 车机可能用这个来显示网络状态
        break;

    case ESP_HF_CLIENT_BTRH_EVT:
        ESP_LOGI(TAG, "📞 响应和保持状态: %d", param->btrh.status);
        break;

    case ESP_HF_CLIENT_AT_RESPONSE_EVT:
        ESP_LOGI(TAG, "📞 AT命令响应: code=%d", param->at_response.code);
        if (param->at_response.code == ESP_HF_AT_RESPONSE_CODE_OK)
        {
            ESP_LOGI(TAG, "  ✓ AT命令成功");
        }
        else if (param->at_response.code == ESP_HF_AT_RESPONSE_CODE_ERR)
        {
            ESP_LOGW(TAG, "  ✗ AT命令失败");
        }
        else if (param->at_response.code == ESP_HF_AT_RESPONSE_CODE_CME)
        {
            ESP_LOGW(TAG, "  ✗ CME错误: %d", param->at_response.cme);
        }
        break;

    case ESP_HF_CLIENT_VOLUME_CONTROL_EVT:
        ESP_LOGI(TAG, "🔊 HFP音量控制: type=%d, volume=%d",
                 param->volume_control.type, param->volume_control.volume);
        break;

    default:
        ESP_LOGD(TAG, "📞 未处理的HFP事件: %d", event);
        break;
    }
}

/* ===================== 蓝牙初始化 ===================== */

static esp_err_t bt_classic_init(void)
{
    esp_err_t ret;
    ESP_LOGI(TAG, "启动蓝牙...");

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    bt_cfg.mode = ESP_BT_MODE_CLASSIC_BT;

    ret = esp_bt_controller_init(&bt_cfg);
    if (ret)
        return ret;
    ret = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);
    if (ret)
        return ret;
    ret = esp_bluedroid_init();
    if (ret)
        return ret;
    ret = esp_bluedroid_enable();
    if (ret)
        return ret;

    esp_bt_gap_register_callback(bt_gap_cb);

    // 伪装成智能手机 - 车机通常优先识别手机
    // Major: Phone (0x02), Minor: Smartphone (0x0C)
    esp_bt_cod_t cod = {
        .major = 0x02, // Phone (手机)
        .minor = 0x0C, // Smartphone (智能手机)
        .service = ESP_BT_COD_SRVC_TELEPHONY | ESP_BT_COD_SRVC_AUDIO};
    esp_bt_gap_set_cod(cod, ESP_BT_SET_COD_MAJOR_MINOR);

    // 设置设备名称
    esp_bt_gap_set_device_name(bt_name);

    // 打印COD配置信息（用于调试）
    ESP_LOGI(TAG, "蓝牙COD配置:");
    ESP_LOGI(TAG, "  Major Class: 0x%02X (Phone)", cod.major);
    ESP_LOGI(TAG, "  Minor Class: 0x%02X (Smartphone)", cod.minor);
    ESP_LOGI(TAG, "  Service: 0x%06X (Telephony+Audio)", cod.service);
    ESP_LOGI(TAG, "  完整COD: 0x%02X%02X%02X",
             (cod.service >> 13) & 0xFF, cod.major, cod.minor);

    // 设置可发现和可连接模式
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);

    // 增加发射功率
    esp_bredr_tx_power_set(ESP_PWR_LVL_P9, ESP_PWR_LVL_P9);
    ESP_LOGI(TAG, "蓝牙发射功率已设置为最大");

    // ===== 重要：AVRCP必须在A2DP之前初始化 =====
    ESP_LOGI(TAG, "正在初始化AVRCP...");

    // AVRCP Controller初始化
    ret = esp_avrc_ct_init();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "✗ AVRCP CT初始化失败: %s", esp_err_to_name(ret));
        return ret;
    }
    esp_avrc_ct_register_callback(avrc_ct_callback);
    ESP_LOGI(TAG, "✓ AVRCP Controller已初始化");

    // AVRCP Target初始化
    ret = esp_avrc_tg_init();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "✗ AVRCP Target初始化失败: %s", esp_err_to_name(ret));
        return ret;
    }
    esp_avrc_tg_register_callback(avrc_tg_callback);
    ESP_LOGI(TAG, "✓ AVRCP Target已初始化");

    // ===== A2DP Source初始化 =====
    ESP_LOGI(TAG, "正在初始化A2DP Source...");
    esp_a2d_register_callback(a2dp_callback);
    ret = esp_a2d_source_init();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "✗ A2DP Source初始化失败: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "✓ A2DP Source已初始化");

    // ===== HFP Client初始化 =====
    ESP_LOGI(TAG, "正在初始化HFP Client...");
    esp_hf_client_register_callback(hfp_client_callback);
    ret = esp_hf_client_init();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "✗ HFP初始化失败: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "✓ HFP Client已初始化");

    ESP_LOGI(TAG, "🎉 蓝牙启动完成: %s", bt_name);
    ESP_LOGI(TAG, "📋 已启用协议: A2DP Source, AVRCP CT/TG, HFP Client");
    return ESP_OK;
}

static void bt_classic_deinit(void)
{
    esp_hf_client_deinit();
    esp_avrc_tg_deinit();
    esp_avrc_ct_deinit();
    esp_a2d_source_deinit();
    esp_bluedroid_disable();
    esp_bluedroid_deinit();
    esp_bt_controller_disable();
    esp_bt_controller_deinit();
    bt_connected = false;
    avrcp_connected = false;
    hfp_connected = false;
    ESP_LOGI(TAG, "蓝牙已停止");
}

/* ===================== WiFi ===================== */

static esp_err_t wifi_init(void)
{
    esp_err_t ret;
    ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE)
        return ret;
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE)
        return ret;

    esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&cfg);
    if (ret)
        return ret;

    wifi_config_t ap_cfg = {
        .ap = {
            .ssid_len = strlen(wifi_ssid),
            .password = "123456789",
            .channel = 6,
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK,
            .pmf_cfg = {.required = false},
        },
    };
    strcpy((char *)ap_cfg.ap.ssid, wifi_ssid);

    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    ret = esp_wifi_start();
    if (ret == ESP_OK)
    {
        ESP_LOGI(TAG, "✓ WiFi: %s (密码: 123456789)", wifi_ssid);
    }
    return ret;
}

static void wifi_deinit(void)
{
    esp_wifi_stop();
    esp_wifi_deinit();
}

/* ===================== 旋码监听任务 ===================== */

static void switch_monitor_task(void *arg)
{
    int last_left = -1;
    int last_right = -1;

    // 旋钮1状态机: 0=初始态, 1=已到1, 2=已到9
    int knob1_state = 0;
    // 旋钮2状态机: 0=初始态, 1=已到1
    int knob2_state = 0;

    while (1)
    {
        int left = read_bcd(BCD1_1, BCD1_2, BCD1_4, BCD1_8);
        int right = read_bcd(BCD2_1, BCD2_2, BCD2_4, BCD2_8);

        // ========== 旋钮1逻辑 (音乐切换) ==========
        if (left != last_left)
        {
            ESP_LOGI(TAG, "旋钮1: %d", left);

            // 状态机转换
            if (knob1_state == 0 && left == 0)
            {
                // 保持在初始态
            }
            else if (knob1_state == 0 && left == 1)
            {
                // 从0转到1
                knob1_state = 1;
                ESP_LOGI(TAG, "[旋钮1] 检测到转到位置1");
            }
            else if (knob1_state == 0 && left == 9)
            {
                // 从0转到9
                knob1_state = 2;
                ESP_LOGI(TAG, "[旋钮1] 检测到转到位置9");
            }
            else if (knob1_state == 1 && left == 0)
            {
                // 从1回到0 → 上一首
                ESP_LOGI(TAG, "🎵 触发: 上一首");
                music_previous();
                knob1_state = 0;
            }
            else if (knob1_state == 2 && left == 0)
            {
                // 从9回到0 → 下一首
                ESP_LOGI(TAG, "🎵 触发: 下一首");
                music_next();
                knob1_state = 0;
            }
            else
            {
                // 其他转换 → 重置状态
                knob1_state = 0;
                ESP_LOGI(TAG, "[旋钮1] 状态重置");
            }

            last_left = left;
        }

        // ========== 旋钮2逻辑 (播放/暂停) ==========
        if (right != last_right)
        {
            ESP_LOGI(TAG, "旋钮2: %d", right);

            // 状态机转换
            if (knob2_state == 0 && right == 0)
            {
                // 保持在初始态
            }
            else if (knob2_state == 0 && right == 1)
            {
                // 从0转到1
                knob2_state = 1;
                ESP_LOGI(TAG, "[旋钮2] 检测到转到位置1");
            }
            else if (knob2_state == 1 && right == 0)
            {
                // 从1回到0 → 播放/暂停
                ESP_LOGI(TAG, "🎵 触发: 播放/暂停");
                music_play_pause();
                knob2_state = 0;
            }
            else
            {
                // 其他转换 → 重置状态
                knob2_state = 0;
                ESP_LOGI(TAG, "[旋钮2] 状态重置");
            }

            last_right = right;
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/* ===================== 按键任务 ===================== */

static void button_task(void *arg)
{
    int last = 1;

    while (1)
    {
        int now = gpio_get_level(BOOT_KEY);

        if (last == 1 && now == 0)
        {
            vTaskDelay(pdMS_TO_TICKS(50));

            if (gpio_get_level(BOOT_KEY) == 0)
            {
                if (!radio_on)
                {
                    // 启动
                    ESP_LOGI(TAG, "");
                    ESP_LOGI(TAG, "========================================");
                    ESP_LOGI(TAG, "  启动 WiFi+蓝牙...");
                    ESP_LOGI(TAG, "========================================");
                    led_mode = 0;

                    esp_err_t bt_ret = bt_classic_init();
                    vTaskDelay(pdMS_TO_TICKS(500));
                    esp_err_t wifi_ret = wifi_init();

                    if (bt_ret == ESP_OK && wifi_ret == ESP_OK)
                    {
                        radio_on = true;
                        led_mode = 2; // 绿灯
                        ESP_LOGI(TAG, "");
                        ESP_LOGI(TAG, "🎉 启动成功");
                        ESP_LOGI(TAG, "");
                    }
                    else
                    {
                        ESP_LOGE(TAG, "✗ 启动失败 (BT:%s WiFi:%s)",
                                 esp_err_to_name(bt_ret),
                                 esp_err_to_name(wifi_ret));
                        bt_classic_deinit();
                        wifi_deinit();
                        led_mode = 3;
                        vTaskDelay(pdMS_TO_TICKS(2000));
                        led_mode = 1;
                    }
                }
                else
                {
                    // 关闭
                    ESP_LOGI(TAG, "");
                    ESP_LOGI(TAG, "关闭 WiFi+蓝牙");
                    bt_classic_deinit();
                    wifi_deinit();
                    radio_on = false;
                    led_mode = 1;
                }

                // 等待按键释放
                while (gpio_get_level(BOOT_KEY) == 0)
                {
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
                vTaskDelay(pdMS_TO_TICKS(50));
            }
        }

        last = now;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* ===================== main ===================== */

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 读取MAC地址生成唯一标识
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    snprintf(my_mac_id, sizeof(my_mac_id), "%02X%02X", mac[4], mac[5]);
    snprintf(wifi_ssid, sizeof(wifi_ssid), "ESP32_%s", my_mac_id);
    snprintf(bt_name, sizeof(bt_name), "ESP32_%s", my_mac_id);

    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  ESP32 蓝牙控制器 [%s]", my_mac_id);
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  MAC: %02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "  WiFi: %s (密码: 123456789)", wifi_ssid);
    ESP_LOGI(TAG, "  蓝牙: %s (PIN: 1234)", bt_name);
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  🎵 音乐播放列表:");
    for (int i = 0; i < sizeof(playlist) / sizeof(music_metadata_t); i++)
    {
        ESP_LOGI(TAG, "    %d. %s - %s", i + 1, playlist[i].artist, playlist[i].title);
    }
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  📞 通讯录:");
    for (int i = 0; i < phonebook_size; i++)
    {
        ESP_LOGI(TAG, "    %s: %s", phonebook[i].name, phonebook[i].number);
    }
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

    // 初始化BOOT按键
    gpio_config_t boot_conf = {
        .pin_bit_mask = (1ULL << BOOT_KEY),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&boot_conf);

    // 初始化旋码开关输入
    gpio_config_t bcd_conf = {
        .pin_bit_mask = (1ULL << BCD1_4) | (1ULL << BCD1_8) |
                        (1ULL << BCD2_1) | (1ULL << BCD2_2) |
                        (1ULL << BCD2_4) | (1ULL << BCD2_8),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&bcd_conf);

    // GPIO34/35单独配置（没有上拉）
    gpio_set_direction(BCD1_1, GPIO_MODE_INPUT);
    gpio_set_direction(BCD1_2, GPIO_MODE_INPUT);

    vTaskDelay(pdMS_TO_TICKS(100));

    // 读取当前旋码状态
    int left = read_bcd(BCD1_1, BCD1_2, BCD1_4, BCD1_8);
    int right = read_bcd(BCD2_1, BCD2_2, BCD2_4, BCD2_8);
    ESP_LOGI(TAG, "当前旋钮: 旋钮1=%d, 旋钮2=%d", left, right);
    ESP_LOGI(TAG, "");

    // 创建任务
    xTaskCreate(led_task, "led", 2048, NULL, 5, NULL);
    xTaskCreate(switch_monitor_task, "switch", 3072, NULL, 5, NULL);
    xTaskCreate(button_task, "button", 4096, NULL, 5, NULL);

    // 开机进入待机状态
    led_mode = 1;

    ESP_LOGI(TAG, "💡 系统就绪 - 待机中（蓝灯闪烁）");
    ESP_LOGI(TAG, "💡 按BOOT键启动WiFi+蓝牙");
    ESP_LOGI(TAG, "💡 转动旋钮查看识别结果");
    ESP_LOGI(TAG, "");
}