#include "mbed.h"
#include "stm32l475e_iot01_audio.h"
#include "FILTER_LIB.h"
#include "WiFiInterface.h"
#include "MQTTClient.h"
#include "MQTTmbed.h"

#ifndef MQTT_BROKER_IP
#define MQTT_BROKER_IP "192.168.2.138"
#endif


// MQTT 
#define AVG_WINDOW_SECONDS 30
#define DB_VALUES_PER_SECOND 10  // Tune this based on how often you process audio
#define AVG_BUFFER_SIZE (AVG_WINDOW_SECONDS * DB_VALUES_PER_SECOND)

float dB_buffer[AVG_BUFFER_SIZE];
int dB_index = 0;
int dB_count = 0;

bool connect_to_wifi(WiFiInterface *wifi);
bool connect_to_mqtt(WiFiInterface *wifi);
void publish_mqtt_message(const char *topic, const char *payload);

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
int blink_event_id = 0;

volatile bool mic_enabled = false;
InterruptIn button1(BUTTON1);
DigitalOut led1(LED1); // led is on when audio is recording


// ---- 9-LED bar -----
static const PinName PINS[] = { D0, D1, D2, D3, D4, D5, D6, D7, D8 };
static const int N = sizeof(PINS) / sizeof(PINS[0]);
static DigitalOut* leds[N];

static const int BLUE_POS[]   = {0, 8};

static const int GREEN_POS[]  = {1, 7}; // YELLOW 

static const int YELLOW_POS[] = {2, 6}; // GREEN

static const int RED_POS[]    = {3, 5};

static const int WHITE_POS[]  = {4};

// ===== Beat / onset detection =====
struct OnePoleLP {
    float a = 0.0f, y = 0.0f;
    void init(float fc, float fs) { a = 1.0f - expf(-2.0f * 3.1415926f * fc / fs); }
    inline float step(float x) { y += a * (x - y); return y; }
};

struct Envelope {
    float att = 0.25f, dec = 0.02f, env = 0.0f;
    inline float step(float x) {
        float target = fabsf(x);
        float k = (target > env) ? att : dec;
        env += k * (target - env);
        return env;
    }
};

struct BeatDetector {
    OnePoleLP lp_bass, lp_mid, lp_hi_pre;
    Envelope env_bass, env_mid, env_hi;
    float avg_bass=0, avg_mid=0, avg_hi=0;
    float ema_b=0.02f, ema_m=0.02f, ema_h=0.02f;
    int ref_bass=0, ref_mid=0, ref_hi=0;
    int ref_len_b=8, ref_len_m=8, ref_len_h=8;

    void init(float fs) {
        lp_bass.init(150.0f, fs);     // bass
        lp_mid.init(800.0f, fs);      // pre-smooth mids
        lp_hi_pre.init(3000.0f, fs);  // pre-smooth highs
        env_bass.att=0.35f; env_bass.dec=0.07f;
        env_mid.att =0.30f; env_mid.dec =0.06f;
        env_hi.att  =0.30f; env_hi.dec  =0.06f;
    }

    void process(const int16_t* s, size_t n, bool &kick, bool &midHit, bool &hiHit) {
        kick=midHit=hiHit=false;
        float eB=0, eM=0, eH=0;

        for (size_t i=0;i<n;++i){
            float x = s[i] / 32768.0f;

            float xb = lp_bass.step(x);     // bass LP
            eB = env_bass.step(xb);

            float xm = lp_mid.step(x - xb); // mids ≈ remove bass then smooth
            eM = env_mid.step(xm);

            float xh = lp_hi_pre.step(x - (xb + xm)); // highs ≈ residual
            eH = env_hi.step(xh);
        }

        avg_bass = (1-ema_b)*avg_bass + ema_b*eB;
        avg_mid  = (1-ema_m)*avg_mid  + ema_m*eM;
        avg_hi   = (1-ema_h)*avg_hi   + ema_h*eH;

        float thrB = avg_bass*1.9f + 0.0025f;
        float thrM = avg_mid *1.8f + 0.0020f;
        float thrH = avg_hi  *1.6f + 0.0015f;

        if (ref_bass>0) --ref_bass;
        if (ref_mid >0) --ref_mid;
        if (ref_hi  >0) --ref_hi;

        if (eB>thrB && ref_bass==0) { kick=true;   ref_bass=ref_len_b; }
        if (eM>thrM && ref_mid==0)  { midHit=true; ref_mid =ref_len_m; }
        if (eH>thrH && ref_hi==0)   { hiHit=true;  ref_hi  =ref_len_h; }
    }
} gBeat;

static volatile int pulse_b=0, pulse_g=0, pulse_y=0, pulse_r=0, pulse_w=0;
static const int PULSE_LEN = 7;

template<size_t M>
static inline void group_on(const int (&pos)[M]) {
    for (size_t i=0;i<M;++i) leds[pos[i]]->write(1);
}
template<size_t M>
static inline void group_off(const int (&pos)[M]) {
    for (size_t i=0;i<M;++i) leds[pos[i]]->write(0);
}

static inline void leds_show() {
    // all off
    for (int i=0;i<N;++i) leds[i]->write(0);
    if (pulse_b>0) group_on(BLUE_POS);
    if (pulse_g>0) group_on(GREEN_POS);
    if (pulse_y>0) group_on(YELLOW_POS);
    if (pulse_r>0) group_on(RED_POS);
    if (pulse_w>0) group_on(WHITE_POS);
}

static inline void decay_() {
    if (pulse_b>0) --pulse_b;
    if (pulse_g>0) --pulse_g;
    if (pulse_y>0) --pulse_y;
    if (pulse_r>0) --pulse_r;
    if (pulse_w>0) --pulse_w;
}


// Wifi
static WiFiInterface *g_wifi = nullptr;

// MQTT
SocketAddress broker;
TCPSocket socket;
MQTT::Client<TCPSocket, Countdown> *client;

extern "C" void disable_unused_clocks() {
    // disable secondary PLLs
    RCC->CR &= ~(RCC_CR_PLLSAI1ON | RCC_CR_PLLSAI2ON);

    // turn off low-speed oscillators
    RCC->CSR &= ~RCC_CSR_LSION;    // internal low-speed RC

    // gate buses for unused peripherals
    // AHB1: TSC, CRC
    RCC->AHB1ENR &= ~(RCC_AHB1ENR_TSCEN | RCC_AHB1ENR_CRCEN);

    RCC->AHB2ENR &= ~(RCC_AHB2ENR_RNGEN    // RNG
                    | RCC_AHB2ENR_ADCEN    // ADC
                    | RCC_AHB2ENR_GPIOCEN 
                    | RCC_AHB2ENR_GPIODEN
                    | RCC_AHB2ENR_GPIOEEN
                    | RCC_AHB2ENR_GPIOFEN
                    | RCC_AHB2ENR_GPIOGEN
                    | RCC_AHB2ENR_GPIOHEN);

    // APB1: I2C, SPI2, I think all of the timers need to stay (ask me how i know)
    RCC->APB1ENR1 &= ~(RCC_APB1ENR1_I2C1EN
                     | RCC_APB1ENR1_I2C2EN
                     | RCC_APB1ENR1_SPI2EN);

    // APB1: LPUART1
    RCC->APB1ENR2 &= ~RCC_APB1ENR2_LPUART1EN;

    // APB2: SPI1, TIM1 
    RCC->APB2ENR &= ~(RCC_APB2ENR_SPI1EN | RCC_APB2ENR_TIM1EN);
}

static inline void leds_all(int v) {
    for (int i = 0; i < N; ++i) leds[i]->write(v);
}
static inline void leds_only(const int* idx, int count, int v) {
    for (int i = 0; i < count; ++i) leds[idx[i]]->write(v);
}

void toggle_led()
{
    led1 = !led1;
}


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

void start_blinking_led() {
    if (blink_event_id == 0) {
        blink_event_id = ev_queue.call_every(100ms, toggle_led);
    }
}

void stop_blinking_led() {
    if (blink_event_id != 0) {
        ev_queue.cancel(blink_event_id);
        blink_event_id = 0;
        led1 = mic_enabled;
    }
}

void audio_processing_thread() {
    bool mqtt_ready = false;
    const float WAKE_THRESHOLD = -50.0f;

    while (true) {
        // sleep until any IRQ (DMA handler)
        __WFI();
        osSignalWait(0x1, osWaitForever);

        // Process the buffer half just filled
        if (current_buffer) {
            float dB = process_audio(current_buffer);

            // Audio reaction
            bool kick=false, midHit=false, hiHit=false;
            gBeat.process(current_buffer, num_samples, kick, midHit, hiHit);

            if (kick)   pulse_b = PULSE_LEN;   // Blue group (bass)
            if (midHit) pulse_y = PULSE_LEN;   // Yellow group (mids)
            if (hiHit)  pulse_r = PULSE_LEN;   // Red group (highs)
            if (kick)   pulse_g = PULSE_LEN;

            // White: peak/accents -- two bands hit together
            if ((kick && midHit) || (midHit && hiHit) || (kick && hiHit)) pulse_w = PULSE_LEN;

            decay_();
            leds_show();

            // only act (and log) when we actually exceed threshold
            if (dB < WAKE_THRESHOLD) {
                continue;
            }

            // trigger led blinking when volume is approaching max
            if (dB >= -30.0f) {
                start_blinking_led();
            } else {
                stop_blinking_led();
            }

            // Above threshold—add into rolling window
            dB_buffer[dB_index++] = dB;
            if (dB_index >= AVG_BUFFER_SIZE) dB_index = 0;
            if (dB_count < AVG_BUFFER_SIZE) dB_count++;

            // If we  have a full window, compute stats and publish via MQTT
            if (dB_count == AVG_BUFFER_SIZE) {
                float sum = 0.0f, min_dB = dB_buffer[0], max_dB = dB_buffer[0];
                for (int i = 0; i < AVG_BUFFER_SIZE; i++) {
                    float v = dB_buffer[i];
                    sum += v;
                    min_dB = (v < min_dB ? v : min_dB);
                    max_dB = (v > max_dB ? v : max_dB);
                }
                float avg_dB = sum / AVG_BUFFER_SIZE;

                char payload[128];
                snprintf(payload, sizeof(payload),
                         "{\"avg_decibel\": %.1f, \"min_decibel\": %.1f, \"max_decibel\": %.1f}",
                         avg_dB, min_dB, max_dB);

                if (!mqtt_ready) {
                    printf("[MQTT] connecting to broker…\r\n");
                    if (!connect_to_mqtt(g_wifi)) {
                        printf("[MQTT] connect failed, retry next window\n");
                        continue;
                    }
                    mqtt_ready = true;
                }

                publish_mqtt_message("sound/volume", payload);

                dB_count = 0;  // reset for the next window
            }
        }
    }
}

void toggle_microphone() {
    mic_enabled = !mic_enabled;
    led1 = mic_enabled;

    if (mic_enabled) {
        BSP_AUDIO_IN_Resume(AUDIO_INSTANCE);
    } else {
        BSP_AUDIO_IN_Pause(AUDIO_INSTANCE);
    }
    leds_all(0);
}


bool connect_to_wifi(WiFiInterface *wifi) {
    printf("Connecting to Wi-Fi...\n");

    if (!wifi) {
        printf("ERROR: No WiFiInterface found.\n");
        return false;
    }

    int ret = wifi->connect();
    if (ret != 0) {
        printf("WiFi connection failed: %d\n", ret);
        return false;
    }

    SocketAddress a;
    wifi->get_ip_address(&a);
    printf("Wi-Fi connected! IP: %s\n", a.get_ip_address() ? a.get_ip_address() : "None");
    return true;
}

bool connect_to_mqtt(WiFiInterface *wifi) {
    broker.set_ip_address(MQTT_BROKER_IP);
    broker.set_port(1883);

    socket.open(wifi);
    if (socket.connect(broker) != 0) {
        printf("Failed to connect to MQTT broker\n");
        return false;
    }

    client = new MQTT::Client<TCPSocket, Countdown>(socket);
    MQTTPacket_connectData connectData = MQTTPacket_connectData_initializer;
    connectData.MQTTVersion = 3;
    connectData.clientID.cstring = (char *)"disco-board";

    if (client->connect(connectData) != 0) {
        printf("MQTT connection failed\n");
        return false;
    }

    printf("MQTT connected to broker!\n");
    return true;
}

void publish_mqtt_message(const char *topic, const char *payload) {
    MQTT::Message msg;
    msg.qos = MQTT::QOS0;
    msg.retained = false;
    msg.dup = false;
    msg.payload = (void *)payload;
    msg.payloadlen = strlen(payload);

    int pub = client->publish(topic, msg);
    printf("MQTT Publish to '%s' => %s\n", topic, pub == 0 ? "OK" : "FAIL");
}

void startup_animation() {
    const int step0[] = {4};
    const int step1[] = {3,5};
    const int step2[] = {2,6};
    const int step3[] = {1,7};
    const int step4[] = {0,8};

    const struct { const int* a; int n; } steps[] = {
        { step0, 1 }, { step1, 2 }, { step2, 2 }, { step3, 2 }, { step4, 2 }
    };

    leds_all(0);
    for (int s = 0; s < (int)(sizeof(steps)/sizeof(steps[0])); ++s) {
        leds_only(steps[s].a, steps[s].n, 1);
        ThisThread::sleep_for(120ms);
    }
    ThisThread::sleep_for(100ms);
    leds_all(0);

    for (int i = 0; i < N; ++i) {
        leds_all(0);
        leds[i]->write(1);
        ThisThread::sleep_for(90ms);
    }
    leds_all(0);

    for (int k = 0; k < 3; ++k) {
        leds_all(1);
        ThisThread::sleep_for(120ms);
        leds_all(0);
        ThisThread::sleep_for(120ms);
    }

    leds_all(0);
}


int main()
{
   disable_unused_clocks();
    // Fixes no output issue which ocassionally happens
    static UnbufferedSerial serial_port(USBTX, USBRX, 115200);
    fflush(stdout);

    gBeat.init(16000.0f);
    for (int i = 0; i < N; ++i) leds[i] = new DigitalOut(PINS[i], 0);
    startup_animation();
    
    
    WiFiInterface *wifi = WiFiInterface::get_default_instance();
    g_wifi = wifi;
    if (!connect_to_wifi(wifi)) return -1;

    // event queue runs in separate thread
    event_thread.start(callback(&ev_queue, &EventQueue::dispatch_forever));

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
    BSP_AUDIO_IN_Pause(AUDIO_INSTANCE);
    led1 = 0;

    processing_thread.start(audio_processing_thread);
    processing_thread_id = processing_thread.get_id();

    button1.fall(&toggle_microphone);
}
