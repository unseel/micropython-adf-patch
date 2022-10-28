/*
 * ESPRESSIF MIT License
 *
 * Copyright (c) 2019 <ESPRESSIF SYSTEMS (SHANGHAI) CO., LTD>
 *
 * Permission is hereby granted for use on all ESPRESSIF SYSTEMS products, in which case,
 * it is free of charge, to any person obtaining a copy of this software and associated
 * documentation files (the "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the Software is furnished
 * to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or
 * substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#include <stdio.h>
#include <string.h>

#include "py/objstr.h"
#include "py/runtime.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_audio.h"
#include "audio_pipeline.h"
#include "audio_element.h"
#include "audio_event_iface.h"
#include "audio_common.h"

#include "audio_hal.h"
#include "esp_peripherals.h"
#include "periph_wifi.h"
#include "board.h"
#include "esp_netif.h"

#include "mp3_decoder.h"
#include "wav_decoder.h"

#include "http_stream.h"
#include "i2s_stream.h"
// #include "vfs_stream.h"

static const char *TAG = "audio-player";

const mp_obj_type_t audio_player_type;

typedef struct _audio_player_obj_t {
    mp_obj_base_t base;
    mp_obj_t callback;

    esp_audio_handle_t player;
    esp_audio_state_t state;
} audio_player_obj_t;

STATIC const qstr player_info_fields[] = {
    MP_QSTR_input, MP_QSTR_codec
};

STATIC const MP_DEFINE_STR_OBJ(player_info_input_obj, "http|file stream");
STATIC const MP_DEFINE_STR_OBJ(player_info_codec_obj, "mp3|amr");

STATIC MP_DEFINE_ATTRTUPLE(
    player_info_obj,
    player_info_fields,
    2,
    (mp_obj_t)&player_info_input_obj,
    (mp_obj_t)&player_info_codec_obj);

STATIC mp_obj_t player_info(void)
{
    return (mp_obj_t)&player_info_obj;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_0(audio_player_info_obj, player_info);

STATIC void audio_state_cb(esp_audio_state_t *state, void *ctx)
{
    audio_player_obj_t *self = (audio_player_obj_t *)ctx;
    memcpy(&self->state, state, sizeof(esp_audio_state_t));
    if (self->callback != mp_const_none) {
        mp_obj_dict_t *dict = mp_obj_new_dict(3);

        mp_obj_dict_store(dict, MP_ROM_QSTR(MP_QSTR_status), MP_OBJ_TO_PTR(mp_obj_new_int(state->status)));
        mp_obj_dict_store(dict, MP_ROM_QSTR(MP_QSTR_err_msg), MP_OBJ_TO_PTR(mp_obj_new_int(state->err_msg)));
        mp_obj_dict_store(dict, MP_ROM_QSTR(MP_QSTR_media_src), MP_OBJ_TO_PTR(mp_obj_new_int(state->media_src)));

        mp_sched_schedule(self->callback, dict);
    }
}

STATIC int _http_stream_event_handle(http_stream_event_msg_t *msg)
{
    if (msg->event_id == HTTP_STREAM_RESOLVE_ALL_TRACKS) {
        return ESP_OK;
    }

    if (msg->event_id == HTTP_STREAM_FINISH_TRACK) {
        return http_stream_next_track(msg->el);
    }
    if (msg->event_id == HTTP_STREAM_FINISH_PLAYLIST) {
        return http_stream_restart(msg->el);
    }
    return ESP_OK;
}

STATIC esp_audio_handle_t audio_player_create(void)
{
    esp_netif_init();
    ESP_LOGI(TAG, "audio player create");
    // init audio board
    audio_board_handle_t board_handle = audio_board_init();
    audio_hal_ctrl_codec(board_handle->audio_hal, AUDIO_HAL_CODEC_MODE_BOTH, AUDIO_HAL_CTRL_START);

    // init player
    esp_audio_cfg_t cfg = DEFAULT_ESP_AUDIO_CONFIG();
    cfg.vol_handle = board_handle->audio_hal;
    cfg.vol_set = (audio_volume_set)audio_hal_set_volume;
    cfg.vol_get = (audio_volume_get)audio_hal_get_volume;
    cfg.resample_rate = 48000;
    cfg.prefer_type = ESP_AUDIO_PREFER_MEM;
    esp_audio_handle_t player = esp_audio_create(&cfg);

    // add input stream
    // fatfs stream
    // vfs_stream_cfg_t fs_reader = VFS_STREAM_CFG_DEFAULT();
    // fs_reader.type = AUDIO_STREAM_READER;
    // fs_reader.task_core = 1;
    // esp_audio_input_stream_add(player, vfs_stream_init(&fs_reader));
    // http stream
    http_stream_cfg_t http_cfg = HTTP_STREAM_CFG_DEFAULT();
    http_cfg.event_handle = _http_stream_event_handle;
    http_cfg.type = AUDIO_STREAM_READER;
    http_cfg.enable_playlist_parser = true;
    http_cfg.task_core = 1;
    audio_element_handle_t http_stream_reader = http_stream_init(&http_cfg);
    esp_audio_input_stream_add(player, http_stream_reader);

    // add decoder
    // mp3
    mp3_decoder_cfg_t mp3_dec_cfg = DEFAULT_MP3_DECODER_CONFIG();
    mp3_dec_cfg.task_core = 1;
    esp_audio_codec_lib_add(player, AUDIO_CODEC_TYPE_DECODER, mp3_decoder_init(&mp3_dec_cfg));
    // wav
    wav_decoder_cfg_t wav_dec_cfg = DEFAULT_WAV_DECODER_CONFIG();
    wav_dec_cfg.task_core = 1;
    esp_audio_codec_lib_add(player, AUDIO_CODEC_TYPE_DECODER, wav_decoder_init(&wav_dec_cfg));

    // Create writers and add to esp_audio
    i2s_stream_cfg_t i2s_writer = I2S_STREAM_CFG_DEFAULT();
    i2s_writer.type = AUDIO_STREAM_WRITER;
    i2s_writer.i2s_config.sample_rate = 48000;
    i2s_writer.task_core = 1;
    esp_audio_output_stream_add(player, i2s_stream_init(&i2s_writer));

    return player;
}

STATIC mp_obj_t audio_player_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args)
{
    mp_arg_check_num(n_args, n_kw, 1, 1, false);

    esp_netif_init();

    audio_pipeline_handle_t pipeline;
    audio_element_handle_t http_stream_reader, i2s_stream_writer, mp3_decoder;

    ESP_LOGI(TAG, "[ 1 ] Start audio codec chip");
    audio_board_handle_t board_handle = audio_board_init();
    audio_hal_ctrl_codec(board_handle->audio_hal, AUDIO_HAL_CODEC_MODE_DECODE, AUDIO_HAL_CTRL_START);

    ESP_LOGI(TAG, "[2.0] Create audio pipeline for playback");
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    pipeline = audio_pipeline_init(&pipeline_cfg);
    mem_assert(pipeline);

    ESP_LOGI(TAG, "[2.1] Create http stream to read data");
    http_stream_cfg_t http_cfg = HTTP_STREAM_CFG_DEFAULT();
    http_stream_reader = http_stream_init(&http_cfg);

    ESP_LOGI(TAG, "[2.2] Create i2s stream to write data to codec chip");
    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
    i2s_cfg.type = AUDIO_STREAM_WRITER;
    i2s_stream_writer = i2s_stream_init(&i2s_cfg);

    ESP_LOGI(TAG, "[2.3] Create mp3 decoder to decode mp3 file");
    mp3_decoder_cfg_t mp3_cfg = DEFAULT_MP3_DECODER_CONFIG();
    mp3_decoder = mp3_decoder_init(&mp3_cfg);

    ESP_LOGI(TAG, "[2.4] Register all elements to audio pipeline");
    audio_pipeline_register(pipeline, http_stream_reader, "http");
    audio_pipeline_register(pipeline, mp3_decoder,        "mp3");
    audio_pipeline_register(pipeline, i2s_stream_writer,  "i2s");

    ESP_LOGI(TAG, "[2.5] Link it together http_stream-->mp3_decoder-->i2s_stream-->[codec_chip]");
    const char *link_tag[3] = {"http", "mp3", "i2s"};
    audio_pipeline_link(pipeline, &link_tag[0], 3);

    ESP_LOGI(TAG, "[2.6] Set up  uri (http as http_stream, mp3 as mp3 decoder, and default output is i2s)");
    audio_element_set_uri(http_stream_reader, "http://www.hochmuth.com/mp3/Bloch_Prayer.mp3");

    ESP_LOGI(TAG, "[ 3 ] Start and wait for Wi-Fi network");
    // esp_periph_config_t periph_cfg = DEFAULT_ESP_PERIPH_SET_CONFIG();
    // esp_periph_set_handle_t set = esp_periph_set_init(&periph_cfg);
    // periph_wifi_cfg_t wifi_cfg = {
    //     .ssid = "Shudan",
    //     .password = "1q2w3e4r5t6Y",
    // };
    // esp_periph_handle_t wifi_handle = periph_wifi_init(&wifi_cfg);
    // esp_periph_start(set, wifi_handle);
    // periph_wifi_wait_for_connected(wifi_handle, portMAX_DELAY);
    
    // Example of using an audio event -- START
    ESP_LOGI(TAG, "[ 4 ] Set up  event listener");
    audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
    audio_event_iface_handle_t evt = audio_event_iface_init(&evt_cfg);

    ESP_LOGI(TAG, "[4.1] Listening event from all elements of pipeline");
    audio_pipeline_set_listener(pipeline, evt);

    ESP_LOGI(TAG, "[4.2] Listening event from peripherals");
    // audio_event_iface_set_listener(esp_periph_set_get_event_iface(set), evt);

    ESP_LOGI(TAG, "[ 5 ] Start audio_pipeline");
    audio_pipeline_run(pipeline);

    while (1) {
        audio_event_iface_msg_t msg;
        esp_err_t ret = audio_event_iface_listen(evt, &msg, portMAX_DELAY);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "[ * ] Event interface error : %d", ret);
            continue;
        }

        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT
            && msg.source == (void *) mp3_decoder
            && msg.cmd == AEL_MSG_CMD_REPORT_MUSIC_INFO) {
            audio_element_info_t music_info = {0};
            audio_element_getinfo(mp3_decoder, &music_info);

            ESP_LOGI(TAG, "[ * ] Receive music info from mp3 decoder, sample_rates=%d, bits=%d, ch=%d",
                     music_info.sample_rates, music_info.bits, music_info.channels);

            audio_element_setinfo(i2s_stream_writer, &music_info);
            i2s_stream_set_clk(i2s_stream_writer, music_info.sample_rates, music_info.bits, music_info.channels);
            continue;
        }

        /* Stop when the last pipeline element (i2s_stream_writer in this case) receives stop event */
        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT && msg.source == (void *) i2s_stream_writer
            && msg.cmd == AEL_MSG_CMD_REPORT_STATUS
            && (((int)msg.data == AEL_STATUS_STATE_STOPPED) || ((int)msg.data == AEL_STATUS_STATE_FINISHED))) {
            ESP_LOGW(TAG, "[ * ] Stop event received");
            break;
        }
    }
    // Example of using an audio event -- END

    ESP_LOGI(TAG, "[ 6 ] Stop audio_pipeline");
    audio_pipeline_stop(pipeline);
    audio_pipeline_wait_for_stop(pipeline);
    audio_pipeline_terminate(pipeline);

    /* Terminate the pipeline before removing the listener */
    audio_pipeline_unregister(pipeline, http_stream_reader);
    audio_pipeline_unregister(pipeline, i2s_stream_writer);
    audio_pipeline_unregister(pipeline, mp3_decoder);

    audio_pipeline_remove_listener(pipeline);

    /* Stop all peripherals before removing the listener */
    // esp_periph_set_stop_all(set);
    // audio_event_iface_remove_listener(esp_periph_set_get_event_iface(set), evt);

    /* Make sure audio_pipeline_remove_listener & audio_event_iface_remove_listener are called before destroying event_iface */
    audio_event_iface_destroy(evt);

    /* Release all resources */
    audio_pipeline_deinit(pipeline);
    audio_element_deinit(http_stream_reader);
    audio_element_deinit(i2s_stream_writer);
    audio_element_deinit(mp3_decoder);
    // esp_periph_set_destroy(set);

    // static esp_audio_handle_t basic_player = NULL;

    audio_player_obj_t *self = m_new_obj_with_finaliser(audio_player_obj_t);
    // self->base.type = type;
    // self->callback = args[0];
    // if (basic_player == NULL) {
    //     basic_player = audio_player_create();
    // }
    // self->player = basic_player;

    return MP_OBJ_FROM_PTR(self);


}

STATIC mp_obj_t audio_player_play_helper(audio_player_obj_t *self, mp_uint_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args)
{
    enum {
        ARG_uri,
        ARG_pos,
        ARG_sync,
    };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_uri, MP_ARG_REQUIRED | MP_ARG_OBJ, { .u_obj = mp_const_none } },
        { MP_QSTR_pos, MP_ARG_INT, { .u_int = 0 } },
        { MP_QSTR_sync, MP_ARG_BOOL, { .u_obj = mp_const_false } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    if (args[ARG_uri].u_obj != mp_const_none) {
        const char *uri = mp_obj_str_get_str(args[ARG_uri].u_obj);
        int pos = args[ARG_pos].u_int;

        esp_audio_state_t state = { 0 };
        esp_audio_state_get(self->player, &state);
        if (state.status == AUDIO_STATUS_RUNNING || state.status == AUDIO_STATUS_PAUSED) {
            esp_audio_stop(self->player, TERMINATION_TYPE_NOW);
            int wait = 20;
            esp_audio_state_get(self->player, &state);
            while (wait-- && (state.status == AUDIO_STATUS_RUNNING || state.status == AUDIO_STATUS_PAUSED)) {
                vTaskDelay(pdMS_TO_TICKS(100));
                esp_audio_state_get(self->player, &state);
            }
        }
        esp_audio_callback_set(self->player, audio_state_cb, self);
        if (args[ARG_sync].u_obj == mp_const_false) {
            self->state.status = AUDIO_STATUS_RUNNING;
            self->state.err_msg = ESP_ERR_AUDIO_NO_ERROR;
            return mp_obj_new_int(esp_audio_play(self->player, AUDIO_CODEC_TYPE_DECODER, uri, pos));
        } else {
            return mp_obj_new_int(esp_audio_sync_play(self->player, uri, pos));
        }
    } else {
        return mp_obj_new_int(ESP_ERR_AUDIO_INVALID_PARAMETER);
    }
}

STATIC mp_obj_t audio_player_play(mp_uint_t n_args, const mp_obj_t *args, mp_map_t *kw_args)
{

    ESP_LOGI(TAG, "[ 3 ] Start and wait for Wi-Fi network");
    esp_periph_config_t periph_cfg = DEFAULT_ESP_PERIPH_SET_CONFIG();
    esp_periph_set_handle_t set = esp_periph_set_init(&periph_cfg);
    periph_wifi_cfg_t wifi_cfg = {
        .ssid = "Shudan",
        .password = "1q2w3e4r5t6Y",
    };
    esp_periph_handle_t wifi_handle = periph_wifi_init(&wifi_cfg);
    esp_periph_start(set, wifi_handle);
    periph_wifi_wait_for_connected(wifi_handle, portMAX_DELAY);


    ESP_LOGI(TAG, "audio player play");
    return audio_player_play_helper(args[0], n_args - 1, args + 1, kw_args);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_KW(audio_player_play_obj, 1, audio_player_play);

STATIC mp_obj_t audio_player_stop_helper(audio_player_obj_t *self, mp_uint_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args)
{
    enum {
        ARG_termination,
    };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_termination, MP_ARG_INT, { .u_int = TERMINATION_TYPE_NOW } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    return mp_obj_new_int(esp_audio_stop(self->player, args[ARG_termination].u_int));
}

STATIC mp_obj_t audio_player_stop(mp_uint_t n_args, const mp_obj_t *args, mp_map_t *kw_args)
{
    return audio_player_stop_helper(args[0], n_args - 1, args + 1, kw_args);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_KW(audio_player_stop_obj, 1, audio_player_stop);

STATIC mp_obj_t audio_player_pause(mp_obj_t self_in)
{
    audio_player_obj_t *self = self_in;
    return mp_obj_new_int(esp_audio_pause(self->player));
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(audio_player_pause_obj, audio_player_pause);

STATIC mp_obj_t audio_player_resume(mp_obj_t self_in)
{
    audio_player_obj_t *self = self_in;
    return mp_obj_new_int(esp_audio_resume(self->player));
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(audio_player_resume_obj, audio_player_resume);

STATIC mp_obj_t audio_player_vol_helper(audio_player_obj_t *self, mp_uint_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args)
{
    enum {
        ARG_vol,
    };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_vol, MP_ARG_INT, { .u_int = 0xffff } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    if (args[ARG_vol].u_int == 0xffff) {
        int vol = 0;
        esp_audio_vol_get(self->player, &vol);
        return mp_obj_new_int(vol);
    } else {
        if (args[ARG_vol].u_int >= 0 && args[ARG_vol].u_int <= 100) {
            return mp_obj_new_int(esp_audio_vol_set(self->player, args[ARG_vol].u_int));
        } else {
            return mp_obj_new_int(ESP_ERR_AUDIO_INVALID_PARAMETER);
        }
    }
}

STATIC mp_obj_t audio_player_vol(mp_uint_t n_args, const mp_obj_t *args, mp_map_t *kw_args)
{
    return audio_player_vol_helper(args[0], n_args - 1, args + 1, kw_args);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_KW(audio_player_vol_obj, 1, audio_player_vol);

STATIC mp_obj_t audio_player_get_vol(mp_obj_t self_in)
{
    audio_player_obj_t *self = self_in;
    int vol = 0;
    esp_audio_vol_get(self->player, &vol);
    return mp_obj_new_int(vol);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(audio_player_get_vol_obj, audio_player_get_vol);

STATIC mp_obj_t audio_player_set_vol(mp_obj_t self_in, mp_obj_t vol)
{
    audio_player_obj_t *self = self_in;
    int volume = mp_obj_get_int(vol);
    return mp_obj_new_int(esp_audio_vol_set(self->player, volume));
}
STATIC MP_DEFINE_CONST_FUN_OBJ_2(audio_player_set_vol_obj, audio_player_set_vol);

STATIC mp_obj_t audio_player_state(mp_obj_t self_in)
{
    audio_player_obj_t *self = self_in;
    mp_obj_dict_t *dict = mp_obj_new_dict(3);

    mp_obj_dict_store(dict, MP_ROM_QSTR(MP_QSTR_status), MP_OBJ_TO_PTR(mp_obj_new_int(self->state.status)));
    mp_obj_dict_store(dict, MP_ROM_QSTR(MP_QSTR_err_msg), MP_OBJ_TO_PTR(mp_obj_new_int(self->state.err_msg)));
    mp_obj_dict_store(dict, MP_ROM_QSTR(MP_QSTR_media_src), MP_OBJ_TO_PTR(mp_obj_new_int(self->state.media_src)));

    return dict;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(audio_player_state_obj, audio_player_state);

STATIC mp_obj_t audio_player_pos(mp_obj_t self_in)
{
    audio_player_obj_t *self = self_in;
    int pos = -1;
    int err = esp_audio_pos_get(self->player, &pos);
    if (err == ESP_ERR_AUDIO_NO_ERROR) {
        return mp_obj_new_int(pos);
    } else {
        return mp_const_none;
    }
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(audio_player_pos_obj, audio_player_pos);

STATIC mp_obj_t audio_player_time(mp_obj_t self_in)
{
    audio_player_obj_t *self = self_in;
    int time = 0;
    int err = esp_audio_time_get(self->player, &time);
    if (err == ESP_ERR_AUDIO_NO_ERROR) {
        return mp_obj_new_int(time);
    } else {
        return mp_const_none;
    }
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(audio_player_time_obj, audio_player_time);

STATIC const mp_rom_map_elem_t player_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_info), MP_ROM_PTR(&audio_player_info_obj) },
    { MP_ROM_QSTR(MP_QSTR_play), MP_ROM_PTR(&audio_player_play_obj) },
    { MP_ROM_QSTR(MP_QSTR_stop), MP_ROM_PTR(&audio_player_stop_obj) },
    { MP_ROM_QSTR(MP_QSTR_pause), MP_ROM_PTR(&audio_player_pause_obj) },
    { MP_ROM_QSTR(MP_QSTR_resume), MP_ROM_PTR(&audio_player_resume_obj) },
    { MP_ROM_QSTR(MP_QSTR_vol), MP_ROM_PTR(&audio_player_vol_obj) },
    { MP_ROM_QSTR(MP_QSTR_get_vol), MP_ROM_PTR(&audio_player_get_vol_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_vol), MP_ROM_PTR(&audio_player_set_vol_obj) },
    { MP_ROM_QSTR(MP_QSTR_get_state), MP_ROM_PTR(&audio_player_state_obj) },
    { MP_ROM_QSTR(MP_QSTR_pos), MP_ROM_PTR(&audio_player_pos_obj) },
    { MP_ROM_QSTR(MP_QSTR_time), MP_ROM_PTR(&audio_player_time_obj) },

    // esp_audio_status_t
    { MP_ROM_QSTR(MP_QSTR_STATUS_UNKNOWN), MP_ROM_INT(AUDIO_STATUS_UNKNOWN) },
    { MP_ROM_QSTR(MP_QSTR_STATUS_RUNNING), MP_ROM_INT(AUDIO_STATUS_RUNNING) },
    { MP_ROM_QSTR(MP_QSTR_STATUS_PAUSED), MP_ROM_INT(AUDIO_STATUS_PAUSED) },
    { MP_ROM_QSTR(MP_QSTR_STATUS_STOPPED), MP_ROM_INT(AUDIO_STATUS_STOPPED) },
    { MP_ROM_QSTR(MP_QSTR_STATUS_FINISHED), MP_ROM_INT(AUDIO_STATUS_FINISHED) },
    { MP_ROM_QSTR(MP_QSTR_STATUS_ERROR), MP_ROM_INT(AUDIO_STATUS_ERROR) },

    // audio_termination_type
    { MP_ROM_QSTR(MP_QSTR_TERMINATION_NOW), MP_ROM_INT(TERMINATION_TYPE_NOW) },
    { MP_ROM_QSTR(MP_QSTR_TERMINATION_DONE), MP_ROM_INT(TERMINATION_TYPE_DONE) },
};

STATIC MP_DEFINE_CONST_DICT(player_locals_dict, player_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    audio_player_type,
    MP_QSTR_player,
    MP_TYPE_FLAG_NONE,
    make_new, audio_player_make_new,
    locals_dict, &player_locals_dict
    );
