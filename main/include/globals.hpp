#ifndef GLOBALS_H
#define GLOBALS_H

#include <cstddef>
#include <cstdint>
#include "driver/i2s_std.h"


extern volatile bool isTalking;
extern volatile bool speechDetectedFlag;
extern volatile bool silenceDetectedFlag;
extern volatile bool audioProcessedFlag;
extern volatile bool doneTalkingFlag;
extern volatile bool errorDetected;

extern volatile int movementSteps;
extern volatile int noseSteps;
extern volatile int armSteps;
extern volatile int cartSteps;


extern size_t inputSpeechSamples; // estimation of audio input length in samples


constexpr int VILLAGER_UTTERANCE_MS = 1200;
constexpr int VILLAGER_SAMPLE_RATE  = 16000;
constexpr size_t MAX_OUTPUT_SAMPLES = 20000;

constexpr int NUM_VILLAGER_SAMPLES = 12;


extern int16_t villagerOutput[MAX_OUTPUT_SAMPLES];
extern size_t  villagerOutputLen;


extern i2s_chan_handle_t rx_chan;
extern i2s_chan_handle_t tx_chan;
extern int talkingTimer;

#endif
