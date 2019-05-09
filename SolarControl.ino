#include <OneWire.h>
#include <DallasTemperature.h>

// Data wire is plugged into port 10 on the Arduino
#define RELAY_PIN 10
#define ONE_WIRE_BUS 11
#define TEMPERATURE_PRECISION 9

// Setup a oneWire instance to communicate with any OneWire devices (not just Maxim/Dallas temperature ICs)
OneWire oneWire(ONE_WIRE_BUS);

// Pass our oneWire reference to Dallas Temperature. 
DallasTemperature sensors(&oneWire);

// Thermocouple attached to pipe coming out of tank (going to pump and then to panel) (white)
DeviceAddress coldThermometer = { 0x3B, 0xBB, 0xB7, 0x58, 0x09, 0xFC, 0x2C, 0x5D };
// Thermocouple attached to pipe retnuning from panel (yellow)
DeviceAddress hotThermometer = { 0x3B, 0xA8, 0x09, 0x59, 0x09, 0xFC, 0x6C, 0x7B };
// Thermocouple in tank (green)
DeviceAddress tankThermometer = { 0x3B, 0x6C, 0xB6, 0x58, 0x09, 0xFC, 0x2C, 0xD1 };
// Thermocouple in panel (red)
DeviceAddress panelThermometer = { 0x3B, 0x6C, 0xCE, 0x58, 0x09, 0xFC, 0x2C, 0x82 };

// Define this to manually test system
#define TEST_SYSTEM

// If the tank becomes too hot the liner breaks. In addition the maximum operating temperature
// for the pump is 79 degrees and the minimum inlet pressure increases as temp increases:
// 3 ft of wtaer column at 60 degrees and 9 ft at 88 degrees (we have 4 ft)
#ifdef TEST_SYSTEM
  #define MAX_TANK_TEMPERATURE 32
#else
  #define MAX_TANK_TEMPERATURE 60
#endif

// Turn pump on if panel is at least this much warmer than tank. If temp diff is too
// small tis likely means that there isn't much sun and that it's not wort turning on the pump.
// If there is sun the panels get pretty hot.
#ifdef TEST_SYSTEM
  #define MIN_PANEL_TEMP_DIFF 5
#else
  #define MIN_PANEL_TEMP_DIFF 30
#endif

// Start with pump turned off
int currentPumpState = LOW;

// Last time that relay was switched (since start of controller in millis)
unsigned long lastSwitchMillis;
// Last time the pump was on
unsigned long lastPumpOnMillis;
// Wait at least a minute between switches of the relay.
// We don't want to switch the relay too often since every switch means that
// the pump gets turned on or off. Cycling the pump is not great.
#ifdef TEST_SYSTEM
  #define MIN_MS_BETWEEN_SWITCHES (5*1000)
#else
  #define MIN_MS_BETWEEN_SWITCHES (60*1000)
#endif

// We keep pump off for at most one hour. We want to cycle at least every hour to get
// more accurate temperature readings from tank and panels. If we move water we get better
// data than from the sensors that just monitor one spot in the tank and one in the panel.
#ifdef TEST_SYSTEM
  #define MAX_PUMP_OFF_TIME (30*1000)
#else
  #define MAX_PUMP_OFF_TIME (60*60*1000)
#endif

void setup(void)
{
  // start serial port
  Serial.begin(9600);

  // Prepare relay pin
  pinMode(RELAY_PIN, OUTPUT);

  // set the resolution to 9 bit
  sensors.setResolution(panelThermometer, TEMPERATURE_PRECISION);
  sensors.setResolution(coldThermometer, TEMPERATURE_PRECISION);
  sensors.setResolution(tankThermometer, TEMPERATURE_PRECISION);
  sensors.setResolution(hotThermometer, TEMPERATURE_PRECISION);

  lastSwitchMillis = millis();
  lastPumpOnMillis = 0;
}

// Check temperature for valid range
bool isValidTemperature(float temp) {
  // The panels can get very hot, hence the large range. Water
  // can only go to slightly above 100 of course...
  return temp > -10 && temp < 400;
}

// function to print a device address
void printAddress(DeviceAddress deviceAddress)
{
  for (uint8_t i = 0; i < 8; i++)
  {
    // zero pad the address if necessary
    if (deviceAddress[i] < 16) Serial.print("0");
    Serial.print(deviceAddress[i], HEX);
  }
}

// function to print the temperature for a device
void printTemperature(DeviceAddress deviceAddress)
{
  float tempC = sensors.getTempC(deviceAddress);
  if (isValidTemperature(tempC)) {
    Serial.print(tempC);
  } else {
    Serial.print("invalid");
  }
}

// main function to print information about a device
void printData(DeviceAddress deviceAddress, const char *name)
{
  Serial.print(name);
  Serial.print(": ");
  printTemperature(deviceAddress);
  Serial.print("; ");
}


// Turns pump on or off depending on temperature parameters
void controlPump(void) {
  int desiredPumpState = currentPumpState;
  float tempCold = sensors.getTempC(coldThermometer);
  float tempHot = sensors.getTempC(hotThermometer);
  float tempTank = sensors.getTempC(tankThermometer);
  float tempPanel = sensors.getTempC(panelThermometer);

  if (millis() - lastPumpOnMillis > MAX_PUMP_OFF_TIME) {
    Serial.println("-- Turning pump ON, too long since last cycle");
    desiredPumpState = HIGH;
  } else if (currentPumpState == HIGH) {
    // If pump is on we can use temp readings from inlet and return that
    // should be more precise.

    // If hot thermocouple is broken we use panel temp
    float tempHotWithAlt = isValidTemperature(tempHot) ? tempHot : tempPanel;

    // Turn pump off if:
    // - cold therocouple is broken (we should just turn pump off since otherwise we risk
    // overheating tank liner and pump and breaking them)
    // - return thermacouple and panel thermacouple are broken (no way of telling
    // if we're actually heating water)
    // - tank gets too hot
    // - return water is colder than panel input (no sun).
    if (!isValidTemperature(tempCold)) {
      desiredPumpState = LOW;
      Serial.println("-- Turning pump OFF, invalid cold temp");
    } else if (!isValidTemperature(tempHot) && !isValidTemperature(tempPanel)) {
      desiredPumpState = LOW;
      Serial.println("-- Turning pump OFF, invalid hot and panel temp");
    } else if (tempCold > MAX_TANK_TEMPERATURE) {
      desiredPumpState = LOW;
      Serial.println("-- Turning pump OFF, tank too hot");
    } else if (tempCold > tempHot) {
      desiredPumpState = LOW;
      Serial.println("-- Turning pump OFF, panel return water colder than panel input");
    }
  } else {
    // If pump is off we need to use readings from tank/panel.

    // Only turn pump off if we have temperature data from panel and tank and if tank temp less than maximum.
    if (isValidTemperature(tempPanel) && isValidTemperature(tempTank) && tempTank < MAX_TANK_TEMPERATURE) {
      if (tempPanel - tempTank > MIN_PANEL_TEMP_DIFF) {
        desiredPumpState = HIGH;
        Serial.println("-- Turning pump ON, panels are hot");
      }
    }
  }

  if (desiredPumpState != currentPumpState
      && millis() - lastSwitchMillis > MIN_MS_BETWEEN_SWITCHES) {
    // Switch relay if required
    currentPumpState = desiredPumpState;
    digitalWrite(RELAY_PIN, currentPumpState);
    lastSwitchMillis = millis();
  }
  if (currentPumpState == HIGH) {
    lastPumpOnMillis = millis();
  }
}

void printSystemState(void) {
  if (currentPumpState == HIGH) {
    Serial.print("Pump is ON. ");
  } else {
    Serial.print("Pump is OFF. ");
  }
  Serial.print("Seconds since last pump state change: ");
  Serial.print((millis() - lastSwitchMillis) / 1000);
  Serial.print(". Seconds since last pump on: ");
  Serial.println((millis() - lastPumpOnMillis) / 1000);
}

void loop(void)
{ 
  sensors.requestTemperatures();
  Serial.println("");

  // print the device information
  printData(coldThermometer, "Cold");
  printData(hotThermometer, "Hot");
  printData(tankThermometer, "Tank");
  printData(panelThermometer, "Panel");
  Serial.println("");
  controlPump();
  printSystemState();
  delay(5000);
}
