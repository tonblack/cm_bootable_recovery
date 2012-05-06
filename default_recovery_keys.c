#include <linux/input.h>

#include "recovery_ui.h"
#include "common.h"
#include "extendedcommands.h"


int device_toggle_display(volatile char* key_pressed, int key_code) {
    return 0;
}

int device_handle_key(int key_code, int visible) {
    if (visible) {
        switch (key_code) {
            case KEY_HOME:
            case KEY_VOLUMEDOWN:
                return HIGHLIGHT_DOWN;

            case KEY_VOLUMEUP:
                return HIGHLIGHT_UP;

            case KEY_END:
            case KEY_MENU:
                return SELECT_ITEM;
            
            case KEY_BACK:
                if (ui_menu_level > 0) {
                    return GO_BACK;
                }
        }
    }

    return NO_ACTION;
}
