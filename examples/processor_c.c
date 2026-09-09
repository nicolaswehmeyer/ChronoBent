/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nicolas Wehmeyer */
#include "chronobent/chronobent.h"
#include <stdio.h>
#include <string.h>

struct pcm { const float *samples; uint64_t frames; uint32_t channels; };
static int read_pcm(void *user, uint64_t first, size_t frames, float *out) {
    const struct pcm *pcm = (const struct pcm *)user;
    if (first > pcm->frames || frames > pcm->frames - first) return 0;
    memcpy(out, pcm->samples + first * pcm->channels, frames * pcm->channels * sizeof(float));
    return 1;
}
int main(void) {
    float samples[4096] = {0}, output[512];
    struct pcm source = {samples, 2048, 2};
    chronobent_config config = chronobent_default_config(48000, 2);
    chronobent_parameters parameters = chronobent_default_parameters();
    chronobent_processor *processor = NULL;
    chronobent_status status = chronobent_processor_create(&config, 1024, &processor);
    parameters.tempo = 1.25; /* Faster playback, original pitch. */
    if (status == CHRONOBENT_OK)
        status = chronobent_processor_set_source(processor, read_pcm, &source, source.frames, &parameters);
    while (status == CHRONOBENT_OK) {
        size_t produced = 0;
        status = chronobent_processor_render(processor, output, 256, &produced);
        /* Send exactly produced frames to a file or a worker-to-audio queue.
         * On SOURCE_UNAVAILABLE retain this prefix and resume after recovery. */
    }
    chronobent_processor_destroy(processor);
    if (status != CHRONOBENT_END) fprintf(stderr, "%s\n", chronobent_status_string(status));
    return status == CHRONOBENT_END ? 0 : 1;
}
