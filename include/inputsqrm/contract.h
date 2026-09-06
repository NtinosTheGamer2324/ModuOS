#ifndef INPUT_CONTRACT_H
#define INPUT_CONTRACT_H

#include "inputsqrm/types.h"

#define INPUT_CLASS 2
#define INPUT_CONTROLLER 1

typedef struct {
    /* Generic Standard API */
    int (*register_controller)(controller_types_t type, const char *name);
    int (*remove_controller)(int id, const char *name);
    
    /* Checks what type the controller is to verify. */
    /* Keyboard API */
    void (*push_key_event)(int controllerid, key_event_t key);

    /* Mouse API */
    void (*push_mouse_coord_event)(int controllerid, vector2_t coords);
    void (*push_mouse_click_event)(int controllerid, mouse_key_event_t event);
} input_api_t;

#endif