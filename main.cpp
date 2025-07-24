#include "mbed.h"
#include "stm32l475e_iot01_audio.h"
#include "FILTER_LIB.h"

// for collecting audio input
static int16_t PCM_Buffer[PCM_BUFFER_LEN / 2];
static BSP_AUDIO_Init_t MicParams;
static size_t num_samples = (PCM_BUFFER_LEN / 2) / sizeof(int16_t);

static float sampling_time = 1.0f / 16000.0f;
static float CUTOFF = 150.0f;
HPF hp_filter(sampling_time, CUTOFF);

static EventQueue ev_queue;

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
    float dB = process_audio((int16_t*)PCM_Buffer);
    
    ev_queue.call([dB]() {
        print_volume_bar(dB);
        fflush(stdout);
    });
}

// second half of buffer
void BSP_AUDIO_IN_TransferComplete_CallBack(uint32_t Instance) {
    float dB = process_audio((int16_t*)(PCM_Buffer + num_samples));
    
    ev_queue.call([dB]() {
        print_volume_bar(dB);
        fflush(stdout);
    });
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

    ev_queue.dispatch_forever();
}