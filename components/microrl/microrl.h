#ifndef MICRORL_H
#define MICRORL_H

#ifdef __cplusplus
extern "C" {
#endif

#define MICRORL_BUFFER_SIZE 128
#define MICRORL_MAX_ARGS 8

typedef struct microrl microrl_t;
typedef void (*microrl_print_t)(const char *text);
typedef int (*microrl_execute_t)(int argc, const char *const *argv);

struct microrl {
    char buffer[MICRORL_BUFFER_SIZE];
    int length;
    int last_char_was_cr;
    microrl_print_t print;
    microrl_execute_t execute;
};

void microrl_init(microrl_t *rl, microrl_print_t print);
void microrl_set_execute_callback(microrl_t *rl, microrl_execute_t execute);
void microrl_insert_char(microrl_t *rl, int ch);

#ifdef __cplusplus
}
#endif

#endif
