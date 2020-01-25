/**
 * Blinds
 *
 * ESP 8266 smart blinds
 */

#include <Arduino.h>
#include <Servo.h>
#include <EEPROM.h>
#include <ESPAsyncWebServer.h>
#include <Config.h>

AsyncWebServer server(80);

// addresses of data stored in EEPROM
struct {
  int initialized = 0;
  int secondsToClose = sizeof(bool);
  int secondsToOpen = sizeof(bool) + sizeof(unsigned int);
  int currentPosition = sizeof(bool) + (sizeof(unsigned int) * 2);
} eepromAddresses;

// default timing settings
unsigned int secondsToClose = DEFAULT_SECONDS_TO_CLOSE;
unsigned int secondsToOpen = DEFAULT_SECONDS_TO_OPEN;
unsigned int milliesPerPercentClose = (secondsToClose * 1000) / 100;
unsigned int millisPerPercentOpen = (secondsToOpen * 1000) / 100;

// milliseconds of spinning, after which the servo has to be reset
const unsigned int millisPerServoReset = (SERVO_RESET_EVERY_SECONDS * 1000);

// servo configs
const int servoPin = 2;
Servo servo;

// blind position / state vars
double startingPosition = 0;
double currentPosition = 0;
double desiredPosition = 0;
bool spinning = false;
int direction; // 1 is closing, -1 is opening
int dutyCycle = 90; // 0 is counter-clockwise, 180 is clockwise

// stores the last time a cycle was initiated
unsigned long previousMillis = 0;

// stores the last time servo was reset
unsigned long lastResetMillis = 0;

// calculated interval (ms) of spin cycle
long interval = 0;

void startSpinning(double newPosition){
  digitalWrite(LED_BUILTIN, LOW);
  if (newPosition != desiredPosition || newPosition != currentPosition) {
    if (spinning) {
      // detach from servo before spinning again
      servo.detach();
    }

    desiredPosition = newPosition;

    unsigned long currentMillis = millis();

    Serial.print("current position: ");
    Serial.println(currentPosition);
    Serial.print("desired position: ");
    Serial.println(desiredPosition);

    // calculate spin cycle length (ms)
    unsigned int millisPerPercent = desiredPosition > currentPosition ? millisPerPercentOpen : milliesPerPercentClose;
    interval = (long)(abs(desiredPosition - currentPosition) * millisPerPercent);
    Serial.print("calculated interval: ");
    Serial.println(interval);

    // start moving servo in desired direction
    dutyCycle = desiredPosition > currentPosition ? 0 : 180;
    servo.attach(servoPin);
    servo.write(dutyCycle);

    // set state vars
    direction = desiredPosition > currentPosition ? 1 : -1;
    previousMillis = currentMillis;
    lastResetMillis = previousMillis;
    spinning = true;
    startingPosition = currentPosition;
    Serial.print("calculated direction: ");
    Serial.println(direction);
  }
  digitalWrite(LED_BUILTIN, HIGH);
}

void resetServo(){
  Serial.println("detatching servo");
  servo.detach();
  delay(SERVO_RESET_DELAY_MILLISECONDS);
  Serial.println("re-attatching servo");
  servo.attach(servoPin);
  servo.write(dutyCycle);
}

void stopSpinning(){
  if (SERVO_STOP_SIGNAL_REQUIRED){
    servo.write(SERVO_STOP_DUTYCYCLE);
    delay(SERVO_STOP_SIGNAL_MILLISECONDS);
  }
  servo.detach();
  spinning = false;
  currentPosition = desiredPosition;
  EEPROM.put(eepromAddresses.currentPosition, currentPosition);
  EEPROM.commit();
  Serial.println("finished spinning!");
  digitalWrite(2, HIGH);
}

void runServer(){
  // get position
  server.on("/position", HTTP_GET, [] (AsyncWebServerRequest *request) {
    digitalWrite(LED_BUILTIN, LOW);
    request->send(200, "text/plain", String(currentPosition));
    digitalWrite(LED_BUILTIN, HIGH);
  });

  // set position with ?position=N (0 to 100)
  server.on("/set", HTTP_GET, [] (AsyncWebServerRequest *request) {
    digitalWrite(LED_BUILTIN, LOW);
    if (request->hasParam("position")) {
      double newPosition = request->getParam("position")->value().toDouble();
      startSpinning(newPosition);
    }
    request->send(204);
    digitalWrite(LED_BUILTIN, HIGH);
  });

  // get state
  server.on("/state", HTTP_GET, [] (AsyncWebServerRequest *request) {
    digitalWrite(LED_BUILTIN, LOW);
    int state = spinning ? (direction == -1 ? 0 : 1) : 2;
    request->send(200, "text/plain", String(state));
    digitalWrite(LED_BUILTIN, HIGH);
  });

  // 404
  server.onNotFound([](AsyncWebServerRequest *request) {
    request->send(404);
  });

  server.begin();
}

void setup(){
  Serial.begin(115200);
  EEPROM.begin(512);

  // stop servo from spinning while turning on
  servo.detach();

  // initialize LED pins as outputs
  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(2, OUTPUT);

  digitalWrite(LED_BUILTIN, LOW);

  // wifi connect
  WiFi.mode(WIFI_STA);
  WiFi.hostname(HOSTNAME);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  if (WiFi.waitForConnectResult() != WL_CONNECTED) {
    Serial.println("WiFi Failed!");
    return;
  }

  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());

  // get stored settings or use defaults
  bool initialized = false;
  EEPROM.get(eepromAddresses.initialized, initialized);
  if (initialized) {
    Serial.println("Using settings & position stored on EEPROM");
    EEPROM.get(eepromAddresses.secondsToClose, secondsToClose);
    EEPROM.get(eepromAddresses.secondsToOpen, secondsToOpen);
    EEPROM.get(eepromAddresses.currentPosition, currentPosition);
  } else {
    EEPROM.put(eepromAddresses.initialized, true);
    EEPROM.put(eepromAddresses.secondsToClose, secondsToClose);
    EEPROM.put(eepromAddresses.secondsToOpen, secondsToOpen);
    EEPROM.put(eepromAddresses.currentPosition, currentPosition);
    EEPROM.commit();
  }

  // milliseconds of rotation per 1% change in blinds
  milliesPerPercentClose = (secondsToClose * 1000) / 100;
  millisPerPercentOpen = (secondsToOpen * 1000) / 100;

  Serial.print("Initial position: ");
  Serial.println(currentPosition);
  Serial.print("Seconds to close: ");
  Serial.println(secondsToClose);
  Serial.print("Seconds to open: ");
  Serial.println(secondsToOpen);

  digitalWrite(2, HIGH);

  runServer();

  digitalWrite(LED_BUILTIN, HIGH);
}

void loop(){
  unsigned long currentMillis = millis();

  if (spinning){
    // calculate current position
    double percentCompleted = ((double)(currentMillis - previousMillis))/((double)interval);
    currentPosition = (percentCompleted * (desiredPosition-startingPosition)) + startingPosition;

    // if position has been reached or is out of range, stop spinning
    if (currentPosition == desiredPosition || percentCompleted > 1 || currentPosition < 0 || currentPosition > 100){
      stopSpinning();
    }

    // calculate how long the servo has been spinning for
    unsigned long spinningMillis = currentMillis - lastResetMillis;

    // reset the servo if we need to
    if (SERVO_RESET_REQUIRED && spinning && spinningMillis >= millisPerServoReset){
      resetServo();
      lastResetMillis = currentMillis;
    }
  }
}
