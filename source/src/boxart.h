#ifndef BOXA_H
#define BOXA_H

void boxart_load(const char *rom_name);
void boxart_draw(u16 pos_x, u16 pos_y, u16 border_color);
void boxart_free(void);

#endif