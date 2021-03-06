 /**************************************************
 * ME216M final project
 * Spring 2020
 * 
 * ZoomBox V1
 * Lets your friends know that you are available to 
 * hang out virtually after you dock your phone in 
 * the ZoomBox device. Automatically launches a Zoom 
 * call by detecting a wave gesture. 
 **************************************************/

/* Dependencies:
 *  https://github.com/adafruit/Adafruit_MQTT_Library
 *  https://github.com/knolleary/pubsubclient
 */
 
#include <Adafruit_NeoPixel.h>
#include <Arduino_LSM6DS3.h>
#include "EventManager.h"
#include "Ultrasonic.h"
#include <TimeLib.h>
#include <WiFiNINA.h>

// SETUP: Set your own settings here:
const char* ZoomBoxWiFi_SSID = "VPPK_2020";
const char* ZoomBoxWiFi_Password = "bangalore";
const char* ZoomBoxMQTT_SharedFeed = "p13i/feeds/zoombox";
const int   ZoomBoxFriend_MeIndex = 1;
const int   ZoomBoxSM_waveMinThreshold = 40;
const int   ZoomBoxSM_waveMaxThreshold = 70;

//---------------------------------
//      Defintions & Variables     
//---------------------------------

#define DEBUG true

#define ARRAY_LENGTH(array) ((sizeof(array))/(sizeof(array[0])))

const int LOOP_DELAY_PERIOD = 5;

// Pin definitions
#define LIGHT_SENSOR_PIN A2 
#define LED_STICK_PIN 6 
#define NUMPIXELS 10
#define MY_LED_ID 8 // number from 0 - 9
#define ULTRA_SENSOR_PIN 4 // NOTE: digital
#define LED_PIN LED_BUILTIN
#define LED_ON_TIME_MS 2000 // duration of time for LED to light on gesture detection
#define TIMEOUT 5000 // duration of time for LED to light on gesture detection

// configure the NeoPixel library
Adafruit_NeoPixel pixels(NUMPIXELS, LED_STICK_PIN, NEO_GRB + NEO_KHZ800);

// configure ultrasonic sensor library 
Ultrasonic sensor(ULTRA_SENSOR_PIN);

WiFiSSLClient ZoomBoxWifi_wifi;

// gesture recognition variables
int analogUltraVal;
bool feature1 = true;
bool feature2 = false;
int startTime = 0;
int featureCount = 0;

// phone detection variables
int currentLightVal = 0;   // variable to store the value coming from sensor
int prevLightVal = 0; // variable to store previous value of sensor
int phoneThresh = 400; 
int waiting = 10; //brightness level of LED
int active = 50;

// LED on arduino
unsigned long ledStartTime;
int ledState = 0; // 1 iff LED currently lit

// MQTT timer 
unsigned long MQTT_startTime;

uint32_t rainbow[NUMPIXELS] = {
  pixels.Color(148, 0, 211),  // light purple
  pixels.Color(75, 0, 130),   // dark purple
  pixels.Color(0, 0, 255),    // blue
  pixels.Color(0, 255, 0),    // green
  pixels.Color(255, 255, 0),  // yellow
  pixels.Color(255, 127, 0),  // orange
  pixels.Color(255, 0, 0),    // red
  pixels.Color(255, 255, 255),    // red
  pixels.Color(255, 255, 255),    // red
  pixels.Color(255, 255, 255),    // red
};

uint32_t PIXEL_OFF = pixels.Color(0, 0, 0);

//---------------------------------
//     Event & State Variables     
//---------------------------------
EventManager eventManager;
#define EVENT_WAVE_DETECTED EventManager::kEventUser0
#define EVENT_PHONE_DOCKED EventManager::kEventUser1
#define EVENT_PHONE_REMOVED EventManager::kEventUser2
#define EVENT_FRIEND_AVAILABLE EventManager::kEventUser3
#define EVENT_FRIEND_STARTED_CALL EventManager::kEventUser4
#define EVENT_FRIEND_LEFT_CALL EventManager::kEventUser5
#define EVENT_FRIEND_UNAVAILABLE EventManager::kEventUser6

// states for state machine
enum SystemState_t {STATE_ON_CALL, STATE_WAITING, STATE_IDLE};
SystemState_t currentState = STATE_IDLE;

//---------------------------------
//        Functions 
//---------------------------------

void signalFriendAvailable(char friendId) {
  int startIndex = getFriendLedStartIndex(friendId);
  int endIndex = getFriendLedEndIndex(friendId);
  uint32_t color = getFriendLedColor(friendId);

  for (int i = startIndex; i <= endIndex; i++) {
    pixels.setPixelColor(i, color);
    pixels.setBrightness(10);
  }
  pixels.show();
}

void signalFriendOnCall(char friendId) {
  Serial.print("Setting bright leds for friend with id=");
  Serial.println(friendId);
  
  int startIndex = getFriendLedStartIndex(friendId);
  int endIndex = getFriendLedEndIndex(friendId);
  uint32_t color = getFriendLedColor(friendId);

  for (int i = startIndex; i <= endIndex; i++) {
    pixels.setPixelColor(i, color);
    pixels.setBrightness(50);
  }
  pixels.show();
}

void signalFriendUnavailable(char friendId) {
  int startIndex = getFriendLedStartIndex(friendId);
  int endIndex = getFriendLedEndIndex(friendId);

  for (int i = startIndex; i <= endIndex; i++) {
    pixels.setPixelColor(i, PIXEL_OFF);
    pixels.setBrightness(10);
  }
  pixels.show();
}

void signalWiFiConnected() {
  pixels.clear();

  // The number of lights we should be cycling through
  const int maxIterationLights = 50;

  for (int i = 0; i < maxIterationLights; i++) {
    int lightIndex = i % NUMPIXELS;
    int previousLightIndex = lightIndex > 0 ? lightIndex - 1 : NUMPIXELS - 1;
    // Turn off the previous pixel
    pixels.setPixelColor(previousLightIndex, PIXEL_OFF);
    // Turn on current pixel
    pixels.setPixelColor(lightIndex, rainbow[lightIndex]);
    pixels.setBrightness(50);
    pixels.show();
    
    delay(100);
  }

  pixels.clear();
  pixels.show();
}

/* convertUltraVal
 *  converts ultrasonic reading to analog
 *  @param intVal - raw reading from ultrasonic
 *  @return none */
void convertUltraVal(int intVal) {
  analogUltraVal = map(intVal, 0, 400, 0, 1024);
} 

/* goIdle
 *  turns off my pixel on friends' devices
 *  @param none
 *  @return none  */
 void goIdle() {
    pixels.clear();
    pixels.show(); 
}

/* detectWaveFeature
 *  detects and counts features of a wave
 *  @param none
 *  @return none  */
void detectWaveFeature() {
  int currentTime = millis();
  
  if (analogUltraVal < ZoomBoxSM_waveMinThreshold && feature1) { // dip in reading
    feature1 = false;
    feature2 = true; 
    startTime = millis();
    featureCount++;
  }

  if (analogUltraVal > ZoomBoxSM_waveMaxThreshold && feature2) { // peaks in reading 
    feature2 = false; 
    feature1 = true;
    //startTime1 = millis();
    featureCount++;
  }
  
  // resets count after timeout period
  if ((currentTime - startTime) > TIMEOUT) { 
      feature2 = false; 
      feature1 = true;
      featureCount = 0;
  }

}

//---------------------------------
//        Event Checkers  
//---------------------------------
/* Checks if an event has happened, and posts them 
 *  (and any corresponding parameters) to the event 
 * queue, to be handled by the state machine. */

/* detectWave
 *  reads ultrasonic sensor and calls detectWaveFeature()
 *  to determine if user has waved
 *  @param none
 *  @return none  */
void detectWave() {
  int eventParameter = 0; 

  // turns off LED after timeout period
  if (ledState == 1 && (millis() - ledStartTime) > LED_ON_TIME_MS) {
    digitalWrite(LED_PIN, LOW); 
  }

  convertUltraVal(sensor.MeasureInCentimeters()); 
  
  if (analogUltraVal > 0) { // filter out 0 readings
    detectWaveFeature();

  if (featureCount == 5) { 
    Serial.println("Detected wave!");
    // if wave detected
    eventManager.queueEvent(EVENT_WAVE_DETECTED, eventParameter);
    //Serial.println("wave detected");
    // turn on LED for 2 seconds
    ledState = 1;
    ledStartTime = millis();
    digitalWrite(LED_PIN, HIGH);   
      // reset check
      feature2 = false; 
      feature1 = true;
      featureCount = 0;
    }
  }
}

/* detectPhone
 *  detects presence of phone using ambient light sensor
 *  @param none
 *  @return none  */
void detectPhone() {
  int eventParameter = 0; 

  currentLightVal = analogRead(LIGHT_SENSOR_PIN); 
  
  if (currentLightVal < phoneThresh && prevLightVal > phoneThresh) {
     eventManager.queueEvent(EVENT_PHONE_DOCKED, eventParameter);
  } else if (currentLightVal > phoneThresh && prevLightVal < phoneThresh) {
     eventManager.queueEvent(EVENT_PHONE_REMOVED, eventParameter);
  }
  prevLightVal = currentLightVal;

}

//---------------------------------
//           State Machine  
//---------------------------------
/* Responds to events based on the current state. */

void ZOOMBOX_SM( int event, int param) {
  
    SystemState_t nextState = currentState; //initialize next state

    switch (currentState) { 
        case STATE_IDLE:
            if (event == EVENT_PHONE_DOCKED) {
              Serial.println("IDLE -> phone detected");

              ZoomBoxFriend_signalAvailability();
              
              nextState = STATE_WAITING;
            } 
  
            if (event == EVENT_FRIEND_AVAILABLE) {
              Serial.print("IDLE -> friend available id=");
              Serial.println((char) param);

              signalFriendAvailable((char) param);
            }
            
            if (event == EVENT_FRIEND_UNAVAILABLE) {
              Serial.print("IDLE -> friend unavailable id=");
              Serial.println((char) param);

              signalFriendUnavailable((char) param);
            }
            
            break;          
        case STATE_WAITING:
        
            if (event == EVENT_WAVE_DETECTED) {
              Serial.println("WAITING -> wave detected");
              // launch zoom call
              ZoomBoxFriend_signalStartCall();
              
              nextState = STATE_ON_CALL;
            }  

           if (event == EVENT_PHONE_REMOVED) {
              Serial.println("WAITING -> phone removed");
              
              ZoomBoxFriend_signalUnavailable();
              
              nextState = STATE_IDLE;
            }

            if (event == EVENT_FRIEND_AVAILABLE) {
              Serial.print("WAITING -> friend available id=");
              Serial.println((char) param);

              signalFriendAvailable((char) param);
            }
            
            if (event == EVENT_FRIEND_UNAVAILABLE) {
              Serial.print("WAITING -> friend unavailable id=");
              Serial.println((char) param);

              signalFriendUnavailable((char) param);
            }
            
            if (event == EVENT_FRIEND_STARTED_CALL) {
              Serial.print("WAITING -> friend started zoom call id=");
              Serial.println((char) param);
              signalFriendOnCall(param);
              nextState = STATE_ON_CALL;
            }

            break;          
        case STATE_ON_CALL:
        
            if (event == EVENT_WAVE_DETECTED) {
              Serial.println("ON CALL-> wave detected");
              ZoomBoxFriend_signalLeaveCall();
              nextState = STATE_WAITING;
            } 
            
            if (event == EVENT_PHONE_REMOVED) {
              Serial.println("ON CALL-> phone removed");
              ZoomBoxFriend_signalUnavailable();
              nextState = STATE_IDLE;
            } 

            if (event == EVENT_FRIEND_STARTED_CALL) {
              Serial.print("ON CALL -> friend started zoom call id=");
              Serial.println((char) param);
              signalFriendOnCall(param);
            }
            
            if (event == EVENT_FRIEND_LEFT_CALL) {
              Serial.print("ON CALL-> friend left call id=");
              Serial.println((char) param);
              
              signalFriendUnavailable((char) param);

              nextState = STATE_IDLE;
            }
            
            if (event == EVENT_FRIEND_UNAVAILABLE) {
              Serial.print("ON_CALL -> friend unavailable id=");
              Serial.println((char) param);

              signalFriendUnavailable((char) param);
            }

            break;         
        default:
            Serial.println("STATE: Unknown State");
            break;
      }
       currentState = nextState;
}
