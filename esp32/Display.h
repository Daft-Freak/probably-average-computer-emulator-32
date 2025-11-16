#pragma once
#include <cstdint>

void init_display();

void set_display_size(int w, int h); // TODO

// callback
void display_draw_line(void *, int line, uint16_t *buf);