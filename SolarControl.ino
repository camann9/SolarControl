#include <OneWire.h>
#include <DallasTemperature.h>

#define RELAY_PIN 10
#define ONE_WIRE_BUS 6
#define PUMP_WATER_PIN 5
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
DeviceAddress panelThermometer = { 0x3B, 0x70, 0xD1, 0x58, 0x09, 0xFC, 0x4C, 0xD2 };

// Define this to manually test system
//#define TEST_SYSTEM

// If the tank becomes too hot the liner breaks. In addition the maximum operating temperature
// for the pump is 79 degrees and the minimum inlet pressure increases as temp increases:
// 3 ft of water column at 60 degrees and 9 ft at 88 degrees (we have 4 ft)
#ifdef TEST_SYSTEM
  #define MAX_TANK_TEMPERATURE 32
#else
  #define MAX_TANK_TEMPERATURE 60
#endif

// Turn pump on if panel is at least this much warmer than tank. If temp diff is too
// small this likely means that there isn't much sun and that it's not worth turning on the pump.
// If there is sun the panels get pretty hot.
#ifdef TEST_SYSTEM
  #define MIN_PANEL_TEMP_DIFF 5
#else
  #define MIN_PANEL_TEMP_DIFF 10
#endif

// Turn pump off if we're not gaining much heat from having it on. no need to waste energy if heat gain is minimal.
#ifdef TEST_SYSTEM
  #define MIN_HOT_COLD_TEMP_DIFF 0
#else
  #define MIN_HOT_COLD_TEMP_DIFF 1
#endif

// Start with pump turned on
int currentPumpState = HIGH;

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

// Time that we last requested temperatures and controlled the pump. We don't
// want to call control to often.
unsigned long lastControlCycleMillis;
#define MS_BETWEEN_CONTROL_CYCLES 2000

// We keep pump off for at most 20 minutes. We want to cycle at least every 20 minutes to get
// more accurate temperature readings from tank and panels. If we move water we get better
// data than from the sensors that if we just monitor one spot in the tank and one in the panel
// with sensors.
#ifdef TEST_SYSTEM
  #define MAX_MS_BETWEEN_TEMP_CHECK (30*1000)
#else
  #define MAX_MS_BETWEEN_TEMP_CHECK (30*60*1000)
#endif
// Temperature check only needs to run for 30 seconds. We will get
// enough hot water to temp sensor in 30 seconds if panels are actually hot
// to keep the system running.
#ifdef TEST_SYSTEM
  #define MIN_MS_TEMP_CHECK (5*1000)
#else
  #define MIN_MS_TEMP_CHECK (30*1000)
#endif
bool runningTempCheck;

#define SERIAL_BUFFER_SIZE 128
String serialBuffer;
bool inputReady = false;

void setup(void)
{
  // start serial port
  Serial.begin(9600);

  // Prepare relay pin
  pinMode(RELAY_PIN, OUTPUT);
  // The pump water level check has a pullup. When there is no water in the
  // pump housing (desired state) the state of the pin will be HIGH.
  // If there is water, the pin will be pulled to GND via the other wire.
  pinMode(PUMP_WATER_PIN, INPUT_PULLUP);

  // set the resolution to 9 bit
  sensors.setResolution(panelThermometer, TEMPERATURE_PRECISION);
  sensors.setResolution(coldThermometer, TEMPERATURE_PRECISION);
  sensors.setResolution(tankThermometer, TEMPERATURE_PRECISION);
  sensors.setResolution(hotThermometer, TEMPERATURE_PRECISION);

  lastSwitchMillis = millis();
  lastPumpOnMillis = millis();
  lastControlCycleMillis = millis();
  // Start system in temp check mode
  runningTempCheck  = true;
}

// Check temperature for valid range
bool isValidTemperature(float temp) {
  // The panels can get very hot, hence the large range. Water
  // can only go to slightly above 100 of course...
  return temp > -10 && temp < 400;
}

// function to print the temperature for a device
void printTemperature(DeviceAddress deviceAddress)
{
  float tempC = sensors.getTempC(deviceAddress);
  if (isValidTemperature(tempC)) {
    output(String(tempC));
  } else {
    output("invalid");
  }
}

// main function to print information about a device
void printData(DeviceAddress deviceAddress, const char *name)
{
  output(name);
  output(": ");
  printTemperature(deviceAddress);
  output("; ");
}


// Turns pump on or off depending on temperature parameters
void controlPump(void) {
  int desiredPumpState = currentPumpState;
  float tempCold = sensors.getTempC(coldThermometer);
  float tempHot = sensors.getTempC(hotThermometer);
  float tempTank = sensors.getTempC(tankThermometer);
  float tempPanel = sensors.getTempC(panelThermometer);
  // Emergency shutoff if there is water in pump housing. Because this pin
  // uses the internal pullup it is actually connected on LOW.
  bool emergencyShutoffTriggered = (digitalRead(PUMP_WATER_PIN) == LOW);

  if (millis() - lastPumpOnMillis > MAX_MS_BETWEEN_TEMP_CHECK
      && (!isValidTemperature(tempTank) || !isValidTemperature(tempPanel) || tempPanel + 10 > tempTank)) {
    // We run temperature check only of panel temp is at least somewhere close to the tank
    // tmeperature. If the panel temp is slightly lower than tank temp that's fine, might
    // be incorrect data. But if it's much lower we don't even need to try.
    outputln("-- Turning pump ON, too long since last cycle");
    desiredPumpState = HIGH;
    runningTempCheck = true;
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
      outputln("-- Turning pump OFF, invalid cold temp");
    } else if (!isValidTemperature(tempHot) && !isValidTemperature(tempPanel)) {
      desiredPumpState = LOW;
      outputln("-- Turning pump OFF, invalid hot and panel temp");
    } else if (tempCold > MAX_TANK_TEMPERATURE) {
      desiredPumpState = LOW;
      outputln("-- Turning pump OFF, tank too hot");
    } else if (tempHot - tempCold < MIN_HOT_COLD_TEMP_DIFF) {
      desiredPumpState = LOW;
      outputln("-- Turning pump OFF, panel return water not much warmer (or colder) than panel input");
    }
  } else {
    // If pump is off we need to use readings from tank/panel.

    // Only turn pump off if we have temperature data from panel and tank and if tank temp less than maximum.
    if (isValidTemperature(tempPanel) && isValidTemperature(tempTank) && tempTank < MAX_TANK_TEMPERATURE) {
      if (tempPanel - tempTank > MIN_PANEL_TEMP_DIFF) {
        desiredPumpState = HIGH;
        outputln("-- Turning pump ON, panels are hot");
      }
    }
  }

  if (emergencyShutoffTriggered) {
    output("WATER IN PUMP HOUSING\n");
    desiredPumpState = LOW;
  }

  if (desiredPumpState != currentPumpState) {
    bool allowStateChange = false;
    if (emergencyShutoffTriggered) {
      // Allow quick shutoff if emergency shutoff triggered.
      allowStateChange = true;
    } else if (millis() - lastSwitchMillis > MIN_MS_TEMP_CHECK
        && runningTempCheck
        && desiredPumpState == LOW) {
      // Allow quick shutoff if we're currently in temp check mode. We don't
      // want to pump a lot of warm water through the panels if it's cold
      // outside.
      allowStateChange = true;
    } else if (millis() - lastSwitchMillis > MIN_MS_BETWEEN_SWITCHES) {
      // If none of the special conditions are triggered, respect wait time between
      // relay switches.
      allowStateChange = true;
    } else {
      outputln("-- Pump state change requested but too short since last state change");
    }
    if (allowStateChange) {
      currentPumpState = desiredPumpState;
      lastSwitchMillis = millis();
    }
  }
  if (currentPumpState == HIGH) {
    lastPumpOnMillis = millis();
  } else {
    // Turning the pump off ends temp check. If we restart the pump due to another
    // temp check the  this will become true again. If we restart the pump due to
    // hot panels the cycle will not be marked as temp check and shutoff time will
    // be longer to prevent frequent cycling.
    runningTempCheck = false;
  }
  digitalWrite(RELAY_PIN, currentPumpState);
}

void printSystemState(void) {
  if (currentPumpState == HIGH) {
    output("Pump is ON. ");
  } else {
    output("Pump is OFF. ");
  }
  output("Last pump state change: T - ");
  output(String((millis() - lastSwitchMillis) / 1000));
  output("s. Last pump on: T - ");
  output(String((millis() - lastPumpOnMillis) / 1000));
  output("s. ");
  
  if (currentPumpState == HIGH) {
    if (runningTempCheck) {
      output("Trigger: temp check, ");
      if (millis() - lastSwitchMillis > MIN_MS_TEMP_CHECK) {
        outputln(", could turn off.");
      } else {
        output(String((MIN_MS_TEMP_CHECK - (millis() - lastSwitchMillis)) / 1000));
        outputln("s remaining.");
      }
    } else {
      output("NO temp check.");
    }
  } else {
    outputln("\n");
  }
}

void loop(void)
{ 
  if (millis() - lastControlCycleMillis > MS_BETWEEN_CONTROL_CYCLES) {
    lastControlCycleMillis = millis();
    sensors.requestTemperatures();
    outputln("");
  
    // print the device information
    printData(coldThermometer, "Cold");
    printData(hotThermometer, "Hot");
    printData(tankThermometer, "Tank");
    printData(panelThermometer, "Panel");
    outputln("");
    controlPump();
    printSystemState();
  }
  handleInput();
}

void output(String out) {
  Serial.print(out);
}

void outputln(String out) {
  output(out);
  output("\n");
}

void receiveSerialCharacters() {
  while (Serial.available()) {
    char inChar = Serial.read();
    if (inChar == '\n') {
      inputReady = true;
    } else if (serialBuffer.length() < SERIAL_BUFFER_SIZE && !inputReady) {
      // Only accept input if buffer is not overflowing and if we're not currently processing
      serialBuffer += inChar;
    }
  }
}

void handleInput() {
  receiveSerialCharacters();
  if (inputReady) {
    String inputToHandle = serialBuffer;
    serialBuffer = "";
    inputReady = false;

    if (inputToHandle == "p" || inputToHandle == "ping") {
      Serial.println("pong");
    }
  }
}
