#ifndef __EVENT_H__
#define __EVENT_H__

#include "internal.h"

typedef enum {
    EVENT_LINE_PRE_DRAW,

    N_EVENTS,
} yed_event_kind_t;

typedef struct {
    yed_event_kind_t  kind;
    yed_frame        *frame;
    int               row, col;
    array_t           line_attrs;
} yed_event;

typedef void (*yed_event_handler_fn_t)(yed_event*);

typedef struct {
    yed_event_kind_t       kind;
    yed_event_handler_fn_t fn;
} yed_event_handler;

void yed_init_events(void);
void yed_add_event_handler(yed_event_handler handler);
void yed_delete_event_handler(yed_event_handler handler);

void yed_trigger_event(yed_event *event);

#endif
