/* Edge Impulse ingestion SDK
 * Copyright (c) 2022 EdgeImpulse Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *     http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 */

/* ---------------- 原有 includes ---------------- */
#include <Intelligent_Sitting_Posture_Detector_inferencing.h>
#include <Arduino_LSM9DS1.h>
#include <Arduino_LPS22HB.h>
#include <Arduino_HTS221.h>
#include <Arduino_APDS9960.h>

/* ---------------- 新增库 ----------------------- */
#include <Adafruit_NeoPixel.h>

/* ---------------- 硬件引脚宏 ------------------- */
#define LED_PIN         11          // D11 → Neopixel DIN
#define NUM_LEDS        8
#define VIBRATION_PIN   12          // D12 → MOSFET Gate（振动马达）

Adafruit_NeoPixel strip(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);

/* ---------------- 原文件常量/类型 --------------- */
enum sensor_status {
    NOT_USED = -1,
    NOT_INIT,
    INIT,
    SAMPLED
};

/** Struct to link sensor axis name to sensor value function */
typedef struct{
    const char *name;
    float *value;
    uint8_t (*poll_sensor)(void);
    bool (*init_sensor)(void);    
    sensor_status status;
} eiSensors;

#define CONVERT_G_TO_MS2    9.80665f
#define MAX_ACCEPTED_RANGE  2.0f           // EI firmware accelerometer ±2 g
#define N_SENSORS           18

/* ----------- 前向声明（原文件内容） ------------ */
float   ei_get_sign(float number);
bool    init_IMU(void);
bool    init_HTS(void);
bool    init_BARO(void);
bool    init_APDS(void);

uint8_t poll_acc(void);
uint8_t poll_gyr(void);
uint8_t poll_mag(void);
uint8_t poll_HTS(void);
uint8_t poll_BARO(void);
uint8_t poll_APDS_color(void);
uint8_t poll_APDS_proximity(void);
uint8_t poll_APDS_gesture(void);

static const bool debug_nn = false;        // 打开可看特征等

static float data[N_SENSORS];
static bool  ei_connect_fusion_list(const char *input_list);
static int8_t fusion_sensors[N_SENSORS];
static int    fusion_ix = 0;

/* --------- 番茄钟 / 姿态监控新增全局变量 -------- */
enum TimerState { IDLE, RUNNING, PAUSED_BAD, PAUSED_STANDUP, FINISHED };
static TimerState timerState = IDLE;

static unsigned long startTime   = 0;
static unsigned long pauseStart  = 0;
static unsigned long totalPaused = 0;

const unsigned long POMODORO_MS  = 5UL * 60UL * 1000UL; // 25 min
const unsigned long BAD_PAUSE_MS = 3000UL;               // 5 s

static unsigned long lastBlink = 0;
static bool          blinkOn   = false;
/* ------------------------------------------------ */

/* ---------------- 传感器列表 -------------------- */
eiSensors sensors[] =
{
    "accX", &data[0],  &poll_acc, &init_IMU,  NOT_USED,
    "accY", &data[1],  &poll_acc, &init_IMU,  NOT_USED,
    "accZ", &data[2],  &poll_acc, &init_IMU,  NOT_USED,
    "gyrX", &data[3],  &poll_gyr, &init_IMU,  NOT_USED,
    "gyrY", &data[4],  &poll_gyr, &init_IMU,  NOT_USED,
    "gyrZ", &data[5],  &poll_gyr, &init_IMU,  NOT_USED,
    "magX", &data[6],  &poll_mag, &init_IMU,  NOT_USED,
    "magY", &data[7],  &poll_mag, &init_IMU,  NOT_USED,
    "magZ", &data[8],  &poll_mag, &init_IMU,  NOT_USED,

    "temperature", &data[9],  &poll_HTS, &init_HTS, NOT_USED,
    "humidity",    &data[10], &poll_HTS, &init_HTS, NOT_USED,

    "pressure", &data[11], &poll_BARO, &init_BARO, NOT_USED,

    "red",       &data[12], &poll_APDS_color,     &init_APDS, NOT_USED,
    "green",     &data[13], &poll_APDS_color,     &init_APDS, NOT_USED,
    "blue",      &data[14], &poll_APDS_color,     &init_APDS, NOT_USED,
    "brightness",&data[15], &poll_APDS_color,     &init_APDS, NOT_USED,
    "proximity", &data[16], &poll_APDS_proximity, &init_APDS, NOT_USED,
    "gesture",   &data[17], &poll_APDS_gesture,   &init_APDS, NOT_USED,
};

/* ------------------- setup() ------------------- */
void setup()
{
    /* -------- Serial & EI info -------- */
    Serial.begin(115200);
    while (!Serial);
    Serial.println("Edge Impulse Sensor Fusion Inference");
    Serial.print("Interval = ");
    Serial.print(EI_CLASSIFIER_INTERVAL_MS);
    Serial.println(" ms");

    /* -------- 连接所需传感器 -------- */
    if(!ei_connect_fusion_list(EI_CLASSIFIER_FUSION_AXES_STRING)) {
        ei_printf("ERR: Errors in sensor list detected\r\n");
        return;
    }

    for(int i = 0; i < fusion_ix; i++) {
        if (sensors[fusion_sensors[i]].status == NOT_INIT) {
            sensors[fusion_sensors[i]].status =
                (sensor_status)sensors[fusion_sensors[i]].init_sensor();
            if (!sensors[fusion_sensors[i]].status)
                ei_printf("%s axis sensor initialization failed.\r\n",
                          sensors[fusion_sensors[i]].name);             
            else
                ei_printf("%s axis sensor initialization successful.\r\n",
                          sensors[fusion_sensors[i]].name);
        }
    }

    /* -------- Neopixel & 震动马达 -------- */
    pinMode(VIBRATION_PIN, OUTPUT);
    digitalWrite(VIBRATION_PIN, LOW);

    strip.begin();
    strip.clear();
    strip.show();
}

/* ------------------- loop() ------------------- */
void loop()
{
    ei_printf("\nStarting inferencing in 0.1 seconds...\r\n");
    delay(100);

    if (EI_CLASSIFIER_RAW_SAMPLES_PER_FRAME != fusion_ix) {
        ei_printf("ERR: Sensors don't match the sensors required in the model\r\n"
                  "Following sensors are required: %s\r\n",
                  EI_CLASSIFIER_FUSION_AXES_STRING);
        return;
    }

    ei_printf("Sampling...\r\n");

    /* -- 采样缓冲区 -- */
    float buffer[EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE] = { 0 };

    for (size_t ix = 0; ix < EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE;
         ix += EI_CLASSIFIER_RAW_SAMPLES_PER_FRAME) {

        int64_t next_tick = (int64_t)micros() +
                            ((int64_t)EI_CLASSIFIER_INTERVAL_MS * 1000);

        for(int i = 0; i < fusion_ix; i++) {
            if (sensors[fusion_sensors[i]].status == INIT) {
                sensors[fusion_sensors[i]].poll_sensor();
                sensors[fusion_sensors[i]].status = SAMPLED;
            }
            if (sensors[fusion_sensors[i]].status == SAMPLED) {
                buffer[ix + i] = *sensors[fusion_sensors[i]].value;
                sensors[fusion_sensors[i]].status = INIT;
            }
        }

        int64_t wait_time = next_tick - (int64_t)micros();
        if(wait_time > 0) delayMicroseconds(wait_time);
    }

    /* -- Edge Impulse 推理 -- */
    signal_t signal;
    int err = numpy::signal_from_buffer(buffer,
                                        EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE,
                                        &signal);
    if (err != 0) { ei_printf("ERR:(%d)\r\n", err); return; }

    ei_impulse_result_t result = { 0 };
    err = run_classifier(&signal, &result, debug_nn);
    if (err != EI_IMPULSE_OK) { ei_printf("ERR:(%d)\r\n", err); return; }

    ei_printf("Predictions (DSP: %d ms., Classification: %d ms., Anomaly: %d ms.):\r\n",
        result.timing.dsp, result.timing.classification, result.timing.anomaly);
    for (size_t ix = 0; ix < EI_CLASSIFIER_LABEL_COUNT; ix++) {
        ei_printf("%s: %.5f\r\n",
                  result.classification[ix].label,
                  result.classification[ix].value);
    }
#if EI_CLASSIFIER_HAS_ANOMALY == 1
    ei_printf("    anomaly score: %.3f\r\n", result.anomaly);
#endif

    /* --------------- 番茄钟状态机 --------------- */
    float uprightP = 0, badP = 0, standP = 0, sitP = 0;
    for (size_t ix = 0; ix < EI_CLASSIFIER_LABEL_COUNT; ix++) {
        const char *lab = result.classification[ix].label;
        float v = result.classification[ix].value;
        if      (!strcmp(lab, "Upright"))   uprightP = v;
        else if (!strcmp(lab, "Bad"))       badP     = v;
        else if (!strcmp(lab, "Stand_Up"))  standP   = v;
        else if (!strcmp(lab, "Sit_Down"))  sitP     = v;
    }

    unsigned long now = millis();

    switch (timerState)
    {
        case IDLE:
            if (uprightP > 0.80f) {
                timerState  = RUNNING;
                startTime   = now;
                totalPaused = 0;
                updateProgressLEDs(0);
                ei_printf("Pomodoro started!\r\n");
            }
            break;

        case RUNNING:
            if (badP > 0.80f) {
                timerState = PAUSED_BAD;
                pauseStart = now;
                setStripColor(strip.Color(255, 0, 0));
                digitalWrite(VIBRATION_PIN, HIGH);
                ei_printf("Bad posture – pause.\r\n");
                break;
            }
            if (standP > 0.45f) {
                timerState = PAUSED_STANDUP;
                pauseStart = now;
                setStripColor(strip.Color(255, 255, 0));
                ei_printf("Stand up – pause.\r\n");
                break;
            }

            {
                unsigned long elapsed = now - startTime - totalPaused;
                if (elapsed >= POMODORO_MS) {
                    timerState = FINISHED;
                    lastBlink  = now;
                    strip.clear(); strip.show();
                    ei_printf("Pomodoro finished!\r\n");
                } else {
                    updateProgressLEDs(elapsed);
                }
            }
            break;

        case PAUSED_BAD:
            if (now - pauseStart >= BAD_PAUSE_MS) {
                totalPaused += now - pauseStart;
                digitalWrite(VIBRATION_PIN, LOW);
                timerState = RUNNING;
                ei_printf("Bad pause over – resume.\r\n");
            }
            break;

        case PAUSED_STANDUP:
            if (sitP > 0.45f) {
                totalPaused += now - pauseStart;
                timerState  = RUNNING;
                ei_printf("Sit down – resume.\r\n");
            }
            break;

        case FINISHED:
            if (now - lastBlink >= 500) {
                lastBlink = now;
                blinkOn = !blinkOn;
                uint32_t c = blinkOn ? strip.Color(0, 255, 0) : 0;
                setStripColor(c);
            }
            break;
    }
    /* --------------- 状态机结束 --------------- */
}

/* ------------------ LED 工具函数 ------------------ */
void setStripColor(uint32_t c) {
    for (int i = 0; i < NUM_LEDS; ++i) strip.setPixelColor(i, c);
    strip.show();
}
void updateProgressLEDs(unsigned long elapsed) {
    float ratio = min(1.0f, (float)elapsed / POMODORO_MS);
    int lit = (int)(ratio * NUM_LEDS + 0.5f);   // 0~8
    for (int i = 0; i < NUM_LEDS; ++i)
        strip.setPixelColor(i,
            (i < lit) ? strip.Color(0, 255, 0) : 0);
    strip.show();
}

/* ========== 以下为原始文件其余函数 ========== */
#if !defined(EI_CLASSIFIER_SENSOR) || \
    (EI_CLASSIFIER_SENSOR != EI_CLASSIFIER_SENSOR_FUSION && \
     EI_CLASSIFIER_SENSOR != EI_CLASSIFIER_SENSOR_ACCELEROMETER)
#error "Invalid model for current sensor"
#endif

static int8_t ei_find_axis(char *axis_name)
{
    for(int ix = 0; ix < N_SENSORS; ix++) {
        if(strstr(axis_name, sensors[ix].name)) {
            return ix;
        }
    }
    return -1;
}

static bool ei_connect_fusion_list(const char *input_list)
{
    char *buff;
    bool is_fusion = false;

    char *input_string = (char *)ei_malloc(strlen(input_list) + 1);
    if (input_string == NULL) return false;
    memset(input_string, 0, strlen(input_list) + 1);
    strncpy(input_string, input_list, strlen(input_list));

    memset(fusion_sensors, 0, N_SENSORS);
    fusion_ix = 0;

    buff = strtok(input_string, "+");

    while (buff != NULL) {
        int8_t found_axis = 0;
        is_fusion = false;
        found_axis = ei_find_axis(buff);

        if(found_axis >= 0) {
            if(fusion_ix < N_SENSORS) {
                fusion_sensors[fusion_ix++] = found_axis;
                sensors[found_axis].status = NOT_INIT;
            }
            is_fusion = true;
        }
        buff = strtok(NULL, "+ ");
    }

    ei_free(input_string);
    return is_fusion;
}

float ei_get_sign(float number) {
    return (number >= 0.0f) ? 1.0f : -1.0f;
}

bool init_IMU(void) {
    static bool init_status = false;
    if (!init_status) init_status = IMU.begin();
    return init_status;
}
bool init_HTS(void) {
    static bool init_status = false;
    if (!init_status) init_status = HTS.begin();
    return init_status;
}
bool init_BARO(void) {
    static bool init_status = false;
    if (!init_status) init_status = BARO.begin();
    return init_status;
}
bool init_APDS(void) {
    static bool init_status = false;
    if (!init_status) init_status = APDS.begin();
    return init_status;
}

uint8_t poll_acc(void) {
    if (IMU.accelerationAvailable()) {
        IMU.readAcceleration(data[0], data[1], data[2]);
        for (int i = 0; i < 3; i++) {
            if (fabs(data[i]) > MAX_ACCEPTED_RANGE)
                data[i] = ei_get_sign(data[i]) * MAX_ACCEPTED_RANGE;
        }
        data[0] *= CONVERT_G_TO_MS2;
        data[1] *= CONVERT_G_TO_MS2;
        data[2] *= CONVERT_G_TO_MS2;
    }
    return 0;
}
uint8_t poll_gyr(void) {
    if (IMU.gyroscopeAvailable())
        IMU.readGyroscope(data[3], data[4], data[5]);
    return 0;
}
uint8_t poll_mag(void) {
    if (IMU.magneticFieldAvailable())
        IMU.readMagneticField(data[6], data[7], data[8]);
    return 0;
}
uint8_t poll_HTS(void) {
    data[9]  = HTS.readTemperature();
    data[10] = HTS.readHumidity();
    return 0;
}
uint8_t poll_BARO(void) {
    data[11] = BARO.readPressure();   // kPa
    return 0;
}
uint8_t poll_APDS_color(void) {
    int temp_data[4];
    if (APDS.colorAvailable()) {
        APDS.readColor(temp_data[0], temp_data[1], temp_data[2], temp_data[3]);
        data[12] = temp_data[0];
        data[13] = temp_data[1];
        data[14] = temp_data[2];
        data[15] = temp_data[3];
    }
    return 0;
}
uint8_t poll_APDS_proximity(void) {
    if (APDS.proximityAvailable())
        data[16] = (float)APDS.readProximity();
    return 0;
}
uint8_t poll_APDS_gesture(void) {
    if (APDS.gestureAvailable())
        data[17] = (float)APDS.readGesture();
    return 0;
}
