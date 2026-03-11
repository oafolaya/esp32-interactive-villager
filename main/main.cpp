extern "C" {
    #include <stdio.h>
    //#include "driver/i2s_std.h" 
}

#include <iostream>
#include "globals.hpp"
#include <cstdint>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cmath>
#include "audio_samples.hpp"
#include <cstdio>
#include <cstring>
#include "driver/ledc.h"
#include "sdkconfig.h"
#include "driver/i2s.h"
#include "esp_mac.h"
#include "bootLoader_random.h"
#include "esp_random.h"
#include "driver/gpio.h"



int randomInRange(int min, int max){
    uint32_t random = min + (esp_random()) % (max - min + 1);
    return random;
};

enum class State {
    LISTENING,
    RECORDING,
    PROCESSING,
    TALKING,
    ERROR
};

void resetSystem(){    
    isTalking = false;
    speechDetectedFlag = false;
    silenceDetectedFlag = false;
    audioProcessedFlag = false;
    doneTalkingFlag = false;
    errorDetected = false;
    villagerOutputLen = 0;
}


/*
Microphone is INMP441 (IS2 - digital audio stream)
Amplifier is MAX98357A
Mic (INMP441) → I2S (RX) → gather buffer → detect silence → PROCESSING → select villager sound → I2S (TX) → MAX98357A → speaker

*/

#define PIN_I2S_BCLK        GPIO_NUM_18
#define PIN_I2S_LRCLK       GPIO_NUM_19
#define PIN_I2S_DOUT_MIC    GPIO_NUM_27  // Mic to esp32
#define PIN_I2S_DIN_SPK     GPIO_NUM_22  // Esp32 to amplifier

#define PIN_ARM_PWM         GPIO_NUM_14  
#define PIN_ARM_IN1         GPIO_NUM_26
#define PIN_ARM_IN2         GPIO_NUM_25

#define PIN_CART_STBY       GPIO_NUM_21
#define PIN_ARM_STBY        GPIO_NUM_13

#define PIN_SERVO           GPIO_NUM_15 // was 23

#define PIN_CART_LEFT_IN1   GPIO_NUM_16
#define PIN_CART_LEFT_IN2   GPIO_NUM_17
#define PIN_CART_RIGHT_IN1  GPIO_NUM_4
#define PIN_CART_RIGHT_IN2  GPIO_NUM_5  // should change to 
#define PIN_CART_RIGHT_PWM  GPIO_NUM_33 // was 11
#define PIN_CART_LEFT_PWM   GPIO_NUM_32 // was 12

#define LEDC_LEFT_CHANNEL   LEDC_CHANNEL_0
#define LEDC_RIGHT_CHANNEL  LEDC_CHANNEL_1
#define LEDC_SERVO_CHANNEL  LEDC_CHANNEL_2
#define LEDC_ARM_CHANNEL    LEDC_CHANNEL_3
#define TEST_ARM_MOTOR      0
#define I2S_PORT            I2S_NUM_0
#define CROSSFADE_SAMPLES 128



TaskHandle_t AudioTaskHandle = NULL;
TaskHandle_t AnimationTaskHandle = NULL;
TaskHandle_t MinecartTaskHandle = NULL;
TaskHandle_t MainTaskHandle = NULL;


static void testSpeakerTone() {
    static constexpr int SAMPLE_RATE = 16000;
    static constexpr int DURATION_MS = 500;   // 
    static constexpr int TONE_FREQ   = 440;    // A4 tone
    static constexpr int NUM_SAMPLES = SAMPLE_RATE * DURATION_MS / 1000;

    static int32_t tone_buffer[NUM_SAMPLES];

    for (int n = 0; n < NUM_SAMPLES; ++n) {
        float t = (float)n / SAMPLE_RATE;
        float s = sinf(2.0f * M_PI * TONE_FREQ * t);   // -1.0 to +1.0
        int16_t sample16 = (int16_t)(s * 12000.0f);    // not full volume to avoid clipping

        // Left-justify 16-bit sample in 32-bit slot 
        tone_buffer[n] = (int32_t)sample16 << 16;
    }

    size_t written = 0;
    i2s_write(
        I2S_PORT,
        tone_buffer,
        NUM_SAMPLES * sizeof(int32_t),
        &written,
        portMAX_DELAY
    );

    printf("TEST: Wrote %u bytes of tone to I2S\n", (unsigned)written);
}

/*These functions enable motors when needed.*/
static inline void enableCartMotors(){
    gpio_set_level(PIN_CART_STBY,1);
}

static inline void disableCartMotors(){
    gpio_set_level(PIN_CART_STBY,0);
}

static inline void enableArmMotor(){
    gpio_set_level(PIN_ARM_STBY, 1);
}

static inline void disableArmMotor(){
    gpio_set_level(PIN_ARM_STBY, 0);

}
/* These functions set the speed and direction of the cart motors. */
void leftMotorForward(int PWM){
    enableCartMotors();
    gpio_set_level(PIN_CART_LEFT_IN1, 1);
    gpio_set_level(PIN_CART_LEFT_IN2, 0);
    ledc_set_duty(LEDC_HIGH_SPEED_MODE, LEDC_LEFT_CHANNEL, PWM);
    ledc_update_duty(LEDC_HIGH_SPEED_MODE, LEDC_LEFT_CHANNEL);
}

void rightMotorForward(int PWM){
    enableCartMotors();
    gpio_set_level(PIN_CART_RIGHT_IN1, 1);
    gpio_set_level(PIN_CART_RIGHT_IN2, 0);
    ledc_set_duty(LEDC_HIGH_SPEED_MODE, LEDC_RIGHT_CHANNEL, PWM);
    ledc_update_duty(LEDC_HIGH_SPEED_MODE, LEDC_RIGHT_CHANNEL);
}

void leftMotorBackward(int PWM){
    enableCartMotors();
    gpio_set_level(PIN_CART_LEFT_IN1, 0);
    gpio_set_level(PIN_CART_LEFT_IN2, 1);
    ledc_set_duty(LEDC_HIGH_SPEED_MODE, LEDC_LEFT_CHANNEL, PWM);
    ledc_update_duty(LEDC_HIGH_SPEED_MODE, LEDC_LEFT_CHANNEL);
}
void rightMotorBackward(int PWM){
    enableCartMotors();
    gpio_set_level(PIN_CART_RIGHT_IN1, 0);
    gpio_set_level(PIN_CART_RIGHT_IN2, 1);
    ledc_set_duty(LEDC_HIGH_SPEED_MODE, LEDC_RIGHT_CHANNEL, PWM);
    ledc_update_duty(LEDC_HIGH_SPEED_MODE, LEDC_RIGHT_CHANNEL);
}
void stopMotors(){
    ledc_set_duty(LEDC_HIGH_SPEED_MODE, LEDC_LEFT_CHANNEL, 0);
    ledc_update_duty(LEDC_HIGH_SPEED_MODE, LEDC_LEFT_CHANNEL);

    ledc_set_duty(LEDC_HIGH_SPEED_MODE, LEDC_RIGHT_CHANNEL, 0);
    ledc_update_duty(LEDC_HIGH_SPEED_MODE, LEDC_RIGHT_CHANNEL);

    gpio_set_level(PIN_CART_LEFT_IN1, 0);
    gpio_set_level(PIN_CART_LEFT_IN2, 0);
    gpio_set_level(PIN_CART_RIGHT_IN1, 0);
    gpio_set_level(PIN_CART_RIGHT_IN2, 0);
}

static void servo_write_angle(float angleDegree){
    const int SERVO_MIN_US = 500; // pulse for 0 degrees
    const int SERVO_CENTER_US = 1500; // pulse for 90 degrees (center). Servo must return to this position
    const int SERVO_MAX_US = 2500; // pusle for 180 degrees

    constexpr float pulse_per_degree = (SERVO_MAX_US - SERVO_MIN_US) / 180.0; // 

    if (angleDegree < 0)   angleDegree = 0;
    if (angleDegree > 180) angleDegree = 180;

    float pulse_us = SERVO_MIN_US + angleDegree * pulse_per_degree; // convert angle to pulse

    // Servo task runs at 50 hz and 16-bit resolution. Max duty cycle from 0 - 65535
    const int SERVO_PERIOD_US = 20000; // 20 ms period
    const int SERVO_MAX_DUTY  = 65535; // 2^16-1
    uint32_t duty = (pulse_us * SERVO_MAX_DUTY) / SERVO_PERIOD_US;

    // Apply duty to LEDC
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_SERVO_CHANNEL, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_SERVO_CHANNEL);     
}


static void moveNoseOnce(){
    const TickType_t NOSE_DELAY_MS = pdMS_TO_TICKS(120);
    const int NOSE_AMPLITUDE = 20; // how far the nose deviates from centers in degrees
    const int CENTER_NOSE_ANGLE = 90; // default position for nose
    const int MIN_NOSE_ANGLE = CENTER_NOSE_ANGLE - NOSE_AMPLITUDE; // degrees
    const int MAX_NOSE_ANGLE = CENTER_NOSE_ANGLE + NOSE_AMPLITUDE; // degrees
   
    servo_write_angle(CENTER_NOSE_ANGLE); // start position, in middle
    vTaskDelay(NOSE_DELAY_MS);

    servo_write_angle(MIN_NOSE_ANGLE);    // left position
    vTaskDelay(NOSE_DELAY_MS);

    servo_write_angle(MAX_NOSE_ANGLE);    // right position
    vTaskDelay(NOSE_DELAY_MS);

    servo_write_angle(CENTER_NOSE_ANGLE); // returns to center position
    vTaskDelay(NOSE_DELAY_MS);
}

void moveArm(int PWM){ // enables motor for arms
    enableArmMotor();
    gpio_set_level(PIN_ARM_IN1, 1);
    gpio_set_level(PIN_ARM_IN2, 0);
    ledc_set_duty(LEDC_HIGH_SPEED_MODE, LEDC_ARM_CHANNEL, PWM);
    ledc_update_duty(LEDC_HIGH_SPEED_MODE, LEDC_ARM_CHANNEL);
}

void stopArm(){ // disables motors for arms
    ledc_set_duty(LEDC_HIGH_SPEED_MODE, LEDC_ARM_CHANNEL, 0);
    ledc_update_duty(LEDC_HIGH_SPEED_MODE, LEDC_ARM_CHANNEL);

    gpio_set_level(PIN_ARM_IN1, 0);
    gpio_set_level(PIN_ARM_IN2, 0);
    disableArmMotor();
}

void arm_motor_test_task(void *param)
{ // test function to see if motor works
    while (true)
    {
        const TickType_t slowTime  = pdMS_TO_TICKS(400);
        const TickType_t runTime   = pdMS_TO_TICKS(1500);
        const TickType_t stopTime  = pdMS_TO_TICKS(1500);

        printf("Forward slow...");
        moveArm(30);
        vTaskDelay(slowTime);

        printf("Forward fast...");
        moveArm(60);
        vTaskDelay(runTime);

        printf("Stop...");
        stopArm();
        vTaskDelay(stopTime);
    }
}


static inline int16_t clamp16(int32_t x){
    if (x > 32767) return 32767;
    if (x < -32768) return -32768;
    return (int16_t)x;
}



bool speechDetected(){return speechDetectedFlag;} 
bool silenceDetected(){return silenceDetectedFlag;}
bool audioProcessed(){return audioProcessedFlag;}
bool doneTalking(){return doneTalkingFlag;}

static inline void crossfade(
    int16_t* dst, 
    const int16_t* src, 
    size_t dstOffset, 
    size_t fadeSamples
){
    for (size_t i = 0; i < fadeSamples; ++i) {
        size_t di = dstOffset - fadeSamples + i;

        float outGain = 1.0f - (float)i / fadeSamples;
        float inGain = (float)i / fadeSamples;

        int32_t mixed = (int32_t)(dst[di] * outGain) + (int32_t)(src[i] * inGain);

        dst[di] = clamp16(mixed);
    }
}

static void buildVillagerOutput(){
    // How long we want the villager audio to be (in samples)
    size_t target = inputSpeechSamples;


    // Minimum length so it always "says something"
    if (target < 6000) target = 6000;
    if (target > MAX_OUTPUT_SAMPLES) target = MAX_OUTPUT_SAMPLES;

    villagerOutputLen = 0;
    bool firstChunk = true;

    while (villagerOutputLen + CROSSFADE_SAMPLES < target) {
        // Pick a random villager clip
        int idx = randomInRange(0, NUM_VILLAGER_SAMPLES - 1);
        const int16_t* src = villager_samples[idx];
        size_t srcLen = villager_lengths[idx];

        // Skip absurdly short clips that can't crossfade well
        if (srcLen <= CROSSFADE_SAMPLES + 16) {
            continue;
        }

        // Pick a chunk size from this clip
        size_t chunk = randomInRange(800, 2400);
        if (chunk > srcLen) {
            chunk = srcLen;
        }

        // Don’t overshoot target
        if (villagerOutputLen + chunk > target) {
            chunk = target - villagerOutputLen;
        }

        // If still too small, just copy and bail out
        if (chunk <= CROSSFADE_SAMPLES) {
            memcpy(&villagerOutput[villagerOutputLen], src, chunk * sizeof(int16_t));
            villagerOutputLen += chunk;
            break;
        }

        // Copy chunk into output
        memcpy(&villagerOutput[villagerOutputLen], src, chunk * sizeof(int16_t));

        // Crossfade overlap between previous tail and new head
        if (!firstChunk && villagerOutputLen >= CROSSFADE_SAMPLES) {
            crossfade(
                villagerOutput,
                src,
                villagerOutputLen,
                CROSSFADE_SAMPLES
            );
        }

        villagerOutputLen += chunk;
        firstChunk = false;
    }

    // Safety: if somehow nothing was written, just dump one whole sample
    if (villagerOutputLen == 0 && NUM_VILLAGER_SAMPLES > 0) {
        size_t safetyLen = villager_lengths[0];
        if (safetyLen > target) safetyLen = target;
        memcpy(villagerOutput, villager_samples[0], safetyLen * sizeof(int16_t));
        villagerOutputLen = safetyLen;
    }

    printf("DEBUG: buildVillagerOutput target=%u, finalLen=%u, inputSpeechSamples=%u\n",
           (unsigned)target,
           (unsigned)villagerOutputLen,
           (unsigned)inputSpeechSamples);
}

void playVillagerSpeech() {
    doneTalkingFlag = false;

    printf("DEBUG: villagerOutputLen = %d\n", (int)villagerOutputLen);

    // MONO 32-bit frames
    static int32_t out_buffer[MAX_OUTPUT_SAMPLES];

    for (size_t i = 0; i < villagerOutputLen; i++) {
        int16_t s = villagerOutput[i];

        // Place 16-bit sample in LOW 16 bits of a 32-bit I2S frame
        out_buffer[i] = (int32_t)s << 16;
    }

    size_t written = 0;

    i2s_write(
        I2S_PORT,
        out_buffer,
        villagerOutputLen * sizeof(int32_t), 
        &written,
        portMAX_DELAY
    );

    printf("DEBUG: wrote %u bytes in villager playback (mono)\n", (unsigned)written);

    doneTalkingFlag = true;
}



void AudioTask(void *param){
    static constexpr int FRAME_SAMPLES = 256;
    static constexpr int SAMPLE_RATE = 16000;
    static int32_t audio_buffer[FRAME_SAMPLES];
    const int MAX_INPUT_SAMPLES = 60000;  

    static const float SPEECH_THRESHOLD = 6.0f; // please tune
    static const float SILENCE_THRESHOLD = 2.0f; // 
    static const int FRAMES_TO_DETECT_SPEECH = 1; // number of frames of silence to stop 
    static const int FRAMES_TO_DETECT_SILENCE = 2; 

    const TickType_t audioTaskDelay = pdMS_TO_TICKS(10);

    int record_index = 0;
    int loud_frames = 0;
    int quiet_frames = 0;

    speechDetectedFlag = false;
    silenceDetectedFlag = false;
    inputSpeechSamples = 0;



    size_t bytes_read = 0;
    
    while (true) {
        esp_err_t err = i2s_read(I2S_PORT, audio_buffer, sizeof(audio_buffer), &bytes_read, portMAX_DELAY); // read a frame from I2S microphone audio

        if (err != ESP_OK || bytes_read == 0){ // error catch
            errorDetected = true;
            printf("ERROR: i2s_read err=%d bytes=%u\n", err, (unsigned)bytes_read);
            continue;
        }

        // Root mean squares calculation to detect speech/silence
        float sumSquares = 0.0f;
        int16_t sample16;

        for (int i = 0; i < FRAME_SAMPLES; i++){
            sample16 = clamp16(audio_buffer[i] >> 16);
            float s = static_cast<float>(sample16);
            sumSquares += s * s;
        }

        float rms = sqrtf(sumSquares / FRAME_SAMPLES);

        // Update speech / silence counter based on RMS
        bool frameHasSpeech = (rms > SPEECH_THRESHOLD);
        bool frameHasSilence = (rms < SILENCE_THRESHOLD);

        if (frameHasSpeech){
            loud_frames++;
            quiet_frames = 0;
        } 
        else if (frameHasSilence){
            quiet_frames++;
            loud_frames = 0;
        }
        else{

        }
        
        if (!speechDetectedFlag && loud_frames >= FRAMES_TO_DETECT_SPEECH) {
            speechDetectedFlag  = true;
            silenceDetectedFlag = false;
            inputSpeechSamples  = 0;
            loud_frames  = 0;
            quiet_frames = 0;
            printf("DEBUG: *** SPEECH START *** rms=%.2f\n", rms);
        }

        // Count samples while in speech
        if (speechDetectedFlag) {
            inputSpeechSamples += FRAME_SAMPLES;

             
            if (inputSpeechSamples > MAX_INPUT_SAMPLES) {
                inputSpeechSamples = MAX_INPUT_SAMPLES;
            }
        }   
        

        // End of speech (silence)
        if ((speechDetectedFlag && quiet_frames >= FRAMES_TO_DETECT_SILENCE) || (inputSpeechSamples >= MAX_INPUT_SAMPLES)) {
            speechDetectedFlag  = false;
            silenceDetectedFlag = true;
            loud_frames  = 0;
            quiet_frames = 0;
            printf("DEBUG: *** SPEECH END *** rms=%.2f totalSamples=%u\n",
                   rms, (unsigned)inputSpeechSamples);
        }

        printf("bytes=%u rms=%.2f speech=%d silence=%d inputSamples=%u\n",
               (unsigned)bytes_read,
               rms,
               speechDetectedFlag,
               silenceDetectedFlag,
               (unsigned)inputSpeechSamples);

        vTaskDelay(audioTaskDelay);
    }
}

enum class AnimationState{
    Idle, // Not talking, servo at 90 and arm stopped
    NoseWiggle, // Start of talking, servo wiggles, arm off
    ArmMoving // After wiggle, arm moves, servo at 90 
};

/*
void AnimationTask(void *param){ // moves nose and arms 

    bool wasTalking = false; // tracks state of isTalking var from last tick
    AnimationState animState = AnimationState::Idle; // initialize to idle

    const int SERVO_CENTER_ANGLE = 90; // degrees, default (safe) position of servo
    const int ARM_PWM = 30; // speed setting for motor controlling arm
    

    const TickType_t animationDelay = pdMS_TO_TICKS(20);
    const TickType_t armSettlingDelay = pdMS_TO_TICKS(300);

    while (true){

        if (isTalking){
            if (!wasTalking){
                animState = AnimationState::NoseWiggle; // talking just started so wiggle nose first
            }

                switch (animState){
                    case AnimationState::Idle: 
                        stopArm(); 
                        servo_write_angle(SERVO_CENTER_ANGLE); 
                        break;
                    
                    case AnimationState::NoseWiggle: 
                        stopArm(); 
                        moveNoseOnce(); 
                        servo_write_angle(SERVO_CENTER_ANGLE); 
                        animState = AnimationState::ArmMoving;
                        break;

                    case AnimationState::ArmMoving: 
                        servo_write_angle(SERVO_CENTER_ANGLE);
                        moveArm(ARM_PWM); 
                        break;
                }
        }
        else{
            stopArm();
            vTaskDelay(armSettlingDelay); // allows arm to fall
            servo_write_angle(SERVO_CENTER_ANGLE);
            animState = AnimationState::Idle;
        }

        wasTalking = isTalking;
        vTaskDelay(animationDelay);
    }
}
*/


void AnimationTask(void *param) {
    const int SERVO_CENTER = 90;
    const TickType_t tick = pdMS_TO_TICKS(20);
    const int arm_pwm = 50;

    while (true) {
        if (!isTalking || movementSteps <= 0) {
            // Idle pose
            stopArm();
            servo_write_angle(SERVO_CENTER);
            vTaskDelay(tick);
            continue;
        }

        // --- Phase 1: Nose wiggle (300 ms) ---
        int noseAngle = randomInRange(45, 135);
        servo_write_angle(noseAngle);
        vTaskDelay(pdMS_TO_TICKS(120));
        servo_write_angle(SERVO_CENTER);

        vTaskDelay(pdMS_TO_TICKS(3000));

        
        moveArm(arm_pwm);
        vTaskDelay(pdMS_TO_TICKS(200));
        stopArm();

        movementSteps--;
    }
}



/*

enum class AnimationState{
    Idle, // Not talking, servo at 90 and arm stopped
    NoseWiggle, // Start of talking, servo wiggles, arm off
    ArmMoving // After wiggle, arm moves, servo at 90 
};

void AnimationTask(void *param){ // moves nose and arms 

    bool wasTalking = false; // tracks state of isTalking var from last tick
    AnimationState animState = AnimationState::Idle; // initialize to idle

    const int SERVO_CENTER_ANGLE = 90; // degrees, default (safe) position of servo
    const int ARM_PWM = 30; // speed setting for motor controlling arm
    

    const TickType_t animationDelay = pdMS_TO_TICKS(20);
    const TickType_t armSettlingDelay = pdMS_TO_TICKS(300);

    while (true){

        if (isTalking){
            if (!wasTalking){
                animState = AnimationState::NoseWiggle; // talking just started so wiggle nose first
            }

                switch (animState){
                    case AnimationState::Idle: 
                        stopArm(); 
                        servo_write_angle(SERVO_CENTER_ANGLE); 
                        break;
                    
                    case AnimationState::NoseWiggle: 
                        stopArm(); 
                        moveNoseOnce(); 
                        servo_write_angle(SERVO_CENTER_ANGLE); 
                        animState = AnimationState::ArmMoving;
                        break;

                    case AnimationState::ArmMoving: 
                        servo_write_angle(SERVO_CENTER_ANGLE);
                        moveArm(ARM_PWM); 
                        break;
                }
        }
        else{
            stopArm();
            vTaskDelay(armSettlingDelay); // allows arm to fall
            servo_write_angle(SERVO_CENTER_ANGLE);
            animState = AnimationState::Idle;
        }

        wasTalking = isTalking;
        vTaskDelay(animationDelay);
    }
}
*/


void MinecartTask(void *param){

    enum class CartMotion{
        STOP, 
        FORWARD,
        BACKWARD,
        RIGHT,
        LEFT,
        COUNT // Kepps track of number of states, add new states before this
    };

    const int min = 0;
    const int max = (static_cast<int>(CartMotion::COUNT) - 1);

    const TickType_t minecartDelay = pdMS_TO_TICKS(300);

    while (true) {
        if (!isTalking || cartSteps <= 0) {
            stopMotors();
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
    
        enableCartMotors();

        // int random = randomInRange(min, max);

        CartMotion motion = static_cast<CartMotion>(1); 

        switch (motion){
            case CartMotion::FORWARD: 
                leftMotorForward(100); 
                rightMotorForward(100); 
                break;

            case CartMotion::BACKWARD: 
                leftMotorBackward(100); 
                rightMotorBackward(100); 
                break;

            case CartMotion::RIGHT: 
                leftMotorForward(100); 
                rightMotorBackward(50); 
                break;

            case CartMotion::LEFT: 
                leftMotorBackward(50); 
                rightMotorForward(100); 
                break;

            case CartMotion::STOP: 
                stopMotors(); 
                break;

            default: 
                stopMotors();
                break;
            }

        vTaskDelay(minecartDelay); // Required to make process yield to scheduler
        stopMotors();
        cartSteps--;
    }
}



/*
void MinecartTask(void *param){

    enableCartMotors();

    enum class CartMotion{
        STOP, 
        FORWARD,
        BACKWARD,
        RIGHT,
        LEFT,
        COUNT // Kepps track of number of states, add new states before this
    };

    const int min = 0;
    const int max = (static_cast<int>(CartMotion::COUNT) - 1);

    const TickType_t minecartDelay = pdMS_TO_TICKS(300);

    while (true) {
        enableCartMotors();

        if (isTalking) {

            int random = randomInRange(min, max);

            CartMotion motion = static_cast<CartMotion>(random); 

            switch (motion){
                case CartMotion::FORWARD: 
                    leftMotorForward(60); 
                    rightMotorForward(60); 
                    break;

                case CartMotion::BACKWARD: 
                    leftMotorBackward(60); 
                    rightMotorBackward(60); 
                    break;

                case CartMotion::RIGHT: 
                    leftMotorForward(60); 
                    rightMotorBackward(30); 
                    break;

                case CartMotion::LEFT: 
                    leftMotorBackward(30); 
                    rightMotorForward(60); 
                    break;

                case CartMotion::STOP: 
                    stopMotors(); 
                    break;

                default: 
                    stopMotors();
                    break;
            }
        }
        else {
            stopMotors();
        }

        vTaskDelay(minecartDelay); // Required to make process yield to scheduler
    }
}
*/

void MainTask(void *param){
    State state = State::LISTENING; 
    const TickType_t mainTaskDelay = pdMS_TO_TICKS(20);

    while (true) {  
        switch (state) {

        case State::LISTENING:
            isTalking = false;

            if (speechDetected()){
                state = State::RECORDING;
            }
            break;

        case State::RECORDING:
            isTalking = false;

            if (silenceDetected()){
                silenceDetectedFlag = false;
                state = State::PROCESSING;
            } 
            break;

        case State::PROCESSING:
            isTalking = false;
            buildVillagerOutput();
            audioProcessedFlag = true;
            state = State::TALKING;
            break;

        case State::TALKING:
            isTalking = true;
            playVillagerSpeech();
            movementSteps = 3;
            cartSteps = 2;
            vTaskDelay(pdMS_TO_TICKS(1200));

            // FORCE talking to end (ensures movement only happens once)
            isTalking = false;
            state = State::LISTENING;
            break;

        case State::ERROR:
            isTalking = false;
            // add error recovery logic
            break;
        }
        printf(
            "STATE: state=%d | speech=%d | silence=%d | processed=%d | talking=%d | done=%d\n",
            (int)state,
            speechDetectedFlag,
            silenceDetectedFlag,
            audioProcessedFlag,
            isTalking,
            doneTalkingFlag
        );

        vTaskDelay(mainTaskDelay);
    }
}   




/*

void MainTask(void *param) {

    const TickType_t delay = pdMS_TO_TICKS(3000);

    while (true) {

        // 1. Force system to PROCESS
        printf("TEST: Building villager output...\n");
        buildVillagerOutput();
        audioProcessedFlag = true;
        vTaskDelay(pdMS_TO_TICKS(500));

        // 2. Force TALKING state
        printf("TEST: Talking...\n");
        isTalking = true;
        playVillagerSpeech();
        isTalking = false;

        // 3. Cooldown period before repeating
        printf("TEST: Cycle complete. Waiting...\n");
        vTaskDelay(delay);
    }
}

*/

/*
----------------Old driver/i2s_std.h i2s initialization -----------------


void init_i2s_channels(){
    i2s_chan_config_t chan_cfg{
        .id = I2S_NUM_0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = 4,
        .dma_frame_num = 256,
        .auto_clear = true,
    };

    ESP_ERROR_CHECK(i2s_new_channel( &chan_cfg, &rx_chan,&tx_chan));
}

void init_i2s_mic() {
    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(16000), // sets sample rate
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_32BIT,
                        I2S_SLOT_MODE_MONO
                    ),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = PIN_I2S_BCLK,
            .ws   = PIN_I2S_LRCLK,
            .dout = I2S_GPIO_UNUSED,
            .din  = PIN_I2S_DOUT_MIC
        }
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_chan, &std_cfg));
}

void init_i2s_speaker() {
    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = PIN_I2S_BCLK,
            .ws   = PIN_I2S_LRCLK,
            .dout = PIN_I2S_DIN_SPK,   // MAX98357A amplifier
            .din  = I2S_GPIO_UNUSED,
        },
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_chan, &std_cfg));
}

*/

/*
void MainTask(void *param) {
    const TickType_t delay = pdMS_TO_TICKS(2000);

    while (true) {
        printf("TEST: Building villager output...\n");
        inputSpeechSamples = 8000; // pretend you spoke ~0.5s
        buildVillagerOutput();

        printf("TEST: Playing villager output...\n");
        playVillagerSpeech();

        vTaskDelay(delay);
    }
}
*/


void init_i2s_legacy() {
    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_RX),
        .sample_rate = VILLAGER_SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 4,
        .dma_buf_len = 256,
        .use_apll = false,
        .tx_desc_auto_clear = true,
        .fixed_mclk = 0,
        .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        .bits_per_chan = I2S_BITS_PER_CHAN_DEFAULT
    };

    i2s_pin_config_t pin_config = {
        .mck_io_num   = I2S_PIN_NO_CHANGE,
        .bck_io_num   = PIN_I2S_BCLK,
        .ws_io_num    = PIN_I2S_LRCLK,
        .data_out_num = PIN_I2S_DIN_SPK,  // TX → AMP
        .data_in_num  = PIN_I2S_DOUT_MIC  // RX → MIC  <<<<<< FIX
    };

    ESP_ERROR_CHECK(i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL));
    ESP_ERROR_CHECK(i2s_set_pin(I2S_PORT, &pin_config));
    ESP_ERROR_CHECK(i2s_zero_dma_buffer(I2S_PORT));
}




extern "C" void app_main() {
  
 std::cout << "Hello World!";
 // int32_t raw;

    ledc_timer_config_t timer = {
    .speed_mode = LEDC_HIGH_SPEED_MODE,
    .duty_resolution = LEDC_TIMER_8_BIT,
    .timer_num = LEDC_TIMER_0,
    .freq_hz = 20000,
    .clk_cfg = LEDC_AUTO_CLK
    };
    ledc_timer_config(&timer);
    

    ledc_channel_config_t left_channel = {
        .gpio_num = PIN_CART_LEFT_PWM,
        .speed_mode = LEDC_HIGH_SPEED_MODE,
        .channel = LEDC_LEFT_CHANNEL,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0
    };
    ledc_channel_config(&left_channel);

    ledc_channel_config_t right_channel = {
        .gpio_num = PIN_CART_RIGHT_PWM,
        .speed_mode = LEDC_HIGH_SPEED_MODE,
        .channel = LEDC_RIGHT_CHANNEL,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0
    };
    ledc_channel_config(&right_channel);

    ledc_channel_config_t arm_channel = {
        .gpio_num = PIN_ARM_PWM,
        .speed_mode = LEDC_HIGH_SPEED_MODE,
        .channel = LEDC_ARM_CHANNEL,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0
    };
    ledc_channel_config(&arm_channel);

    ledc_timer_config_t servoTimer = {
    .speed_mode = LEDC_LOW_SPEED_MODE,
    .duty_resolution = LEDC_TIMER_16_BIT,
    .timer_num = LEDC_TIMER_1,
    .freq_hz = 50, 
    .clk_cfg = LEDC_AUTO_CLK
    };
    ledc_timer_config(&servoTimer);

    ledc_channel_config_t servo_channel = {
    .gpio_num = PIN_SERVO,
    .speed_mode = LEDC_LOW_SPEED_MODE,
    .channel = LEDC_SERVO_CHANNEL,
    .timer_sel = LEDC_TIMER_1,
    .duty = 0,
    .hpoint = 0
    };
    ledc_channel_config(&servo_channel);

    gpio_config_t io_conf = {};
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf.intr_type = GPIO_INTR_DISABLE;

   
    io_conf.pin_bit_mask =
       ((1ULL << PIN_CART_LEFT_IN1)  |
        (1ULL << PIN_CART_LEFT_IN2)  |
        (1ULL << PIN_CART_RIGHT_IN1) |
        (1ULL << PIN_CART_RIGHT_IN2) |
        (1ULL << PIN_ARM_IN1)        |
        (1ULL << PIN_ARM_IN2));

    gpio_config(&io_conf);

    
    gpio_config_t io_conf_stby = {};
    io_conf_stby.intr_type = GPIO_INTR_DISABLE;
    io_conf_stby.mode = GPIO_MODE_OUTPUT;
    io_conf_stby.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf_stby.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf_stby.pin_bit_mask = 
        ((1ULL << PIN_CART_STBY) |
        (1ULL << PIN_ARM_STBY));

    gpio_config(&io_conf_stby);

    servo_write_angle(90);
    vTaskDelay(pdMS_TO_TICKS(300));

    #if TEST_ARM_MOTOR
    #warning "TEST_ARM MODE ENABLED"
        xTaskCreate(arm_motor_test_task, "arm_test", 4096, NULL, 1, NULL);
    #endif

    init_i2s_legacy();

    testSpeakerTone();
    enableCartMotors();


    enableArmMotor();
    gpio_set_level(PIN_ARM_IN1, 1);
    gpio_set_level(PIN_ARM_IN2, 0);
    ledc_set_duty(LEDC_HIGH_SPEED_MODE, LEDC_ARM_CHANNEL, 200);
    ledc_update_duty(LEDC_HIGH_SPEED_MODE, LEDC_ARM_CHANNEL);

    moveArm(200);
    vTaskDelay(pdMS_TO_TICKS(1000));
    servo_write_angle(45);
    vTaskDelay(pdMS_TO_TICKS(1000));
    servo_write_angle(135);
    vTaskDelay(pdMS_TO_TICKS(1000));
    stopArm();

    xTaskCreatePinnedToCore(
    AudioTask, 
    "audio", 
    8192,  // Stack size of the task (in BYTES)
    NULL,  // Parameter of the task,which is passed into the task function
    6,     // Priority of the task (1-25)
    &AudioTaskHandle,  // Task handle
    1);    // Pin to core 1 (ESP32 has two cores)

    xTaskCreatePinnedToCore(AnimationTask, "animation",4096, NULL, 5, &AnimationTaskHandle, 1);
    xTaskCreatePinnedToCore(MinecartTask, "minecart", 4096, NULL, 5, &MinecartTaskHandle, 1);
    xTaskCreatePinnedToCore(MainTask, "state", 4096, NULL, 4, &MainTaskHandle, 1);
    //ESP_LOGI("I2S", "rx=%p tx=%p", rx_chan, tx_chan);
}













