#include <furi.h>
#include <gui/gui.h>
#include <input/input.h>
#include <dialogs/dialogs.h>
#include <stdlib.h>
#include <storage/storage.h>
#include <core/timer.h>

#include <lib/subghz/transmitter.h>
#include <lib/subghz/protocols/raw.h>
#include <lib/toolbox/stream/stream.h>
#include <lib/flipper_format/flipper_format_i.h>


#define SUBGHZ_TBT_FOLDER "/ext/tbt_list"
#define SUBGHZ_FOLDER "/ext/subghz"
#define SUBGHZ_TBT_EXT ".tbt"
#define SUBGHZ_EXT ".sub"
#define TAG "tbt"

#define WIDTH 128
#define HEIGHT 64

#define CLOCK_TIME_FORMAT "%.2d:%.2d:%.2d"
#define TIME_LEN 12

typedef enum {
    EventTypeTick,
    EventTypeKey,
} EventType;

typedef struct {
    EventType type;
    InputEvent input;
} TbtEvent;


enum AppState
{
    AppNoTasks = 0,
    AppInvalid = -1,
};

typedef enum
{
    ChangeIncrement = 0,
    ChangeDecrement = 1,
} TimeEditChangeType;

enum AtEditMode
{
    AtEditNo = 0,
    AtEditHours = 1,
    AtEditMinutes = 2,
    AtEditSeconds = 3,
    AtModeMax = 4,
};

typedef struct {
    uint8_t hh;
    uint8_t mm;
    uint8_t ss;
} TriggerTime;

typedef struct {
    FuriMutex* mutex;
    FuriMessageQueue* event_queue;
    ViewPort* view_port;
    Gui* gui;

    string_t subghz_selected_file;

    uint8_t state;
    TriggerTime at;
    uint8_t at_edit_mode;
    bool is_shoot_done;
} TbtAppState;

int transmit_current(void* context);

static FuriHalSubGhzPreset str_to_preset(string_t preset) {
    if(string_cmp_str(preset, "FuriHalSubGhzPresetOok270Async") == 0) {
        return FuriHalSubGhzPresetOok270Async;
    }
    if(string_cmp_str(preset, "FuriHalSubGhzPresetOok650Async") == 0) {
        return FuriHalSubGhzPresetOok650Async;
    }
    if(string_cmp_str(preset, "FuriHalSubGhzPreset2FSKDev238Async") == 0) {
        return FuriHalSubGhzPreset2FSKDev238Async;
    }
    if(string_cmp_str(preset, "FuriHalSubGhzPreset2FSKDev476Async") == 0) {
        return FuriHalSubGhzPreset2FSKDev476Async;
    }
    if(string_cmp_str(preset, "FuriHalSubGhzPresetMSK99_97KbAsync") == 0) {
        return FuriHalSubGhzPresetMSK99_97KbAsync;
    }
    if(string_cmp_str(preset, "FuriHalSubGhzPresetMSK99_97KbAsync") == 0) {
        return FuriHalSubGhzPresetMSK99_97KbAsync;
    }
    return FuriHalSubGhzPresetCustom;
}

bool high_noon_is_now(TriggerTime* at) {
    FuriHalRtcDateTime curr_dt;
    furi_hal_rtc_get_datetime(&curr_dt);

    if (curr_dt.hour == at->hh)
    {
        if (curr_dt.minute == at->mm)
        {
            if (curr_dt.second >= at->ss)
            {
                return true;
            }
        }
        else if (curr_dt.minute >= at->mm)
        {
            return true;
        }
    }

    return false;
}

////////////////////////////////////////////////////////////////////////////////

static void render_callback(Canvas* canvas, void* ctx) {
    TbtAppState* app = ctx;
    furi_check(furi_mutex_acquire(app->mutex, FuriWaitForever) == FuriStatusOk);

    FuriHalRtcDateTime curr_dt;
    furi_hal_rtc_get_datetime(&curr_dt);

    char time_string[TIME_LEN];
    snprintf(time_string, TIME_LEN, CLOCK_TIME_FORMAT, curr_dt.hour, curr_dt.minute, curr_dt.second);

    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);
    canvas_set_font(canvas, FontSecondary);

    canvas_draw_str_aligned(
        canvas, WIDTH / 2, HEIGHT / 2, AlignCenter, AlignCenter, app->subghz_selected_file->ptr);

    canvas_set_color(canvas, ColorBlack);
    canvas_draw_str_aligned(canvas, 1, HEIGHT - 9, AlignLeft, AlignTop, time_string);


    char at_time_string[TIME_LEN];
    snprintf(at_time_string, TIME_LEN, CLOCK_TIME_FORMAT, app->at.hh, app->at.mm, app->at.ss);

    uint16_t at_width = canvas_string_width(canvas, at_time_string);

    if (app->is_shoot_done)
    {
        canvas_draw_disc(canvas, 10, 10, 5);
    }
    else
    {
        canvas_draw_circle(canvas, 10, 10, 5);
    }

    canvas_draw_str(canvas,  WIDTH - (at_width + 1), HEIGHT - 2, at_time_string);

    switch (app->at_edit_mode)
    {
    case AtEditHours:
        canvas_draw_line(canvas, WIDTH - (at_width + 1), HEIGHT - 1, WIDTH - (at_width + 1) + 12, HEIGHT - 1);
        break;
    case AtEditMinutes:
        canvas_draw_line(canvas, WIDTH - (at_width + 1) + 12, HEIGHT - 1, WIDTH - (at_width + 1) + 24, HEIGHT - 1);
        break;
    case AtEditSeconds:
        canvas_draw_line(canvas, WIDTH - (at_width + 1) + 24, HEIGHT - 1, WIDTH - (at_width + 1) + 36, HEIGHT - 1);
        break;
    case AtEditNo:
    case AtModeMax:
    default:
        break;
    }

    // if ((!app->is_shoot_done) && (app->at.hh == curr_dt.hour) && (curr_dt.minute >= app->at.mm))
    // {
    //     app->is_shoot_done = !app->is_shoot_done;
    // }

    if (!app->is_shoot_done && high_noon_is_now(&app->at))
    {
        app->is_shoot_done = !app->is_shoot_done;
        transmit_current(app);
    }


    furi_mutex_release(app->mutex);
}

static void input_callback(InputEvent* input, void* ctx) {
    TbtAppState* app = ctx;

    TbtEvent event = {.type = EventTypeKey, .input = *input };

    furi_message_queue_put(app->event_queue, &event, 0);
}

////////////////////////////////////////////////////////////////////////////////

TbtAppState* transmit_by_time_app_construct() {
    TbtAppState* app = malloc(sizeof(TbtAppState));

    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->event_queue = furi_message_queue_alloc(32, sizeof(TbtEvent));

        // view port
    app->view_port = view_port_alloc();
    view_port_draw_callback_set(app->view_port, render_callback, app);
    view_port_input_callback_set(app->view_port, input_callback, app);

    // gui
    app->gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(app->gui, app->view_port, GuiLayerFullscreen);

    string_init(app->subghz_selected_file);

    FuriHalRtcDateTime curr_dt;
    furi_hal_rtc_get_datetime(&curr_dt);

    app->at.hh = curr_dt.hour;
    app->at.mm = curr_dt.minute;
    app->at.ss = curr_dt.second;

    app->state = AppNoTasks;
    app->at_edit_mode = AtEditNo;
    app->is_shoot_done = true;
    return app;
}

void transmit_by_time_app_free(TbtAppState* app) {

    gui_remove_view_port(app->gui, app->view_port);
    furi_record_close(RECORD_GUI);
    view_port_free(app->view_port);

    furi_message_queue_free(app->event_queue);
    furi_mutex_free(app->mutex);

    string_clear(app->subghz_selected_file);

    free(app);
}


void select_subghz_file(TbtAppState* app)
{

    DialogsApp* dialogs = furi_record_open(RECORD_DIALOGS);
    DialogsFileBrowserOptions browser_options;
    dialog_file_browser_set_basic_options(&browser_options, SUBGHZ_EXT, &I_sub1_10px);
    browser_options.hide_ext = false;

    string_set_str(app->subghz_selected_file, SUBGHZ_FOLDER);

    const bool res =
        dialog_file_browser_show(dialogs, app->subghz_selected_file, app->subghz_selected_file, &browser_options);
        furi_record_close(RECORD_DIALOGS);

    if(!res) {
        FURI_LOG_E(TAG, "No file selected");
    }
}



// void t_callback(void* context) {
//     TbtAppState* app = context;

//     FuriHalRtcDateTime curr_dt;
//     furi_hal_rtc_get_datetime(&curr_dt);

//     // if ((!app->is_shoot_done) && (app->at.hh == curr_dt.hour) && (curr_dt.minute >= app->at.mm))
//     // {
//     //     app->is_shoot_done = !app->is_shoot_done;
//     // }

//     if (!app->is_shoot_done && high_noon_is_now(&app->at))
//     {
//         app->is_shoot_done = !app->is_shoot_done;
//         transmit_current(app);
//     }

//     TbtEvent event = {.type = EventTypeTick};

//     furi_message_queue_put(app->event_queue, &event, FuriWaitForever);
// }


int transmit_current(void* context)
{
    int status = 0;
    // UNUSED(context);
    // UNUSED(str_to_preset);
    TbtAppState* app = context;

    Storage* storage = furi_record_open(RECORD_STORAGE);
    FlipperFormat* fff_file = flipper_format_file_alloc(storage);

    if(!flipper_format_file_open_existing(fff_file, string_get_cstr(app->subghz_selected_file))) {
        FURI_LOG_E(TAG, "Failed to open %s", string_get_cstr(app->subghz_selected_file));

        furi_record_close(RECORD_STORAGE);
        flipper_format_free(fff_file);
        return 0;
    }

    uint32_t frequency = 0;
    if(!flipper_format_read_uint32(fff_file, "Frequency", &frequency, 1)) {
        FURI_LOG_W(TAG, "  (TX) Missing Frequency, defaulting to 433.92MHz");
        frequency = 433920000;
    }

    if(!furi_hal_subghz_is_tx_allowed(frequency)) {
        return -2;
    }
    string_t data, preset, protocol;
    string_init(data);
    string_init(preset);
    string_init(protocol);

    // check if preset is present
    if(!flipper_format_read_string(fff_file, "Preset", preset)) {
        FURI_LOG_E(TAG, "  (TX) Missing Preset");
        return -3;
    }

    // check if protocol is present
    if(!flipper_format_read_string(fff_file, "Protocol", protocol)) {
        FURI_LOG_E(TAG, "  (TX) Missing Protocol");
        return -4;
    }

    FlipperFormat* fff_data = flipper_format_string_alloc();

    if(!string_cmp_str(protocol, "RAW")) {
        subghz_protocol_raw_gen_fff_data(fff_data, string_get_cstr(app->subghz_selected_file));
    } else {
        stream_copy_full(
            flipper_format_get_raw_stream(fff_file), flipper_format_get_raw_stream(fff_data));
    }
    flipper_format_free(fff_file);


    // furi_hal_power_suppress_charge_enter();

    SubGhzEnvironment* environment = subghz_environment_alloc();
    SubGhzTransmitter* transmitter = subghz_transmitter_alloc_init(environment, string_get_cstr(protocol));

    subghz_transmitter_deserialize(transmitter, fff_data);
    furi_hal_subghz_reset();
    furi_hal_subghz_load_preset(str_to_preset(preset));
    frequency = furi_hal_subghz_set_frequency_and_path(frequency);

    FURI_LOG_D(TAG, "  (TX) Start sending ...");

    furi_hal_subghz_start_async_tx(subghz_transmitter_yield, transmitter);
    while(!furi_hal_subghz_is_async_tx_complete()) {
        furi_delay_ms(50);
    }

    FURI_LOG_D(TAG, "  (TX) Done sending.");

    furi_hal_subghz_stop_async_tx();
    furi_hal_subghz_sleep();

    subghz_transmitter_free(transmitter);

    // furi_hal_power_suppress_charge_exit();

    string_clear(data);
    string_clear(preset);
    string_clear(protocol);

    flipper_format_free(fff_data);

    return status;
}

void at_mode_change(TbtAppState* app, int8_t change)
{
    int8_t next_mode = app->at_edit_mode + change;
    if ((next_mode < AtModeMax) && (next_mode >= AtEditNo))
    {
        app->at_edit_mode += change;
    }
}

void edit_time(TbtAppState* app, TimeEditChangeType type)
{
    int8_t change = (type == ChangeIncrement ? 1 : -1);

    switch (app->at_edit_mode)
    {
    case AtEditHours:
        if (((app->at.hh + change) >= 0) && ((app->at.hh + change) < 24))
        {
            app->is_shoot_done = false;
            app->at.hh += change;
        }
        break;
    case AtEditMinutes:
        if (((app->at.mm + change) >= 0) && ((app->at.mm + change) < 60))
        {
            app->is_shoot_done = false;
            app->at.mm += change;
        }
        break;
    case AtEditSeconds:
        if (((app->at.ss + change) >= 0) && ((app->at.ss + change) < 60))
        {
            app->is_shoot_done = false;
            app->at.ss += change;
        }
        break;
    case AtEditNo:
    case AtModeMax:
    default:
        break;
    }
}

int32_t transmit_by_time_app() {
    TbtAppState* app = transmit_by_time_app_construct();
    furi_hal_power_suppress_charge_enter();

    {
        Storage* storage = furi_record_open(RECORD_STORAGE);
        if(!storage_simply_mkdir(storage, SUBGHZ_TBT_FOLDER)) {
            FURI_LOG_E(TAG, "Could not create folder %s", SUBGHZ_TBT_FOLDER);
        }
        furi_record_close(RECORD_STORAGE);
    }

        // Configure timer
    FURI_LOG_I(TAG, "Initializing timer");
    // FuriTimer* timer = furi_timer_alloc(t_callback, FuriTimerTypePeriodic, app);
    // furi_timer_start(timer, furi_kernel_get_tick_frequency() * 10);

    TbtEvent evt;
    for(bool processing = true; processing;) {
        furi_check(
            furi_message_queue_get(app->event_queue, &evt, FuriWaitForever) == FuriStatusOk);

        switch(evt.input.key) {
        case InputKeyUp:
            if(evt.input.type == InputTypeShort) {
                // transmit_current(app);
                edit_time(app, ChangeIncrement);
            }
            break;
        case InputKeyDown:
            if(evt.input.type == InputTypeShort) {
                edit_time(app, ChangeDecrement);
            }
            break;
        case InputKeyLeft:
            if(evt.input.type == InputTypeShort) {
                at_mode_change(app, -1);
            }
            break;
        case InputKeyRight:
            if(evt.input.type == InputTypeShort) {
                at_mode_change(app, +1);
            }
            break;
        case InputKeyOk:
            if(evt.input.type == InputTypeLong) {
                select_subghz_file(app);
            }
            break;
        case InputKeyBack:
            FURI_LOG_D(TAG, "Pressed Back button. Application will exit");
            processing = false;
            break;
        }
    }

    // furi_timer_free(timer);
    transmit_by_time_app_free(app);

    furi_hal_power_suppress_charge_exit();
    return 0;
}
