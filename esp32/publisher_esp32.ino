/*
 publisher_esp32_ngrok.ino
 ESP32 (esp32dev) - FreeRTOS publisher that sends simulated sensor data
 - Simula temperatura y humedad (2 tareas)
 - Imprime payloads por Serial (ver en Wokwi)
 - Intenta conectar y enviar al gateway público via ngrok (6.tcp.ngrok.io:14342)
 - Core: no librerías externas necesarias (WiFi.h viene con el core)
*/

#include <Arduino.h>
#include <WiFi.h>
#include <sys/time.h>
#include <arpa/inet.h> // htonl

// --- WiFi (Wokwi simulated) ---
#define WIFI_SSID     "Wokwi-GUEST"
#define WIFI_PASS     ""

// --- ngrok endpoint (tu ID) ---
#define GATEWAY_HOST  "6.tcp.ngrok.io"
#define GATEWAY_PORT  14342

#define PUBLISHER_ID  "esp32-wokwi-ngrok-01"
#define TOPIC         "sensors/test/environment"

#define SAMPLE_QUEUE_LEN 8
#define MAX_PAYLOAD 1024

typedef struct {
  unsigned long ts_ms;
  float temp;
  float hum;
} sample_t;

static QueueHandle_t sampleQueue = NULL;

static void build_payload(char *buf, size_t buflen, const char *node_id, unsigned long ts_ms, float temp, float hum) {
  int n = snprintf(buf, buflen,
    "{\"node\":\"%s\",\"ts\":%lu,\"topic\":\"%s\",\"data\":{\"temp\":%.2f,\"hum\":%.2f}}",
    node_id, ts_ms, TOPIC, temp, hum);
  if (n < 0 || n >= (int)buflen) buf[buflen-1] = '\0';
}

static void temp_task(void *pv) {
  (void)pv;
  TickType_t lastWake = xTaskGetTickCount();
  const TickType_t interval = pdMS_TO_TICKS(700);
  while (1) {
    sample_t s;
    s.ts_ms = (unsigned long)(esp_timer_get_time() / 1000ULL);
    s.temp = 20.0f + (random(0, 120) / 10.0f); // 20.0 - 31.9
    sample_t prev;
    if (xQueuePeek(sampleQueue, &prev, 0) == pdTRUE) s.hum = prev.hum;
    else s.hum = 50.0f;
    if (xQueueSend(sampleQueue, &s, pdMS_TO_TICKS(20)) != pdTRUE) {
      sample_t tmp; xQueueReceive(sampleQueue, &tmp, 0); xQueueSend(sampleQueue, &s, 0);
    }
    vTaskDelayUntil(&lastWake, interval);
  }
}

static void hum_task(void *pv) {
  (void)pv;
  TickType_t lastWake = xTaskGetTickCount();
  const TickType_t interval = pdMS_TO_TICKS(1100);
  while (1) {
    sample_t s;
    s.ts_ms = (unsigned long)(esp_timer_get_time() / 1000ULL);
    s.hum = 30.0f + (random(0,700)/10.0f); // 30.0 - 99.9
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

static bool send_all(WiFiClient &c, const uint8_t *data, size_t len, unsigned long timeout_ms=3000) {
  unsigned long start = millis();
  size_t sent = 0;
  while (sent < len) {
    size_t w = c.write(data + sent, len - sent);
    if (w > 0) { sent += w; continue; }
    if (!c.connected()) return false;
    if (millis() - start > timeout_ms) return false;
    delay(1);
  }
  return true;
}

static void sender_task(void *pv) {
  (void)pv;
  WiFiClient client;
  char payload[MAX_PAYLOAD];
  char header[128];
  const bool try_send = true; // ACTIVADO: envia via ngrok -> gateway en Codespace
  while (1) {
    sample_t sample;
    if (xQueueReceive(sampleQueue, &sample, pdMS_TO_TICKS(3000)) != pdTRUE) {
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }
    unsigned long now_ms = (unsigned long)(esp_timer_get_time() / 1000ULL);
    build_payload(payload, sizeof(payload), PUBLISHER_ID, now_ms, sample.temp, sample.hum);

    // Mostrar en Serial siempre
    Serial.print("[PAYLOAD] ");
    Serial.println(payload);

    if (!try_send) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }

    // Intentar conectar al gateway via ngrok
    Serial.printf("[SENDER] connecting to %s:%d ...\n", GATEWAY_HOST, GATEWAY_PORT);
    if (!client.connect(GATEWAY_HOST, GATEWAY_PORT, 5000)) {
      Serial.println("[SENDER] connect failed, retry later");
      client.stop();
      vTaskDelay(pdMS_TO_TICKS(2000));
      continue;
    }
    Serial.println("[SENDER] connected, sending HELLO");
    client.print(String("HELLO PUBLISHER ") + PUBLISHER_ID + "\n");

    // esperar OK corto
    unsigned long t0 = millis();
    String resp = "";
    while (millis() - t0 < 3000) {
      while (client.available()) {
        char ch = client.read();
        if (ch == '\n') goto got_ok;
        resp += ch;
      }
      delay(1);
    }
got_ok:
    resp.trim();
    Serial.printf("[SENDER] gateway replied: '%s'\n", resp.c_str());
    if (resp != "OK") {
      Serial.println("[SENDER] HELLO not acknowledged, closing");
      client.stop();
      vTaskDelay(pdMS_TO_TICKS(2000));
      continue;
    }

    // enviar header PUB + 4B BE + payload
    size_t len = strlen(payload);
    int hn = snprintf(header, sizeof(header), "PUB %s %u\n", TOPIC, (unsigned)len);
    if (hn < 0) { Serial.println("[SENDER] header snprintf error"); client.stop(); vTaskDelay(pdMS_TO_TICKS(2000)); continue; }
    if (!send_all(client, (const uint8_t*)header, hn)) { Serial.println("[SENDER] header send failed"); client.stop(); vTaskDelay(pdMS_TO_TICKS(2000)); continue; }
    uint32_t be = htonl((uint32_t)len);
    if (!send_all(client, (const uint8_t*)&be, sizeof(be))) { Serial.println("[SENDER] len send failed"); client.stop(); vTaskDelay(pdMS_TO_TICKS(2000)); continue; }
    if (!send_all(client, (const uint8_t*)payload, len)) { Serial.println("[SENDER] payload send failed"); client.stop(); vTaskDelay(pdMS_TO_TICKS(2000)); continue; }

    Serial.printf("[SENDER] sent payload len=%u\n", (unsigned)len);
    // opcional: esperar OK del gateway
    unsigned long tq = millis();
    String okr = "";
    bool got_ok = false;
    while (millis() - tq < 1000) {
      while (client.available()) {
        char ch = client.read();
        if (ch == '\n') { okr.trim(); if (okr == "OK") got_ok = true; break; }
        okr += ch;
      }
      if (got_ok) break;
      delay(1);
    }

    client.stop();
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\nESP32 publisher (esp32dev) starting (ngrok) ...");

  randomSeed((unsigned)esp_random());

  sampleQueue = xQueueCreate(SAMPLE_QUEUE_LEN, sizeof(sample_t));
  if (!sampleQueue) {
    Serial.println("Queue create failed");
    while (1) delay(1000);
  }

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("Connecting to WiFi '%s' ...\n", WIFI_SSID);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > 15000) { Serial.println("WiFi connect timeout; retrying..."); start = millis(); }
    delay(300); Serial.print(".");
  }
  Serial.println();
  Serial.print("WiFi connected, IP: "); Serial.println(WiFi.localIP());

  xTaskCreatePinnedToCore(temp_task, "temp", 3072, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(hum_task,  "hum",  3072, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(sender_task,"send", 8192, NULL, 2, NULL, 1);
}

void loop() {
  delay(1000);
}