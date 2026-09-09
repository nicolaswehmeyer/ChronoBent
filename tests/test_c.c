/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nicolas Wehmeyer */
#include "chronobent/chronobent.h"
#include "chronobent/chronobent.h"
#include <stdio.h>
#include <string.h>

static int read_source(void *user, uint64_t first, size_t frames, float *out) {
    const float *source = (const float *)user;
    size_t i;
    for (i = 0; i < frames; ++i) out[i] = source[first + i];
    return 1;
}
int main(void) {
    const chronobent_config config = {48000, 1, 0, 1, 0};
    chronobent *instance = NULL;
    float source[3] = {0.25f, -0.5f, 0.75f}, output[4] = {0, 0, 0, 17};
    size_t produced = 99, i;
    if (chronobent_create(&config, &instance) != CHRONOBENT_OK) return 1;
    if (chronobent_reset(instance, 3, 1, 1) != CHRONOBENT_OK) return 2;
    if (chronobent_render(instance, read_source, source, output, 4, &produced) != CHRONOBENT_END || produced != 3) return 3;
    for (i = 0; i < 3; ++i) if (output[i] != source[i]) return 4;
    if (output[3] != 17) return 5;
    if (strcmp(chronobent_version(), "0.2.0") != 0) return 6;
    chronobent_destroy(instance);
    chronobent_destroy(NULL);
    puts("chronobent C API: passed");
    return 0;
}
