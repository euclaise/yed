#include "plugin.h"

/* COMMANDS */
void vimish_take_key(int n_args, char **args);
void vimish_bind(int n_args, char **args);
void vimish_exit_insert(int n_args, char **args);
void vimish_write(int n_args, char **args);
void vimish_quit(int n_args, char **args);
void vimish_write_quit(int n_args, char **args);
void vimish_man_word(int n_args, char **args);
/* END COMMANDS */

#define MODE_NAV    (0x0)
#define MODE_INSERT (0x1)
#define MODE_DELETE (0x2)
#define MODE_YANK   (0x3)
#define N_MODES     (0x4)

static char *mode_strs[] = {
    "NAV",
    "INSERT",
    "DELETE",
    "YANK"
};

typedef struct {
    int   len;
    int   keys[MAX_SEQ_LEN];
    char *cmd;
    int   key;
} vimish_key_binding;

static yed_plugin *Self;
static int mode;
static array_t mode_bindings[N_MODES];
static array_t repeat_keys;
static int repeating;
static int till_pending; /* 0 = not pending, 1 = pending forward, 2 = pending backward */
static int last_till_key;
static int last_till_dir; /* 1 = forward, 2 = backward */

void vimish_unload(yed_plugin *self);
void vimish_nav(int key, char* key_str);
void vimish_insert(int key, char* key_str);
void vimish_delete(int key, char* key_str);
void vimish_yank(int key, char* key_str);
void bind_keys(void);
void vimish_change_mode(int new_mode, int by_line, int cancel);
void enter_insert(void);
void exit_insert(void);
void enter_delete(int by_line);
void exit_delete(int cancel);
void enter_yank(int by_line);
void exit_yank(int cancel);
void vimish_make_binding(int b_mode, int n_keys, int *keys, char *cmd);

int yed_plugin_boot(yed_plugin *self) {
    int i;

    Self = self;

    for (i = 0; i < N_MODES; i += 1) {
        mode_bindings[i] = array_make(vimish_key_binding);
    }

    repeat_keys = array_make(int);

    yed_plugin_set_unload_fn(Self, vimish_unload);

    yed_plugin_set_command(Self, "vimish-take-key",    vimish_take_key);
    yed_plugin_set_command(Self, "vimish-bind",        vimish_bind);
    yed_plugin_set_command(Self, "vimish-exit-insert", vimish_exit_insert);
    yed_plugin_set_command(Self, "w",                  vimish_write);
    yed_plugin_set_command(Self, "W",                  vimish_write);
    yed_plugin_set_command(Self, "q",                  vimish_quit);
    yed_plugin_set_command(Self, "Q",                  vimish_quit);
    yed_plugin_set_command(Self, "wq",                 vimish_write_quit);
    yed_plugin_set_command(Self, "Wq",                 vimish_write_quit);
    yed_plugin_set_command(Self, "vimish-man-word",    vimish_man_word);

    bind_keys();

    mode = MODE_NAV;
    yed_set_var("vimish-mode", mode_strs[mode]);

    return 0;
}

void vimish_unload(yed_plugin *self) {
    int                 i;
    vimish_key_binding *b;

    for (i = 0; i < N_MODES; i += 1) {
        array_traverse(mode_bindings[i], b) {
            free(b->cmd);
        }
        array_free(mode_bindings[i]);
    }
    array_free(repeat_keys);
}

void bind_keys(void) {
    int key;

    for (key = 1; key < REAL_KEY_MAX; key += 1) {
        yed_plugin_bind_key(Self, key, "vimish-take-key", 1);
    }
}

void vimish_change_mode(int new_mode, int by_line, int cancel) {
    vimish_key_binding *b;

    array_traverse(mode_bindings[mode], b) {
        yed_unbind_key(b->key);
        if (b->len > 1) {
            yed_delete_key_sequence(b->key);
        }
    }

    array_traverse(mode_bindings[new_mode], b) {
        if (b->len > 1) {
            b->key = yed_plugin_add_key_sequence(Self, b->len, b->keys);
        } else {
            b->key = b->keys[0];
        }

        yed_plugin_bind_key(Self, b->key, b->cmd, 0);
    }

    switch (mode) {
        case MODE_NAV:                         break;
        case MODE_INSERT: exit_insert();       break;
        case MODE_DELETE: exit_delete(cancel); break;
        case MODE_YANK:   exit_yank(cancel);   break;
    }

    mode = new_mode;

    switch (new_mode) {
        case MODE_NAV:                           break;
        case MODE_INSERT: enter_insert();        break;
        case MODE_DELETE: enter_delete(by_line); break;
        case MODE_YANK:   enter_yank(by_line);   break;
    }

    yed_append_text_to_cmd_buff(mode_strs[new_mode]);
    yed_set_var("vimish-mode", mode_strs[new_mode]);
}

static void _vimish_take_key(int key, char *maybe_key_str) {
    char *key_str, buff[32];

    if (maybe_key_str) {
        key_str = maybe_key_str;
    } else {
        sprintf(buff, "%d", key);
        key_str = buff;
    }

    switch (mode) {
        case MODE_NAV:    vimish_nav(key, key_str);    break;
        case MODE_INSERT: vimish_insert(key, key_str); break;
        case MODE_DELETE: vimish_delete(key, key_str); break;
        case MODE_YANK:   vimish_yank(key, key_str);   break;
        default:
            yed_append_text_to_cmd_buff("[!] invalid mode (?)");
    }
}

void vimish_take_key(int n_args, char **args) {
    int key;

    if (n_args != 1) {
        yed_append_text_to_cmd_buff("[!] expected one argument but got ");
        yed_append_int_to_cmd_buff(n_args);
        return;
    }

    sscanf(args[0], "%d", &key);

    _vimish_take_key(key, args[0]);
}

void vimish_bind(int n_args, char **args) {
    char *mode_str, *key_str, *cmd;
    int   b_mode, i, n_keys, key_i, keys[MAX_SEQ_LEN];
    char  key_c;

    if (n_args < 1) {
        yed_append_text_to_cmd_buff("[!] missing 'mode' as first argument");
        return;
    }

    mode_str = args[0];

    if      (strcmp(mode_str, "nav")    == 0)    { b_mode = MODE_NAV;    }
    else if (strcmp(mode_str, "insert") == 0)    { b_mode = MODE_INSERT; }
    else if (strcmp(mode_str, "delete") == 0)    { b_mode = MODE_DELETE; }
    else if (strcmp(mode_str, "yank")   == 0)    { b_mode = MODE_YANK;   }
    else {
        yed_append_text_to_cmd_buff("[!] no mode named '");
        yed_append_text_to_cmd_buff(mode_str);
        yed_append_text_to_cmd_buff("'");
        return;
    }

    if (n_args < 2) {
        yed_append_text_to_cmd_buff("[!] missing 'keys' as second through penultimate arguments");
        return;
    }

    if (n_args < 3) {
        yed_append_text_to_cmd_buff("[!] missing 'cmd' as last argument");
        return;
    }

    n_keys = n_args - 2;
    for (i = 0; i < n_keys; i += 1) {
        key_str = args[i + 1];
        key_c   = key_i = -1;
        if (strlen(key_str) == 1) {
            sscanf(key_str, "%c", &key_c);
            key_i = key_c;
        } else if (strcmp(key_str, "tab") == 0) {
            key_i = TAB;
        } else if (strcmp(key_str, "enter") == 0) {
            key_i = ENTER;
        } else if (strcmp(key_str, "esc") == 0) {
            key_i = ESC;
        } else if (strcmp(key_str, "spc") == 0) {
            key_i = ' ';
        } else if (strcmp(key_str, "bsp") == 0) {
            key_i = BACKSPACE;
        } else if (sscanf(key_str, "ctrl-%c", &key_c)) {
            key_i = CTRL_KEY(key_c);
        }

        if (key_i == -1) {
            yed_append_text_to_cmd_buff("[!] invalid key '");
            yed_append_text_to_cmd_buff(key_str);
            yed_append_text_to_cmd_buff("'");
            return;
        }

        keys[i] = key_i;
    }

    cmd = args[n_args - 1];

    if (!yed_get_command(cmd)) {
        yed_append_text_to_cmd_buff("[!] no command named '");
        yed_append_text_to_cmd_buff(cmd);
        yed_append_text_to_cmd_buff("' found");
        return;
    }

    vimish_make_binding(b_mode, n_keys, keys, cmd);
}

void vimish_make_binding(int b_mode, int n_keys, int *keys, char *cmd) {
    int                 i;
    vimish_key_binding  binding,
                       *b;

    if (n_keys <= 0) {
        return;
    }

    binding.len = n_keys;
    for (i = 0; i < n_keys; i += 1) {
        binding.keys[i] = keys[i];
    }
    binding.cmd = strdup(cmd);
    binding.key = KEY_NULL;

    array_push(mode_bindings[b_mode], binding);

    if (b_mode == mode) {
        array_traverse(mode_bindings[mode], b) {
            yed_unbind_key(b->key);
            if (b->len > 1) {
                yed_delete_key_sequence(b->key);
            }
        }

        array_traverse(mode_bindings[mode], b) {
            if (b->len > 1) {
                b->key = yed_plugin_add_key_sequence(Self, b->len, b->keys);
            } else {
                b->key = b->keys[0];
            }

            yed_plugin_bind_key(Self, b->key, b->cmd, 0);
        }
    }
}

static void vimish_push_repeat_key(int key) {
    if (repeating) {
        return;
    }

    array_push(repeat_keys, key);
}

static void vimish_pop_repeat_key(void) {
    if (repeating) {
        return;
    }

    array_pop(repeat_keys);
}

static void vimish_repeat(void) {
    int *key_it;

    repeating = 1;
    array_traverse(repeat_keys, key_it) {
        _vimish_take_key(*key_it, NULL);
    }
    repeating = 0;
}

static void vimish_start_repeat(int key) {
    if (repeating) {
        return;
    }

    array_clear(repeat_keys);
    array_push(repeat_keys, key);
}

void vimish_exit_insert(int n_args, char **args) {
    vimish_push_repeat_key(CTRL_C);
    vimish_change_mode(MODE_NAV, 0, 0);
}

static void fill_cmd_prompt(const char *cmd) {
    int   len, i, key;
    char  key_str_buff[32];
    char *key_str;

    len = strlen(cmd);

    YEXE("command-prompt");

    for (i = 0; i < len; i += 1) {
        key = cmd[i];
        sprintf(key_str_buff, "%d", key);
        key_str = key_str_buff;
        YEXE("command-prompt", key_str);
    }

    key_str = "32"; /* space */
    YEXE("command-prompt", key_str);
}

static void vimish_till_fw(int key) {
    yed_frame *f;
    yed_line  *line;
    int        col;
    char       c;

    if (!ys->active_frame || !ys->active_frame->buffer)    { goto out; }

    f = ys->active_frame;

    line = yed_buff_get_line(f->buffer, f->cursor_line);

    if (!line)    { goto out; }

    for (col = f->cursor_col + 1; col <= array_len(line->chars); col += 1) {
        c = yed_line_col_to_char(line, col);
        if (c == key) {
            yed_set_cursor_within_frame(f, col, f->cursor_line);
            break;
        }
    }

    last_till_key = key;
    last_till_dir = 1;

out:
    till_pending = 0;
}

static void vimish_till_bw(int key) {
    yed_frame *f;
    yed_line  *line;
    int        col;
    char       c;

    if (!ys->active_frame || !ys->active_frame->buffer)    { goto out; }

    f = ys->active_frame;

    line = yed_buff_get_line(f->buffer, f->cursor_line);

    if (!line)    { goto out; }

    for (col = f->cursor_col - 1; col >= 1; col -= 1) {
        c = yed_line_col_to_char(line, col);
        if (c == key) {
            yed_set_cursor_within_frame(f, col, f->cursor_line);
            break;
        }
    }

    last_till_key = key;
    last_till_dir = 2;

out:
    till_pending = 0;
}

static void vimish_repeat_till(void) {
    if (last_till_dir == 1) {
        vimish_till_fw(last_till_key);
    } else if (last_till_dir == 2) {
        vimish_till_bw(last_till_key);
    }
}

int vimish_nav_common(int key, char *key_str) {
    if (till_pending == 1) {
        vimish_till_fw(key);
        return 1;
    } else if (till_pending == 2) {
        vimish_till_bw(key);
        return 1;
    }

    switch (key) {
        case 'h':
        case ARROW_LEFT:
            YEXE("cursor-left");
            break;

        case 'j':
        case ARROW_DOWN:
            YEXE("cursor-down");
            break;

        case 'k':
        case ARROW_UP:
            YEXE("cursor-up");
            break;

        case 'l':
        case ARROW_RIGHT:
            YEXE("cursor-right");
            break;

        case PAGE_UP:
            YEXE("cursor-page-up");
            break;

        case PAGE_DOWN:
            YEXE("cursor-page-down");
            break;

        case 'w':
            YEXE("cursor-next-word");
            break;

        case 'b':
            YEXE("cursor-prev-word");
            break;

        case '0':
            YEXE("cursor-line-begin");
            break;

        case '$':
            YEXE("cursor-line-end");
            break;

        case '{':
            YEXE("cursor-prev-paragraph");
            break;

        case '}':
            YEXE("cursor-next-paragraph");
            break;

        case 'g':
            YEXE("cursor-buffer-begin");
            break;

        case 'G':
            YEXE("cursor-buffer-end");
            break;

        case '/':
            YEXE("find-in-buffer");
            break;

        case '?':
            YEXE("replace-current-search");
            break;

        case 'n':
            YEXE("find-next-in-buffer");
            break;

        case 'N':
            YEXE("find-prev-in-buffer");
            break;

        case 'f':
        case 't':
            till_pending = 1;
            break;

        case 'F':
        case 'T':
            till_pending = 2;
            break;

        case ';':
            vimish_repeat_till();
            break;

        default:
            return 0;
    }
    return 1;
}

void vimish_nav(int key, char *key_str) {
    if (vimish_nav_common(key, key_str)) {
        return;
    }

    YEXE("select-off");

    switch (key) {
        case 'd':
            vimish_start_repeat(key);
            vimish_change_mode(MODE_DELETE, 0, 0);
            break;

        case 'D':
            vimish_start_repeat(key);
            vimish_change_mode(MODE_DELETE, 1, 0);
            break;

        case 'y':
            vimish_change_mode(MODE_YANK, 0, 0);
            break;

        case 'Y':
            vimish_change_mode(MODE_YANK, 1, 0);
            break;

        case 'p':
            vimish_start_repeat(key);
            YEXE("paste-yank-buffer");
            break;

        case 'a':
            vimish_start_repeat(key);
            YEXE("cursor-right");
            goto enter_insert;

        case 'A':
            vimish_start_repeat(key);
            YEXE("cursor-line-end");
            goto enter_insert;

        case 'i':
            vimish_start_repeat(key);
enter_insert:
            vimish_change_mode(MODE_INSERT, 0, 0);
            break;

        case DEL_KEY:
            vimish_start_repeat(key);
            YEXE("delete-forward");
            break;

        case '.':
            vimish_repeat();
            break;

        case ':':
            YEXE("command-prompt");
            break;

        default:
            yed_append_text_to_cmd_buff("[!] [NAV] unhandled key ");
            yed_append_int_to_cmd_buff(key);
    }
}

void vimish_insert(int key, char *key_str) {
    vimish_push_repeat_key(key);

    switch (key) {
        case ARROW_LEFT:
            YEXE("cursor-left");
            break;

        case ARROW_DOWN:
            YEXE("cursor-down");
            break;

        case ARROW_UP:
            YEXE("cursor-up");
            break;

        case ARROW_RIGHT:
            YEXE("cursor-right");
            break;

        case BACKSPACE:
            YEXE("delete-back");
            break;

        case DEL_KEY:
            YEXE("delete-forward");
            break;

        case ESC:
        case CTRL_C:
            vimish_change_mode(MODE_NAV, 0, 1);
            break;

        default:
            if (key == ENTER || key == TAB || !iscntrl(key)) {
                YEXE("insert", key_str);
            } else {
                vimish_pop_repeat_key();
                yed_append_text_to_cmd_buff("[!] [INSERT] unhandled key ");
                yed_append_int_to_cmd_buff(key);
            }
    }

}

void vimish_delete(int key, char *key_str) {
    vimish_push_repeat_key(key);

    if (vimish_nav_common(key, key_str)) {
        return;
    }

    switch (key) {
        case ESC:
        case 'd':
            vimish_change_mode(MODE_NAV, 0, 0);
            break;

        case 'c':
            vimish_change_mode(MODE_NAV, 0, 0);
            vimish_change_mode(MODE_INSERT, 0, 0);
            break;

        case CTRL_C:
            vimish_change_mode(MODE_NAV, 0, 1);
            break;

        default:
            vimish_pop_repeat_key();
            yed_append_text_to_cmd_buff("[!] [DELETE] unhandled key ");
            yed_append_int_to_cmd_buff(key);
    }
}

void vimish_yank(int key, char *key_str) {
    if (vimish_nav_common(key, key_str)) {
        return;
    }

    switch (key) {
        case ESC:
        case 'y':
            vimish_change_mode(MODE_NAV, 0, 0);
            break;

        case CTRL_C:
            vimish_change_mode(MODE_NAV, 0, 1);
            break;

        default:
            yed_append_text_to_cmd_buff("[!] [YANK] unhandled key ");
            yed_append_int_to_cmd_buff(key);
    }
}

void vimish_write(int n_args, char **args) {
    yed_execute_command("write-buffer", n_args, args);
}

void vimish_quit(int n_args, char **args) {
    if (array_len(ys->frames) > 1) {
        YEXE("frame-delete");
    } else {
        YEXE("quit");
    }
}

void vimish_write_quit(int n_args, char **args) {
    YEXE("w");
    YEXE("q");
}

void vimish_man_word(int n_args, char **args) {
    char *word;

    word = yed_word_under_cursor();

    if (!word) {
        yed_append_text_to_cmd_buff("[!] cursor is not on a word");
        return;
    }

    YEXE("man", word);

    free(word);
}

void enter_insert(void) {}
void exit_insert(void) {}

void enter_delete(int by_line) {
    if (by_line) {
        YEXE("select-lines");
    } else {
        YEXE("select");
    }

}

void enter_yank(int by_line) {
    if (by_line) {
        YEXE("select-lines");
    } else {
        YEXE("select");
    }
}

void exit_delete(int cancel) {
    yed_frame  *frame;
    yed_buffer *buff;
    yed_range  *sel;
    char       *preserve;

    if (!cancel) {
        if (ys->active_frame
        &&  ys->active_frame->buffer
        &&  ys->active_frame->buffer->has_selection) {

            frame = ys->active_frame;
            buff  = frame->buffer;
            sel   = &buff->selection;

            if (sel->kind != RANGE_LINE
            &&  sel->anchor_row == sel->cursor_row
            &&  sel->anchor_col == sel->cursor_col) {

                YEXE("select-lines");
            }

            preserve = "1";
            YEXE("yank-selection", preserve);
            YEXE("delete-back");
        }
    }

    YEXE("select-off");
}

void exit_yank(int cancel) {
    yed_frame  *frame;
    yed_buffer *buff;
    yed_range  *sel;

    if (!cancel) {
        if (ys->active_frame
        &&  ys->active_frame->buffer
        &&  ys->active_frame->buffer->has_selection) {

            frame = ys->active_frame;
            buff  = frame->buffer;
            sel   = &buff->selection;

            if (sel->kind != RANGE_LINE
            &&  sel->anchor_row == sel->cursor_row
            &&  sel->anchor_col == sel->cursor_col) {

                YEXE("select-lines");
            }

            YEXE("yank-selection");
        }
    }

    YEXE("select-off");
}
