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

// ESP32 GPIO pins for HX711
const int LOADCELL_DOUT_PIN = 18;
const int LOADCELL_SCK_PIN = 16;

// RGB component.
const int freq = 5000;    // PWM Frequency (5 kHz)
const int resolution = 8; // PWM Resolution (8 bits = 0 to 255 brightness levels)
const int redPin = 22;
const int greenPin = 21;
const int bluePin = 19;

// Wifi configuration.
const char *ssid = "xxx";
const char *password = "xxx";
unsigned long lastReconnectAttempt = 0;
const unsigned long RECONNECT_INTERVAL = 15000; // 15 seconds - WiFi reconnection retry interval

// Threshold to START recording (in grams/ml)
const float START_THRESHOLD = 1.0;

// Duration of stability to trigger STOP (in ms) (10 seconds)
const unsigned long STOP_TIMEOUT = 10000;

// Tolerance for "no change" during stop condition (in grams)
const float STABILITY_TOLERANCE = 1.0;

// Time between data points (in ms) (500ms = 2Hz)
const unsigned long SAMPLING_INTERVAL = 500;

// How many sensor readings to average per sample point.
// At default 10SPS HX711 rate, 5 readings take ~500ms.
const int READINGS_TO_AVERAGE = 3;

// Calibration factor, set your own value
// IMPORTANT: Must be calibrated experimentally. A value of 0 will cause silent failures.
const float CALIBRATION_FACTOR = -1025.42;

// CSV and HTTP constants
const char CSV_HEADER[] = "Time (ms),Weight (g),FlowRate (ml/s)\n";
const char CONTENT_TYPE_HTML[] = "text/html";
const char CONTENT_TYPE_CSV[] = "text/csv";
const char CONTENT_TYPE_PLAIN[] = "text/plain";

// Scale object
HX711 scale;

// Server object
WebServer server(80); // HTTP server on port 80

enum TestState
{
  OFFLINE,
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
portMUX_TYPE measurementsMutex = portMUX_INITIALIZER_UNLOCKED;

// Timing variables
unsigned long lastSampleTime = 0;
unsigned long testStartTime = 0;
unsigned long stabilityStartTime = 0;
float stableWeightReference = 0;

// Function prototypes
void setup();                                   // Main setup function.
void loop();                                    // Main loop function.
void setupWiFi();                               // Initializes WiFi connection.
void WiFiEvent(WiFiEvent_t event);              // Handles WiFi state changes.
void setRGBColor(int red, int green, int blue); // Sets the RGB LED color.
void setupScale();                              // Initializes and tares the HX711 scale.
void checkStartCondition();                     // Checks if measurement should start.
void performMeasurement();                      // Reads scale, logs data, and checks stop.
void checkStopCondition(float currentWeight);   // Checks if measurement should stop.
void finalizeTest();                            // Completes and trims measurement data.
void resetTest();                               // Clears data and tares scale.
void handleReset();                             // Handles HTTP reset request.
void setupWebServer();                          // Initializes the HTTP server.
void handleRoot();                              // Serves the main control page.
void handleResultsCSV();                        // Serves data as CSV.
void handleNotFound();                          // Handles 404 errors.
String stateToString(TestState state);          // Converts state enum to string.
void handleResultsHTML();                       // Serves results with chart/summary.

/**
 * @brief Main setup function.
 * Initializes serial, scale, LEDs, WiFi, and web server.
 * Pre-allocates vector memory for measurements to reduce fragmentation.
 */
void setup()
{
  Serial.begin(115200);
  Serial.println("\n=== ESP32Flow Starting ===");
  setupScale();

  ledcAttach(redPin, freq, resolution);
  ledcAttach(greenPin, freq, resolution);
  ledcAttach(bluePin, freq, resolution);

  // Pre-allocate memory for measurements vector
  // Estimate: ~10 seconds at 2Hz = 20 points, budget for 500 measurements
  measurements.reserve(500);

  setupWiFi();
  setupWebServer();
}

/**
 * @brief Main loop function.
 */
void loop()
{
  server.handleClient();

  switch (currentState)
  {
  case OFFLINE:
    setRGBColor(255, 0, 0); // Red
    // Check if it's time to try reconnecting
    if (millis() - lastReconnectAttempt >= RECONNECT_INTERVAL)
    {
      Serial.println("Offline. Attempting WiFi reconnection...");
      WiFi.begin(ssid, password);
      lastReconnectAttempt = millis();
    }
    break;

  case IDLE:
    setRGBColor(0, 255, 0); // green.
    checkStartCondition();
    break;

  case MEASURING:
    setRGBColor(255, 165, 0); // orange.
    if (millis() - lastSampleTime >= SAMPLING_INTERVAL)
    {
      performMeasurement();
      lastSampleTime = millis();
    }
    break;

  case COMPLETED:
    setRGBColor(0, 0, 255); // blue.
    break;
  }
}

/**
 * @brief Initializes WiFi connection.
 */
void setupWiFi()
{
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.onEvent(WiFiEvent);
  WiFi.begin(ssid, password);
}

/**
 * @brief Handles WiFi state changes.
 */
void WiFiEvent(WiFiEvent_t event)
{
  switch (event)
  {
  case ARDUINO_EVENT_WIFI_STA_START:
    Serial.println("WiFi starting... Attempting connection.");
    currentState = OFFLINE;
    setRGBColor(255, 0, 0); // red.
    break;

  case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
    Serial.println("Wi-Fi Disconnected....");
    if (currentState == MEASURING)
    {
      Serial.println("Aborting test due to disconnect.");
    }
    currentState = OFFLINE;
    setRGBColor(255, 0, 0); // red.
    lastReconnectAttempt = millis();
    break;

  case ARDUINO_EVENT_WIFI_STA_CONNECTED:
    Serial.println("Wi-Fi Connected! Waiting for IP...");
    break;

  case ARDUINO_EVENT_WIFI_STA_GOT_IP:
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());

    portENTER_CRITICAL(&measurementsMutex);
    if (measurements.size() > 0)

    {
      // Force a reset when coming back online.
      currentState = COMPLETED;
    }
    else
    {
      currentState = IDLE;
    }
    portEXIT_CRITICAL(&measurementsMutex);
    break;

  case ARDUINO_EVENT_WIFI_STA_STOP:
    currentState = OFFLINE;
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
  ledcWrite(redPin, red);
  ledcWrite(greenPin, green);
  ledcWrite(bluePin, blue);
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
 * @brief Checks if measurement should start.
 */
void checkStartCondition()
{
  float currentWeight = scale.get_units(1);

  if (currentWeight >= START_THRESHOLD)
  {
    Serial.println("Recording started at " + String(currentWeight) + "g");

    currentState = MEASURING;
    testStartTime = millis();
    lastSampleTime = millis() - SAMPLING_INTERVAL; // Force immediate first sample
    stabilityStartTime = millis();
    stableWeightReference = currentWeight;
  }
}

/**
 * @brief Reads scale, logs data, and checks stop.
 */
void performMeasurement()
{
  float currentWeight = scale.get_units(READINGS_TO_AVERAGE);
  unsigned long currentTime = millis() - testStartTime;

  float flowRate = 0;
  float deltaW = 0;
  float deltaT = 0;

  portENTER_CRITICAL(&measurementsMutex);
  if (measurements.size() > 0)
  {
    deltaW = currentWeight - measurements.back().weight;
    deltaT = currentTime - measurements.back().timeOffset;

    // Safety check: prevent divide-by-zero and ensure valid flow rate calculation
    if (deltaT > 0)
    {
      flowRate = (deltaW * 1000.0f) / deltaT;
    }
  }
  measurements.push_back({currentTime, currentWeight, flowRate});
  portEXIT_CRITICAL(&measurementsMutex);
  checkStopCondition(currentWeight);
}

/**
 * @brief Checks if measurement should stop.
 */
void checkStopCondition(float currentWeight)
{
  float change = fabs(currentWeight - stableWeightReference);

  if (change > STABILITY_TOLERANCE)
  {
    stabilityStartTime = millis();
    stableWeightReference = currentWeight;
  }
  else
  {
    if (millis() - stabilityStartTime >= STOP_TIMEOUT)
    {
      Serial.println("Recording stopped");
      finalizeTest();
    }
  }
}

/**
 * @brief Completes and trims measurement data.
 * Removes data points recorded after stability was detected.
 */
void finalizeTest()
{
  currentState = COMPLETED;
  unsigned long actualEndTime = stabilityStartTime - testStartTime;

  portENTER_CRITICAL(&measurementsMutex);

  // Find the first data point AFTER actualEndTime and trim from there
  size_t trimIndex = measurements.size(); // Default: keep all
  for (size_t i = 0; i < measurements.size(); i++)
  {
    if (measurements[i].timeOffset > actualEndTime)
    {
      trimIndex = i;
      break; // Found first point beyond end time
    }
  }

  // Trim to actualEndTime (keep data up to and including the point at or before actualEndTime)
  if (trimIndex < measurements.size())
  {
    measurements.resize(trimIndex);
  }

  portEXIT_CRITICAL(&measurementsMutex);
}

/**
 * @brief Clears data and tares scale.
 */
void resetTest()
{
  portENTER_CRITICAL(&measurementsMutex);
  measurements.clear();
  measurements.shrink_to_fit();
  portEXIT_CRITICAL(&measurementsMutex);

  Serial.println("Taring...");
  scale.tare();

  if (currentState != OFFLINE)

  {
    currentState = IDLE;
  }

  Serial.println("Ready for new test.");
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
 * @brief Serves the main control page.
 */
void handleRoot()
{
  String html = R"rawliteral(
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
    .then(response => {
     if (response.ok) {
      document.getElementById('status').innerText = 'Success! Ready to start measuring.';
     } else {
      document.getElementById('status').innerText = 'Error reseting.';
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

  server.send(200, CONTENT_TYPE_HTML, html);
}

/**
 * @brief Serves results with chart/summary, streaming content to save memory.
 */
void handleResultsHTML()
{
  std::vector<DataPoint> measurements_copy;
  portENTER_CRITICAL(&measurementsMutex);
  measurements_copy = measurements;
  portEXIT_CRITICAL(&measurementsMutex);

  float totalVoided = 0.0;
  float maxFlowRate = 0.0;
  float totalFlowRateSum = 0.0;
  float testDuration = 0.0;
  String stateStr = stateToString(currentState);

  if (!measurements_copy.empty())
  {
    totalVoided = measurements_copy.back().weight;
    testDuration = measurements_copy.back().timeOffset / 1000.0;
    for (size_t i = 0; i < measurements_copy.size(); i++)
    {
      float currentFlow = measurements_copy[i].flowRate;
      totalFlowRateSum += currentFlow;
      if (currentFlow > maxFlowRate)
      {
        maxFlowRate = currentFlow;
      }
    }
  }
  float averageFlow = (measurements_copy.size() > 0) ? (totalFlowRateSum / measurements_copy.size()) : 0.0;

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
)rawliteral");

  server.sendContent(R"rawliteral(
<script type="text/javascript" src="https://www.gstatic.com/charts/loader.js"></script>
<script type="text/javascript">
 google.charts.load('current', {'packages':['corechart']});
 google.charts.setOnLoadCallback(drawChart);
 function drawChart() {
  var data = google.visualization.arrayToDataTable([
    ['Time (s)', 'Flow Rate (ml/s)', 'Volume (ml)'],
)rawliteral");

  if (measurements_copy.empty())
  {
    server.sendContent("[0, 0, 0]");
  }
  else
  {
    for (size_t i = 0; i < measurements_copy.size(); i++)
    {
      // Build row efficiently using formatted string
      char row[100];
      snprintf(row, sizeof(row), "[%.2f,%.2f,%.2f]%s",
               measurements_copy[i].timeOffset / 1000.0,
               measurements_copy[i].flowRate,
               measurements_copy[i].weight,
               (i < measurements_copy.size() - 1) ? "," : "");
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
  std::vector<DataPoint> measurements_copy;
  portENTER_CRITICAL(&measurementsMutex);
  measurements_copy = measurements;
  portEXIT_CRITICAL(&measurementsMutex);

  // Get timestamp from query parameter (client timezone) if provided
  String filename = "results.csv";
  if (server.hasArg("ts"))
  {
    String clientTimestamp = server.arg("ts");
    filename = "uroflow_" + clientTimestamp + ".csv";
  }

  // Build Content-Disposition header with dynamic filename
  String dispositionHeader = "attachment; filename=" + filename;
  server.sendHeader("Content-Disposition", dispositionHeader);
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, CONTENT_TYPE_CSV, "");
  server.sendContent(CSV_HEADER);

  if (!measurements_copy.empty())
  {
    for (const auto &m : measurements_copy)
    {
      String line = String(m.timeOffset) + "," + String(m.weight, 2) + "," + String(m.flowRate, 2) + "\n";
      server.sendContent(line);
    }
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
  resetTest();
  server.send(200, "text/plain", "OK. Measurement reset. Waiting for start.");
}

/**
 * @brief Converts state enum to string.
 */
String stateToString(TestState state)
{
  switch (state)
  {
  case OFFLINE:
    return "OFFLINE (WiFi Disconnected)";
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