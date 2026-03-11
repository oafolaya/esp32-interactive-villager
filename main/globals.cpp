#include "globals.hpp"


volatile bool isTalking = false;
volatile bool speechDetectedFlag = false; // Is user currently speaking
volatile bool silenceDetectedFlag = false; // speech has ended / no speech detected
volatile bool audioProcessedFlag = false; // Is Villager buffer ready for output
volatile bool doneTalkingFlag = false; // Is playback finished
volatile bool errorDetected = false;

volatile int movementSteps = 0;
volatile int cartSteps = 0;

size_t inputSpeechSamples = 0;

int talkingTimer = 0;


int16_t villagerOutput[MAX_OUTPUT_SAMPLES];
size_t  villagerOutputLen = 0;


i2s_chan_handle_t rx_chan = nullptr;
i2s_chan_handle_t tx_chan = nullptr;
