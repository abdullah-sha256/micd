#include "mbed.h"
#include "stm32l475e_iot01_audio.h"
#include "FILTER_LIB.h"
#include "WiFiInterface.h"
#include "MQTTClient.h"
#include "MQTTmbed.h"


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

volatile bool mic_enabled = true;
InterruptIn button1(BUTTON1);
DigitalOut led1(LED1); // led is on when audio is recording

// MQTT
SocketAddress broker;
TCPSocket socket;
MQTT::Client<TCPSocket, Countdown> *client;

extern "C" void disable_unused_clocks() {
    // oscillators
    RCC->CR &= ~RCC_CR_HSEON;         // Disable HSE if unused
    RCC->CR &= ~RCC_CR_HSION;         // Disable HSI16 if unused
    RCC->CR &= ~RCC_CR_PLLON;         // Disable main PLL if not used
    RCC->CR &= ~RCC_CR_PLLSAI1ON;     // Disable PLLSAI1
    RCC->CR &= ~RCC_CR_PLLSAI2ON;     // Disable PLLSAI2
    RCC->CR &= ~RCC_CR_HSIKERON;      // Kernel HSI off

    // --- AHB Peripherals ---
    RCC->AHB1ENR &= ~(RCC_AHB1ENR_TSCEN | RCC_AHB1ENR_CRCEN);
    RCC->AHB2ENR &= ~(RCC_AHB2ENR_RNGEN | RCC_AHB2ENR_ADCEN | RCC_AHB2ENR_OTGFSEN | RCC_AHB2ENR_GPIOHEN | RCC_AHB2ENR_GPIOGEN | 
                      RCC_AHB2ENR_GPIOFEN | RCC_AHB2ENR_GPIOEEN | RCC_AHB2ENR_GPIODEN | RCC_AHB2ENR_GPIOCEN);
    // // --- APB1 Peripherals ---
    RCC->APB1ENR1 &= ~(RCC_APB1ENR1_I2C2EN | RCC_APB1ENR1_I2C1EN | RCC_APB1ENR1_SPI2EN | RCC_APB1ENR1_TIM6EN | RCC_APB1ENR1_TIM2EN );
    RCC->APB1ENR2 &= ~(RCC_APB1ENR2_LPUART1EN);
    RCC->APB2ENR &= ~(RCC_APB2ENR_SPI1EN | RCC_APB2ENR_TIM1EN | RCC_APB2ENR_SDMMC1EN);
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

            // Add to buffer
            dB_buffer[dB_index++] = dB;
            if (dB_index >= AVG_BUFFER_SIZE) dB_index = 0;
            if (dB_count < AVG_BUFFER_SIZE) dB_count++;

            // Only compute and send average every full window
            if (dB_count == AVG_BUFFER_SIZE) {
                float sum = 0;
                float min_dB = 0;
                float max_dB = -60;

                for (int i = 0; i < AVG_BUFFER_SIZE; i++) {
                    float val = dB_buffer[i];
                    sum += val;
                    if (val < min_dB) min_dB = val;
                    if (val > max_dB) max_dB = val;
                }
                float avg_dB = sum / AVG_BUFFER_SIZE;

                char payload[128];
                snprintf(payload, sizeof(payload),
                        "{\"avg_decibel\": %.1f, \"min_decibel\": %.1f, \"max_decibel\": %.1f}",
                        avg_dB, min_dB, max_dB);


                publish_mqtt_message("sound/volume", payload);

                dB_count = 0; // reset after publishing
            }
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


bool connect_to_wifi(WiFiInterface *wifi) {
    printf("Connecting to Wi-Fi...\n");

    if (!wifi) {
        printf("ERROR: No WiFiInterface found.\n");
        return false;
    }

    int ret = wifi->connect("BELL740", "46E6AC951223", NSAPI_SECURITY_WPA_WPA2);
    if (ret != 0) {
        printf("❌ WiFi connection failed: %d\n", ret);
        return false;
    }

    SocketAddress a;
    wifi->get_ip_address(&a);
    printf("✅ Wi-Fi connected! IP: %s\n", a.get_ip_address() ? a.get_ip_address() : "None");
    return true;
}

bool connect_to_mqtt(WiFiInterface *wifi) {
    wifi->gethostbyname("192.168.2.138", &broker);
    broker.set_port(1883);

    socket.open(wifi);
    if (socket.connect(broker) != 0) {
        printf("❌ Failed to connect to MQTT broker\n");
        return false;
    }

    client = new MQTT::Client<TCPSocket, Countdown>(socket);
    MQTTPacket_connectData connectData = MQTTPacket_connectData_initializer;
    connectData.MQTTVersion = 3;
    connectData.clientID.cstring = (char *)"disco-board";

    if (client->connect(connectData) != 0) {
        printf("❌ MQTT connection failed\n");
        return false;
    }

    printf("📡 MQTT connected to broker!\n");
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
    printf("📤 MQTT Publish to '%s' => %s\n", topic, pub == 0 ? "OK" : "FAIL");
}


int main()
{
    disable_unused_clocks();
    // Fixes no output issue which ocassionally happens
    static UnbufferedSerial serial_port(USBTX, USBRX, 115200);
    fflush(stdout);

    WiFiInterface *wifi = WiFiInterface::get_default_instance();
    if (!connect_to_wifi(wifi)) return -1;
    if (!connect_to_mqtt(wifi)) return -1;

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