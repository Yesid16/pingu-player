/* ui.h - interfaz de linea de comandos (con pinguino) */
#ifndef UI_H
#define UI_H

#include "player.h"

void ui_init(void);
void ui_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void ui_prompt(void);
void ui_event(void *ctx, const char *msg);     /* callback del Player (otro hilo) */

void ui_banner(void);
void ui_help(void);
void ui_penguin(PState st);
void ui_status(const PlayerStatus *st);
void ui_list(Playlist *pl, PState st);

#endif
