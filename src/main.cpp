#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESP32Servo.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>


// ===== Ultrasonic pins =====
#define TRIG_PIN 4
#define ECHO_PIN 2

// ===== Outputs =====
#define LED_PIN 5
#define BUZZER_PIN 18
#define SERVO_PIN 13

// ===== Constants =====
const float DETECTION_LIMIT_CM = 50.0;
const int SCAN_STEP = 5;
const int SCAN_DELAY = 200; // Increased from 100 to 300ms for smoother movement

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
bool isDetecting = false; // Flag to track detection state

// ===== Distance measurement =====
float getDistance() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  
  long duration = pulseIn(ECHO_PIN, HIGH, 30000);
  float distance = duration * 0.0343 / 2;
  
  if (distance <= 0 || distance > 60) distance = 60;
  return distance;
}

// ===== Web page =====
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
      font-family: sans-serif;
    }
    canvas {
      background: #000;
      margin-top: 20px;
      border: 1px solid #333;
      border-radius: 50%;
    }
    #info {
      font-size: 18px;
      margin-top: 20px;
    }
    .detecting {
      color: #f00;
      font-weight: bold;
    }
  </style>
</head>
<body>
  <h2>ESP32 Ultrasonic Radar</h2>
  <canvas id="radar" width="300" height="300"></canvas>
  <p id="info">Angle: --°, Distance: -- cm</p>

  <script>
    const canvas = document.getElementById('radar');
    const ctx = canvas.getContext('2d');
    const center = 150, radius = 140;

    function drawRadar(angle, distance) {
      ctx.fillStyle = "black";
      ctx.fillRect(0, 0, 300, 300);

      // circles
      ctx.strokeStyle = "#0f0";
      for (let r=40; r<=radius; r+=40) {
        ctx.beginPath();
        ctx.arc(center, center, r, 0, 2*Math.PI);
        ctx.stroke();
      }

      // sweep line
      const rad = angle * Math.PI/180;
      const x = center + radius * Math.cos(rad);
      const y = center - radius * Math.sin(rad);
      ctx.strokeStyle = "#0f0";
      ctx.beginPath();
      ctx.moveTo(center, center);
      ctx.lineTo(x, y);
      ctx.stroke();

      // object
      const limited = Math.min(distance, 30);
      const objRadius = (limited / 30) * radius;
      const dx = center + objRadius * Math.cos(rad);
      const dy = center - objRadius * Math.sin(rad);
      ctx.fillStyle = distance <= 20 ? "#f00" : "#555";
      ctx.beginPath();
      ctx.arc(dx, dy, 5, 0, 2*Math.PI);
      ctx.fill();

      const infoElement = document.getElementById("info");
      infoElement.innerText = "Angle: " + angle + "°, Distance: " + distance.toFixed(1) + " cm";
      
      if (distance <= 20) {
        infoElement.className = "detecting";
        infoElement.innerText += " - OBJECT DETECTED!";
      } else {
        infoElement.className = "";
      }
    }

    async function updateRadar() {
      const res = await fetch("/data");
      const d = await res.json();
      drawRadar(d.angle, d.distance);
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
                ",\"distance\":" + String(lastDistance, 1) + "}";
  server.send(200, "application/json", json);
}

// ===== Setup =====
void setup() {
  Serial.begin(9600);

  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("ESP32 Radar Ready");
  delay(1000);
  lcd.clear();

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(LED_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  
  digitalWrite(LED_PIN, LOW);
  digitalWrite(BUZZER_PIN, LOW);
  
  radarServo.attach(SERVO_PIN);
  radarServo.write(currentAngle);
  
  WiFi.softAP(ssid, password);
  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());
  
  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.begin();
  
  Serial.println("Server ready");
}

// ===== Loop =====
void loop() {
  server.handleClient();
  
  // FIRST: Move servo to next position
  radarServo.write(currentAngle);
  delay(SCAN_DELAY);
  
  // THEN: Measure distance at this angle
  lastDistance = getDistance();
  Serial.printf("Angle: %d°, Distance: %.1f cm\n", currentAngle, lastDistance);
  
  // If object is close → STOP SERVO & alert
  if (lastDistance <= DETECTION_LIMIT_CM) {
    if (!isDetecting) {
      digitalWrite(LED_PIN, HIGH);
      digitalWrite(BUZZER_PIN, HIGH);
      isDetecting = true;

      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("Object Detected!");
      lcd.setCursor(0, 1);
      lcd.print(String(lastDistance, 1) + " cm");
      
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
      lcd.setCursor(0, 0);
      lcd.print("Object Cleared");
      delay(1000);
      lcd.clear();
    } else {
      lcd.setCursor(0, 0);
      lcd.print("No Object Det.");
      lcd.setCursor(0, 1);
      lcd.print(String(lastDistance, 1) + " cm   ");
    }
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