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
    if (strcmp(chronobent_version(), "0.5.0") != 0) return 6;
    {
        chronobent_controls controls = chronobent_default_controls();
        chronobent_processor *processor = NULL;
        chronobent_config profile;
        if (chronobent_config_for_profile(48000,1,CHRONOBENT_PROFILE_COMPACT,&profile) != CHRONOBENT_OK) return 7;
        if (chronobent_processor_create(&profile,0,&processor) != CHRONOBENT_OK) return 8;
        if (chronobent_processor_set_source_controls(processor,read_source,source,3,&controls) != CHRONOBENT_OK) return 9;
        controls.transients=CHRONOBENT_TRANSIENT_MIXED;
        if (chronobent_processor_set_controls(processor,&controls) != CHRONOBENT_OK) return 10;
        if (chronobent_processor_get_controls(processor,&controls) != CHRONOBENT_OK || controls.transients != 2) return 11;
        if (chronobent_processor_render(processor,output,4,&produced) != CHRONOBENT_END || produced != 3) return 12;
        chronobent_processor_destroy(processor);
    }
    chronobent_destroy(instance);
    chronobent_destroy(NULL);
    {
        const chronobent_pitch_range range = {.0625,16};
        chronobent_pitch_range actual;
        chronobent_processor *processor = NULL;
        if (chronobent_create_with_pitch_range(&config,&range,&instance) != CHRONOBENT_OK) return 13;
        if (chronobent_get_pitch_range(instance,&actual) != CHRONOBENT_OK || actual.maximum != 16) return 14;
        if (chronobent_reset(instance,3,1,16) != CHRONOBENT_OK) return 15;
        if (chronobent_render(instance,read_source,source,output,4,&produced) != CHRONOBENT_END || produced != 3) return 16;
        chronobent_destroy(instance);
        if (chronobent_processor_create_with_pitch_range(&config,0,&range,&processor) != CHRONOBENT_OK) return 17;
        if (chronobent_processor_get_pitch_range(processor,&actual) != CHRONOBENT_OK || actual.minimum != .0625) return 18;
        chronobent_processor_destroy(processor);
    }
    puts("chronobent C API: passed");
    return 0;
}
