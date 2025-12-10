#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESP32Servo.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// ===== Ultrasonic pins =====
#define TRIG_PIN 4
#define ECHO_PIN 2

// ===== Rotary Encoder pins =====
#define ENCODER_CLK 25
#define ENCODER_DT 26
#define ENCODER_SW 27  // Optional button for reset to default

// ===== Outputs =====
#define LED_PIN 5
#define BUZZER_PIN 18
#define SERVO_PIN 13

// ===== Constants =====
const float MIN_DETECTION_LIMIT = 30.0;
const float MAX_DETECTION_LIMIT = 400.0;  // HC-SR04 max range ~400cm
const float RANGE_INCREMENT = 5.0;         // Adjust by 5cm per encoder click
const int SCAN_STEP = 5;
const int SCAN_DELAY = 200;

// ===== Wi-Fi =====
const char* ssid = "ESP32-Radar";
const char* password = "12345678";

// ===== LCD =====
LiquidCrystal_I2C lcd(0x27, 16, 2);

WebServer server(80);
Servo radarServo;

// ===== Variables =====
int currentAngle = 0;
bool movingForward = true;
float lastDistance = 0;
bool isDetecting = false;
float detectionLimit = MIN_DETECTION_LIMIT;  // Dynamic detection range

// ===== Encoder Variables =====
volatile int encoderPos = 0;
int lastEncoderPos = 0;
bool lastCLK = HIGH;
unsigned long lastEncoderUpdate = 0;
const unsigned long ENCODER_DEBOUNCE = 5;  // 5ms debounce

// ===== Distance measurement with better filtering =====
float getDistance() {
  // Take 3 readings and use median to reduce noise
  float readings[3];
  for(int i = 0; i < 3; i++) {
    digitalWrite(TRIG_PIN, LOW);
    delayMicroseconds(2);
    digitalWrite(TRIG_PIN, HIGH);
    delayMicroseconds(10);
    digitalWrite(TRIG_PIN, LOW);
    
    long duration = pulseIn(ECHO_PIN, HIGH, 30000);
    readings[i] = duration * 0.0343 / 2;
    if(i < 2) delayMicroseconds(50);
  }
  
  // Simple bubble sort for median
  if(readings[0] > readings[1]) { float t = readings[0]; readings[0] = readings[1]; readings[1] = t; }
  if(readings[1] > readings[2]) { float t = readings[1]; readings[1] = readings[2]; readings[2] = t; }
  if(readings[0] > readings[1]) { float t = readings[0]; readings[0] = readings[1]; readings[1] = t; }
  
  float distance = readings[1];  // Return median
  
  if (distance <= 0 || distance > MAX_DETECTION_LIMIT) {
    distance = MAX_DETECTION_LIMIT;
  }
  return distance;
}

// ===== Encoder interrupt handler =====
void IRAM_ATTR readEncoder() {
  if (millis() - lastEncoderUpdate < ENCODER_DEBOUNCE) return;
  
  bool clkState = digitalRead(ENCODER_CLK);
  bool dtState = digitalRead(ENCODER_DT);
  
  if (clkState != lastCLK && clkState == LOW) {
    if (dtState != clkState) {
      encoderPos++;  // Clockwise
    } else {
      encoderPos--;  // Counter-clockwise
    }
    lastEncoderUpdate = millis();
  }
  lastCLK = clkState;
}

// ===== Update detection limit based on encoder =====
void updateDetectionLimit() {
  if (encoderPos != lastEncoderPos) {
    int delta = encoderPos - lastEncoderPos;
    detectionLimit += (delta * RANGE_INCREMENT);
    
    // Constrain to valid range
    if (detectionLimit < MIN_DETECTION_LIMIT) {
      detectionLimit = MIN_DETECTION_LIMIT;
      encoderPos = lastEncoderPos;  // Stop at minimum
    } else if (detectionLimit > MAX_DETECTION_LIMIT) {
      detectionLimit = MAX_DETECTION_LIMIT;
      encoderPos = lastEncoderPos;  // Stop at maximum
    }
    
    lastEncoderPos = encoderPos;
    
    Serial.printf("Detection limit changed to: %.1f cm\n", detectionLimit);
    
    // Show on LCD temporarily
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Range Set:");
    lcd.setCursor(0, 1);
    lcd.print(String(detectionLimit, 0) + " cm");
    delay(800);
    lcd.clear();
  }
}

// ===== Web page with dynamic range display =====
const char MAIN_page[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <title>ESP32 Radar</title>
  <style>
    body {
      background: #0a0a0a;
      color: white;
      text-align: center;
      font-family: 'Segoe UI', sans-serif;
      margin: 0;
      padding: 20px;
    }
    h2 { margin: 10px 0; color: #0f0; }
    canvas {
      background: #000;
      margin: 20px auto;
      border: 2px solid #0f0;
      border-radius: 50%;
      display: block;
    }
    #info {
      font-size: 18px;
      margin: 15px 0;
    }
    #range {
      font-size: 16px;
      color: #0ff;
      margin: 10px 0;
      padding: 10px;
      background: rgba(0, 255, 255, 0.1);
      border-radius: 5px;
      display: inline-block;
    }
    .detecting {
      color: #f00;
      font-weight: bold;
      animation: blink 1s infinite;
    }
    @keyframes blink {
      0%, 50% { opacity: 1; }
      51%, 100% { opacity: 0.3; }
    }
  </style>
</head>
<body>
  <h2>ESP32 Ultrasonic Radar</h2>
  <div id="range">Detection Range: <span id="rangeValue">--</span> cm</div>
  <canvas id="radar" width="400" height="400"></canvas>
  <p id="info">Angle: --°, Distance: -- cm</p>

  <script>
    const canvas = document.getElementById('radar');
    const ctx = canvas.getContext('2d');
    const center = 200, radius = 180;

    function drawRadar(angle, distance, range) {
      ctx.fillStyle = "black";
      ctx.fillRect(0, 0, 400, 400);

      // Range circles
      ctx.strokeStyle = "#0f0";
      ctx.lineWidth = 1;
      const numCircles = 4;
      for (let i = 1; i <= numCircles; i++) {
        ctx.beginPath();
        ctx.arc(center, center, (radius / numCircles) * i, 0, 2 * Math.PI);
        ctx.stroke();
        
        // Range labels
        ctx.fillStyle = "#0f0";
        ctx.font = "10px monospace";
        const labelDist = (range / numCircles) * i;
        ctx.fillText(labelDist.toFixed(0), center + 5, center - (radius / numCircles) * i);
      }

      // Center point
      ctx.fillStyle = "#0f0";
      ctx.beginPath();
      ctx.arc(center, center, 3, 0, 2 * Math.PI);
      ctx.fill();

      // Sweep line with gradient
      const rad = (180 - angle) * Math.PI / 180;
      const x = center + radius * Math.cos(rad);
      const y = center + radius * Math.sin(rad);
      
      const gradient = ctx.createLinearGradient(center, center, x, y);
      gradient.addColorStop(0, "rgba(0, 255, 0, 0.8)");
      gradient.addColorStop(1, "rgba(0, 255, 0, 0.1)");
      
      ctx.strokeStyle = gradient;
      ctx.lineWidth = 2;
      ctx.beginPath();
      ctx.moveTo(center, center);
      ctx.lineTo(x, y);
      ctx.stroke();

      // Object detection
      if (distance <= range) {
        const objRadius = (distance / range) * radius;
        const dx = center + objRadius * Math.cos(rad);
        const dy = center + objRadius * Math.sin(rad);
        
        const isClose = distance <= (range * 0.4);
        ctx.fillStyle = isClose ? "#f00" : "#ff0";
        ctx.shadowBlur = isClose ? 15 : 10;
        ctx.shadowColor = ctx.fillStyle;
        ctx.beginPath();
        ctx.arc(dx, dy, isClose ? 8 : 6, 0, 2 * Math.PI);
        ctx.fill();
        ctx.shadowBlur = 0;
      }

      // Update info
      document.getElementById("rangeValue").innerText = range.toFixed(0);
      const infoElement = document.getElementById("info");
      infoElement.innerText = "Angle: " + angle + "°, Distance: " + distance.toFixed(1) + " cm";
      
      if (distance <= range) {
        infoElement.className = "detecting";
        infoElement.innerText += " - OBJECT DETECTED!";
      } else {
        infoElement.className = "";
      }
    }

    async function updateRadar() {
      try {
        const res = await fetch("/data");
        const d = await res.json();
        drawRadar(d.angle, d.distance, d.range);
      } catch(e) {
        console.error("Update failed:", e);
      }
    }

    setInterval(updateRadar, 200);
  </script>
</body>
</html>
)rawliteral";

// ===== Handlers =====
void handleRoot() {
  server.send_P(200, "text/html", MAIN_page);
}

void handleData() {
  String json = "{\"angle\":" + String(currentAngle) + 
                ",\"distance\":" + String(lastDistance, 1) + 
                ",\"range\":" + String(detectionLimit, 1) + "}";
  server.send(200, "application/json", json);
}

// ===== Setup =====
void setup() {
  Serial.begin(9600);

  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("ESP32 Radar Ready");
  lcd.setCursor(0, 1);
  lcd.print("Initializing...");
  delay(1500);
  lcd.clear();

  // Pin setup
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(LED_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  
  // Encoder setup
  pinMode(ENCODER_CLK, INPUT_PULLUP);
  pinMode(ENCODER_DT, INPUT_PULLUP);
  pinMode(ENCODER_SW, INPUT_PULLUP);
  
  attachInterrupt(digitalPinToInterrupt(ENCODER_CLK), readEncoder, CHANGE);
  
  digitalWrite(LED_PIN, LOW);
  digitalWrite(BUZZER_PIN, LOW);
  
  // Servo setup
  radarServo.attach(SERVO_PIN);
  radarServo.write(currentAngle);
  
  // WiFi setup
  WiFi.softAP(ssid, password);
  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());
  
  lcd.setCursor(0, 0);
  lcd.print("IP:");
  lcd.setCursor(0, 1);
  lcd.print(WiFi.softAPIP());
  delay(2000);
  lcd.clear();
  
  // Web server setup
  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.begin();
  
  Serial.println("Server ready");
  Serial.printf("Initial detection range: %.1f cm\n", detectionLimit);
}

// ===== Loop =====
void loop() {
  server.handleClient();
  
  // Check for encoder button press (optional reset to default)
  if (digitalRead(ENCODER_SW) == LOW) {
    delay(50);  // Debounce
    if (digitalRead(ENCODER_SW) == LOW) {
      detectionLimit = MIN_DETECTION_LIMIT;
      encoderPos = 0;
      lastEncoderPos = 0;
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("Range Reset");
      lcd.setCursor(0, 1);
      lcd.print(String(detectionLimit, 0) + " cm");
      delay(1000);
      lcd.clear();
      while(digitalRead(ENCODER_SW) == LOW);  // Wait for release
    }
  }
  
  // Update detection limit from encoder
  updateDetectionLimit();
  
  // Move servo to next position
  radarServo.write(currentAngle);
  delay(SCAN_DELAY);
  
  // Measure distance at this angle
  lastDistance = getDistance();
  Serial.printf("Angle: %d°, Distance: %.1f cm, Limit: %.1f cm\n", 
                currentAngle, lastDistance, detectionLimit);
  
  // Object detection logic
  if (lastDistance <= detectionLimit) {
    if (!isDetecting) {
      digitalWrite(LED_PIN, HIGH);
      digitalWrite(BUZZER_PIN, HIGH);
      isDetecting = true;

      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("Object Detected!");
      lcd.setCursor(0, 1);
      lcd.print(String(lastDistance, 1) + "cm @" + String(currentAngle) + "deg");
      
      Serial.println(">>> OBJECT DETECTED - SERVO STOPPED <<<");
    }
    delay(100);
    return;
  } else {
    if (isDetecting) {
      digitalWrite(LED_PIN, LOW);
      digitalWrite(BUZZER_PIN, LOW);
      isDetecting = false;
      lcd.clear();
    }
    
    // Normal scanning display
    lcd.setCursor(0, 0);
    lcd.print("Scan:" + String(currentAngle) + "deg ");
    lcd.setCursor(0, 1);
    lcd.print("R:" + String(detectionLimit, 0) + " D:" + String(lastDistance, 0) + "  ");
  }

  // Calculate next angle (only runs when NO object detected)
  if (movingForward) {
    currentAngle += SCAN_STEP;
    if (currentAngle >= 180) {
      currentAngle = 180;
      movingForward = false;
    }
  } else {
    currentAngle -= SCAN_STEP;
    if (currentAngle <= 0) {
      currentAngle = 0;
      movingForward = true;
    }
  }
}