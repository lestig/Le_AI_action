// Project: ESP8266 Laundry Room Controller
// Version: 3.0
// Description: Manages laundry room environment and settings through various sensors and inputs.
// Engineer: FA+
// Architect/PM: FA

/* :::  A <2.09>:    System Configuration :::
        A.1 <2.09>:  Library Inclusions */

#include <Arduino.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <RF24.h>
#include <DHT.h>
#include <TimeLib.h>
#include <ESP8266WiFi.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include "logger.h"

/* :::  B <2.09>:        Constant Definitions :::
        B.1 <2.09>:      WiFi and NTP Configuration */

const char* ssid = "SSID";
const char* password = "PASS*";
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 0, 60000); // 0 offset for UTC, update interval 60000ms

// B.2 <2.09>: Pin Definitions

// B.2.1 <2.09>: Sensor and Control Pins

#define CE_PIN 4
#define CSN_PIN 5
#define BUTTON_PIN 16
#define POT_PIN A0
#define SDA_PIN 0
#define SCL_PIN 3
#define BUZZER_PIN 15
#define DHTPIN 2
#define DHTTYPE DHT22 // DHT22 sensor type

// LCD settings
#define LCD_ADDRESS 0x27
#define LCD_COLS 16
#define LCD_ROWS 2

// B.3 <2.09>:   Potentiometer Mapping Definitions

// B.3.1 <2.09>: Knob Temperature and Date Adjustment Ranges

#define TEMP_MIN 35
#define TEMP_MAX 90
#define TEMP_POT_MIN 10
#define TEMP_POT_MAX 1023
#define DATE_MIN 0
#define DATE_MAX 100
#define DATE_POT_MIN 10
#define DATE_POT_MAX 1023

/* :::  C <2.09>:    Global Variables and Function Declarations :::
        C.1 <2.09>:  Screen and Component State Tracking */

enum Screen { MAIN_SCREEN, HUMIDITY_TEMP_SCREEN, EDIT_DATE_TIME, SET_TEMPERATURE };
enum DateTimeComponent { MONTH, DAY, YEAR, HOUR, MINUTE };
enum ScreenState { MAIN_SCREEN_STATE, HUMIDITY_TEMP_STATE, EDIT_DATE_TIME_STATE, SET_TEMPERATURE_STATE };

// C.2 <2.09>: Devices Initialization State Tracking

bool allowTempAdjust = false;       // Global flag to allow temperature adjustment
float setTemp = 70.0;               // Initial set temperature
float lastSetTemp = 70.0;           // Tracks the last set temperature to reduce sensitivity
bool heaterOn = false;
Screen currentScreen = MAIN_SCREEN;
DateTimeComponent currentDateTimeComponent = MONTH;
unsigned long lastActivityTime = millis();
bool backlightOn = true;

DHT dht(DHTPIN, DHTTYPE);           // DHT sensor setup
RF24 radio(CE_PIN, CSN_PIN);        // Create an RF24 object - RF communication settings
const byte rfAddress[6] = "00001";
LiquidCrystal_I2C lcd(LCD_ADDRESS, LCD_COLS, LCD_ROWS);     // LCD Setup

// C.3 <2.09>:   Utility Functions

// C.3.1 <2.09>: Temperature and Time Adjustment using Potentiometer

float adjustTemperature(int potValue) {
    // Adjust Temp using knob
    return map(potValue, TEMP_POT_MAX, TEMP_POT_MIN, TEMP_MIN, TEMP_MAX);
}

int adjustDateTime(int potValue) {
    // Adjust Date/Time using knob
    return map(potValue, DATE_POT_MAX, DATE_POT_MIN, DATE_MIN, DATE_MAX);
}

// C.4 <2.09>:   Function Declarations

void updateScreen(Screen newScreen);
void checkAndAdjustSetTemperature(int potValue);
void initializeSystem();
void setupWiFi();
void initializeSensors();
void setDateAndTime();
void waitForSensorStabilization();
void displayMainScreen();
void displaySetTemperatureScreen();
void displayHumidityTempScreen();
void displayEditDateTimeScreen();
void handleUserInput();
void monitorTemperature();
void handleAutoScreenOff();
void handleKnobMovement(int potValue);
void handleDateTimeEdit(int potValue);
void switchScreens();
void sendRFSignal(String command, float temp, String dateTime);
float getRelayTemperature();

// C.4.1 <2.08>:   Update LCD with specific screen

void updateScreen(Screen newScreen) {
    if (currentScreen != newScreen) {
        currentScreen = newScreen; // Update the global screen state
        switch (newScreen) {
            case MAIN_SCREEN:
                displayMainScreen();
                break;
            case HUMIDITY_TEMP_SCREEN:
                displayHumidityTempScreen();
                break;
            case EDIT_DATE_TIME:
                displayEditDateTimeScreen();
                break;
            case SET_TEMPERATURE:
                displaySetTemperatureScreen();
                break;
        }
        Serial.print("Screen updated to: ");
        Serial.println(currentScreen);
    }
}


/* ::: C.4.2 <2.09>: Adjust Temperature with Logging Outputs ::: */

void checkAndAdjustSetTemperature(int potValue) {
    float newSetTemp = adjustTemperature(potValue);                                          // Map potentiometer value to the temperature range

    // Log values in a group for easy readability
    LOG_VERBOSE("1. : C.4.2 <2.09> ::: System Configuration : ** Temperature Adjustment **");
    LOG_VERBOSE("2. : C.4.2 <2.09> ::: checkAndAdjustSetTemperature() : Potentiometer Value: " + String(potValue));
    LOG_VERBOSE("3. : C.4.2 <2.09> ::: checkAndAdjustSetTemperature() : New Set Temperature: " + String(newSetTemp));

    // Adjust only if there's a significant difference
    if (abs(newSetTemp - lastSetTemp) > 0.5) {                                             // Reduce threshold for quicker response
        setTemp = newSetTemp;
        lastSetTemp = setTemp;                                                              // Update last known set temperature

        LOG_VERBOSE("4. : C.4.2 <2.09> ::: checkAndAdjustSetTemperature() : Updated Set Temp: " + String(setTemp));
        LOG_INFO("5. : C.4.2 <2.09> ::: checkAndAdjustSetTemperature() : Temperature adjusted successfully.");

        // Trigger a screen update to reflect changes
        updateScreen(SET_TEMPERATURE);                                                      // Refresh screen with updated temperature
    } else {
        LOG_WARN("6. : C.4.2 <2.09> ::: checkAndAdjustSetTemperature() : No significant temperature change detected, skipping update.");
    }
}

/* ::: C.4.3 <2.09>: State Machine Wrapper ::: */

void switchToState(ScreenState newState) {
    static ScreenState currentState = MAIN_SCREEN_STATE;                    // Holds the current state

    // If the new state differs from the current one, transition
    if (currentState != newState) {
        currentState = newState;
        updateScreen(static_cast<Screen>(newState));                        // Convert to Screen enum for LCD update
        LOG_INFO("7. : C.4.3 <2.09> ::: switchToState() : Switched to state: " + String(currentState));
    }
}

/* ::: D <2.09>:    Function Prototypes :::
        D.1 <2.09>:  Main Operation Functions */

// D.1.1 <2.09>:     Void setup()

void setup() {
    Serial.begin(115200);
    LOG_INFO("8. : D.1.1 <2.09> ::: setup() : System Initialization...");
    initializeSystem();
}

// D1.1.2 <2.09>:    Void loop()

void loop() {
    handleUserInput();
    monitorTemperature();
    handleAutoScreenOff();

    long timeSinceLastActivity = millis() - lastActivityTime;
    if (timeSinceLastActivity > 10000 && currentScreen != MAIN_SCREEN) {
        updateScreen(MAIN_SCREEN);
        LOG_INFO("9. : D1.1.2 <2.09> ::: loop() : Returned to Main Screen due to inactivity.");
    }
}

/* ::: D2 <2.09>:        Supporting Operation Functions :::
        D2.1 <2.09>:      System Initialization */

// D2.1.1 <2.09>:    Initialize system loop()

void initializeSystem() {
    setupWiFi();
    timeClient.begin();
    initializeSensors();
    setDateAndTime();
    int initialPotValue = analogRead(POT_PIN);
    lastSetTemp = adjustTemperature(initialPotValue);   // Initialize with the actual first reading
    currentScreen = MAIN_SCREEN;                        // Ensure the currentScreen variable is set before updating the screen
    updateScreen(MAIN_SCREEN);                          // Use updateScreen to handle all display updates
    waitForSensorStabilization();
    allowTempAdjust = true;                             // Enable temperature adjustments after initial setup
    LOG_INFO("10. : D.2.1.1 <2.09> ::: initializeSystem() : System Initialization Complete. Main screen displayed.");
}

// D2.1.2 <2.09>:    AutoStabilization Routine

void waitForSensorStabilization() {
    int stableReadingsCount = 0;
    int lastReading = analogRead(POT_PIN);
    while (stableReadingsCount < 10) {                  // Require 10 stable readings
        int currentReading = analogRead(POT_PIN);
        if (abs(currentReading - lastReading) <= 2) {   // Adjust threshold based on expected noise level
            stableReadingsCount++;
        } else {
            stableReadingsCount = 0;                    // Reset if readings are not stable
        }
        lastReading = currentReading;
        delay(100);                                     // Short delay between readings
    }
    LOG_INFO("11. : D.2.1.2 <2.09> ::: waitForSensorStabilization() : Sensor stabilized.");
}

/* ::: E <2.09>:     Sensor and Communication Initialization :::
       E.1 <2.09>:   Sensor Setup and RF Communication */

// E.1.1 <2.09>:     Initialize sensors - Screen, RF, Local Temp

void initializeSensors() {
    Wire.begin(SDA_PIN, SCL_PIN);
    lcd.init();
    lcd.backlight();
    radio.begin();
    radio.openReadingPipe(1, rfAddress);
    radio.startListening();
    dht.begin();
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    LOG_INFO("12. : E.1.1 <2.09> ::: initializeSensors() : All sensors initialized successfully.");
}

// E.1.2 <2.09>:     Initialize Communication - Wifi

void setupWiFi() {
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    LOG_INFO("13. : E.1.2 <2.09> ::: setupWiFi() : Connecting to WiFi...");
}

// E.1.3 <2.09>:     Initialize - Set Date and Time

void setDateAndTime() {
    if (!timeClient.update()) {
        timeClient.forceUpdate();
    }
    setTime(timeClient.getEpochTime());
    LOG_INFO("14. : E.1.3 <2.09> ::: setDateAndTime() : Time updated to: " + timeClient.getFormattedTime());
}

/* ::: F <2.09>:     Display Functions :::
       F.1 <2.09>:   Screen Display Management */

// F.1.1 <2.09>: Screen 0 - Main Screen

void displayMainScreen() {
    lcd.clear();
    float roomTemp = dht.readTemperature(true);     // True for Fahrenheit
    char dateBuffer[20];
    snprintf(dateBuffer, sizeof(dateBuffer), "%02d/%02d/%02d %02d:%02d", month(), day(), year() % 100, hour(), minute());
    lcd.setCursor(0, 0);
    lcd.print(dateBuffer);
    char tempBuffer[20];
    snprintf(tempBuffer, sizeof(tempBuffer), "Room:%d Set:%d", int(roomTemp), int(setTemp));
    lcd.setCursor(0, 1);
    lcd.print(tempBuffer);
    LOG_VERBOSE("15. : F.1.1 <2.09> ::: displayMainScreen() : ** Main Screen Displayed **");
    LOG_VERBOSE("16. : F.1.1 <2.09> ::: displayMainScreen() : Room Temperature: " + String(roomTemp));
    LOG_VERBOSE("17. : F.1.1 <2.09> ::: displayMainScreen() : Set Temperature: " + String(setTemp));
}

// F.1.2 <2.09>: Screen 1 - Setting Temperature

void displaySetTemperatureScreen() {
    static int lastSetTemp = -1;
    static int lastCurrentTemp = -1;

    float currentTemp = dht.readTemperature(true);
    int setTempInt = int(setTemp);
    int currentTempInt = int(currentTemp);

    // Only clear the screen and print static information once
    if (lastSetTemp != setTempInt || lastCurrentTemp != currentTempInt) {
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("Set Temp: " + String(setTempInt) + "F");
        lcd.setCursor(0, 1);
        lcd.print("Current: " + String(currentTempInt) + "F");
        LOG_VERBOSE("18. : F.1.2 <2.09> ::: displaySetTemperatureScreen() : ** Set Temperature Screen Displayed **");
        LOG_VERBOSE("19. : F.1.2 <2.09> ::: displaySetTemperatureScreen() : Set Temperature: " + String(setTempInt));
        LOG_VERBOSE("20. : F.1.2 <2.09> ::: displaySetTemperatureScreen() : Current Room Temperature: " + String(currentTempInt));
    }

    lastSetTemp = setTempInt;
    lastCurrentTemp = currentTempInt;
}

// F.1.3 <2.09>: Screen 2 - Humidity & Relay Temp Screen

void displayHumidityTempScreen() {
    lcd.clear();
    float humidity = dht.readHumidity();
    float relayTemp = getRelayTemperature();
    lcd.setCursor(0, 0);
    lcd.print("Humidity: " + String(int(humidity)) + "%");
    lcd.setCursor(0, 1);
    lcd.print("Relay Temp: " + String(int(relayTemp)) + "F");
    LOG_VERBOSE("21. : F.1.3 <2.09> ::: displayHumidityTempScreen() : ** Humidity/Relay Temp Screen Displayed **");
    LOG_VERBOSE("22. : F.1.3 <2.09> ::: displayHumidityTempScreen() : Humidity: " + String(humidity));
    LOG_VERBOSE("23. : F.1.3 <2.09> ::: displayHumidityTempScreen() : Relay Temperature: " + String(relayTemp));
}

// F.1.4 <2.09>: Screen 3 - Edit Date & Time

void displayEditDateTimeScreen() {
    static String lastDisplay = "";  // Store the last displayed string
    String currentDisplay = "Edit Date/Time: ";
    // Append current date/time component value to `currentDisplay`
    currentDisplay += String(year()) + "-" + String(month()) + "-" + String(day()) + " " + String(hour()) + ":" + String(minute());

    if (currentDisplay != lastDisplay) {
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print(currentDisplay);
        lastDisplay = currentDisplay;  // Update the last displayed value
        LOG_VERBOSE("24. : F.1.4 <2.09> ::: displayEditDateTimeScreen() : ** Edit Date/Time Screen Displayed **");
        LOG_VERBOSE("25. : F.1.4 <2.09> ::: displayEditDateTimeScreen() : Current Display: " + currentDisplay);
    }
}

/* ::: G <2.09>:   Screen Logic Transitions :::
/* ::: G.1 <2.09>: LongPress Screen Transition */

void longPressAction() {
    switchToState(EDIT_DATE_TIME_STATE);                                                    // Switch directly to Screen 3 (Date/Time editing)
    LOG_INFO("26. : G.1 <2.09> ::: longPressAction() : Activated Edit Date/Time Screen via Long Press");
}

/* ::: G.2 <2.09>: ShortPress Screen Transition */

void shortPressAction() {
    static bool backlightActivatedOnly = false;

    if (backlightActivatedOnly) {
        backlightActivatedOnly = false;                                                     // Reset backlight-only state
        return;                                                                             // Do not change screens
    }

    // Switch screens according to the current state
    if (currentScreen == MAIN_SCREEN) {
        switchToState(HUMIDITY_TEMP_STATE);                                                 // Go to Screen 2
    } else if (currentScreen == HUMIDITY_TEMP_SCREEN) {
        switchToState(MAIN_SCREEN_STATE);                                                   // Go back to Screen 0
    } else if (currentScreen == EDIT_DATE_TIME) {
        cycleDateTimeComponents();                                                          // Cycle through the date/time components
    } else if (currentScreen == SET_TEMPERATURE) {
        switchToState(MAIN_SCREEN_STATE);                                                   // Return to Main Screen
    }

    LOG_INFO("27. : G.2 <2.09> ::: shortPressAction() : Performed Screen Transition via Short Press");
}

// G.2.1 <2.09>: Switch Screen

void switchScreens() {
    if (currentScreen == SET_TEMPERATURE || currentScreen == EDIT_DATE_TIME) return;
    Screen nextScreen = (currentScreen == MAIN_SCREEN) ? HUMIDITY_TEMP_SCREEN : MAIN_SCREEN;
    switchToState(static_cast<ScreenState>(nextScreen));  // Convert to ScreenState enum
    LOG_INFO("28. : G.2.1 <2.09> ::: switchScreens() : Switched Screens via Button Press");
}

// G.3 <2.09>: Activate Backlight

void activateBacklight() {
    if (!backlightOn) {
        lcd.backlight();
        backlightOn = true;
        LOG_INFO("29. : G.3 <2.09> ::: activateBacklight() : Backlight Activated");
    }
    lastActivityTime = millis();            // Reset activity timer whenever the backlight is activated
}

// G.4 <2.09>: Turn Off Backlight

void handleAutoScreenOff() {
    if (millis() - lastActivityTime > 30000 && backlightOn) {
        lcd.noBacklight();
        backlightOn = false;
        LOG_INFO("30. : G.4 <2.09> ::: handleAutoScreenOff() : Backlight Turned Off");
    }
}

/* ::: H <2.09>: Action Handlers :::
       H1 <2.09>: User Input and Response */

/* ::: H1.1 <2.09>: Handle User Input ::: */

void handleUserInput() {
    static unsigned long lastButtonPressTime = 0;
    static bool buttonPressed = false;
    static int lastPotValue = 0;                                                            // Store the last potentiometer value
    static bool backlightActivatedOnly = false;                                             // Track backlight-only activation
    int buttonState = digitalRead(BUTTON_PIN);

    // Backlight-only Activation
    if (!backlightOn && buttonState == LOW) {
        activateBacklight();                                                                // Turn on the backlight only
        buttonPressed = false;                                                              // Reset button press state
        backlightActivatedOnly = true;                                                      // Mark backlight-only activation

        // Log that the backlight was activated
        LOG_INFO("31. : H1.1 <2.09> ::: handleUserInput() : Backlight activated via button press.");

        // Temporary feedback (e.g., "Backlight ON")
        //lcd.clear();
        lcd.setCursor(0, 1);
        lcd.print("Backlight ON");
        delay(500);                                                                         // Show message briefly for 500ms

        // Restore the current screen state
        updateScreen(currentScreen);
        return;                                                                             // Avoid screen changes
    }

    // Read the current potentiometer value
    int currentPotValue = analogRead(POT_PIN);
    logIfChanged("Potenciometriny:", currentPotValue, previousPotValue, 10);                  // Log if the value changes by 5 or more

    // Button Handling
    if (buttonState == LOW) {
        if (!buttonPressed) {
            buttonPressed = true;
            lastButtonPressTime = millis();
            activateBacklight();                                                            // Ensure backlight is activated on button press
            backlightActivatedOnly = false;                                                 // Clear the backlight-only flag
            LOG_VERBOSE("32. : H1.1 <2.09> ::: handleUserInput() : Button pressed. Backlight activated.");
        }
    } else if (buttonPressed) {
        unsigned long pressDuration = millis() - lastButtonPressTime;
        LOG_VERBOSE("33. : H1.1 <2.09> ::: handleUserInput() : Button Press Duration: " + String(pressDuration) + " ms");

        if (pressDuration >= 3000) {                                                        // Long Press
            longPressAction();
            LOG_INFO("34. : H1.1 <2.09> ::: handleUserInput() : Long button press detected. Transition to Edit Date/Time screen.");
        } else if (pressDuration > 50) {                                                    // Short Press with debouncing
            shortPressAction();
            LOG_INFO("35. : H1.1 <2.09> ::: handleUserInput() : Short button press detected. Switching screens.");
        }
        buttonPressed = false;                                                              // Reset button press state
    }

    // Knob Handling
    if (abs(currentPotValue - lastPotValue) > 10) {                                         // Adjust threshold to avoid noise
        lastPotValue = currentPotValue;
        LOG_VERBOSE("36. : H1.1 <2.09> ::: handleUserInput() : Significant Potentiometer Movement Detected. New Value: " + String(currentPotValue));

        // Switch to the appropriate screen based on current state
        if (currentScreen != EDIT_DATE_TIME) {
            updateScreen(SET_TEMPERATURE);                                                  // Change directly to Screen 1 (Set Temp)
            LOG_INFO("37. : H1.1 <2.09> ::: handleUserInput() : Screen switched to Set Temperature due to knob movement.");
        }

        // Reduce update frequency
        static unsigned long lastUpdate = 0;
        if (millis() - lastUpdate > 50) {                                                   // Update every 50 ms
            lastUpdate = millis();
            if (currentScreen == SET_TEMPERATURE || currentScreen == EDIT_DATE_TIME) {
                lastActivityTime = millis();                                                // Reset inactivity timer
                handleKnobMovement(currentPotValue);                                        // Apply movement logic
                LOG_VERBOSE("38. : H1.2 <2.09> ::: handleKnobMovement() : Knob movement handled. Adjusting settings accordingly.");
            }
        }
    }
}


/* ::: H1.2 <2.09>: Handle Knob movements ::: */

void handleKnobMovement(int potValue) {
    activateBacklight();                                                                    // Ensure backlight is activated on knob movement

    if (currentScreen == SET_TEMPERATURE) {
        checkAndAdjustSetTemperature(potValue);
        updateScreen(SET_TEMPERATURE);                                                      // Immediate update via ScreenState
    } else if (currentScreen == EDIT_DATE_TIME) {
        handleDateTimeEdit(potValue);
        updateScreen(EDIT_DATE_TIME);                                                       // Immediate update via ScreenState
    }
    LOG_VERBOSE("39. : H1.2 <2.09> ::: handleKnobMovement() : Knob movement handled.");
}

/* :::  H1.3 <2.09>: Handle Date&Time Edit Logic ::: */

void handleDateTimeEdit(int potValue) {
    static int lastAdjustedValue = -1;                                                      // Initialize with an impossible value
    int adjustedValue = adjustDateTime(potValue);
    if (lastAdjustedValue != adjustedValue) {
        // Apply adjustments based on the current component being edited
        tmElements_t tm;
        breakTime(now(), tm);                                                               // Get the current time components

        switch (currentDateTimeComponent) {
            case MONTH:
                tm.Month = constrain(adjustedValue, 1, 12);
                break;
            case DAY:
                tm.Day = constrain(adjustedValue, 1, 31);
                break;
            case YEAR:
                tm.Year = CalendarYrToTm(adjustedValue);                                    // Ensure 'adjustedValue' is the full year (e.g., 2024)
                break;
            case HOUR:
                tm.Hour = constrain(adjustedValue, 0, 23);
                break;
            case MINUTE:
                tm.Minute = constrain(adjustedValue, 0, 59);
                break;
        }

        time_t newTime = makeTime(tm);
        setTime(newTime);
        displayEditDateTimeScreen();                                                        // Update the screen with new values
        lastAdjustedValue = adjustedValue;
    }
    LOG_INFO("40. : H1.3 <2.09> ::: handleDateTimeEdit() : Date & Time adjustment handled.");
}

/* :::  H1.4 <2.09>: Handle Cycling through Date/Time adjustments ::: */

void cycleDateTimeComponents() {
    currentDateTimeComponent = static_cast<DateTimeComponent>((currentDateTimeComponent + 1) % 5);
    displayEditDateTimeScreen();                                                            // Refresh display to indicate the new component being adjusted
    LOG_INFO("41. : H1.4 <2.09> ::: cycleDateTimeComponents() : Date/Time component cycled.");
}

/* :::  I <2.09>:    Temperature Monitoring :::
        I.1 <2.09>:  Monitor and Control Temperature */

void monitorTemperature() {
    float roomTemp = dht.readTemperature(true);
    float relayTemp = getRelayTemperature();
    String dateTime = String(year()) + "/" + String(month()) + "/" + String(day()) + " " + String(hour()) + ":" + String(minute()) + ":" + String(second());
    if (roomTemp < setTemp and !heaterOn) {
        sendRFSignal("HEATER_ON", setTemp, dateTime);
        heaterOn = true;
        lcd.setCursor(0, 1);
        lcd.print("Heater ON          ");
        LOG_INFO("42a. : I.1 <2.09> ::: monitorTemperature() : Room Temp < SetTEMP --> Heater_ON");
    } else if (roomTemp > setTemp + 1 and heaterOn) {
        sendRFSignal("HEATER_OFF", setTemp, dateTime);
        heaterOn = false;
        lcd.setCursor(0, 1);
        lcd.print("Heater OFF         ");
        LOG_INFO("42b. : I.1 <2.09> ::: monitorTemperature() : Room Temp > SetTEMP --> Heater_OFF");

    }
    if (relayTemp >= 90.0) {
        digitalWrite(BUZZER_PIN, HIGH);
        lcd.setCursor(0, 1);
        lcd.print("ALARM: Temp High!  ");
        delay(1000);                                                                        // Buzzer on for 1 second
        digitalWrite(BUZZER_PIN, LOW);
        LOG_ERROR("42c. : I.1 <2.09> ::: monitorTemperature() : High Temperature Alarm Triggered.");
    } else {
        // Cast setTemp and roomTemp to integers
        int setTempInt = static_cast<int>(setTemp);
        int roomTempInt = static_cast<int>(roomTemp);

        // Pass the integer values to the logIfChanged function
        logIfChanged("I.1 <2.09> ::: monitorTemperature() No Heater ON or Heater OFF action", setTempInt, roomTempInt, 10);
    }
}

/* :::  J <2.09>:    RF Communication :::
/* :::  J.1 <2.09>:  Handle RF Messages ::: */

void sendRFSignal(String command, float temp, String dateTime) {
    char buffer[100];
    snprintf(buffer, sizeof(buffer), "%s:%.1f:%s", command.c_str(), temp, dateTime.c_str());
    radio.stopListening();                                                                  // Stop listening before sending
    radio.write(&buffer, sizeof(buffer));                                                   // Send the data to Nano
    radio.startListening();                                                                 // Use the dot to call the startListening method
    LOG_INFO("44. : J.1 <2.09> ::: sendRFSignal() : RF Command Sent: " + String(buffer));
}

// J.1.2 <2.09>:  Relay Temperature

float getRelayTemperature() {
    return 85.0;                                                                            // This will be fetched from the RF module in reality
}
