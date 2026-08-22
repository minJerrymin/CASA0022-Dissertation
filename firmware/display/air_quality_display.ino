#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <GxEPD2_BW.h>
#include <Fonts/FreeMonoBold24pt7b.h>
#include <Fonts/FreeMonoBold12pt7b.h>
#include <Fonts/FreeMonoBold9pt7b.h>
#include <math.h>
#include <time.h>
#include "arduino_secrets.h"

// =====================================================
// Network and ThingSpeak settings
// =====================================================

const char* WIFI_SSID = SECRET_WIFI_SSID;
const char* WIFI_PASSWORD = SECRET_WIFI_PASSWORD;

const char* CHANNEL_ID = SECRET_THINGSPEAK_CHANNEL_ID;
const char* READ_API_KEY = SECRET_THINGSPEAK_READ_API_KEY;

// Polling the lightweight last-entry endpoint does not refresh the display.
// A full history request is made only when a new entry ID is detected.
const unsigned long FETCH_INTERVAL_MS = 60UL * 1000UL;

// Expected sensor cadence is one sample every 10 minutes.
// Thirty samples therefore cover approximately five hours.
const uint8_t HISTORY_CAPACITY = 30;

// Eight pages at 20 seconds each give 160 seconds per carousel cycle.
// After three complete cycles, retain the summary page with a full refresh.
const uint8_t CAROUSEL_CYCLES = 3;

unsigned long lastFetchTime = 0;


// =====================================================
// Waveshare Pico-ePaper-4.2 V2 connections
// =====================================================

#define EPD_CS    13  // FireBeetle D7
#define EPD_DC    25  // FireBeetle D2
#define EPD_RST   26  // FireBeetle D3
#define EPD_BUSY  34  // FireBeetle A2

GxEPD2_BW<GxEPD2_420_GDEY042T81,
          GxEPD2_420_GDEY042T81::HEIGHT> display(
  GxEPD2_420_GDEY042T81(
    EPD_CS,
    EPD_DC,
    EPD_RST,
    EPD_BUSY
  )
);


// =====================================================
// Data models
// =====================================================

struct AirQualityData {
  float temperature;
  float humidity;
  float pm25;
  float pm10;
  float pressure;
  String createdAt;
  int entryID;
};

enum Metric : uint8_t {
  METRIC_TEMPERATURE = 0,
  METRIC_HUMIDITY,
  METRIC_PM25,
  METRIC_PM10,
  METRIC_PRESSURE,
  METRIC_COUNT
};

struct HistoryData {
  float values[METRIC_COUNT][HISTORY_CAPACITY];
  uint8_t count;
  String firstCreatedAt;
  String lastCreatedAt;
};

AirQualityData latestData;
HistoryData historyData;
int lastLoadedEntryID = -1;


// =====================================================
// Carousel state
// =====================================================

enum DisplayPage : uint8_t {
  PAGE_SUMMARY = 0,
  PAGE_COMFORT,
  PAGE_AIR_QUALITY,
  PAGE_TEMPERATURE_TREND,
  PAGE_HUMIDITY_TREND,
  PAGE_PM25_TREND,
  PAGE_PM10_TREND,
  PAGE_PRESSURE_TREND,
  PAGE_COUNT
};

const unsigned long PAGE_DURATION_MS[PAGE_COUNT] = {
  20000UL,
  20000UL,
  20000UL,
  20000UL,
  20000UL,
  20000UL,
  20000UL,
  20000UL,
};

bool carouselActive = false;
uint8_t currentPage = PAGE_SUMMARY;
uint8_t completedCycles = 0;
unsigned long pageShownAt = 0;

bool displayEverInitialised = false;
bool displayIsAwake = false;


// =====================================================
// Wi-Fi connection
// =====================================================

bool connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    return true;
  }

  Serial.println();
  Serial.print("Connecting to Wi-Fi: ");
  Serial.println(WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long startTime = millis();

  while (WiFi.status() != WL_CONNECTED &&
         millis() - startTime < 20000UL) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("Wi-Fi connected.");
    Serial.print("ESP32 IP address: ");
    Serial.println(WiFi.localIP());
    Serial.print("Wi-Fi RSSI: ");
    Serial.print(WiFi.RSSI());
    Serial.println(" dBm");
    return true;
  }

  Serial.println("Wi-Fi connection failed.");
  return false;
}


// =====================================================
// Generic helpers
// =====================================================

float jsonFieldToFloat(JsonVariantConst value) {
  if (value.isNull()) {
    return NAN;
  }

  const char* text = value.as<const char*>();
  if (text == nullptr || text[0] == '\0') {
    return NAN;
  }

  return strtof(text, nullptr);
}


String formatThingSpeakTime(const String& isoTime) {
  if (isoTime.length() < 19) return "unknown";

  struct tm utcTime = {};
  int year, month, day, hour, minute, second;

  if (sscanf(isoTime.c_str(), "%d-%d-%dT%d:%d:%d",
             &year, &month, &day, &hour, &minute, &second) != 6) {
    return "unknown";
  }

  utcTime.tm_year = year - 1900;
  utcTime.tm_mon  = month - 1;
  utcTime.tm_mday = day;
  utcTime.tm_hour = hour;
  utcTime.tm_min  = minute;
  utcTime.tm_sec  = second;

  utcTime.tm_isdst = 0;

  setenv("TZ", "UTC0", 1);
  tzset();
  time_t epoch = mktime(&utcTime);

  setenv("TZ", "GMT0BST,M3.5.0/1,M10.5.0/2", 1);
  tzset();

  struct tm ukTime;
  localtime_r(&epoch, &ukTime);



  char buffer[24];
  strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M %Z", &ukTime);
  return String(buffer);
}


float currentMetricValue(Metric metric) {
  switch (metric) {
    case METRIC_TEMPERATURE: return latestData.temperature;
    case METRIC_HUMIDITY:    return latestData.humidity;
    case METRIC_PM25:        return latestData.pm25;
    case METRIC_PM10:        return latestData.pm10;
    case METRIC_PRESSURE:    return latestData.pressure;
    default:                 return NAN;
  }
}


const char* metricName(Metric metric) {
  switch (metric) {
    case METRIC_TEMPERATURE: return "TEMPERATURE";
    case METRIC_HUMIDITY:    return "HUMIDITY";
    case METRIC_PM25:        return "PM2.5";
    case METRIC_PM10:        return "PM10";
    case METRIC_PRESSURE:    return "PRESSURE";
    default:                 return "UNKNOWN";
  }
}


const char* metricUnit(Metric metric) {
  switch (metric) {
    case METRIC_TEMPERATURE: return "C";
    case METRIC_HUMIDITY:    return "%";
    case METRIC_PM25:
    case METRIC_PM10:        return "ug/m3";
    case METRIC_PRESSURE:    return "hPa";
    default:                 return "";
  }
}


unsigned int metricDecimals(Metric metric) {
  return (metric == METRIC_PM25 || metric == METRIC_PM10) ? 0 : 1;
}


float metricMinimumSpan(Metric metric) {
  switch (metric) {
    case METRIC_TEMPERATURE: return 2.0f;
    case METRIC_HUMIDITY:    return 5.0f;
    case METRIC_PM25:
    case METRIC_PM10:        return 5.0f;
    case METRIC_PRESSURE:    return 2.0f;
    default:                 return 1.0f;
  }
}


// =====================================================
// Display text helpers
// =====================================================

void drawCenteredText(const String& text,
                      int16_t centreX,
                      int16_t baselineY,
                      const GFXfont* font) {
  int16_t x1;
  int16_t y1;
  uint16_t width;
  uint16_t height;

  display.setFont(font);
  display.getTextBounds(text.c_str(), 0, 0, &x1, &y1, &width, &height);
  display.setCursor(centreX - (int16_t)width / 2, baselineY);
  display.print(text);
}


void drawHeader(const String& title) {
  display.fillRect(0, 0, 400, 38, GxEPD_BLACK);
  display.setTextColor(GxEPD_WHITE);
  drawCenteredText(title, 200, 27, &FreeMonoBold12pt7b);
  display.setTextColor(GxEPD_BLACK);
}


void drawFooter(const String& leftText, const String& rightText) {
  display.drawLine(0, 270, 399, 270, GxEPD_BLACK);
  display.setFont(nullptr);
  display.setTextSize(1);
  display.setCursor(7, 283);
  display.print(leftText);
  display.setCursor(322, 283);
  display.print(rightText);
}


void wakeDisplayIfNeeded() {
  if (displayIsAwake) {
    return;
  }

  display.init(
    0,
    !displayEverInitialised,
    2,
    false
  );

  displayEverInitialised = true;
  displayIsAwake = true;
  display.setRotation(0);
}


// =====================================================
// Page 1: all-data summary
// =====================================================

void drawSummaryPage() {
  drawHeader("AIR CONDITIONS OVERVIEW");

  display.drawRect(5, 45, 192, 132, GxEPD_BLACK);
  display.drawRect(203, 45, 192, 132, GxEPD_BLACK);

  drawCenteredText("PM2.5", 101, 72, &FreeMonoBold12pt7b);
  drawCenteredText("PM10", 299, 72, &FreeMonoBold12pt7b);
  drawCenteredText(String(latestData.pm25, 0), 101, 128, &FreeMonoBold24pt7b);
  drawCenteredText(String(latestData.pm10, 0), 299, 128, &FreeMonoBold24pt7b);
  drawCenteredText("ug/m3", 101, 158, &FreeMonoBold9pt7b);
  drawCenteredText("ug/m3", 299, 158, &FreeMonoBold9pt7b);

  display.drawRect(5, 184, 126, 82, GxEPD_BLACK);
  display.drawRect(137, 184, 126, 82, GxEPD_BLACK);
  display.drawRect(269, 184, 126, 82, GxEPD_BLACK);

  drawCenteredText("TEMP", 68, 205, &FreeMonoBold9pt7b);
  drawCenteredText("HUMIDITY", 200, 205, &FreeMonoBold9pt7b);
  drawCenteredText("PRESSURE", 332, 205, &FreeMonoBold9pt7b);

  drawCenteredText(String(latestData.temperature, 1), 68, 238, &FreeMonoBold12pt7b);
  drawCenteredText(String(latestData.humidity, 1), 200, 238, &FreeMonoBold12pt7b);
  drawCenteredText(String(latestData.pressure, 1), 332, 238, &FreeMonoBold12pt7b);

  drawCenteredText("C", 68, 258, &FreeMonoBold9pt7b);
  drawCenteredText("%", 200, 258, &FreeMonoBold9pt7b);
  drawCenteredText("hPa", 332, 258, &FreeMonoBold9pt7b);

  drawFooter(
    "Updated: " + formatThingSpeakTime(latestData.createdAt),
    "ID:" + String(latestData.entryID)
  );
}


// =====================================================
// Page 2: local comfort estimate
// =====================================================

float calculateDewPoint(float temperature, float humidity) {
  if (humidity <= 0.0f || humidity > 100.0f) {
    return NAN;
  }

  const float a = 17.62f;
  const float b = 243.12f;
  float gamma = logf(humidity / 100.0f) +
                (a * temperature) / (b + temperature);
  return (b * gamma) / (a - gamma);
}


String comfortVerdict(float temperature, float humidity) {
  if (temperature >= 29.0f) return "HOT";
  if (temperature > 26.0f && humidity > 60.0f) return "WARM & HUMID";
  if (temperature > 26.0f) return "WARM";
  if (temperature < 16.0f) return "COLD";
  if (temperature < 18.0f) return "COOL";
  if (humidity < 30.0f) return "DRY";
  if (humidity > 70.0f) return "HUMID";
  if (humidity > 60.0f) return "SLIGHTLY HUMID";
  return "COMFORTABLE";
}


String comfortAdvice(float temperature, float humidity) {
  if (temperature >= 29.0f) return "COOL THE SPACE / HYDRATE";
  if (temperature > 26.0f && humidity > 60.0f) return "VENTILATE OR USE A FAN";
  if (temperature > 26.0f) return "INCREASE AIR MOVEMENT";
  if (temperature < 18.0f) return "A LITTLE MORE WARMTH MAY HELP";
  if (humidity < 30.0f) return "AIR MAY FEEL TOO DRY";
  if (humidity > 70.0f) return "VENTILATE / DEHUMIDIFY";
  if (humidity > 60.0f) return "VENTILATION MAY IMPROVE COMFORT";
  return "CONDITIONS ARE BROADLY PLEASANT";
}


float pressureChangeOverHistory() {
  int first = -1;
  int last = -1;

  for (uint8_t i = 0; i < historyData.count; ++i) {
    if (!isnan(historyData.values[METRIC_PRESSURE][i])) {
      if (first < 0) first = i;
      last = i;
    }
  }

  if (first < 0 || last <= first) {
    return NAN;
  }

  return historyData.values[METRIC_PRESSURE][last] -
         historyData.values[METRIC_PRESSURE][first];
}


String pressureTrendLabel(float change) {
  if (isnan(change)) return "UNKNOWN";
  if (change > 1.5f) return "RISING";
  if (change < -1.5f) return "FALLING";
  return "STEADY";
}


void drawComfortPage() {
  drawHeader("COMFORT ESTIMATE");

  String verdict = comfortVerdict(latestData.temperature, latestData.humidity);
  String advice = comfortAdvice(latestData.temperature, latestData.humidity);
  float dewPoint = calculateDewPoint(latestData.temperature, latestData.humidity);
  float pressureChange = pressureChangeOverHistory();

  drawCenteredText(verdict, 200, 82, &FreeMonoBold12pt7b);
  display.drawLine(35, 94, 365, 94, GxEPD_BLACK);

  display.drawRect(8, 108, 122, 80, GxEPD_BLACK);
  display.drawRect(139, 108, 122, 80, GxEPD_BLACK);
  display.drawRect(270, 108, 122, 80, GxEPD_BLACK);

  drawCenteredText("TEMP", 69, 130, &FreeMonoBold9pt7b);
  drawCenteredText("HUMIDITY", 200, 130, &FreeMonoBold9pt7b);
  drawCenteredText("DEW POINT", 331, 130, &FreeMonoBold9pt7b);

  drawCenteredText(String(latestData.temperature, 1) + " C", 69, 169, &FreeMonoBold12pt7b);
  drawCenteredText(String(latestData.humidity, 1) + " %", 200, 169, &FreeMonoBold12pt7b);
  drawCenteredText(isnan(dewPoint) ? "--" : String(dewPoint, 1) + " C",
                   331, 169, &FreeMonoBold12pt7b);

  drawCenteredText("PRESSURE " + String(latestData.pressure, 1) + " hPa",
                   200, 211, &FreeMonoBold9pt7b);

  String pressureLine = "5H TREND: " + pressureTrendLabel(pressureChange);
  if (!isnan(pressureChange)) {
    pressureLine += " (";
    if (pressureChange > 0.0f) pressureLine += "+";
    pressureLine += String(pressureChange, 1) + " hPa)";
  }
  drawCenteredText(pressureLine, 200, 234, &FreeMonoBold9pt7b);

  drawCenteredText(advice, 200, 258, &FreeMonoBold9pt7b);
  drawFooter("Heuristic from local sensor data", "2/8");
}


// =====================================================
// Page 3: indicative particulate assessment
// =====================================================

uint8_t particulateBand(float value, Metric metric) {
  if (metric == METRIC_PM25) {
    if (value <= 35.0f) return 0;
    if (value <= 53.0f) return 1;
    if (value <= 70.0f) return 2;
    return 3;
  }

  if (value <= 50.0f) return 0;
  if (value <= 75.0f) return 1;
  if (value <= 100.0f) return 2;
  return 3;
}


const char* bandName(uint8_t band) {
  switch (band) {
    case 0: return "LOW";
    case 1: return "MODERATE";
    case 2: return "HIGH";
    default: return "VERY HIGH";
  }
}


const char* bandAdvice(uint8_t band) {
  switch (band) {
    case 0: return "AIR IS GENERALLY CLEAR";
    case 1: return "SENSITIVE PEOPLE: TAKE CARE";
    case 2: return "REDUCE PROLONGED OUTDOOR ACTIVITY";
    default: return "LIMIT EXPOSURE AND SEEK CLEANER AIR";
  }
}


void drawAirQualityPage() {
  drawHeader("Particulate matter level");

  uint8_t pm25Band = particulateBand(latestData.pm25, METRIC_PM25);
  uint8_t pm10Band = particulateBand(latestData.pm10, METRIC_PM10);
  uint8_t overallBand = pm25Band > pm10Band ? pm25Band : pm10Band;

  drawCenteredText(bandName(overallBand), 200, 86, &FreeMonoBold24pt7b);
  drawCenteredText(bandAdvice(overallBand), 200, 115, &FreeMonoBold9pt7b);

  display.drawRect(12, 132, 180, 102, GxEPD_BLACK);
  display.drawRect(208, 132, 180, 102, GxEPD_BLACK);

  drawCenteredText("PM2.5", 102, 157, &FreeMonoBold12pt7b);
  drawCenteredText("PM10", 298, 157, &FreeMonoBold12pt7b);
  drawCenteredText(String(latestData.pm25, 0) + " ug/m3", 102, 194, &FreeMonoBold12pt7b);
  drawCenteredText(String(latestData.pm10, 0) + " ug/m3", 298, 194, &FreeMonoBold12pt7b);
  drawCenteredText(bandName(pm25Band), 102, 222, &FreeMonoBold9pt7b);
  drawCenteredText(bandName(pm10Band), 298, 222, &FreeMonoBold9pt7b);


  drawFooter("", "3/8");
}


// =====================================================
// Pages 4-8: current value plus five-hour trend
// =====================================================

bool metricRange(Metric metric, float& minimum, float& maximum) {
  bool found = false;
  minimum = 0.0f;
  maximum = 0.0f;

  for (uint8_t i = 0; i < historyData.count; ++i) {
    float value = historyData.values[metric][i];
    if (isnan(value)) continue;

    if (!found) {
      minimum = value;
      maximum = value;
      found = true;
    } else {
      minimum = fminf(minimum, value);
      maximum = fmaxf(maximum, value);
    }
  }

  return found;
}

bool metricAverage(Metric metric, float& average) {
  float total = 0.0f;
  uint8_t validCount = 0;

  for (uint8_t i = 0; i < historyData.count; ++i) {
    float value = historyData.values[metric][i];

    if (isnan(value)) continue;

    total += value;
    validCount++;
  }

  if (validCount == 0) return false;

  average = total / validCount;
  return true;
}

void drawTrendGraph(Metric metric,
                    int16_t graphX,
                    int16_t graphY,
                    int16_t graphWidth,
                    int16_t graphHeight) {
  float dataMin;
  float dataMax;

  if (!metricRange(metric, dataMin, dataMax) || historyData.count < 2) {
    display.drawRect(graphX, graphY, graphWidth, graphHeight, GxEPD_BLACK);
    drawCenteredText("NOT ENOUGH HISTORY",
                     graphX + graphWidth / 2,
                     graphY + graphHeight / 2,
                     &FreeMonoBold9pt7b);
    return;
  }

  float span = dataMax - dataMin;
  float minimumSpan = metricMinimumSpan(metric);
  if (span < minimumSpan) {
    float centre = (dataMax + dataMin) * 0.5f;
    dataMin = centre - minimumSpan * 0.5f;
    dataMax = centre + minimumSpan * 0.5f;
    span = minimumSpan;
  } else {
    float padding = span * 0.10f;
    dataMin -= padding;
    dataMax += padding;
    span = dataMax - dataMin;
  }

  display.drawRect(graphX, graphY, graphWidth, graphHeight, GxEPD_BLACK);

  for (uint8_t grid = 1; grid < 4; ++grid) {
    int16_t y = graphY + (graphHeight * grid) / 4;
    for (int16_t x = graphX + 1; x < graphX + graphWidth - 1; x += 6) {
      display.drawPixel(x, y, GxEPD_BLACK);
    }
  }

  int16_t previousX = 0;
  int16_t previousY = 0;
  bool havePrevious = false;

  for (uint8_t i = 0; i < historyData.count; ++i) {
    float value = historyData.values[metric][i];
    if (isnan(value)) {
      havePrevious = false;
      continue;
    }

    int16_t x = graphX + 2;
    if (historyData.count > 1) {
      x += (int32_t)(graphWidth - 5) * i / (historyData.count - 1);
    }

    float normalised = (value - dataMin) / span;
    const int16_t plotTop = graphY + 16;
    const int16_t plotBottom = graphY + graphHeight - 16;

int16_t y = plotBottom -
            (int16_t)(normalised * (plotBottom - plotTop));

    if (havePrevious) {
      display.drawLine(previousX, previousY, x, y, GxEPD_BLACK);
      display.drawLine(previousX, previousY + 1, x, y + 1, GxEPD_BLACK);
    }

    display.fillCircle(x, y, 2, GxEPD_BLACK);
    previousX = x;
    previousY = y;
    havePrevious = true;
  }

  display.setFont(nullptr);
  display.setTextSize(1);

  display.setCursor(graphX + 5, graphY + 4);
  display.print(dataMax, metricDecimals(metric));


  display.setCursor(graphX + 5, graphY + graphHeight - 12);
  display.print(dataMin, metricDecimals(metric));
}


void drawTrendPage(Metric metric, uint8_t pageNumber) {
  drawHeader(String(metricName(metric)) + " - 5H TREND");

  display.drawRect(5, 45, 126, 218, GxEPD_BLACK);
  drawCenteredText("NOW", 68, 70, &FreeMonoBold12pt7b);

  const GFXfont* valueFont = metric == METRIC_PRESSURE
                           ? &FreeMonoBold12pt7b
                           : &FreeMonoBold24pt7b;
  drawCenteredText(String(currentMetricValue(metric), metricDecimals(metric)),
                   68, 120, valueFont);
  drawCenteredText(metricUnit(metric), 68, 146, &FreeMonoBold9pt7b);

  float dataMin;
  float dataMax;
  bool rangeAvailable = metricRange(metric, dataMin, dataMax);
  float dataAverage;
  bool averageAvailable = metricAverage(metric, dataAverage);

  display.setFont(&FreeMonoBold9pt7b);
  display.setCursor(15, 207);
  display.print("MIN ");
  if (rangeAvailable) display.print(dataMin, metricDecimals(metric));
  else display.print("--");

  display.setCursor(15, 237);
  display.print("MAX ");
  if (rangeAvailable) display.print(dataMax, metricDecimals(metric));
  else display.print("--");

  display.setFont(&FreeMonoBold9pt7b);
  display.setCursor(15, 177);
  display.print("AVG ");

  if (averageAvailable) {
  display.print(dataAverage, metricDecimals(metric));
  } else {
  display.print("--");
  }

  const int16_t graphX = 142;
  const int16_t graphY = 54;
  const int16_t graphWidth = 251;
  const int16_t graphHeight = 180;
  drawTrendGraph(metric, graphX, graphY, graphWidth, graphHeight);

  display.setFont(nullptr);
  display.setTextSize(1);
  display.setCursor(graphX, 249);
  display.print("-5h");
  display.setCursor(372, 249);
  display.print("NOW");

  drawFooter(
   "", String(pageNumber) + "/8"
  );
}


// =====================================================
// Page renderer and e-paper refresh policy
// =====================================================

void drawPageContent(DisplayPage page) {
  switch (page) {
    case PAGE_SUMMARY:
      drawSummaryPage();
      break;
    case PAGE_COMFORT:
      drawComfortPage();
      break;
    case PAGE_AIR_QUALITY:
      drawAirQualityPage();
      break;
    case PAGE_TEMPERATURE_TREND:
      drawTrendPage(METRIC_TEMPERATURE, 4);
      break;
    case PAGE_HUMIDITY_TREND:
      drawTrendPage(METRIC_HUMIDITY, 5);
      break;
    case PAGE_PM25_TREND:
      drawTrendPage(METRIC_PM25, 6);
      break;
    case PAGE_PM10_TREND:
      drawTrendPage(METRIC_PM10, 7);
      break;
    case PAGE_PRESSURE_TREND:
      drawTrendPage(METRIC_PRESSURE, 8);
      break;
    default:
      drawSummaryPage();
      break;
  }
}


void renderPage(DisplayPage page, bool fullRefresh) {
  wakeDisplayIfNeeded();

  if (fullRefresh) {
    Serial.print("Full refresh, page ");
    display.setFullWindow();
  } else {
    Serial.print("Fast refresh, page ");
    // Full-screen partial window requests the controller's differential mode.
    display.setPartialWindow(0, 0, display.width(), display.height());
  }
  Serial.println((int)page + 1);

  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setTextColor(GxEPD_BLACK);
    drawPageContent(page);
  } while (display.nextPage());
}


void stopCarouselAndSleep() {
  carouselActive = false;

  // Leave a useful summary visible while waiting for the next ThingSpeak entry.
  renderPage(PAGE_SUMMARY, true);
  display.hibernate();
  displayIsAwake = false;

  Serial.println("Carousel complete. Summary retained; e-paper is sleeping.");
}


void startCarousel() {
  currentPage = PAGE_SUMMARY;
  completedCycles = 0;
  carouselActive = true;

  renderPage(PAGE_SUMMARY, true);
  pageShownAt = millis();

  Serial.println("Carousel started.");
}


void serviceCarousel() {
  if (!carouselActive) {
    return;
  }

  if (millis() - pageShownAt < PAGE_DURATION_MS[currentPage]) {
    return;
  }

  if (currentPage == PAGE_PRESSURE_TREND) {
    ++completedCycles;

    Serial.print("Completed carousel cycle ");
    Serial.print(completedCycles);
    Serial.print(" of ");
    Serial.println(CAROUSEL_CYCLES);

    if (completedCycles >= CAROUSEL_CYCLES) {
      stopCarouselAndSleep();
      return;
    }

    currentPage = PAGE_SUMMARY;
    renderPage((DisplayPage)currentPage, true);
  } else {
    ++currentPage;
    renderPage((DisplayPage)currentPage, false);
  }

  // Reading time begins after the physical refresh has completed.
  pageShownAt = millis();
}


// =====================================================
// ThingSpeak network requests
// =====================================================

bool fetchLatestThingSpeakData(AirQualityData& result) {
  if (!connectWiFi()) {
    Serial.println("Cannot fetch latest data because Wi-Fi is unavailable.");
    return false;
  }

  String url =
      String("https://api.thingspeak.com/channels/") +
      CHANNEL_ID +
      "/feeds/last.json?api_key=" +
      READ_API_KEY;

  WiFiClientSecure secureClient;
  secureClient.setInsecure();

  HTTPClient http;
  http.setConnectTimeout(10000);
  http.setTimeout(10000);

  Serial.println();
  Serial.println("Requesting latest ThingSpeak entry...");

  if (!http.begin(secureClient, url)) {
    Serial.println("Failed to initialise HTTPS connection.");
    return false;
  }

  int httpCode = http.GET();
  Serial.print("HTTP response code: ");
  Serial.println(httpCode);

  if (httpCode != HTTP_CODE_OK) {
    Serial.print("ThingSpeak request failed: ");
    Serial.println(http.errorToString(httpCode));
    http.end();
    return false;
  }

  String jsonResponse = http.getString();
  http.end();

  JsonDocument doc;
  DeserializationError jsonError = deserializeJson(doc, jsonResponse);

  if (jsonError) {
    Serial.print("Latest JSON parsing failed: ");
    Serial.println(jsonError.c_str());
    return false;
  }

  float temperature = jsonFieldToFloat(doc["field1"]);
  float humidity = jsonFieldToFloat(doc["field2"]);
  float pm25 = jsonFieldToFloat(doc["field3"]);
  float pm10 = jsonFieldToFloat(doc["field4"]);
  float pressure = jsonFieldToFloat(doc["field5"]);

  if (isnan(temperature) || isnan(humidity) || isnan(pm25) ||
      isnan(pm10) || isnan(pressure)) {
    Serial.println("One or more latest ThingSpeak fields are missing.");
    return false;
  }

  const char* createdAt = doc["created_at"].as<const char*>();

  result.temperature = temperature;
  result.humidity = humidity;
  result.pm25 = pm25;
  result.pm10 = pm10;
  result.pressure = pressure;
  result.createdAt = createdAt != nullptr ? createdAt : "unknown";
  result.entryID = doc["entry_id"] | -1;

  Serial.print("Latest entry ID: ");
  Serial.println(result.entryID);
  return result.entryID >= 0;
}


void initialiseEmptyHistory(HistoryData& result) {
  result.count = 0;
  result.firstCreatedAt = "";
  result.lastCreatedAt = "";

  for (uint8_t metric = 0; metric < METRIC_COUNT; ++metric) {
    for (uint8_t i = 0; i < HISTORY_CAPACITY; ++i) {
      result.values[metric][i] = NAN;
    }
  }
}


void createSinglePointHistory(HistoryData& result,
                              const AirQualityData& data) {
  initialiseEmptyHistory(result);
  result.count = 1;
  result.firstCreatedAt = data.createdAt;
  result.lastCreatedAt = data.createdAt;
  result.values[METRIC_TEMPERATURE][0] = data.temperature;
  result.values[METRIC_HUMIDITY][0] = data.humidity;
  result.values[METRIC_PM25][0] = data.pm25;
  result.values[METRIC_PM10][0] = data.pm10;
  result.values[METRIC_PRESSURE][0] = data.pressure;
}


bool fetchThingSpeakHistory(HistoryData& result) {
  if (!connectWiFi()) {
    Serial.println("Cannot fetch history because Wi-Fi is unavailable.");
    return false;
  }

  String url =
      String("https://api.thingspeak.com/channels/") +
      CHANNEL_ID +
      "/feeds.json?api_key=" +
      READ_API_KEY +
      "&results=" +
      String(HISTORY_CAPACITY);

  WiFiClientSecure secureClient;
  secureClient.setInsecure();

  HTTPClient http;
  http.setConnectTimeout(10000);
  http.setTimeout(15000);

  Serial.println("Requesting approximately five hours of ThingSpeak history...");

  if (!http.begin(secureClient, url)) {
    Serial.println("Failed to initialise history HTTPS connection.");
    return false;
  }

  int httpCode = http.GET();
  Serial.print("History HTTP response code: ");
  Serial.println(httpCode);

  if (httpCode != HTTP_CODE_OK) {
    Serial.print("History request failed: ");
    Serial.println(http.errorToString(httpCode));
    http.end();
    return false;
  }

  String jsonResponse = http.getString();
  http.end();

  JsonDocument doc;
  DeserializationError jsonError = deserializeJson(doc, jsonResponse);

  if (jsonError) {
    Serial.print("History JSON parsing failed: ");
    Serial.println(jsonError.c_str());
    return false;
  }

  JsonArray feeds = doc["feeds"].as<JsonArray>();
  if (feeds.isNull() || feeds.size() == 0) {
    Serial.println("ThingSpeak history contains no feeds.");
    return false;
  }

  initialiseEmptyHistory(result);
  result.count = feeds.size() > HISTORY_CAPACITY
               ? HISTORY_CAPACITY
               : (uint8_t)feeds.size();

  uint8_t index = 0;
  for (JsonObject feed : feeds) {
    if (index >= result.count) break;

    result.values[METRIC_TEMPERATURE][index] = jsonFieldToFloat(feed["field1"]);
    result.values[METRIC_HUMIDITY][index] = jsonFieldToFloat(feed["field2"]);
    result.values[METRIC_PM25][index] = jsonFieldToFloat(feed["field3"]);
    result.values[METRIC_PM10][index] = jsonFieldToFloat(feed["field4"]);
    result.values[METRIC_PRESSURE][index] = jsonFieldToFloat(feed["field5"]);

    const char* createdAt = feed["created_at"].as<const char*>();
    if (createdAt != nullptr) {
      if (index == 0) result.firstCreatedAt = createdAt;
      result.lastCreatedAt = createdAt;
    }

    ++index;
  }

  Serial.print("History samples loaded: ");
  Serial.println(result.count);
  return result.count > 0;
}


// =====================================================
// New-entry detection and carousel restart
// =====================================================

void fetchAndProcessNewEntry() {
  AirQualityData newData;

  if (!fetchLatestThingSpeakData(newData)) {
    Serial.println("Fetch failed. Previous display content retained.");
    return;
  }

  if (newData.entryID == lastLoadedEntryID) {
    Serial.println("No new ThingSpeak entry. Carousel state unchanged.");
    return;
  }

  HistoryData newHistory;
  if (!fetchThingSpeakHistory(newHistory)) {
    Serial.println("History unavailable; using the latest point only.");
    createSinglePointHistory(newHistory, newData);
  }

  latestData = newData;
  historyData = newHistory;
  lastLoadedEntryID = newData.entryID;

  Serial.println();
  Serial.println("========== New Air Quality Dataset ==========");
  Serial.print("Temperature: "); Serial.print(latestData.temperature, 1); Serial.println(" C");
  Serial.print("Humidity:    "); Serial.print(latestData.humidity, 1); Serial.println(" %");
  Serial.print("PM2.5:       "); Serial.print(latestData.pm25, 0); Serial.println(" ug/m3");
  Serial.print("PM10:        "); Serial.print(latestData.pm10, 0); Serial.println(" ug/m3");
  Serial.print("Pressure:    "); Serial.print(latestData.pressure, 1); Serial.println(" hPa");
  Serial.print("Created at:  "); Serial.println(latestData.createdAt);
  Serial.print("Entry ID:    "); Serial.println(latestData.entryID);
  Serial.println("==============================================");

  // A new entry immediately interrupts/restarts any older carousel.
  startCarousel();
}


// =====================================================
// Arduino setup and loop
// =====================================================

void setup() {
  Serial.begin(115200);
  delay(1500);

  Serial.println();
  Serial.println("Solar Air Quality multi-page e-paper display");
  Serial.println("------------------------------------------------");

  // FireBeetle hardware SPI: SCK=18, MISO=19 unused, MOSI=23.
  SPI.begin(18, 19, 23, EPD_CS);

  initialiseEmptyHistory(historyData);
  fetchAndProcessNewEntry();
  lastFetchTime = millis();
}


void loop() {
  serviceCarousel();

  if (millis() - lastFetchTime >= FETCH_INTERVAL_MS) {
    lastFetchTime = millis();
    fetchAndProcessNewEntry();
  }

  delay(20);
}
