#include "py/runtime.h"

#include <string.h>
#include <stdlib.h>

#include "hardware/watchdog.h"

#define dlog(...) mp_printf(&mp_plat_print, __VA_ARGS__)

#include "hxcmod.h"

void* tracked_malloc(size_t size) {
    void* ptr = m_tracked_calloc(1, size);
    if (!ptr) {
        dlog("Out of memory, aieee...\n");
        watchdog_reboot(0, SRAM_END, 0);
        for (;;) {
            __wfi();
        }
    }
    return ptr;
}

void* tracked_realloc(void* ptr, size_t size) {
    void* new_ptr = tracked_malloc(size);
    if (ptr) {
        // This should be the old size...
        memcpy(new_ptr, ptr, size);
        m_tracked_free(ptr);
    }
    return new_ptr;
}

#define TSF_IMPLEMENTATION
#define TSF_NO_STDIO
// #define TSF_STATIC
#define TSF_MALLOC tracked_malloc
#define TSF_FREE m_tracked_free
#define TSF_REALLOC tracked_realloc
#include "tsf.h"
        
#define TML_IMPLEMENTATION
#define TML_NO_STDIO
// #define TML_STATIC
#define TML_MALLOC tracked_malloc
#define TML_FREE m_tracked_free
#define TML_REALLOC tracked_realloc
#include "tml.h"

// hxcmod
modcontext ctx;

// TinySoundFont
tsf* tsf_ctx = NULL;
float midi_msec = 0.f;
tml_message* midi_ctx = NULL;
tml_message* midi_root_ctx = NULL;

static mp_obj_t init() {
    hxcmod_init(&ctx);
    hxcmod_setcfg(&ctx, 22050, 0, 1);
    tsf_ctx = NULL;
    midi_ctx = NULL;
    midi_root_ctx = NULL;
    midi_msec = 0.f;
    return mp_const_none;
}

static mp_obj_t load(mp_obj_t buffer_obj) {
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(buffer_obj, &bufinfo, MP_BUFFER_READ);
    int ret = hxcmod_load(&ctx, bufinfo.buf, bufinfo.len);
    midi_ctx = NULL;
    midi_root_ctx = NULL;
    return mp_obj_new_int(ret);
}

static mp_obj_t load_sf2(mp_obj_t buffer_obj) {
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(buffer_obj, &bufinfo, MP_BUFFER_READ);
    // dlog("heap total %d, free %d\n", get_total_heap(), get_free_heap());
    // dlog("loading tsf from %p len %d\n", bufinfo.buf, bufinfo.len);
    tsf_ctx = tsf_load_memory(bufinfo.buf, bufinfo.len);
    if (tsf_ctx) {
        tsf_set_output(tsf_ctx, TSF_MONO, 22050, -10.f);
        tsf_set_volume(tsf_ctx, ctx.global_volume / 255.f);
    }
    return mp_obj_new_int(tsf_ctx != NULL);
}

static mp_obj_t load_midi(mp_obj_t buffer_obj) {
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(buffer_obj, &bufinfo, MP_BUFFER_READ);
    midi_root_ctx = tml_load_memory(bufinfo.buf, bufinfo.len);
    midi_ctx = midi_root_ctx;
    midi_msec = 0.f;
    return mp_obj_new_int(midi_ctx != NULL);
}

static mp_obj_t fillbuffer(mp_obj_t buffer_obj) {
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(buffer_obj, &bufinfo, MP_BUFFER_WRITE);

    if (tsf_ctx && midi_ctx) {
        int sample_block;
        int sample_count = bufinfo.len / sizeof(int16_t);
        uint8_t* stream = bufinfo.buf;
        for (sample_block = TSF_RENDER_EFFECTSAMPLEBLOCK;
             sample_count;
             sample_count -= sample_block, stream += (sample_block * sizeof(int16_t))) {
            if (sample_block > sample_count) {
                sample_block = sample_count;
            }

            // Loop through all MIDI messages which need to be played up
            // until the current playback time.
            for (midi_msec += sample_block * (1000.0f / 22050.0f);
                 midi_ctx && midi_ctx->time != ~0 && midi_msec >= midi_ctx->time;
                 midi_ctx++) {
                switch (midi_ctx->type) {
                    case TML_PROGRAM_CHANGE: //channel program (preset) change (special handling for 10th MIDI channel with drums)
                        tsf_channel_set_presetnumber(tsf_ctx, midi_ctx->channel, midi_ctx->program, (midi_ctx->channel == 9));
                        break;
                    case TML_NOTE_ON: //play a note
                        tsf_channel_note_on(tsf_ctx, midi_ctx->channel, midi_ctx->key, midi_ctx->velocity / 127.0f);
                        break;
                    case TML_NOTE_OFF: //stop a note
                        tsf_channel_note_off(tsf_ctx, midi_ctx->channel, midi_ctx->key);
                        break;
                    case TML_PITCH_BEND: //pitch wheel modification
                        tsf_channel_set_pitchwheel(tsf_ctx, midi_ctx->channel, midi_ctx->pitch_bend);
                        break;
                    case TML_CONTROL_CHANGE: //MIDI controller messages
                        tsf_channel_midi_control(tsf_ctx, midi_ctx->channel, midi_ctx->control, midi_ctx->control_value);
                        break;
                }
            }
            tsf_render_short(tsf_ctx, (int16_t*)stream, sample_block, 0);
        }
        return mp_obj_new_int((int)midi_msec);
    } else {
        // Assumes that HXCMOD_MONO_OUTPUT is set.
        hxcmod_fillbuffer(&ctx, bufinfo.buf, bufinfo.len / 2, NULL);
        return mp_obj_new_int(ctx.patternpos);
    }
}

static mp_obj_t apply_volume(mp_obj_t buffer_obj) {
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(buffer_obj, &bufinfo, MP_BUFFER_WRITE);
    int16_t* samples = bufinfo.buf;
    int sample_count = bufinfo.len / sizeof(int16_t);
    for (int i = 0; i < sample_count; i++) {
        samples[i] = (samples[i] * ctx.global_volume) >> 8;
    }
    return mp_const_none;
}

static mp_obj_t unload() {
    hxcmod_unload(&ctx);
    return mp_const_none;
}

static mp_obj_t unload_sf2() {
    if (tsf_ctx != NULL) {
        tsf_close(tsf_ctx);
        tsf_ctx = NULL;
    }
    return mp_const_none;
}

static mp_obj_t unload_midi() {
    if (midi_root_ctx != NULL) {
        tml_free(midi_root_ctx);
        midi_root_ctx = NULL;
        midi_ctx = NULL;
    }
    return mp_const_none;
}

static mp_obj_t set_volume(mp_obj_t volume) {
    mp_int_t vol = mp_obj_get_int(volume);
    if (vol < 0) {
        vol = 0;
    }
    if (vol > 0xff) {
        vol = 0xff;
    }
    ctx.global_volume = vol;
    if (tsf_ctx != NULL) {
        tsf_set_volume(tsf_ctx, vol / 255.f);
    }
    return mp_const_none;
}

static mp_obj_t seek(mp_obj_t table) {
    mp_int_t t = mp_obj_get_int(table);
    ctx.tablepos = t;
    ctx.patternpos = 0;
    ctx.song.speed = 6;
    ctx.bpm = 125;
    return mp_const_none;
}

static mp_obj_t write_note(mp_obj_t sample_obj, mp_obj_t period_obj, mp_obj_t effect_obj) {
    mp_int_t sample = mp_obj_get_int(sample_obj);
    mp_int_t period = mp_obj_get_int(period_obj);
    mp_int_t effect = mp_obj_get_int(effect_obj);
    note* n = ctx.patterndata[ctx.song.patterntable[ctx.tablepos]];
    n->sampperiod = (sample & 0xf0) | ((period >> 8) & 0xf);
    n->period = period & 0xff;
    n->sampeffect = ((sample << 4) & 0xf0) | ((effect >> 8) & 0xf);
    n->effect = effect & 0xff;
    return mp_const_none;
}

static MP_DEFINE_CONST_FUN_OBJ_0(init_obj, init);
static MP_DEFINE_CONST_FUN_OBJ_1(load_obj, load);
static MP_DEFINE_CONST_FUN_OBJ_1(load_sf2_obj, load_sf2);
static MP_DEFINE_CONST_FUN_OBJ_1(load_midi_obj, load_midi);
static MP_DEFINE_CONST_FUN_OBJ_1(fillbuffer_obj, fillbuffer);
static MP_DEFINE_CONST_FUN_OBJ_1(apply_volume_obj, apply_volume);
static MP_DEFINE_CONST_FUN_OBJ_0(unload_obj, unload);
static MP_DEFINE_CONST_FUN_OBJ_0(unload_sf2_obj, unload_sf2);
static MP_DEFINE_CONST_FUN_OBJ_0(unload_midi_obj, unload_midi);
static MP_DEFINE_CONST_FUN_OBJ_1(set_volume_obj, set_volume);
static MP_DEFINE_CONST_FUN_OBJ_1(seek_obj, seek);
static MP_DEFINE_CONST_FUN_OBJ_3(write_note_obj, write_note);

static const mp_rom_map_elem_t kakenative_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_kakenative) },
    { MP_ROM_QSTR(MP_QSTR_init), MP_ROM_PTR(&init_obj) },
    { MP_ROM_QSTR(MP_QSTR_load), MP_ROM_PTR(&load_obj) },
    { MP_ROM_QSTR(MP_QSTR_load_sf2), MP_ROM_PTR(&load_sf2_obj) },
    { MP_ROM_QSTR(MP_QSTR_load_midi), MP_ROM_PTR(&load_midi_obj) },
    { MP_ROM_QSTR(MP_QSTR_fillbuffer), MP_ROM_PTR(&fillbuffer_obj) },
    { MP_ROM_QSTR(MP_QSTR_apply_volume), MP_ROM_PTR(&apply_volume_obj) },
    { MP_ROM_QSTR(MP_QSTR_unload), MP_ROM_PTR(&unload_obj) },
    { MP_ROM_QSTR(MP_QSTR_unload_sf2), MP_ROM_PTR(&unload_sf2_obj) },
    { MP_ROM_QSTR(MP_QSTR_unload_midi), MP_ROM_PTR(&unload_midi_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_volume), MP_ROM_PTR(&set_volume_obj) },
    { MP_ROM_QSTR(MP_QSTR_seek), MP_ROM_PTR(&seek_obj) },
    { MP_ROM_QSTR(MP_QSTR_write_note), MP_ROM_PTR(&write_note_obj) },
};

static MP_DEFINE_CONST_DICT(kakenative_module_globals, kakenative_module_globals_table);

const mp_obj_module_t kakenative_user_cmodule = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&kakenative_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_kakenative, kakenative_user_cmodule);
