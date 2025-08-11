/*
 * SPDX-FileCopyrightText: 2023-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_check.h"
#include "nvs_flash.h"
#include "app_ui_ctrl.h"
#include "OpenAI.h"
#include "audio_player.h"
#include "app_sr.h"
#include "bsp/esp-bsp.h"
#include "bsp_board.h"
#include "app_audio.h"
#include "app_wifi.h"
#include "settings.h"
#include "esp_http_client.h"

#define SCROLL_START_DELAY_S (1.5)
#define LISTEN_SPEAK_PANEL_DELAY_MS 2000
#define SERVER_ERROR "server_error"
#define INVALID_REQUEST_ERROR "invalid_request_error"
#define SORRY_CANNOT_UNDERSTAND "Sorry, I can't understand."
#define API_KEY_NOT_VALID "API Key is not valid"

static char *TAG = "app_main";
static sys_param_t *sys_param = NULL;

/* program flow. This function is called in app_audio.c */
esp_err_t start_customai(uint8_t *audio, int audio_len)
{
    esp_err_t ret = ESP_OK;
    char *post_data = NULL;
    int post_data_len = 0;
    esp_http_client_handle_t client = NULL;

    ESP_LOGI(TAG, "start_customai: audio_len = %d", audio_len);

    // 显示处理中界面
    ui_ctrl_show_panel(UI_CTRL_PANEL_GET, 0);

    // 构建 multipart/form-data 请求体
    const char *boundary = "----WebKitFormBoundary7MA4YWxkTrZu0gW";

    // 计算所需内存大小
    int estimated_size = audio_len + 1024; // 音频数据 + 表单头部信息
    post_data = malloc(estimated_size);
    if (!post_data)
    {
        ESP_LOGE(TAG, "Failed to allocate memory for POST data");
        ret = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    // 构建完整的 multipart/form-data 请求体
    post_data_len = snprintf(post_data, 512,
                             "--%s\r\n"
                             "Content-Disposition: form-data; name=\"device_id\"\r\n"
                             "\r\n"
                             "1\r\n"
                             "--%s\r\n"
                             "Content-Disposition: form-data; name=\"audio_file\"; filename=\"audio.wav\"\r\n"
                             "Content-Type: audio/wav\r\n"
                             "\r\n",
                             boundary, boundary);

    // 添加音频数据
    memcpy(post_data + post_data_len, audio, audio_len);
    post_data_len += audio_len;

    // 添加结束边界
    int ending_len = snprintf(post_data + post_data_len, 64, "\r\n--%s--\r\n", boundary);
    post_data_len += ending_len;

    ESP_LOGI(TAG, "Total POST data length: %d", post_data_len);

    // HTTP客户端配置 - 按照官方文档标准配置
    esp_http_client_config_t config = {
        .url = "http://wawa.suanzillanxian.cn/upload_audio",
        .method = HTTP_METHOD_POST,
        .timeout_ms = 30000,
    };

    client = esp_http_client_init(&config);
    if (client == NULL)
    {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        ret = ESP_FAIL;
        goto cleanup;
    }

    // 设置Content-Type头部
    char content_type[128];
    snprintf(content_type, sizeof(content_type), "multipart/form-data; boundary=%s", boundary);
    esp_http_client_set_header(client, "Content-Type", content_type);

    // 设置POST数据 - 使用官方API
    esp_http_client_set_post_field(client, post_data, post_data_len);

    // 执行HTTP请求 - 官方推荐的方法
    ret = esp_http_client_perform(client);
    if (ret == ESP_OK)
    {
        int status_code = esp_http_client_get_status_code(client);
        int content_length = esp_http_client_get_content_length(client);

        ESP_LOGI(TAG, "HTTP POST Status = %d, content_length = %d", status_code, content_length);

        if (status_code == 200)
        {
            // 读取响应数据
            if (content_length > 0)
            {
                char *response_buffer = malloc(content_length + 1);
                if (response_buffer)
                {
                    int data_read = esp_http_client_read(client, response_buffer, content_length);
                    if (data_read > 0)
                    {
                        response_buffer[data_read] = '\0';
                        ESP_LOGI(TAG, "Response: %s", response_buffer);

                        // 简单解析JSON响应
                        if (strstr(response_buffer, "\"status\":") && strstr(response_buffer, "\"ok\""))
                        {
                            // 成功响应
                            ui_ctrl_label_show_text(UI_CTRL_LABEL_LISTEN_SPEAK, "Upload successful");
                            ui_ctrl_label_show_text(UI_CTRL_LABEL_REPLY_CONTENT, "Audio processed successfully");
                            ui_ctrl_show_panel(UI_CTRL_PANEL_REPLY, 0);

                            // 显示3秒后返回睡眠状态
                            vTaskDelay(pdMS_TO_TICKS(3000));
                            ui_ctrl_show_panel(UI_CTRL_PANEL_SLEEP, 0);
                        }
                        else
                        {
                            // 服务器返回错误
                            ESP_LOGE(TAG, "Server returned error in response");
                            ui_ctrl_label_show_text(UI_CTRL_LABEL_LISTEN_SPEAK, "Server error");
                            ui_ctrl_show_panel(UI_CTRL_PANEL_SLEEP, LISTEN_SPEAK_PANEL_DELAY_MS);
                            ret = ESP_FAIL;
                        }
                    }
                    free(response_buffer);
                }
                else
                {
                    ESP_LOGE(TAG, "Failed to allocate response buffer");
                    ret = ESP_ERR_NO_MEM;
                }
            }
            else
            {
                ESP_LOGI(TAG, "Empty response from server");
                ui_ctrl_label_show_text(UI_CTRL_LABEL_LISTEN_SPEAK, "Empty response");
                ui_ctrl_show_panel(UI_CTRL_PANEL_SLEEP, LISTEN_SPEAK_PANEL_DELAY_MS);
            }
        }
        else
        {
            ESP_LOGE(TAG, "HTTP request failed with status: %d", status_code);
            ui_ctrl_label_show_text(UI_CTRL_LABEL_LISTEN_SPEAK, "Upload failed");
            ui_ctrl_show_panel(UI_CTRL_PANEL_SLEEP, LISTEN_SPEAK_PANEL_DELAY_MS);
            ret = ESP_FAIL;
        }
    }
    else
    {
        ESP_LOGE(TAG, "HTTP POST request failed: %s", esp_err_to_name(ret));
        ui_ctrl_label_show_text(UI_CTRL_LABEL_LISTEN_SPEAK, "Connection failed");
        ui_ctrl_show_panel(UI_CTRL_PANEL_SLEEP, LISTEN_SPEAK_PANEL_DELAY_MS);
    }

cleanup:
    // 清理资源 - 按照官方文档要求
    if (client)
    {
        esp_http_client_cleanup(client);
    }
    if (post_data)
    {
        free(post_data);
    }

    return ret;
}
/* program flow. This function is called in app_audio.c */
esp_err_t start_openai(uint8_t *audio, int audio_len)
{
    esp_err_t ret = ESP_OK;
    static OpenAI_t *openai = NULL;
    static OpenAI_AudioTranscription_t *audioTranscription = NULL;
    static OpenAI_ChatCompletion_t *chatCompletion = NULL;
    static OpenAI_AudioSpeech_t *audioSpeech = NULL;

    OpenAI_SpeechResponse_t *speechresult = NULL;
    OpenAI_StringResponse_t *result = NULL;
    FILE *fp = NULL;

    if (openai == NULL)
    {
        openai = OpenAICreate(sys_param->key);
        ESP_RETURN_ON_FALSE(NULL != openai, ESP_ERR_INVALID_ARG, TAG, "OpenAICreate faield");

        OpenAIChangeBaseURL(openai, sys_param->url);

        audioTranscription = openai->audioTranscriptionCreate(openai);
        chatCompletion = openai->chatCreate(openai);
        audioSpeech = openai->audioSpeechCreate(openai);

        audioTranscription->setResponseFormat(audioTranscription, OPENAI_AUDIO_RESPONSE_FORMAT_JSON);
        audioTranscription->setLanguage(audioTranscription, "en");
        audioTranscription->setTemperature(audioTranscription, 0.2);

        chatCompletion->setModel(chatCompletion, "gpt-3.5-turbo");
        chatCompletion->setSystem(chatCompletion, "user");
        chatCompletion->setMaxTokens(chatCompletion, CONFIG_MAX_TOKEN);
        chatCompletion->setTemperature(chatCompletion, 0.2);
        chatCompletion->setStop(chatCompletion, "\r");
        chatCompletion->setPresencePenalty(chatCompletion, 0);
        chatCompletion->setFrequencyPenalty(chatCompletion, 0);
        chatCompletion->setUser(chatCompletion, "OpenAI-ESP32");

        audioSpeech->setModel(audioSpeech, "tts-1");
        audioSpeech->setVoice(audioSpeech, "nova");
        audioSpeech->setResponseFormat(audioSpeech, OPENAI_AUDIO_OUTPUT_FORMAT_MP3);
        audioSpeech->setSpeed(audioSpeech, 1.0);
    }

    ui_ctrl_show_panel(UI_CTRL_PANEL_GET, 0);

    // OpenAI Audio Transcription
    char *text = audioTranscription->file(audioTranscription, (uint8_t *)audio, audio_len, OPENAI_AUDIO_INPUT_FORMAT_WAV);

    if (NULL == text)
    {
        ret = ESP_ERR_INVALID_RESPONSE;
        ui_ctrl_label_show_text(UI_CTRL_LABEL_LISTEN_SPEAK, INVALID_REQUEST_ERROR);
        ESP_GOTO_ON_ERROR(ret, err, TAG, "[audioTranscription]: invalid url");
    }

    if (strstr(text, "\"code\": "))
    {
        ret = ESP_ERR_INVALID_RESPONSE;
        ui_ctrl_label_show_text(UI_CTRL_LABEL_LISTEN_SPEAK, text);
        ESP_GOTO_ON_ERROR(ret, err, TAG, "[audioTranscription]: invalid response");
    }

    if (strcmp(text, INVALID_REQUEST_ERROR) == 0 || strcmp(text, SERVER_ERROR) == 0)
    {
        ret = ESP_ERR_INVALID_RESPONSE;
        ui_ctrl_label_show_text(UI_CTRL_LABEL_LISTEN_SPEAK, SORRY_CANNOT_UNDERSTAND);
        ui_ctrl_show_panel(UI_CTRL_PANEL_SLEEP, LISTEN_SPEAK_PANEL_DELAY_MS);
        ESP_GOTO_ON_ERROR(ret, err, TAG, "[audioTranscription]: invalid response");
    }

    // UI listen success
    ui_ctrl_label_show_text(UI_CTRL_LABEL_REPLY_QUESTION, text);
    ui_ctrl_label_show_text(UI_CTRL_LABEL_LISTEN_SPEAK, text);

    // OpenAI Chat Completion
    result = chatCompletion->message(chatCompletion, text, false);
    if (NULL == result)
    {
        ret = ESP_ERR_INVALID_RESPONSE;
        ESP_GOTO_ON_ERROR(ret, err, TAG, "[chatCompletion]: invalid response");
    }

    char *response = result->getData(result, 0);

    if (response != NULL && (strcmp(response, INVALID_REQUEST_ERROR) == 0 || strcmp(response, SERVER_ERROR) == 0))
    {
        // UI listen fail
        ret = ESP_ERR_INVALID_RESPONSE;
        ui_ctrl_label_show_text(UI_CTRL_LABEL_LISTEN_SPEAK, SORRY_CANNOT_UNDERSTAND);
        ui_ctrl_show_panel(UI_CTRL_PANEL_SLEEP, LISTEN_SPEAK_PANEL_DELAY_MS);
        ESP_GOTO_ON_ERROR(ret, err, TAG, "[chatCompletion]: invalid response");
    }

    // UI listen success
    ui_ctrl_label_show_text(UI_CTRL_LABEL_REPLY_QUESTION, text);
    ui_ctrl_label_show_text(UI_CTRL_LABEL_LISTEN_SPEAK, response);

    if (strcmp(response, INVALID_REQUEST_ERROR) == 0)
    {
        ret = ESP_ERR_INVALID_RESPONSE;
        ui_ctrl_label_show_text(UI_CTRL_LABEL_LISTEN_SPEAK, SORRY_CANNOT_UNDERSTAND);
        ui_ctrl_show_panel(UI_CTRL_PANEL_SLEEP, LISTEN_SPEAK_PANEL_DELAY_MS);
        ESP_GOTO_ON_ERROR(ret, err, TAG, "[chatCompletion]: invalid response");
    }

    ui_ctrl_label_show_text(UI_CTRL_LABEL_REPLY_CONTENT, response);
    ui_ctrl_show_panel(UI_CTRL_PANEL_REPLY, 0);

    // OpenAI Speech Response
    speechresult = audioSpeech->speech(audioSpeech, response);
    if (NULL == speechresult)
    {
        ret = ESP_ERR_INVALID_RESPONSE;
        ui_ctrl_show_panel(UI_CTRL_PANEL_SLEEP, 5 * LISTEN_SPEAK_PANEL_DELAY_MS);
        fp = fopen("/spiffs/tts_failed.mp3", "r");
        if (fp)
        {
            audio_player_play(fp);
        }
        ESP_GOTO_ON_ERROR(ret, err, TAG, "[audioSpeech]: invalid response");
    }

    uint32_t dataLength = speechresult->getLen(speechresult);
    char *speechptr = speechresult->getData(speechresult);
    esp_err_t status = ESP_FAIL;
    fp = fmemopen((void *)speechptr, dataLength, "rb");
    if (fp)
    {
        status = audio_player_play(fp);
    }

    if (status != ESP_OK)
    {
        ESP_LOGE(TAG, "Error creating ChatGPT request: %s\n", esp_err_to_name(status));
        // UI reply audio fail
        ui_ctrl_show_panel(UI_CTRL_PANEL_SLEEP, 0);
    }
    else
    {
        // Wait a moment before starting to scroll the reply content
        vTaskDelay(pdMS_TO_TICKS(SCROLL_START_DELAY_S * 1000));
        ui_ctrl_reply_set_audio_start_flag(true);
    }

err:
    // Clearing resources
    if (speechresult)
    {
        speechresult->deleteResponse(speechresult);
    }

    if (result)
    {
        result->deleteResponse(result);
    }

    if (text)
    {
        free(text);
    }
    return ret;
}

/* play audio function */

static void audio_play_finish_cb(void)
{
    ESP_LOGI(TAG, "replay audio end");
    if (ui_ctrl_reply_get_audio_start_flag())
    {
        ui_ctrl_reply_set_audio_end_flag(true);
    }
}

void app_main()
{
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(settings_read_parameter_from_nvs());
    sys_param = settings_get_parameter();

    bsp_spiffs_mount();
    bsp_i2c_init();

    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = BSP_LCD_H_RES * CONFIG_BSP_LCD_DRAW_BUF_HEIGHT,
        .double_buffer = 0,
        .flags = {
            .buff_dma = true,
        }};
    bsp_display_start_with_config(&cfg);
    bsp_board_init();

    ESP_LOGI(TAG, "Display LVGL demo");
    bsp_display_backlight_on();
    ui_ctrl_init();
    app_network_start();

    ESP_LOGI(TAG, "speech recognition start");
    app_sr_start(false);
    audio_register_play_finish_cb(audio_play_finish_cb);

    while (true)
    {

        ESP_LOGD(TAG, "\tDescription\tInternal\tSPIRAM");
        ESP_LOGD(TAG, "Current Free Memory\t%d\t\t%d",
                 heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL),
                 heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        ESP_LOGD(TAG, "Min. Ever Free Size\t%d\t\t%d",
                 heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL),
                 heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));
        vTaskDelay(pdMS_TO_TICKS(5 * 1000));
    }
}
