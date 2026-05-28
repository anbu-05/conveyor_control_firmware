#include "microrl.h"

#include <stddef.h>

/*
 * Clears the current input line.
 * The callbacks stay installed, but the typed command buffer becomes empty.
 */
static void microrl_clear(microrl_t *rl)
{
    rl->length = 0;
    rl->buffer[0] = '\0';
}

/*
 * Splits the current input line into argv-style tokens and runs the execute
 * callback. Spaces and tabs separate arguments.
 */
static void microrl_run_line(microrl_t *rl)
{
    const char *argv[MICRORL_MAX_ARGS];
    int argc = 0;
    int in_token = 0;

    for (int i = 0; i <= rl->length; i++) {
        char ch = rl->buffer[i];

        if (ch == ' ' || ch == '\t' || ch == '\0') {
            rl->buffer[i] = '\0';
            in_token = 0;
            continue;
        }

        if (in_token == 0) {
            if (argc >= MICRORL_MAX_ARGS) {
                if (rl->print != NULL) {
                    rl->print("ERR BAD_ARGS\r\n");
                }
                return;
            }

            argv[argc] = &rl->buffer[i];
            argc++;
            in_token = 1;
        }
    }

    if (argc > 0 && rl->execute != NULL) {
        rl->execute(argc, argv);
    }
}

/*
 * Initializes a microrl object with an empty command buffer and a print
 * callback. The execute callback is set later.
 */
void microrl_init(microrl_t *rl, microrl_print_t print)
{
    rl->print = print;
    rl->execute = NULL;
    rl->last_char_was_cr = 0;
    microrl_clear(rl);
}

/*
 * Installs the callback that runs when the user presses enter on a complete
 * command line.
 */
void microrl_set_execute_callback(microrl_t *rl, microrl_execute_t execute)
{
    rl->execute = execute;
}

/*
 * Handles one input character.
 * Printable characters are appended, backspace edits the buffer, and enter
 * runs the completed command line.
 */
void microrl_insert_char(microrl_t *rl, int ch)
{
    if (ch == '\n' && rl->last_char_was_cr == 1) {
        rl->last_char_was_cr = 0;
        return;
    }

    rl->last_char_was_cr = (ch == '\r') ? 1 : 0;

    if (ch == '\r' || ch == '\n') {
        rl->buffer[rl->length] = '\0';
        microrl_run_line(rl);
        microrl_clear(rl);
        return;
    }

    if (ch == '\b' || ch == 127) {
        if (rl->length > 0) {
            rl->length--;
            rl->buffer[rl->length] = '\0';
        }
        return;
    }

    if (ch < 32 || ch > 126) {
        return;
    }

    if (rl->length >= MICRORL_BUFFER_SIZE - 1) {
        if (rl->print != NULL) {
            rl->print("ERR BAD_ARGS\r\n");
        }
        microrl_clear(rl);
        return;
    }

    rl->buffer[rl->length] = (char)ch;
    rl->length++;
    rl->buffer[rl->length] = '\0';
}
