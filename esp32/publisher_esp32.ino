/*
  publisher_wokwi.ino
  ESP32 (Wokwi) - FreeRTOS publisher simulation
  - Two sensor tasks (temp, hum) produce samples and push to a queue
  - Sender task consumes samples and prints JSON payloads to Serial
  - WiFi uses Wokwi-GUEST by default (simulated network)
  - No external libraries required for Wokwi
*/

#include <Arduino.h>
#include <WiFi.h>
#include <sys/time.h>

#define WIFI_SSID     "Wokwi-GUEST"
#define WIFI_PASS     ""

// If you want the sketch to attempt to connect to a gateway, set here
// NOTE: Wokwi cannot reach your local Codespace by default.
// Use an ngrok / public host if you need end-to-end connectivity.
#define GATEWAY_HOST  "1.1.1.1"
#define GATEWAY_PORT  6000

#define PUBLISHER_ID  "esp32-wokwi-01"
#define TOPIC         "sensors/test/environment"

#define SAMPLE_QUEUE_LEN 8
#define MAX_PAYLOAD 512

typedef struct {
  unsigned long ts_ms;
  float temp;
  float hum;
} sample_t;

static QueueHandle_t sampleQueue = NULL;

/* Build compact JSON payload */
static void build_payload(char *buf, size_t buflen, const char *node_id, unsigned long ts_ms, float temp, float hum) {
  int n = snprintf(buf, buflen,
    "{\"node\":\"%s\",\"ts\":%lu,\"topic\":\"%s\",\"data\":{\"temp\":%.2f,\"hum\":%.2f}}",
    node_id, ts_ms, TOPIC, temp, hum);
  if (n < 0 || n >= (int)buflen) buf[buflen-1] = '\0';
}

/* Simulated temperature sensor task */
static void temp_task(void *pv) {
  (void)pv;
  TickType_t lastWake = xTaskGetTickCount();
  const TickType_t interval = pdMS_TO_TICKS(700); // 700 ms
  while (1) {
    sample_t s;
    s.ts_ms = (unsigned long)(esp_timer_get_time() / 1000ULL);
    s.temp = 20.0f + (random(0, 120) / 10.0f); // 20.0 - 31.9
    // peek to get last humidity if present
    sample_t prev;
    if (xQueuePeek(sampleQueue, &prev, 0) == pdTRUE) s.hum = prev.hum;
    else s.hum = 50.0f;
    // push sample (overwrite oldest if full)
    if (xQueueSend(sampleQueue, &s, pdMS_TO_TICKS(20)) != pdTRUE) {
      sample_t tmp; xQueueReceive(sampleQueue, &tmp, 0); xQueueSend(sampleQueue, &s, 0);
    }
    vTaskDelayUntil(&lastWake, interval);
  }
}

/* Simulated humidity sensor task */
static void hum_task(void *pv) {
  (void)pv;
  TickType_t lastWake = xTaskGetTickCount();
  const TickType_t interval = pdMS_TO_TICKS(1100); // 1100 ms
  while (1) {
    sample_t s;
    s.ts_ms = (unsigned long)(esp_timer_get_time() / 1000ULL);
    s.hum = 30.0f + (random(0, 700) / 10.0f); // 30.0 - 99.9
    // combine with last temp if exists
    sample_t out;
    if (xQueuePeek(sampleQueue, &out, 0) == pdTRUE) {
      out.hum = s.hum;
      out.ts_ms = (unsigned long)(esp_timer_get_time() / 1000ULL);
      if (xQueueSend(sampleQueue, &out, pdMS_TO_TICKS(20)) != pdTRUE) { sample_t tmp; xQueueReceive(sampleQueue, &tmp, 0); xQueueSend(sampleQueue, &out, 0); }
    } else {
      sample_t o;
      o.temp = 22.0f + (random(0,50)/10.0f);
      o.hum = s.hum;
      o.ts_ms = (unsigned long)(esp_timer_get_time() / 1000ULL);
      if (xQueueSend(sampleQueue, &o, pdMS_TO_TICKS(20)) != pdTRUE) { sample_t tmp; xQueueReceive(sampleQueue, &tmp, 0); xQueueSend(sampleQueue, &o, 0); }
    }
    vTaskDelayUntil(&lastWake, interval);
  }
}

/* Sender task: prints payload to Serial (and can attempt TCP if configured) */
static void sender_task(void *pv) {
  (void)pv;
  WiFiClient client;
  char payload[MAX_PAYLOAD];
  char header[128];
  while (1) {
    // Wait for a sample
    sample_t sample;
    if (xQueueReceive(sampleQueue, &sample, pdMS_TO_TICKS(3000)) != pdTRUE) {
      // nothing in queue, sleep a bit
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }
    unsigned long now_ms = (unsigned long)(esp_timer_get_time() / 1000ULL);
    build_payload(payload, sizeof(payload), PUBLISHER_ID, now_ms, sample.temp, sample.hum);

    // Print payload to Serial so you can see it in Wokwi
    Serial.print("[PAYLOAD] ");
    Serial.println(payload);

    // Optional: attempt to connect/send to gateway (use only if gateway is publicly reachable)
    // By default, Wokwi cannot reach Codespaces; keep commented or pointing to a public host.
    bool try_send = false; // set true only if you have a public gateway (ngrok/VPS)
    if (try_send) {
      if (!client.connect(GATEWAY_HOST, GATEWAY_PORT, 3000)) {
        Serial.println("[SENDER] cannot connect to gateway (skipping send)");
      } else {
        // send HELLO (optional)
        client.print(String("HELLO PUBLISHER ") + PUBLISHER_ID + "\n");
        // send PUB header + 4-byte BE len + payload
        int hn = snprintf(header, sizeof(header), "PUB %s %u\n", TOPIC, (unsigned)strlen(payload));
        client.write((const uint8_t*)header, hn);
        uint32_t be = htonl((uint32_t)strlen(payload));
        client.write((const uint8_t*)&be, sizeof(be));
        client.write((const uint8_t*)payload, strlen(payload));
        client.stop();
        Serial.println("[SENDER] sent to gateway");
      }
    }

    // small delay to avoid flooding Serial
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\nESP32 publisher (Wokwi) starting...");

  randomSeed((unsigned)esp_random());

  sampleQueue = xQueueCreate(SAMPLE_QUEUE_LEN, sizeof(sample_t));
  if (!sampleQueue) {
    Serial.println("Queue create failed");
    while (1) delay(1000);
  }

  // Connect to Wokwi simulated WiFi
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("Connecting to WiFi '%s' ...\n", WIFI_SSID);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > 10000) { Serial.println("WiFi connect timeout; retrying..."); start = millis(); }
    delay(300); Serial.print(".");
  }
  Serial.println();
  Serial.print("WiFi connected, IP: "); Serial.println(WiFi.localIP());

  // Create FreeRTOS tasks pinned to core 1 (optional)
  xTaskCreatePinnedToCore(temp_task, "temp", 3072, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(hum_task,  "hum",  3072, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(sender_task,"send", 8192, NULL, 2, NULL, 1);
}

void loop() {
  // idle - everything happens in tasks
  delay(1000);
}
