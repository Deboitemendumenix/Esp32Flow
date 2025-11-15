/**
 * @file esp32flow.ino
 * @brief ESP32 Uroflowmetry Station - Flow-rate measurement device
 * @author Deboitemendumenix
 * @version 1.0.0
 * @license MIT License - See LICENSE file for details
 *
 * This project transforms an ESP32 with HX711 load cell amplifier into
 * a web-enabled uroflowmetry/flow-rate measurement device. It captures
 * weight data over time, calculates flow rate, and provides a web interface
 * with interactive charts and CSV data export.
 */

#include <Arduino.h>
#include "HX711.h"
#include <WiFi.h>
#include <WebServer.h>
#include <vector>
#include <ctime>
#include <freertos/semphr.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// ESP32 GPIO pins for HX711
const int LOADCELL_DOUT_PIN = 18;
const int LOADCELL_SCK_PIN = 16;

// RGB component
const int PWM_FREQ = 5000;    // PWM Frequency (5 kHz)
const int PWM_RESOLUTION = 8; // PWM Resolution (8 bits = 0 to 255 brightness levels)
const int RED_PIN = 22;
const int GREEN_PIN = 21;
const int BLUE_PIN = 19;

// WiFi configuration
const char *WIFI_SSID = "xxx";
const char *WIFI_PASSWORD = "xxx";
// Static IP configuration (leave empty "" for DHCP/router-assigned IP)
// Format: "192.168.1.100" for static, or "" for DHCP
const char *STATIC_IP = "192.168.1.106";
const char *GATEWAY_IP = "192.168.1.1";         // Router gateway (e.g., 192.168.1.1)
const char *SUBNET_MASK = "255.255.255.0";      // Subnet mask (typically 255.255.255.0)
const unsigned long RECONNECT_INTERVAL = 15000; // 15 seconds - WiFi reconnection retry interval
// Timing for WiFi reconnection attempts
unsigned long lastReconnectAttempt = 0;

// Duration of no flow to trigger STOP (in ms) (10 seconds)
const unsigned long STOP_TIMEOUT = 10000;

// Maximum test duration (in ms) (5 minutes)
const unsigned long MAX_TEST_DURATION = 300000;

// Threshold to START recording (in grams)
const float START_THRESHOLD = 2.0;

// Threshold to STOP recording (in ml/s)
// If flow stays below this value for STOP_TIMEOUT, the test ends.
const float STOP_FLOW_THRESHOLD = 2.0;

// Time between data points (in ms)
const unsigned long SAMPLING_INTERVAL = 500;

// Calibration factor, set your own value
// IMPORTANT: Must be calibrated experimentally. A value of 0 will cause silent failures.
const float CALIBRATION_FACTOR = -1025.42;

// Scale object
HX711 scale;

// Server object
WebServer server(80);

enum TestState
{
  OFFLINE,
  ERROR,
  IDLE,
  MEASURING,
  COMPLETED
};
// State of the esp32flow test
TestState currentState = OFFLINE;

// Data structure to store a single measurement
struct DataPoint
{
  unsigned long timeOffset; // Time in ms from start of test
  float weight;
  // Measured weight in g
  float flowRate;
  // Flow rate in g/s
};
std::vector<DataPoint> measurements;
SemaphoreHandle_t measurements_mutex;

// Timing variables
unsigned long lastSampleTime = 0;
unsigned long testStartTime = 0;
unsigned long stabilityStartTime = 0;

// Function prototypes
void setup();
void loop();
void setupWiFi();
void WiFiEvent(WiFiEvent_t event);
void setRGBColor(int red, int green, int blue);
void setupScale();
void resetTest();
void handleReset();
void setupWebServer();
void handleRoot();
void handleResultsCSV();
void handleNotFound();
String stateToString(TestState state);
void handleResultsHTML();
void taskMeasure(void *pvParameters);
void processWeightLogic(float currentWeight);

/**
 * @brief Main setup function.
 */
void setup()
{
  Serial.begin(115200);
  Serial.println("\n=== ESP32Flow Starting ===");
  setupScale();
  measurements_mutex = xSemaphoreCreateMutex();

  ledcAttach(RED_PIN, PWM_FREQ, PWM_RESOLUTION);
  ledcAttach(GREEN_PIN, PWM_FREQ, PWM_RESOLUTION);
  ledcAttach(BLUE_PIN, PWM_FREQ, PWM_RESOLUTION);

  // Reserve memory for 3 minutes of data at the given sampling interval.
  measurements.reserve((3 * 60 * 1000) / SAMPLING_INTERVAL);

  setupWiFi();
  setupWebServer();

  // Create pinned FreeRTOS task
  xTaskCreatePinnedToCore(taskMeasure, "measure", 4096, NULL, 2, NULL, 1);
}

/**
 * @brief Main loop function.
 */
void loop()
{
  updateLED(currentState);
  switch (currentState)
  {
  case OFFLINE:
  case ERROR:
    // Check if it's time to try reconnecting
    if (millis() - lastReconnectAttempt >= RECONNECT_INTERVAL)
    {
      Serial.println("Offline. Attempting WiFi reconnection...");
      WiFi.reconnect();
      lastReconnectAttempt = millis();
    }
    break;
  default:
    server.handleClient();
    break;
  }
}

/**
 * @brief Set the LED color based on state.
 */
void updateLED(TestState state)
{
  static TestState lastState = (TestState)-1;
  if (state == lastState)
  {
    return; // No change
  }
  switch (state)
  {
  case OFFLINE:
    setRGBColor(255, 0, 0); // Red
    break;
  case IDLE:
    setRGBColor(0, 255, 0); // Green
    break;
  case MEASURING:
    setRGBColor(255, 165, 0); // Orange
    break;
  case COMPLETED:
    setRGBColor(0, 0, 255); // Blue
    break;
  case ERROR:
    setRGBColor(255, 0, 255); // Magenta
    break;
  default:
    setRGBColor(0, 0, 0); // Off
    break;
  }
  lastState = state;
}

/**
 * @brief Initializes WiFi connection.
 */
void setupWiFi()
{
  updateLED(OFFLINE);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.onEvent(WiFiEvent);

  // Apply static IP if configured, otherwise use DHCP
  if (strlen(STATIC_IP) > 0)
  {
    Serial.print("Configuring static IP: ");
    Serial.println(STATIC_IP);
    IPAddress ip;
    ip.fromString(STATIC_IP);
    IPAddress gateway;
    gateway.fromString(GATEWAY_IP);
    IPAddress subnet;
    subnet.fromString(SUBNET_MASK);
    WiFi.config(ip, gateway, subnet);
  }

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

/**
 * @brief Handles WiFi state changes.
 */
void WiFiEvent(WiFiEvent_t event)
{
  switch (event)
  {
  case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
    Serial.println("Wi-Fi Disconnected....");
    lastReconnectAttempt = millis();
    if (currentState == MEASURING)
    {
      Serial.println("Aborting test due to disconnect.");
      currentState = ERROR;
      break;
    }
    currentState = OFFLINE;
    break;

  case ARDUINO_EVENT_WIFI_STA_GOT_IP:
    Serial.print("Wi-Fi Connected, IP Address: ");
    Serial.println(WiFi.localIP());
    currentState = COMPLETED;
    break;

  default:
    break;
  }
}

/**
 * @brief Sets the RGB LED color.
 */
void setRGBColor(int red, int green, int blue)
{
  ledcWrite(RED_PIN, red);
  ledcWrite(GREEN_PIN, green);
  ledcWrite(BLUE_PIN, blue);
}

/**
 * @brief Initializes and tares the HX711 scale.
 */
void setupScale()
{
  Serial.println("Initializing HX711...");
  scale.begin(LOADCELL_DOUT_PIN, LOADCELL_SCK_PIN);

  if (!scale.wait_ready_retry(10, 1000))
  {
    Serial.println("ERROR: HX711 not found. Check wiring and power.");
    return;
  }

  Serial.println("Taring...");
  scale.set_scale(CALIBRATION_FACTOR);
  scale.tare();
  Serial.println("Scale tared successfully.");
}

/**
 * @brief Clears data and tares scale.
 */
void resetTest()
{
  if (xSemaphoreTake(measurements_mutex, portMAX_DELAY) == pdTRUE)
  {
    measurements.clear();
    measurements.shrink_to_fit();

    Serial.println("Taring...");
    scale.tare();
    currentState = IDLE;
    Serial.println("Ready for new test.");
    xSemaphoreGive(measurements_mutex);
  }
}

/**
 * @brief Initializes the HTTP server.
 */
void setupWebServer()
{
  server.on("/", HTTP_GET, handleRoot);
  server.on("/index.html", HTTP_GET, handleRoot);
  server.on("/reset", HTTP_GET, handleReset);
  server.on("/results.html", HTTP_GET, handleResultsHTML);
  server.on("/results.csv", HTTP_GET, handleResultsCSV);
  server.onNotFound(handleNotFound);
  server.begin();
}

/**
 * @brief FreeRTOS task - Continuous Background Sampling
 * Reads raw data constantly, averages it, and triggers logic every 250ms.
 */
void taskMeasure(void *pvParameters)
{
  (void)pvParameters;
  const TickType_t delayTicks = pdMS_TO_TICKS(25);

  long readingAccumulator = 0;
  int readingCount = 0;
  unsigned long lastProcessTime = millis();

  for (;;)
  {
    // Accumulate Readings
    if (xSemaphoreTake(measurements_mutex, portMAX_DELAY) == pdTRUE)
    {
      if (scale.is_ready())
      {
        readingAccumulator += scale.read();
        readingCount++;
      }
      xSemaphoreGive(measurements_mutex);
    }

    // Process every SAMPLING_INTERVAL
    if (millis() - lastProcessTime >= SAMPLING_INTERVAL)
    {
      float currentWeight = 0;

      if (xSemaphoreTake(measurements_mutex, portMAX_DELAY) == pdTRUE)
      {
        // Calculate Average
        if (readingCount > 0)
        {
          long rawAverage = readingAccumulator / readingCount;
          currentWeight = (rawAverage - scale.get_offset()) / scale.get_scale();
        }
        else if (!measurements.empty())
        {
          currentWeight = measurements.back().weight;
        }

        // Pass averaged weight to the centralized logic
        processWeightLogic(currentWeight);

        // Reset accumulators
        readingAccumulator = 0;
        readingCount = 0;
        lastProcessTime = millis();

        xSemaphoreGive(measurements_mutex);
      }
    }

    vTaskDelay(delayTicks);
  }
}

/**
 * @brief Central Logic Handler (The State Machine)
 * Handles the entire lifecycle: Detection -> Recording -> Stop Check -> Finalizing
 */
void processWeightLogic(float currentWeight)
{
  unsigned long currentTime = millis();
  static int consistentWeidhtReadings = 0;

  switch (currentState)
  {
  case IDLE:
    if (currentWeight >= START_THRESHOLD)
    {
      consistentWeidhtReadings++;
      if (consistentWeidhtReadings >= 2)
      {
        Serial.println("Start detected: Switch to MEASURING");

        currentState = MEASURING;
        testStartTime = millis();
        stabilityStartTime = millis();

        // Add initial point (T=0)
        measurements.push_back({0, currentWeight, 0});
      }
    }
    else
    {
      // Reset counter if weight drops below threshold, likely noise.
      consistentWeidhtReadings = 0;
    }
    break;

  case MEASURING:
  {
    unsigned long timeOffset = currentTime - testStartTime;

    if (timeOffset > MAX_TEST_DURATION)
    {
      Serial.println("Max duration reached. Force stop.");
      currentState = COMPLETED;
      return;
    }

    // Calculate Flow Rate
    float flowRate = 0;
    if (!measurements.empty())
    {
      DataPoint lastPoint = measurements.back();
      // Ignore negative weight changes
      float deltaW = fmaxf(0.0f, currentWeight - lastPoint.weight);
      float deltaT = timeOffset - lastPoint.timeOffset;

      if (deltaT > 0)
      {
        float rawFlow = (deltaW * 1000.0f) / deltaT;
        // Low Pass Filter (50% new, 50% old)
        flowRate = (rawFlow * 0.5) + (lastPoint.flowRate * 0.5);
      }
    }

    // Store Data
    measurements.push_back({timeOffset, currentWeight, flowRate});

    // Check Stop Condition based on flow rate)
    if (flowRate > STOP_FLOW_THRESHOLD)
    {
      stabilityStartTime = millis();
    }
    else
    {
      // Flow is effectively zero (or very low) -> Check duration
      if (millis() - stabilityStartTime >= STOP_TIMEOUT)
      {
        Serial.println("Flow stopped. Finishing test.");
        currentState = COMPLETED;

        // Remove the data points recorded during the wait period
        unsigned long actualEndTime = stabilityStartTime - testStartTime;
        size_t trimIndex = measurements.size();

        for (size_t i = 0; i < measurements.size(); i++)
        {
          if (measurements[i].timeOffset > actualEndTime)
          {
            trimIndex = i;
            break;
          }
        }

        if (trimIndex < measurements.size())
        {
          measurements.resize(trimIndex);
        }

        Serial.print("Test Finalized. Duration: ");
        Serial.println(actualEndTime / 1000.0);
      }
    }
  }
  break;

  default:
    break;
  }
}

// CSV and HTTP constants
const char CSV_HEADER[] = "Time (ms),Weight (g),FlowRate (ml/s)\n";
const char CONTENT_TYPE_HTML[] = "text/html";
const char CONTENT_TYPE_CSV[] = "text/csv";
const char CONTENT_TYPE_PLAIN[] = "text/plain";

// HTML constant for main control page
const char htmlRoot[] = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
 <title>ESP32 Uroflowmetry</title>
 <meta name="viewport" content="width=device-width, initial-scale=1">
 <style>
  body { font-family: sans-serif; text-align: center; padding: 20px; background-color: #f4f4f9; color: #333; }
  h1, h2 { color: #3498db; }
  button { 
   font-size: 1.2rem; 
   padding: 10px 20px; 
   cursor: pointer; 
   background-color: #2ecc71;
   color: white; 
   border: none; 
   border-radius: 5px; 
   transition: background-color 0.3s;
  }
  button:hover { background-color: #27ae60; }
  a { 
   display: block; 
   margin-top: 15px; 
   font-size: 1.1rem; 
   color: #3498db; 
   text-decoration: none; 
  }
  a:hover { text-decoration: underline; }
 </style>
</head>
<body>
 <h1>ESP32 Uroflowmetry</h1>
 
 <button id="startButton">Reset & Start New Measurement</button>
 <p id="status"></p>

 <a href="/results.html">View Results (HTML)</a>
 <a href="#" id="csvLink">Download Results (CSV)</a>

 <script>
  // Handle CSV download with client timezone
  document.getElementById('csvLink').addEventListener('click', function(e) {
   e.preventDefault();
   // Get client local time and format as YYYYMMDD_HHMMSS
   const now = new Date();
   const year = now.getFullYear();
   const month = String(now.getMonth() + 1).padStart(2, '0');
   const day = String(now.getDate()).padStart(2, '0');
   const hours = String(now.getHours()).padStart(2, '0');
   const minutes = String(now.getMinutes()).padStart(2, '0');
   const seconds = String(now.getSeconds()).padStart(2, '0');
   const timestamp = `${year}${month}${day}_${hours}${minutes}${seconds}`;
   
   // Create and trigger download with timestamped filename
   const link = document.createElement('a');
   link.href = '/results.csv?ts=' + timestamp;
   link.download = `uroflow_${timestamp}.csv`;
   link.click();
  });

  document.getElementById('startButton').addEventListener('click', function() {
   document.getElementById('status').innerText = 'Resetting...';
   fetch('/reset')
    .then(async response => {
     if (response.ok) {
      document.getElementById('status').innerText = 'Success! Ready to start measuring.';
     } else {
      const errorText = await response.text();
      document.getElementById('status').innerText = errorText;
     }
    })
    .catch(error => {
     document.getElementById('status').innerText = 'Connection error.';
    });
  });
 </script>
</body>
</html>
)rawliteral";

/**
 * @brief Serves the main control page.
 */
void handleRoot()
{
  server.send(200, CONTENT_TYPE_HTML, htmlRoot);
}

/**
 * @brief Serves results with chart/summary, streaming content to save memory.
 */
void handleResultsHTML()
{

  float totalVoided = 0.0;
  float maxFlowRate = 0.0;
  float totalFlowRateSum = 0.0;
  float testDuration = 0.0;
  String stateStr = stateToString(currentState);
  std::vector<DataPoint> measurementsCopy;

  if (xSemaphoreTake(measurements_mutex, portMAX_DELAY) == pdTRUE)
  {
    measurementsCopy = measurements;
    xSemaphoreGive(measurements_mutex);
  }
  if (!measurementsCopy.empty())
  {
    totalVoided = measurementsCopy.back().weight;
    testDuration = measurementsCopy.back().timeOffset / 1000.0;
    for (const auto &m : measurementsCopy)
    {
      totalFlowRateSum += m.flowRate;
      if (m.flowRate > maxFlowRate)
        maxFlowRate = m.flowRate;
    }
  }
  float averageFlow = (measurementsCopy.size() > 0) ? (totalFlowRateSum / measurementsCopy.size()) : 0.0;

  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");

  server.sendContent(R"rawliteral(
<!DOCTYPE html><html><head>
<title>ESP32 Uroflowmetry</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
 body { font-family: sans-serif; text-align: center; padding: 20px; background-color: #f4f4f9; color: #333; }
 h1, h2 { color: #3498db; }
 a { display: block; margin-top: 15px; font-size: 1.1rem; color: #3498db; text-decoration: none; }
 a:hover { text-decoration: underline; }
 .summary-table { margin: 20px auto; border-collapse: collapse; width: 80%; max-width: 600px; }
 .summary-table th, .summary-table td { padding: 10px; text-align: left; border: 1px solid #ddd; }
 .summary-table th { background-color: #ecf0f1; font-weight: bold; width: 40%; }
 .summary-table tr:nth-child(even) { background-color: #f9f9f9; }
</style>
<script type="text/javascript" src="https://www.gstatic.com/charts/loader.js"></script>
<script type="text/javascript">
 google.charts.load('current', {'packages':['corechart']});
 google.charts.setOnLoadCallback(drawChart);
 function drawChart() {
  var data = google.visualization.arrayToDataTable([
    ['Time (s)', 'Flow Rate (ml/s)', 'Volume (ml)'],
)rawliteral");

  if (measurementsCopy.empty())
  {
    server.sendContent("[0, 0, 0]");
  }
  else
  {
    for (size_t i = 0; i < measurementsCopy.size(); i++)
    {
      char row[100];
      snprintf(row, sizeof(row), "[%.2f,%.2f,%.2f]%s",
               measurementsCopy[i].timeOffset / 1000.0,
               measurementsCopy[i].flowRate,
               measurementsCopy[i].weight,
               (i < measurementsCopy.size() - 1) ? "," : "");
      server.sendContent(row);
    }
  }

  server.sendContent(R"rawliteral(
  ]);
  var options = {
    title: 'Uroflowmetry',
    curveType: 'function',
    legend: { position: 'bottom' },
    hAxis: { title: 'Time (seconds)' },
    vAxis: { title: 'Flow Rate (ml/s)' },
    series: {
      0: {targetAxisIndex: 0, labelInLegend: 'Flow Rate'},
      1: {targetAxisIndex: 1, labelInLegend: 'Volume'}
    },
    vAxes: {
      0: {title: 'Flow Rate (ml/s)'},
      1: {title: 'Volume (ml)'}
    }
  };
  var chart = new google.visualization.LineChart(document.getElementById('curve_chart'));
  chart.draw(data, options);
 }
</script>
</head><body>
<h2>Uroflowmetry Results</h2>
)rawliteral");

  server.sendContent("<table class='summary-table'>");
  server.sendContent("<tr><th>Current State:</th><td colspan='3'><b>" + stateStr + "</b></td></tr>");
  server.sendContent("<tr><th>Test Duration:</th><td>" + String(testDuration, 2) + " seconds</td>");
  server.sendContent("<th>Total Voided:</th><td>" + String(totalVoided, 2) + " ml</td></tr>");
  server.sendContent("<tr><th>Max Flow Rate:</th><td>" + String(maxFlowRate, 2) + " ml/s</td>");
  server.sendContent("<th>Average Flow:</th><td>" + String(averageFlow, 2) + " ml/s</td></tr>");
  server.sendContent("</table>");
  server.sendContent("<hr>");
  server.sendContent(R"rawliteral(
<div id="curve_chart" style="width: 100%; height: 600px"></div>
<hr>
<a href="/">&#9664; Back to Measurement Control</a>
</body></html>
)rawliteral");

  server.sendContent("");
}

/**
 * @brief Serves data as CSV with timestamped filename.
 * Uses client timezone timestamp from query parameter if available,
 * otherwise uses generic results.csv filename.
 */
void handleResultsCSV()
{
  // Get timestamp from query parameter (client timezone) if provided
  String filename = "results.csv";
  if (server.hasArg("ts"))
    filename = "uroflow_" + server.arg("ts") + ".csv";

  server.sendHeader("Content-Disposition", "attachment; filename=" + filename);
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, CONTENT_TYPE_CSV, "");
  server.sendContent(CSV_HEADER);

  std::vector<DataPoint> measurementsCopy;
  if (xSemaphoreTake(measurements_mutex, portMAX_DELAY) == pdTRUE)
  {
    measurementsCopy = measurements;
    xSemaphoreGive(measurements_mutex);
  }

  for (const auto &m : measurementsCopy)
  {
    String line = String(m.timeOffset) + "," + String(m.weight, 2) + "," + String(m.flowRate, 2) + "\n";
    server.sendContent(line);
  }
  server.sendContent("");
}

/**
 * @brief Handles 404 errors.
 */
void handleNotFound()
{
  server.send(404, CONTENT_TYPE_PLAIN, "404: Not found");
}

/**
 * @brief Handles HTTP reset request.
 */
void handleReset()
{
  if (currentState == MEASURING)
  {
    server.send(412, "text/plain", "Cannot reset during measurement.");
    return;
  }
  resetTest();
  server.send(200, "text/plain", "Measurement reset. Waiting for start!");
}

/**
 * @brief Converts state enum to string.
 */
String stateToString(TestState state)
{
  switch (state)
  {
  case OFFLINE:
    return "OFFLINE";
  case IDLE:
    return "IDLE";
  case MEASURING:
    return "MEASURING";
  case COMPLETED:
    return "COMPLETED";
  default:
    return "UNKNOWN";
  }
}