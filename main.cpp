#include "mbed.h"
#include "stm32l475e_iot01_audio.h"
#include "FILTER_LIB.h"

static int16_t PCM_Buffer[PCM_BUFFER_LEN / 2];
static int16_t *current_buffer = nullptr;
static BSP_AUDIO_Init_t MicParams;
static size_t num_samples = (PCM_BUFFER_LEN / 2) / sizeof(int16_t);
static float sampling_time = 1.0f / 16000.0f;
static float CUTOFF = 200.0f;
HPF hp_filter(sampling_time, CUTOFF);

static EventQueue ev_queue;
Thread processing_thread;
Thread event_thread;
static osThreadId_t processing_thread_id;

volatile bool mic_enabled = true;
InterruptIn button1(BUTTON1);
DigitalOut led1(LED1); // led is on when audio is recording

float process_audio(int16_t *samples) {
    // compute rms
    float sum_squares = 0.0f;
    for (size_t i = 0; i < num_samples; i++) {
        float sample = hp_filter.filter(samples[i]);
        sample /= 32768.0f; // normalize to [-1, 1]
        sum_squares += sample * sample;
    }
    float rms = sqrtf(sum_squares / num_samples);

    // compute dbfs
    float dbfs = 20.0f * log10f(rms);
    if (dbfs > 0.0f) dbfs = 0.0f;
    else if (dbfs < -60.0f) dbfs = -60.0f;

    return dbfs;
}

// TODO: replace this with driving LED once we get them
// the output is slightly out of sync with audio input 
// because printf is slow i think
void print_volume_bar(float dB) {
    // map -60 dB to 0 bars, 0 dB to 20 bars
    int bar_count = (int)((dB + 60.0f) / 3.0f);  // 0–20 steps

    printf("[");
    for (int i = 0; i < bar_count; i++) {
        printf("-");
    }
    for (int i = bar_count; i < 20; i++) {
        printf(" ");
    }
    printf("] %d\n", (int)dB);
}

// first half of buffer
void BSP_AUDIO_IN_HalfTransfer_CallBack (uint32_t Instance) {
    if (!mic_enabled) return;
    current_buffer = PCM_Buffer;
    osSignalSet(processing_thread_id, 0x1);
}

// second half of buffer
void BSP_AUDIO_IN_TransferComplete_CallBack(uint32_t Instance) {
    if (!mic_enabled) return;
    current_buffer = PCM_Buffer + num_samples;
    osSignalSet(processing_thread_id, 0x1);
}

void audio_processing_thread() {
    while (true) {
        osSignalWait(0x1, osWaitForever);

        if (current_buffer != nullptr) {
            float dB = process_audio(current_buffer);
            ev_queue.call(print_volume_bar, dB);
        }
    }
}

void toggle_microphone() {
    mic_enabled = !mic_enabled;
    led1 = !led1;

    if (mic_enabled) {
        BSP_AUDIO_IN_Resume(AUDIO_INSTANCE);
    } else {
        BSP_AUDIO_IN_Pause(AUDIO_INSTANCE);
    }
}

int main()
{
    // set up the microphone and start recording input
    MicParams.BitsPerSample = 16;
    MicParams.ChannelsNbr = AUDIO_CHANNELS;
    MicParams.Device = AUDIO_IN_DIGITAL_MIC1;
    MicParams.SampleRate = AUDIO_SAMPLING_FREQUENCY;
    MicParams.Volume = 32;

    int32_t ret = BSP_AUDIO_IN_Init(AUDIO_INSTANCE, &MicParams);

    if (ret != BSP_ERROR_NONE) {
        printf("Error Audio Init (%d)\r\n", ret);
        fflush(stdout);
        return 1;
    } else {
        printf("OK Audio Init\t(Audio Freq=%d)\r\n", AUDIO_SAMPLING_FREQUENCY);
        fflush(stdout);
    }

    BSP_AUDIO_IN_Record(AUDIO_INSTANCE, (uint8_t*)PCM_Buffer, PCM_BUFFER_LEN);
    led1 = 1;

    processing_thread.start(audio_processing_thread);
    processing_thread_id = processing_thread.get_id();

    button1.fall(&toggle_microphone);

    ev_queue.dispatch_forever();
}