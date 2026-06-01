#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <TFT_eSPI.h>
#include <esp_heap_caps.h>

#define BOOT_BUTTON 0

#define VIDEO_LANDSCAPE 1

#if VIDEO_LANDSCAPE
#define VID_W 160
#define VID_H 128
#define TFT_ROTATION 1
#else
#define VID_W 128
#define VID_H 160
#define TFT_ROTATION 0
#endif

#define VIDEO_FPS 15
#define FRAME_SIZE (VID_W * VID_H * 2UL)
#define FRAME_TIME_US (1000000UL / VIDEO_FPS)

#define WIFI_STA_SSID "MIND DECOR"
#define WIFI_STA_PASS "0937096673"
#define WIFI_CONNECT_TIMEOUT_MS 15000

#define PC_HOST "192.168.1.19"
#define PC_PORT 8000
#define VIDEO_URL_PATH "/video.rgb"

#define TCP_READ_TIMEOUT_MS 8000
#define RECONNECT_DELAY_MS 800
#define BUTTON_DEBOUNCE_MS 40

#define RING_BUFFER_COUNT 4
#define TARGET_FRAMES_PER_BUFFER 24
#define START_BUFFER_READY_COUNT 3

static constexpr bool TFT_SWAP_BYTES = true;
static constexpr bool DEBUG_FRAME_TIME = false;
static constexpr uint32_t DEBUG_EVERY_FRAMES = 60;

TFT_eSPI tft = TFT_eSPI();
WiFiClient client;

struct VideoBuffer {
  uint8_t *data = nullptr;
  uint32_t validFrames = 0;
  volatile bool ready = false;
  volatile bool filling = false;
  volatile bool playing = false;
};

VideoBuffer ring[RING_BUFFER_COUNT];
uint32_t framesPerBuffer = TARGET_FRAMES_PER_BUFFER;
size_t bytesPerBuffer = 0;

SemaphoreHandle_t bufferMutex;
TaskHandle_t netTaskHandle = nullptr;
TaskHandle_t displayTaskHandle = nullptr;

volatile bool streamConnected = false;
volatile bool streamEOF = false;
volatile bool reconnectRequested = false;
volatile bool fatalError = false;

uint32_t streamContentLength = 0;
uint32_t streamFrames = 0;
volatile uint32_t bytesStreamed = 0;
volatile uint32_t framesDisplayed = 0;

volatile uint32_t lastNetReadUs = 0;
volatile uint32_t lastDisplayPushUs = 0;
volatile uint32_t lastBufferFrames = 0;
volatile int lastRSSI = 0;

bool bootPressed() {
  if (digitalRead(BOOT_BUTTON) != LOW) return false;
  delay(BUTTON_DEBOUNCE_MS);
  return digitalRead(BOOT_BUTTON) == LOW;
}

void waitForBootRelease() {
  while (digitalRead(BOOT_BUTTON) == LOW) delay(10);
  delay(80);
}

void initTFT() {
  tft.init();
  tft.setRotation(TFT_ROTATION);
  tft.setSwapBytes(TFT_SWAP_BYTES);
  tft.fillScreen(TFT_BLACK);
}

void showMessage(uint16_t bg, uint16_t fg, const char *line1, const char *line2 = nullptr, const char *line3 = nullptr, const char *line4 = nullptr) {
  tft.fillScreen(bg);
  tft.setTextColor(fg, bg);
  tft.setTextSize(1);
  int y = 14;
  tft.setCursor(4, y);
  tft.println(line1);
  if (line2) { y += 20; tft.setCursor(4, y); tft.println(line2); }
  if (line3) { y += 20; tft.setCursor(4, y); tft.println(line3); }
  if (line4) { y += 20; tft.setCursor(4, y); tft.println(line4); }
}

void showStreamInfo(const char *title, uint16_t color) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextSize(1);
  tft.setTextColor(color, TFT_BLACK);
  tft.setCursor(4, 8); tft.println(title);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(4, 28); tft.print("IP: "); tft.println(WiFi.localIP());
  tft.setCursor(4, 44); tft.print("Host: "); tft.println(PC_HOST);
  tft.setCursor(4, 60); tft.print("FPS: "); tft.println(VIDEO_FPS);
  tft.setCursor(4, 76); tft.print("Frames: "); tft.println(streamFrames);
  tft.setCursor(4, 92); tft.print("Bufs: "); tft.print(RING_BUFFER_COUNT); tft.print(" x "); tft.println(framesPerBuffer);
  tft.setCursor(4, 108); tft.print("Cache: "); tft.print((uint32_t)(bytesPerBuffer * RING_BUFFER_COUNT / 1024)); tft.println(" KB");
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setCursor(4, 140); tft.println("BOOT = reconnect");
}

bool connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_STA_SSID, WIFI_STA_PASS);

  showMessage(TFT_BLACK, TFT_WHITE, "Connecting WiFi", WIFI_STA_SSID);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    showMessage(TFT_RED, TFT_WHITE, "WiFi failed", "Check SSID/pass");
    return false;
  }

  Serial.print("WiFi connected. IP: ");
  Serial.println(WiFi.localIP());
  return true;
}

String readHttpLine(WiFiClient &c) {
  String line = c.readStringUntil('\n');
  line.trim();
  return line;
}

void clearRingState() {
  xSemaphoreTake(bufferMutex, portMAX_DELAY);
  for (uint32_t i = 0; i < RING_BUFFER_COUNT; i++) {
    ring[i].validFrames = 0;
    ring[i].ready = false;
    ring[i].filling = false;
    ring[i].playing = false;
  }
  xSemaphoreGive(bufferMutex);
}

bool allocRingBuffers() {
  if (ring[0].data) return true;

  uint32_t options[] = {16, 12, 8, 6, 4};

  Serial.printf("PSRAM found: %s\n", psramFound() ? "yes" : "no");
  Serial.printf("Free PSRAM: %u\n", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  Serial.printf("Largest PSRAM block: %u\n", heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));

  for (uint32_t o = 0; o < sizeof(options) / sizeof(options[0]); o++) {
    uint32_t frames = options[o];
    size_t bytes = frames * FRAME_SIZE;
    bool ok = true;

    for (uint32_t i = 0; i < RING_BUFFER_COUNT; i++) {
      ring[i].data = nullptr;
    }

    for (uint32_t i = 0; i < RING_BUFFER_COUNT; i++) {
      if (psramFound()) {
        ring[i].data = (uint8_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      }
      if (!ring[i].data && bytes <= 256 * 1024) {
        ring[i].data = (uint8_t *)heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
      }
      if (!ring[i].data) {
        ok = false;
        break;
      }
    }

    if (ok) {
      framesPerBuffer = frames;
      bytesPerBuffer = bytes;
      clearRingState();
      Serial.printf("Ring buffers: %u x %lu frames, %u bytes each, total=%u\n",
                    RING_BUFFER_COUNT,
                    framesPerBuffer,
                    (uint32_t)bytesPerBuffer,
                    (uint32_t)(bytesPerBuffer * RING_BUFFER_COUNT));
      return true;
    }

    for (uint32_t i = 0; i < RING_BUFFER_COUNT; i++) {
      if (ring[i].data) {
        free(ring[i].data);
        ring[i].data = nullptr;
      }
    }
  }

  Serial.println("Ring buffer allocation failed");
  return false;
}

bool openVideoStream() {
  if (client.connected()) client.stop();

  streamContentLength = 0;
  streamFrames = 0;
  bytesStreamed = 0;
  streamEOF = false;
  streamConnected = false;

  showMessage(TFT_BLACK, TFT_CYAN, "Opening stream", PC_HOST, VIDEO_URL_PATH);
  Serial.printf("Connecting to %s:%d\n", PC_HOST, PC_PORT);

  if (!client.connect(PC_HOST, PC_PORT)) {
    Serial.println("TCP connect failed");
    showMessage(TFT_RED, TFT_WHITE, "TCP failed", PC_HOST);
    return false;
  }

  client.setTimeout(TCP_READ_TIMEOUT_MS);
  client.setNoDelay(true);

  client.print(String("GET ") + VIDEO_URL_PATH + " HTTP/1.1\r\n");
  client.print(String("Host: ") + PC_HOST + "\r\n");
  client.print("Connection: close\r\n");
  client.print("User-Agent: ESP32S3-DUALCORE-STREAM\r\n");
  client.print("\r\n");

  String status = readHttpLine(client);
  Serial.println(status);

  if (!status.startsWith("HTTP/1.1 200") && !status.startsWith("HTTP/1.0 200")) {
    showMessage(TFT_RED, TFT_WHITE, "HTTP error", status.c_str());
    client.stop();
    return false;
  }

  while (client.connected()) {
    String h = readHttpLine(client);
    if (h.length() == 0) break;
    String lower = h;
    lower.toLowerCase();
    if (lower.startsWith("content-length:")) {
      String v = h.substring(h.indexOf(':') + 1);
      v.trim();
      streamContentLength = (uint32_t)v.toInt();
    }
  }

  if (streamContentLength > 0) {
    streamFrames = streamContentLength / FRAME_SIZE;
    uint32_t leftover = streamContentLength % FRAME_SIZE;
    Serial.printf("Content-Length=%u frames=%u leftover=%u\n", streamContentLength, streamFrames, leftover);

    if (leftover != 0) {
      showMessage(TFT_RED, TFT_WHITE, "Bad file size", "Not frame aligned");
      client.stop();
      return false;
    }
  } else {
    Serial.println("No Content-Length. Streaming anyway.");
  }

  clearRingState();
  streamConnected = true;
  showStreamInfo("DUAL STREAM", TFT_GREEN);
  delay(600);
  tft.fillScreen(TFT_BLACK);
  return true;
}

bool readExactBytes(uint8_t *dst, size_t len) {
  size_t gotTotal = 0;
  uint32_t startMs = millis();

  while (gotTotal < len) {
    if (!client.connected() && client.available() <= 0) return false;

    int available = client.available();
    if (available <= 0) {
      if (millis() - startMs > TCP_READ_TIMEOUT_MS) {
        Serial.println("Exact read timeout");
        return false;
      }
      vTaskDelay(1);
      continue;
    }

    size_t want = len - gotTotal;
    if (want > (size_t)available) want = available;

    int n = client.read(dst + gotTotal, want);
    if (n <= 0) {
      vTaskDelay(1);
      continue;
    }

    gotTotal += n;
    startMs = millis();
  }

  return true;
}

int findEmptyBuffer() {
  for (uint32_t i = 0; i < RING_BUFFER_COUNT; i++) {
    if (!ring[i].ready && !ring[i].filling && !ring[i].playing) return i;
  }
  return -1;
}

int findReadyBuffer() {
  for (uint32_t i = 0; i < RING_BUFFER_COUNT; i++) {
    if (ring[i].ready && !ring[i].playing && !ring[i].filling) return i;
  }
  return -1;
}

uint32_t fillBufferFrames(uint32_t idx) {
  uint32_t framesToRead = framesPerBuffer;

  if (streamContentLength > 0) {
    uint32_t remainingBytes = streamContentLength - bytesStreamed;
    uint32_t remainingFrames = remainingBytes / FRAME_SIZE;
    if (framesToRead > remainingFrames) framesToRead = remainingFrames;
  }

  uint32_t readFrames = 0;
  uint32_t startUs = micros();

  for (uint32_t i = 0; i < framesToRead; i++) {
    if (!readExactBytes(ring[idx].data + (i * FRAME_SIZE), FRAME_SIZE)) break;
    readFrames++;
    bytesStreamed += FRAME_SIZE;
    if ((i & 0x03) == 0) taskYIELD();
  }

  lastNetReadUs = micros() - startUs;
  lastBufferFrames = readFrames;
  lastRSSI = WiFi.RSSI();

  return readFrames;
}

void networkTask(void *param) {
  while (true) {
    if (reconnectRequested || !streamConnected || (!client.connected() && client.available() <= 0)) {
      reconnectRequested = false;
      client.stop();
      while (!openVideoStream()) {
        vTaskDelay(pdMS_TO_TICKS(RECONNECT_DELAY_MS));
      }
    }

    if (streamContentLength > 0 && bytesStreamed + FRAME_SIZE > streamContentLength) {
      streamEOF = true;
      reconnectRequested = true;
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }

    int idx = -1;
    xSemaphoreTake(bufferMutex, portMAX_DELAY);
    idx = findEmptyBuffer();
    if (idx >= 0) ring[idx].filling = true;
    xSemaphoreGive(bufferMutex);

    if (idx < 0) {
      vTaskDelay(pdMS_TO_TICKS(2));
      continue;
    }

    uint32_t frames = fillBufferFrames(idx);

    xSemaphoreTake(bufferMutex, portMAX_DELAY);
    ring[idx].filling = false;
    ring[idx].validFrames = frames;
    ring[idx].ready = frames > 0;
    xSemaphoreGive(bufferMutex);

    if (frames == 0) {
      reconnectRequested = true;
      vTaskDelay(pdMS_TO_TICKS(20));
    }

    taskYIELD();
  }
}

uint32_t countReadyBuffers() {
  uint32_t readyCount = 0;
  xSemaphoreTake(bufferMutex, portMAX_DELAY);
  for (uint32_t b = 0; b < RING_BUFFER_COUNT; b++) {
    if (ring[b].ready) readyCount++;
  }
  xSemaphoreGive(bufferMutex);
  return readyCount;
}

void displayTask(void *param) {
  uint32_t lastDebugMs = millis();
  uint32_t framesSinceDebug = 0;
  bool playbackStarted = false;
  uint8_t *lastFrame = nullptr;

  while (true) {
    if (bootPressed()) {
      waitForBootRelease();
      Serial.println("BOOT pressed: reconnecting stream");
      reconnectRequested = true;
      playbackStarted = false;
      lastFrame = nullptr;
      showMessage(TFT_BLACK, TFT_YELLOW, "Reconnecting", "BOOT pressed");
      vTaskDelay(pdMS_TO_TICKS(500));
      tft.fillScreen(TFT_BLACK);
    }

    if (!playbackStarted) {
      uint32_t readyCount = countReadyBuffers();
      if (readyCount < START_BUFFER_READY_COUNT) {
        vTaskDelay(pdMS_TO_TICKS(2));
        continue;
      }
      playbackStarted = true;
      tft.fillScreen(TFT_BLACK);
      Serial.println("Playback started after jitter buffer filled");
    }

    int idx = -1;
    xSemaphoreTake(bufferMutex, portMAX_DELAY);
    idx = findReadyBuffer();
    if (idx >= 0) {
      ring[idx].ready = false;
      ring[idx].playing = true;
    }
    xSemaphoreGive(bufferMutex);

    if (idx < 0) {
      if (lastFrame) {
        uint32_t frameStartUs = micros();
        uint32_t pushStartUs = micros();
        tft.pushImage(0, 0, VID_W, VID_H, (uint16_t *)lastFrame);
        lastDisplayPushUs = micros() - pushStartUs;
        framesDisplayed++;
        framesSinceDebug++;
        uint32_t elapsedUs = micros() - frameStartUs;
        if (elapsedUs < FRAME_TIME_US) delayMicroseconds(FRAME_TIME_US - elapsedUs);
      } else {
        vTaskDelay(pdMS_TO_TICKS(1));
      }
      continue;
    }

    uint32_t frames = ring[idx].validFrames;

    for (uint32_t i = 0; i < frames; i++) {
      uint32_t frameStartUs = micros();
      uint8_t *framePtr8 = ring[idx].data + (i * FRAME_SIZE);
      uint16_t *framePtr = (uint16_t *)framePtr8;

      uint32_t pushStartUs = micros();
      tft.pushImage(0, 0, VID_W, VID_H, framePtr);
      lastDisplayPushUs = micros() - pushStartUs;
      lastFrame = framePtr8;

      framesDisplayed++;
      framesSinceDebug++;

      uint32_t elapsedUs = micros() - frameStartUs;
      if (elapsedUs < FRAME_TIME_US) delayMicroseconds(FRAME_TIME_US - elapsedUs);

      if (DEBUG_FRAME_TIME && framesSinceDebug >= DEBUG_EVERY_FRAMES) {
        uint32_t now = millis();
        uint32_t ms = now - lastDebugMs;
        float fps = ms > 0 ? framesSinceDebug * 1000.0f / ms : 0;

        uint32_t readyCount = countReadyBuffers();

        Serial.printf("Frames=%lu FPS=%.2f ready=%lu netFrames=%lu netRead=%lu us push=%lu us bytes=%u/%u RSSI=%d",
                      framesDisplayed,
                      fps,
                      readyCount,
                      lastBufferFrames,
                      lastNetReadUs,
                      lastDisplayPushUs,
                      bytesStreamed,
                      streamContentLength,
                      lastRSSI);

        lastDebugMs = now;
        framesSinceDebug = 0;
      }

      taskYIELD();
    }

    xSemaphoreTake(bufferMutex, portMAX_DELAY);
    ring[idx].playing = false;
    ring[idx].validFrames = 0;
    xSemaphoreGive(bufferMutex);

    taskYIELD();
  }
}

void setup() {
  pinMode(BOOT_BUTTON, INPUT_PULLUP);
  Serial.begin(115200);
  delay(800);

  initTFT();

  bufferMutex = xSemaphoreCreateMutex();
  if (!bufferMutex) {
    showMessage(TFT_RED, TFT_WHITE, "Mutex failed");
    while (true) delay(1000);
  }

  if (!allocRingBuffers()) {
    showMessage(TFT_RED, TFT_WHITE, "No ring buffer", "PSRAM alloc fail");
    while (true) delay(1000);
  }

  if (!connectWiFi()) {
    while (true) delay(1000);
  }

  while (!openVideoStream()) {
    delay(RECONNECT_DELAY_MS);
  }

  xTaskCreatePinnedToCore(networkTask, "netTask", 8192, nullptr, 3, &netTaskHandle, 0);
  xTaskCreatePinnedToCore(displayTask, "displayTask", 8192, nullptr, 4, &displayTaskHandle, 1);
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}
