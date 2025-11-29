/*
 publisher_esp32.ino
 FreeRTOS-style ESP32 publisher for the tinyiot project.

 - Two sensor tasks (simulated): temp_task and hum_task
 - A sender task that collects latest sample and sends JSON to gateway
 - Protocol:
     HELLO PUBLISHER <id>\n
     PUB <topic> <len>\n
     [4-byte BE len] [payload bytes]
 - Configure WIFI_SSID/WIFI_PASS and GATEWAY_HOST/GATEWAY_PORT below.
 - To use with Wokwi + Codespaces, set GATEWAY_HOST to the ngrok host:port as explained below.
*/

#include <Arduino.h>
#include <WiFi.h>
#include <sys/time.h>


#define WIFI_SSID "Wokwi-GUEST"
#define WIFI_PASS ""

// Gateway address: change before build/flash.
// If using ngrok, put the host (e.g. "3.tcp.ngrok.io") and port (e.g. 12345)
#define GATEWAY_HOST  "127.0.0.1"
#define GATEWAY_PORT  6000

#define PUBLISHER_ID  "esp32-01"
#define TOPIC         "sensors/test/environment"

#define SAMPLE_QUEUE_LEN 8
#define MAX_PAYLOAD 1024

// Data structure shared between tasks
typedef struct {
  unsigned long ts_ms;
  float temp;
  float hum;
} sample_t;

static QueueHandle_t sampleQueue = NULL;

// ---------- Helper: build JSON ----------
static void build_payload(char *buf, size_t buflen, const char *node_id, unsigned long ts_ms, float temp, float hum) {
  // compact JSON
  int n = snprintf(buf, buflen,
    "{\"node\":\"%s\",\"ts\":%lu,\"topic\":\"%s\",\"data\":{\"temp\":%.2f,\"hum\":%.2f}}",
    node_id, ts_ms, TOPIC, temp, hum);
  if (n < 0 || n >= (int)buflen) buf[buflen-1] = '\0';
}

// ---------- Networking helper: blocking send_all using WiFiClient ----------
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

// ---------- Task: simulate temperature sensor ----------
static void temp_task(void *pv) {
  (void)pv;
  TickType_t lastWake = xTaskGetTickCount();
  const TickType_t interval = pdMS_TO_TICKS(700); // produce every 700ms
  while (1) {
    // produce a sample and push to queue (update existing sample if queue full)
    sample_t s;
    s.ts_ms = (unsigned long)(esp_timer_get_time() / 1000ULL);
    s.temp = 20.0f + (random(0, 100) / 10.0f); // 20.0 - 29.9
    // only update temp field: to simplify we push full sample (hum will be last known)
    // read latest hum if present (non-blocking)
    sample_t prev;
    if (xQueuePeek(sampleQueue, &prev, 0) == pdTRUE) {
      s.hum = prev.hum;
    } else {
      s.hum = 50.0f; // default
    }
    // push (overwrite oldest if full)
    if (xQueueSend(sampleQueue, &s, pdMS_TO_TICKS(50)) != pdTRUE) {
      // queue full: remove one and push
      sample_t tmp;
      xQueueReceive(sampleQueue, &tmp, 0);
      xQueueSend(sampleQueue, &s, 0);
    }
    vTaskDelayUntil(&lastWake, interval);
  }
}

// ---------- Task: simulate humidity sensor ----------
static void hum_task(void *pv) {
  (void)pv;
  TickType_t lastWake = xTaskGetTickCount();
  const TickType_t interval = pdMS_TO_TICKS(1100); // produce every 1100ms
  while (1) {
    sample_t s;
    s.ts_ms = (unsigned long)(esp_timer_get_time() / 1000ULL);
    s.hum = 30.0f + (random(0, 700) / 10.0f); // 30.0 - 99.9
    if (xQueuePeek(sampleQueue, &s, 0) == pdTRUE) {
      // update hum into existing sample structure
    }
    // Build a sample that includes last known temp (if any)
    sample_t out;
    if (xQueuePeek(sampleQueue, &out, 0) == pdTRUE) {
      out.hum = s.hum;
      out.ts_ms = (unsigned long)(esp_timer_get_time() / 1000ULL);
      if (xQueueSend(sampleQueue, &out, pdMS_TO_TICKS(50)) != pdTRUE) {
        sample_t tmp; xQueueReceive(sampleQueue, &tmp, 0); xQueueSend(sampleQueue, &out, 0);
      }
    } else {
      // No prior temp, push fresh
      sample_t o;
      o.temp = 22.0f + (random(0,50)/10.0f);
      o.hum = s.hum;
      o.ts_ms = (unsigned long)(esp_timer_get_time() / 1000ULL);
      if (xQueueSend(sampleQueue, &o, pdMS_TO_TICKS(50)) != pdTRUE) {
        sample_t tmp; xQueueReceive(sampleQueue, &tmp, 0); xQueueSend(sampleQueue, &o, 0);
      }
    }
    vTaskDelayUntil(&lastWake, interval);
  }
}

// ---------- Task: sender ----------------
static void sender_task(void *pv) {
  (void)pv;
  WiFiClient client;
  char payload[MAX_PAYLOAD];
  char header[128];
  while (1) {
    // ensure WiFi connected
    if (WiFi.status() != WL_CONNECTED) { vTaskDelay(pdMS_TO_TICKS(1000)); continue; }

    Serial.printf("[SENDER] connecting to gateway %s:%d ...\n", GATEWAY_HOST, GATEWAY_PORT);
    if (!client.connect(GATEWAY_HOST, GATEWAY_PORT, 5000)) {
      Serial.println("[SENDER] connect failed, retry in 2s");
      client.stop();
      vTaskDelay(pdMS_TO_TICKS(2000));
      continue;
    }
    Serial.println("[SENDER] connected, sending HELLO");
    client.print(String("HELLO PUBLISHER ") + PUBLISHER_ID + "\n");

    // wait for OK (short)
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
      client.stop();
      vTaskDelay(pdMS_TO_TICKS(2000));
      continue;
    }

    // Sending loop: poll queue for latest sample (flush duplicates by reading many)
    sample_t sample;
    while (client.connected()) {
      // wait for a sample up to 3s
      if (xQueueReceive(sampleQueue, &sample, pdMS_TO_TICKS(3000)) == pdTRUE) {
        // Build JSON payload with ts in ms
        unsigned long now_ms = (unsigned long)(esp_timer_get_time() / 1000ULL);
        build_payload(payload, sizeof(payload), PUBLISHER_ID, now_ms, sample.temp, sample.hum);
        size_t len = strlen(payload);
        int hn = snprintf(header, sizeof(header), "PUB %s %u\n", TOPIC, (unsigned)len);
        if (hn < 0) { Serial.println("[SENDER] header snprintf error"); continue; }
        // send header
        if (!send_all(client, (const uint8_t*)header, hn)) { Serial.println("[SENDER] header send failed"); break; }
        // send 4-byte BE length
        uint32_t be = htonl((uint32_t)len);
        if (!send_all(client, (const uint8_t*)&be, sizeof(be))) { Serial.println("[SENDER] len send failed"); break; }
        // payload
        if (!send_all(client, (const uint8_t*)payload, len)) { Serial.println("[SENDER] payload send failed"); break; }
        Serial.printf("[SENDER] sent payload len=%u\n", (unsigned)len);
        // read optional OK (short)
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
        if (!got_ok) {
          // not fatal
        }
      } else {
        // no sample in 3s -> check connectivity & continue
      }
    } // while connected
    Serial.println("[SENDER] disconnected from gateway, retry in 2s");
    client.stop();
    vTaskDelay(pdMS_TO_TICKS(2000));
  } // forever
}

// ---------- setup ----------
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("ESP32 publisher (FreeRTOS) starting...");

  randomSeed((unsigned)esp_random());

  sampleQueue = xQueueCreate(SAMPLE_QUEUE_LEN, sizeof(sample_t));
  if (!sampleQueue) {
    Serial.println("Queue create failed");
    while (1) delay(1000);
  }

  // Connect WiFi
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.printf("Connecting to WiFi '%s' ...\n", WIFI_SSID);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > 15000) {
      Serial.println("WiFi connect timeout; retrying...");
      start = millis();
    }
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("WiFi connected, IP: "); Serial.println(WiFi.localIP());

  // Create tasks. Pin tasks to a core (optional)
  xTaskCreatePinnedToCore(temp_task, "temp", 4096, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(hum_task, "hum", 4096, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(sender_task, "sender", 8192, NULL, 2, NULL, 1);
}

void loop() {
  delay(1000);
}
