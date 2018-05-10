#ifndef Delay_h
#define Delay_h

#define _GNU_SOURCE

#include <alsa/asoundlib.h>
#include "../VSEffect.h"

#include "DelayS.h"

void aef_init(audioeffect *ae);
void aef_setparameter(audioeffect *ae, int i, float value);
float aef_getparameter(audioeffect *ae, int i);
void aef_process(audioeffect *ae, uint8_t* inbuffer, int inbuffersize);
void aef_reinit(audioeffect *ae);
void aef_close(audioeffect *ae);

#endif
