/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Nicolas Wehmeyer */
#include "chronobent/tuning.h"
int main(void) {
    chronobent_tuner *t=0;
    chronobent_tune_config config=chronobent_tune_default_config(48000,2);
    chronobent_tune_options options=chronobent_tune_default_options();
    size_t produced=99,count=99;
    if(chronobent_tune_create(&config,0,&t)!=CHRONOBENT_OK)return 1;
    if(chronobent_tune_analyze(t,0,0,0,0)!=CHRONOBENT_OK)return 2;
    if(chronobent_tune_set_options(t,&options)!=CHRONOBENT_OK)return 3;
    if(chronobent_tune_set_notes(t,0,0)!=CHRONOBENT_OK)return 4;
    if(chronobent_tune_frames(t,&count)!=0 || count!=0)return 5;
    if(chronobent_tune_render(t,0,0,0,0,0,&produced)!=CHRONOBENT_END || produced!=0)return 6;
    chronobent_tune_destroy(t);return 0;
}
