/*
 * Roku Remote - vertical infrared remote for Flipper Zero.
 *
 * Gestures (directions are as seen on the rotated, vertical screen):
 *   tap  OK / Up / Down / Left / Right  -> OK / Up / Down / Left / Right
 *   hold OK                             -> Power        (sent once)
 *   hold Up                             -> Volume Up    (keeps sending while held)
 *   hold Down                           -> Volume Down  (keeps sending while held)
 *   hold Left                           -> Home         (sent once)
 *   hold Right                          -> Mute         (sent once)
 *   tap  Back                           -> Back
 *   hold Back                           -> exit the app
 *
 * IR codes come from Roku.ir (protocol NECext, address "EA C7 00 00").
 *
 * Vertical orientation: view_port_set_orientation() makes the firmware remap the
 * physical d-pad for us BEFORE our input callback runs (see view_port_map_input in
 * the firmware), so InputKeyUp here is always the key that is "up" on the vertical
 * screen. Remapping again in this file would swap the keys back, so we don't.
 */

#include <furi.h>
#include <gui/gui.h>
#include <input/input.h>
#include <notification/notification_messages.h>
#include <infrared/worker/infrared_transmit.h>
#include <stdio.h>

/*
 * Which way the Flipper is held. With ViewPortOrientationVertical the top of the
 * screen points toward the IR transceiver end, and the firmware remaps the d-pad to
 * match: the physical LEFT button is "Up" on screen, physical Right is "Down",
 * physical Up is "Right" and physical Down is "Left".
 * If you ever want it the other way round, use ViewPortOrientationVerticalFlip.
 */
#define ROKU_ORIENTATION ViewPortOrientationVertical

/* ---- IR codes (from Roku.ir; NECext, address bytes "EA C7 00 00") ------------- */

#define ROKU_ADDRESS 0xC7EA

typedef enum {
    RokuCmdOk,
    RokuCmdUp,
    RokuCmdDown,
    RokuCmdLeft,
    RokuCmdRight,
    RokuCmdPower,
    RokuCmdVolUp,
    RokuCmdVolDown,
    RokuCmdMute,
    RokuCmdBack,
    RokuCmdHome,
    RokuCmdCount,
} RokuCmd;

typedef struct {
    const char* name;
    uint32_t command;
} RokuCode;

static const RokuCode roku_codes[RokuCmdCount] = {
    [RokuCmdOk] = {"OK", 0xD52A}, /* OK     2A D5 00 00 */
    [RokuCmdUp] = {"Up", 0xE619}, /* Up     19 E6 00 00 */
    [RokuCmdDown] = {"Down", 0xCC33}, /* Down   33 CC 00 00 */
    [RokuCmdLeft] = {"Left", 0xE11E}, /* Left   1E E1 00 00 */
    [RokuCmdRight] = {"Right", 0xD22D}, /* Right  2D D2 00 00 */
    [RokuCmdPower] = {"Power", 0xE817}, /* Power  17 E8 00 00 */
    [RokuCmdVolUp] = {"Vol+", 0xF00F}, /* VOL+   0F F0 00 00 */
    [RokuCmdVolDown] = {"Vol-", 0xEF10}, /* VOL-   10 EF 00 00 */
    [RokuCmdMute] = {"Mute", 0xDF20}, /* Mute   20 DF 00 00 */
    [RokuCmdBack] = {"Back", 0x9966}, /* Back   66 99 00 00 */
    /* Home is not in Roku.ir. 03 FC 00 00 is the Home code used by the Roku remotes in
     * Flipper-IRDB, which share this exact NECext address (EA C7). */
    [RokuCmdHome] = {"Home", 0xFC03}, /* Home   03 FC 00 00 */
};

/* ---- App state ---------------------------------------------------------------- */

typedef struct {
    bool pressed[InputKeyMAX]; /* live key state, used for highlighting + hold repeat */
    const char* last_sent; /* name of the last command sent, NULL if none yet */
} RokuModel;

typedef struct {
    Gui* gui;
    ViewPort* view_port;
    NotificationApp* notifications;
    FuriMessageQueue* queue; /* InputEvent, input callback -> main loop */
    FuriMutex* mutex; /* guards model */
    RokuModel model;
} RokuApp;

/* ---- Drawing (canvas is 64 x 128 in vertical orientation) --------------------- */

static void roku_draw_arrow(Canvas* canvas, int32_t cx, int32_t cy, InputKey dir, bool filled) {
    const int32_t half = 5; /* half of the base length */

    if(dir == InputKeyUp || dir == InputKeyDown) {
        const int32_t sign = (dir == InputKeyUp) ? -1 : 1;
        const int32_t apex_y = cy + sign * 4;
        const int32_t base_y = cy - sign * 3;
        if(filled) {
            for(int32_t x = cx - half; x <= cx + half; x++) {
                canvas_draw_line(canvas, cx, apex_y, x, base_y);
            }
        } else {
            canvas_draw_line(canvas, cx, apex_y, cx - half, base_y);
            canvas_draw_line(canvas, cx, apex_y, cx + half, base_y);
            canvas_draw_line(canvas, cx - half, base_y, cx + half, base_y);
        }
    } else {
        const int32_t sign = (dir == InputKeyLeft) ? -1 : 1;
        const int32_t apex_x = cx + sign * 4;
        const int32_t base_x = cx - sign * 3;
        if(filled) {
            for(int32_t y = cy - half; y <= cy + half; y++) {
                canvas_draw_line(canvas, apex_x, cy, base_x, y);
            }
        } else {
            canvas_draw_line(canvas, apex_x, cy, base_x, cy - half);
            canvas_draw_line(canvas, apex_x, cy, base_x, cy + half);
            canvas_draw_line(canvas, base_x, cy - half, base_x, cy + half);
        }
    }
}

static void roku_draw_callback(Canvas* canvas, void* context) {
    furi_assert(canvas);
    furi_assert(context);
    RokuApp* app = context;

    /* Copy the model so we don't hold the mutex while drawing. */
    RokuModel model;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    model = app->model;
    furi_mutex_release(app->mutex);

    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);

    /* Title */
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 32, 1, AlignCenter, AlignTop, "ROKU");

    /* D-pad */
    const int32_t cx = 32;
    const int32_t cy = 37;
    canvas_draw_circle(canvas, cx, cy, 23);

    roku_draw_arrow(canvas, cx, cy - 16, InputKeyUp, model.pressed[InputKeyUp]);
    roku_draw_arrow(canvas, cx, cy + 16, InputKeyDown, model.pressed[InputKeyDown]);
    roku_draw_arrow(canvas, cx - 16, cy, InputKeyLeft, model.pressed[InputKeyLeft]);
    roku_draw_arrow(canvas, cx + 16, cy, InputKeyRight, model.pressed[InputKeyRight]);

    canvas_set_font(canvas, FontSecondary);
    if(model.pressed[InputKeyOk]) {
        canvas_draw_disc(canvas, cx, cy, 9);
        canvas_set_color(canvas, ColorWhite);
        canvas_draw_str_aligned(canvas, cx, cy, AlignCenter, AlignCenter, "OK");
        canvas_set_color(canvas, ColorBlack);
    } else {
        canvas_draw_circle(canvas, cx, cy, 9);
        canvas_draw_str_aligned(canvas, cx, cy, AlignCenter, AlignCenter, "OK");
    }

    /* Last command sent */
    char status[24];
    if(model.last_sent) {
        snprintf(status, sizeof(status), "Sent: %s", model.last_sent);
    } else {
        snprintf(status, sizeof(status), "Ready");
    }
    canvas_draw_line(canvas, 4, 62, 59, 62);
    canvas_draw_str_aligned(canvas, 32, 64, AlignCenter, AlignTop, status);

    /* Hold cheat-sheet (hold Back also exits the app, but isn't listed to save room) */
    canvas_draw_str_aligned(canvas, 32, 74, AlignCenter, AlignTop, "- Hold -");
    canvas_draw_str_aligned(canvas, 4, 83, AlignLeft, AlignTop, "OK: Power");
    canvas_draw_str_aligned(canvas, 4, 92, AlignLeft, AlignTop, "Up: Vol +");
    canvas_draw_str_aligned(canvas, 4, 101, AlignLeft, AlignTop, "Down: Vol -");
    canvas_draw_str_aligned(canvas, 4, 110, AlignLeft, AlignTop, "Left: Home");
    canvas_draw_str_aligned(canvas, 4, 119, AlignLeft, AlignTop, "Right: Mute");
}

/* ---- Input (GUI thread): record key state, forward event to the main loop ----- */

static void roku_input_callback(InputEvent* event, void* context) {
    furi_assert(event);
    furi_assert(context);
    RokuApp* app = context;

    if(event->key < InputKeyMAX &&
       (event->type == InputTypePress || event->type == InputTypeRelease)) {
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        app->model.pressed[event->key] = (event->type == InputTypePress);
        furi_mutex_release(app->mutex);
        view_port_update(app->view_port);
    }

    /* Never block the GUI thread: if the queue is full the event is dropped. */
    furi_message_queue_put(app->queue, event, 0);
}

/* ---- IR (main thread only: infrared_send() blocks until the frame is sent) ----- */

static void roku_send(RokuApp* app, RokuCmd cmd) {
    furi_assert(cmd < RokuCmdCount);

    if(furi_hal_infrared_is_busy()) {
        return;
    }

    InfraredMessage message = {
        .protocol = InfraredProtocolNECext,
        .address = ROKU_ADDRESS,
        .command = roku_codes[cmd].command,
        .repeat = false,
    };

    notification_message(app->notifications, &sequence_blink_start_magenta);
    infrared_send(&message, 1);
    notification_message(app->notifications, &sequence_blink_stop);

    furi_mutex_acquire(app->mutex, FuriWaitForever);
    app->model.last_sent = roku_codes[cmd].name;
    furi_mutex_release(app->mutex);
    view_port_update(app->view_port);
}

static bool roku_is_held(RokuApp* app, InputKey key) {
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    bool held = app->model.pressed[key];
    furi_mutex_release(app->mutex);
    return held;
}

/* Returns false when the app should exit. */
static bool roku_handle_event(RokuApp* app, const InputEvent* event) {
    switch(event->type) {
    case InputTypeShort:
        /* Quick tap (key released before the long-press threshold). */
        switch(event->key) {
        case InputKeyOk:
            roku_send(app, RokuCmdOk);
            break;
        case InputKeyUp:
            roku_send(app, RokuCmdUp);
            break;
        case InputKeyDown:
            roku_send(app, RokuCmdDown);
            break;
        case InputKeyLeft:
            roku_send(app, RokuCmdLeft);
            break;
        case InputKeyRight:
            roku_send(app, RokuCmdRight);
            break;
        case InputKeyBack:
            roku_send(app, RokuCmdBack);
            break;
        default:
            break;
        }
        break;

    case InputTypeLong:
        /* Hold threshold reached. Sent even if already released again, because a
         * long press never produces a Short event afterwards. */
        switch(event->key) {
        case InputKeyOk:
            roku_send(app, RokuCmdPower);
            break;
        case InputKeyUp:
            roku_send(app, RokuCmdVolUp);
            break;
        case InputKeyDown:
            roku_send(app, RokuCmdVolDown);
            break;
        case InputKeyLeft:
            roku_send(app, RokuCmdHome);
            break;
        case InputKeyRight:
            roku_send(app, RokuCmdMute);
            break;
        case InputKeyBack:
            return false; /* hold Back = exit */
        default:
            break;
        }
        break;

    case InputTypeRepeat:
        /* Fires every ~150 ms while held. Only volume keeps transmitting; the
         * held check drops repeats that were still queued after the release. */
        if(event->key == InputKeyUp && roku_is_held(app, InputKeyUp)) {
            roku_send(app, RokuCmdVolUp);
        } else if(event->key == InputKeyDown && roku_is_held(app, InputKeyDown)) {
            roku_send(app, RokuCmdVolDown);
        }
        break;

    default:
        break;
    }

    return true;
}

/* ---- Entry point -------------------------------------------------------------- */

int32_t roku_remote_app(void* p) {
    UNUSED(p);

    RokuApp* app = malloc(sizeof(RokuApp));
    memset(app, 0, sizeof(RokuApp));

    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->queue = furi_message_queue_alloc(16, sizeof(InputEvent));

    app->view_port = view_port_alloc();
    view_port_set_orientation(app->view_port, ROKU_ORIENTATION);
    view_port_draw_callback_set(app->view_port, roku_draw_callback, app);
    view_port_input_callback_set(app->view_port, roku_input_callback, app);

    app->gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(app->gui, app->view_port, GuiLayerFullscreen);

    app->notifications = furi_record_open(RECORD_NOTIFICATION);
    notification_message(app->notifications, &sequence_display_backlight_enforce_on);

    InputEvent event;
    bool running = true;
    while(running) {
        if(furi_message_queue_get(app->queue, &event, FuriWaitForever) == FuriStatusOk) {
            running = roku_handle_event(app, &event);
        }
    }

    notification_message(app->notifications, &sequence_display_backlight_enforce_auto);
    furi_record_close(RECORD_NOTIFICATION);

    view_port_enabled_set(app->view_port, false);
    gui_remove_view_port(app->gui, app->view_port);
    furi_record_close(RECORD_GUI);
    view_port_free(app->view_port);

    furi_message_queue_free(app->queue);
    furi_mutex_free(app->mutex);
    free(app);

    return 0;
}
