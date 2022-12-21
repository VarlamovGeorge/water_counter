#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <ESP8266WebServer.h>
#include <EEPROM.h>
#include <AsyncMqttClient.h>

// MQTT settings:
#define MQTTServerIP 192, 168, 1, 6
AsyncMqttClient mqttClient;

#define WATER_CODE 1 // 1 - cold, 2 - hot

#if WATER_CODE == 1
#define MQTT_WATER_TOPIC            "WATER_COLD"
#define MQTT_SUBSCR_TOPIC           "WATER_COLD/CMD"
#define MQTT_FEEDBACK_TOPIC         "WATER_COLD/FEEDBACK"
#define MQTT_WATER_LASTWILL_TOPIC   "WATER_COLD/LASTWILL"

#else
#define MQTT_WATER_TOPIC            "WATER_HOT"
#define MQTT_SUBSCR_TOPIC           "WATER_HOT/CMD"
#define MQTT_FEEDBACK_TOPIC         "WATER_HOT/FEEDBACK"
#define MQTT_WATER_LASTWILL_TOPIC   "WATER_HOT/LASTWILL"
#endif

/*
EEPROM scheme:
|--------0-31-------|--------32-63------|------64-67------|
|SSID for home Wi-Fi|PASS for home Wi-Fi|Water meter value|
*/

// Home WiFi settings:
String ssid = "";                   // Home Wi-Fi SSID variable
String password = "";               // Home Wi-Fi PASS variable
const char *ssidAP = "WaterCounter";// Wi-Fi SSID for ESP access point
const char *passwordAP = "";        // Wi-Fi PASS for ESP access point
const int WiFiTimeoutSec = 60;      // Wi-Fi connection timeout (seconds)
const int MQTTTimeoutSec = 20;      // MQTT connection timeout (seconds)
ESP8266WebServer server(80);
WiFiClient  client;

// Deep sleep duration (seconds):
const int sleepTimeS = 5;
// MQTT publication frequency (seconds):
#define SENDSEC 1800 // 1800 seconds = half an hour, 86400 seconds = 24 hours
// EEPROM rewrite frequency (seconds):
#define EEWRITESEC 120960 // 120960 seconds = 14 days

// Pin assignment:
#define BUTTON_PIN 4     // GPIO4 for change-mode button
#define WATER_PIN 12 // GPIO12 for water meter

// RTC memory addresses (persistently stored while deep sleep):
#define COUNT_STEP_ADDR 64      // Steps count for measure how much time has passed
#define PREV_WATER_STATE_ADDR 65 // Previous step water meter state (close or open)
#define WATER_VAL_ADDR 67       // Actual water meter indication (1 block of 4 bytes)
#define WATER_OLD_VAL_ADDR 69   // Previous water meter indication (1 block of 4 bytes)

// ADC_MODE(ADC_VCC); // Use if you want to use batteries

// Other variables:
boolean loadState = true; // Boot load mode: true - main (count) mode; false - Wi-Fi AP mode for configuring)
int countStep;

// Additional RTC intended library:
extern "C" {
  #include "user_interface.h"
}

// User struct to store water meters indications (2 blocks of 4 bytes):
typedef struct {
  long waterVal;
} rtcStore;

rtcStore rtcMem; // Struct to store current measurements
rtcStore rtcMemPrev; // Struct to store current previous loaded into RTC measurements

// Auxiliary arrays:
int waterByte[4];

//---------------------------------------------------------------------------------------------
//---------------------------------MQTT handlers:----------------------------------------------
void onMqttConnect(bool sessionPresent) { // MQTT connect handler
  Serial.println("Connected to the MQTT broker");
  mqttClient.subscribe(MQTT_SUBSCR_TOPIC, 1); // Subscribtion on topic with adjusting values from user
  Serial.print("Subscription topic: ");
  Serial.println(MQTT_SUBSCR_TOPIC);
}

void onMqttDisconnect(AsyncMqttClientDisconnectReason reason) {// MQTT disconnect handler
  Serial.println("Disconnected from the broker");
  // Serial.println("Reconnecting to MQTT broker...");
  // mqttClient.connect();
}

void onMqttSubscribe(uint16_t packetId, uint8_t qos) {// MQTT subscription handler
  Serial.print("Subscribe acknowledged");
  Serial.print(" packetId: ");
  Serial.print(packetId);
  Serial.print(" qos: ");
  Serial.println(qos);
}
/*
void onMqttUnsubscribe(uint16_t packetId) {// MQTT unsubscription handler
  Serial.println("** Unsubscribe acknowledged **");
  Serial.print("  packetId: ");
  Serial.println(packetId);
}
*/

// MQTT get message handler:
void onMqttMessage(char* topic, char* payload, AsyncMqttClientMessageProperties properties, size_t len, size_t index, size_t total) {
  String feedbackMessage = "";
  Serial.println("MATT message received");
  Serial.println("topic: "+String(topic)+"; qos: "+String(properties.qos)+"; dup: "+String(properties.dup)+"; retain: "+String(properties.retain)+"; len: "+String(len)+"; index: "+String(index));
  Serial.println("Payload: "+String(payload));

  int waterByteVals[4];
  int cmd_water_type = String(payload).substring(0, String(payload).indexOf("|")).toInt(); // Water type placed before first "|" (1 - cold, 2 - hot)
  String cmd_water_commmand = String(payload).substring(2, String(payload).indexOf("|", 3)); // Command type placed before second "|"
  String cmd_water_value = String(payload).substring(3+cmd_water_commmand.length(), String(payload).length()); // New water meter value from user
  Serial.println("Received water: "+String(cmd_water_type));
  Serial.println("Received comand: "+String(cmd_water_commmand));
  Serial.println("Received value: "+String(cmd_water_value));
  if (cmd_water_type==WATER_CODE){ // Check if command assumes correct water meter type
    if (cmd_water_commmand=="set"){
      rtcMem.waterVal = cmd_water_value.toInt();
      feedbackMessage = "Current water meter value updated: " + String(rtcMem.waterVal);
    }
    else {
      feedbackMessage = "Wrong command received. List of allowed commands: ['set']";
    }
  }
  
  else {
    feedbackMessage = "Wrong command received: " + String(payload) + " 'N|CMD|VALUE' message expected...";
  }
  Serial.println(feedbackMessage);
  mqttClient.publish(MQTT_FEEDBACK_TOPIC, 1, false, feedbackMessage.c_str());
  /*if (strcmp(topic, MQTT_SUBSCR_TOPIC) == 0)  {
    //setRelay(payload);
    mqttClient.publish(MQTT_FEEDBACK_TOPIC, 1, false, payload);
    Serial.println("Publishing Feedback");
  }*/
}

void onMqttPublish(uint16_t packetId) {// MQTT publish handler
  Serial.print("Published");
  Serial.print("  packetId: ");
  Serial.print(packetId);
  Serial.print("\n\n");
}
//---------------------------------------------------------------------------------------------
//---------------------------------------------------------------------------------------------


void setup()
{
  Serial.begin(115200);
    EEPROM.begin(512);
  // Seting all pins as inputs:
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(WATER_PIN, INPUT_PULLUP);
  // Getting operating mode:
  loadState = digitalRead(BUTTON_PIN); // Pressed = volage low level
  Serial.println("\nIs MODE button pressed? " + String(!loadState));
  WiFi.mode(WIFI_AP_STA);// ESP in "station + access point" mode

  //----------MAIN (COUNTER) MODE-------------------------------------------------------------
  if (loadState) { // Button released
    Serial.println("\nESP8266 in MAIN mode!");
    // Readig previous measures from RTC and storing them into rtcMem variable:
    system_rtc_mem_read(WATER_VAL_ADDR, &rtcMem, 4);
    system_rtc_mem_read(WATER_OLD_VAL_ADDR, &rtcMemPrev, 4);
    system_rtc_mem_read(COUNT_STEP_ADDR, &countStep, 4);
    // If step number bigger then (EEWRITESEC/sleepTimeS) or less then 0 - resetting step number:
    if (countStep > (EEWRITESEC / sleepTimeS) || countStep < 0) {
      countStep = 0;
      for (int i = 0; i < 4; ++i) // Getting measurements from EEPROM (byte by byte)
      {
        waterByte[i] = EEPROM.read(64 + i);
      }
      // Translating the meter values into numerical form into rtcMem:
      rtcMem.waterVal = waterByte[0] + 100 * waterByte[1] + 10000 * waterByte[2] + 1000000 * waterByte[3];
      Serial.println("Consumption value is extracted from EEPROM: " + String(rtcMem.waterVal));
    }
    else {
      // Increasing step number:
      countStep += 1;
    }
    // Checking if water meter pins state changed:
    if (checkWaterCounter(WATER_PIN, PREV_WATER_STATE_ADDR)) { //Состояние счетчика холодной воды изменилось = истрачено еще 10 л
      rtcMem.waterVal += 10; // 10 litters more on consumption
    }

    // Writing all info into Serial:
    Serial.println("Step number: " + String(countStep) + "\tConsumption: " + String(rtcMem.waterVal));
    Serial.println("Old values from RTC: " + String(rtcMemPrev.waterVal));
    
    // SENDSEC seconds (equals SENDSEC/sleepTimeS steps) passed - publishing data on MQTT:
    if (countStep % (SENDSEC / sleepTimeS) == 0) {
      // Getting params from EEPROM on first run:
      for (int i = 0; i < 32; ++i) // Getting home Wi-Fi SSID and PASS
      {
        ssid += char(EEPROM.read(i));
        password += char(EEPROM.read(32 + i));
      }
      // Connectig to Wi-Fi:
      Serial.println("Trying to connect to Wi-Fi...");
      Serial.println("SSID: "+ String(ssid.c_str())+"; PASS: "+ String(password.c_str()));
      
      WiFi.begin(ssid.c_str(), password.c_str());
      int c = 0;
      while (c < (WiFiTimeoutSec * 2) && WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
        c++;
      }
      if (WiFi.status() == WL_CONNECTED) {
        Serial.println("");
        Serial.println("WiFi connected");
        // Showing IP-address in Serial:
        Serial.println(WiFi.localIP());

        // MQTT preparations:
        mqttClient.onConnect(onMqttConnect);
        mqttClient.onDisconnect(onMqttDisconnect);
        mqttClient.onSubscribe(onMqttSubscribe);
        // mqttClient.onUnsubscribe(onMqttUnsubscribe);
        mqttClient.onMessage(onMqttMessage);
        mqttClient.onPublish(onMqttPublish);
        mqttClient.setServer(IPAddress(MQTTServerIP), 1883);
        // mqttClient.setKeepAlive(5).setCleanSession(false).setWill(MQTT_WATER_LASTWILL_TOPIC, 2, true, "Device is offline").setCredentials("username", "password").setClientId("m");
        mqttClient.setKeepAlive(5).setCleanSession(false);
        Serial.println("Connecting to MQTT...");
        Serial.println(IPAddress(MQTTServerIP));
        mqttClient.connect();
        int c = 0;
        while (c < (MQTTTimeoutSec * 2) && mqttClient.connected() != true) {
          delay(500);
          Serial.print(".");
          c++;
        }
        // float extVCC = ESP.getVcc(); // Getting supply battery voltage
        // String payload = String(rtcMem.waterVal)+";"+String(extVCC);
        String payload = String(rtcMem.waterVal); // Payload string for publishing
        
        // Publishing data on MQTT:
        mqttClient.publish(MQTT_WATER_TOPIC, 1, false, payload.c_str());
        Serial.println("Data sent to MQTT server: "+ payload);
        delay(1000);
        // Storing sent values into rtcMemPrev (as previous measurements):
        rtcMemPrev.waterVal = rtcMem.waterVal;
      }
      else {
        Serial.println("\nWi-Fi connection failed!.. Try to use another SSID.");
      }
    }
    // EEWRITESEC seconds (equals EEWRITESEC/sleepTimeS steps) passed - saving data into EEPROM:
    if (countStep >= (EEWRITESEC/sleepTimeS)) {
      int waterByteVals[4];
      // Splitting values on bytes:
      for (int i = 0; i < 4; ++i) {
        waterByteVals[i] = (rtcMem.waterVal / int(pow(10, i * 2))) % 100; //[0]-два последних разряда, [3]-два первых разряда
      }
      ClearEeprom(64, 128); // Deleting old data from EEPROM
      delay(10);
      for (int i = 0; i < 4; ++i) {
        EEPROM.write(64 + i, waterByteVals[i]);
      }
      EEPROM.commit();
      Serial.println("Added consumption to EEPROM: " + String(rtcMem.waterVal));
      countStep = 0; // Resetting step number
    }

    // Writing data into RTC memory:
    Serial.println("Writings measures to RTC: " + String(rtcMem.waterVal));
    system_rtc_mem_write(COUNT_STEP_ADDR, &countStep, 4); // Step number
    system_rtc_mem_write(WATER_VAL_ADDR, &rtcMem, 4); // Water meters measurements
    system_rtc_mem_write(WATER_OLD_VAL_ADDR, &rtcMemPrev, 4); // Previous water meters measurements

    // All done - going to deep sleep:
    Serial.println("ESP8266 in sleep mode");
    ESP.deepSleep(sleepTimeS * 1000000);
  }

  //----------ALTERNATIVE MODE (Wi-Fi access point) MODE-------------------------------------------------------------
  else { // Button pressed
    Serial.println("\nESP8266 in ALTERNATIVE mode!!! \n Creating Access Point:");
    Serial.println("Starting AP mode. SSID=");
    Serial.println(ssidAP);
    Serial.println("PASSWORD=");
    Serial.println(passwordAP);
    // Reading last saved in RTC values to show on web page:
    system_rtc_mem_read(WATER_VAL_ADDR, &rtcMem, 4);
    // If shit in data - getting values from EEPROM (can be on the first run):
    if (rtcMem.waterVal > 99999999 || rtcMem.waterVal < 0) {
      for (int i = 0; i < 4; ++i) // Reafding EEPROM values (byte by byte)
      {
        waterByte[i] = EEPROM.read(64 + i);
      }
      // Translating the meter values into numerical form into rtcMem:
      rtcMem.waterVal = waterByte[0] + 100 * waterByte[1] + 10000 * waterByte[2] + 1000000 * waterByte[3];
    }
    // Reading Wi-Fi settings saved in EEPROM to show on web page:
    for (int i = 0; i < 32; ++i) // Getting home Wi-Fi SSID and PASS
    {
      ssid += char(EEPROM.read(i));
      password += char(EEPROM.read(32 + i));
    }
    // Creating access point with params stored in sketch:
    WiFi.softAP(ssidAP, passwordAP);
    delay(10);
    server.on("/", D_AP_SER_Page); // Main HTML-page URL
    server.on("/a", Get_Req);      // URL for page after "Submit" button selection
    server.begin();
    delay(300);
  }
}
//********************************END SETUP BLOCK***********************


// Function for checking if water meter contacts stage has changed:
bool checkWaterCounter(int gpioIn, int countAddrBool) { //Input pin number; RTC memory address with water meter state
  bool isChanged;
  int oldVal, curVal;
  // Getting previos state stored in RTC memory:
  system_rtc_mem_read(countAddrBool, &oldVal, 4);
  curVal = digitalRead(gpioIn); // Current water meter state

  if (curVal == 0 && oldVal == 1) {
    isChanged = true;
  }
  else {
    isChanged = false;
  }
  //Serial.println("GPIO: " + String(gpioIn) + "\toldVal: " + String(oldVal) + "\tcurVal: " + String(curVal) + "\tisChanged: " + String(isChanged));
  
  // Update state in RTC memory:
  system_rtc_mem_write(countAddrBool, &curVal, 4);
  return isChanged;
}

// Function for creating web page for access point:
void D_AP_SER_Page() {
  int Tnetwork = 0, i = 0, len = 0;
  String st = "", s = "";         // String array to store the SSID's of available networks
  Tnetwork = WiFi.scanNetworks(); // Scan for all available Wi-Fi networks
  st = "<ul>";
  for (int i = 0; i < Tnetwork; ++i)
  {
    // Print SSID and RSSI for each network found
    st += "<li>";
    st += i + 1;
    st += ": ";
    st += WiFi.SSID(i);
    st += " (";
    st += WiFi.RSSI(i);
    st += ")";
    st += (WiFi.encryptionType(i) == ENC_TYPE_NONE) ? " " : "*";
    st += "</li>";
  }
  st += "</ul>";
  IPAddress ip = WiFi.softAPIP(); //Getting ESP IP-address
  s = "\n\r\n<!DOCTYPE HTML>\r\n<html><h1>Настройки системы удаленного контроля счетчиков</h1> ";
  s += "\r\n<head>\r\n<meta http-equiv=\"Content-Type\" content=\"text/html; charset=utf-8\">\r\n</head>\r\n";
  s += "<p>";
  s += st;
  s += "<p>";
  s += "Текущие настройки Wi-Fi: SSID=" + String(ssid);
  s += " Password=" + String(password);
  s += "<p>";
  s += "<p>";
  s += "Данные отправляются в MQTT каждые " + String(SENDSEC) + " секунд.";
  s += "<p>";
  s += "Текущие значения счетчиков: ХОЛОДНАЯ ВОДА=" + String(rtcMem.waterVal);
  s += "<p>";
  s += "Имя топика MQTT с даннными: ";
  s += MQTT_WATER_TOPIC;
  s += ". Имя топика MQTT для правки значений: ";
  s += MQTT_SUBSCR_TOPIC;
  s += ". Имя топика MQTT с подтверждениями от ESP: ";
  s += MQTT_FEEDBACK_TOPIC;
  s += ". Формат данных: '123456'.";
  s += "<form method='get' action='a'><label>SSID: </label><input name='ssid' length=32><label>Pasword: </label><input name='pass' length=32><br />";
  s += "<label>Water: </label><input name='waterInit' length=8><br /><input type='submit'>";
  s += "</form>";
  s += "</html>\r\n\r\n";

  server.send( 200 , "text/html", s);
}

// Function for submit data from HTML web page:
void Get_Req() {
  String sssid = ""; // SSID variable
  String passs = ""; // PASS variable

  if (server.hasArg("ssid") && server.hasArg("pass")) {
    sssid = server.arg("ssid"); // Getting SSID from web form
    passs = server.arg("pass"); // Getting PASS from web form
    Serial.println("Wi-Fi settigns from web-page: " + String(sssid) + "; " + String(passs));
  }

  String waterInit = ""; // Current water consumption variable
  int waterInitInt; // Current water consumption variable (integer)
  int waterByteVals[4];

  if (server.hasArg("waterInit")) {
    waterInit = server.arg("waterInit"); // Getting value from web page
    // Transforming into integer:
    waterInitInt = waterInit.toInt();
    Serial.println("Water measures from web-page: "+String(waterInitInt));
    //Разбиваем 8-значное число на 4 байта:
    for (int i = 0; i < 4; ++i) {
      waterByteVals[i] = (waterInitInt / int(pow(10, i * 2))) % 100;
    }
  }

  if (sssid.length() > 1 && passs.length() > 1) {
    ClearEeprom(0, 64); // Deleting old SSID and PASS values from EEPROM
    delay(10);
    // Storing new SSID and PASS from web page:
    for (int i = 0; i < sssid.length(); ++i)
    {
      EEPROM.write(i, sssid[i]);
    }
    for (int i = 0; i < passs.length(); ++i)
    {
      EEPROM.write(32 + i, passs[i]);
    }
    EEPROM.commit();
    Serial.println("Added to EEPROM: SSID=" + sssid + "; Pass=" + passs);
  }

  // Storing new water consuption values from web page:
  if (waterInit.length() > 1) {
    ClearEeprom(64, 128); // Deleting old measures from EEPROM
    delay(10);
    for (int i = 0; i < 4; ++i)
    {
      EEPROM.write(64 + i, waterByteVals[i]);
    }
    EEPROM.commit();
    delay(10);
    Serial.println("Consumption=" + String(waterByteVals[0] + 100 * waterByteVals[1] + 10000 * waterByteVals[2] + 1000000 * waterByteVals[3]));
    // Resetting step number:
    countStep = 0;
    system_rtc_mem_write(COUNT_STEP_ADDR, &countStep, 4); // Step number
    delay(100);

    // Checking that everything was saved correctly:
    for (int i = 0; i < 4; ++i) // Raading EEPROM byte by byte
      {
        waterByte[i] = EEPROM.read(64 + i);
      }
      //Переводим показания счетчиков в числовой вид и выводим в Serial:
      int a = waterByte[0] + 100 * waterByte[1] + 10000 * waterByte[2] + 1000000 * waterByte[3];
      Serial.println("Consumption value is extracted from EEPROM: " + String(a));
  }


  String s = "\r\n\r\n<!DOCTYPE HTML>\r\n<html>";
  s += "\r\n<head>\r\n<meta http-equiv=\"Content-Type\" content=\"text/html; charset=utf-8\">\r\n<!head>\r\n";
  s += "\r\n<h1>Настройки загружены!</h1>";
  s += "<p>Данные успешно сохранены! Для штатной работы выключите и включите систему или просто нажмите кнопку RESET.</html>\r\n\r\n";
  server.send(200, "text/html", s);
}

// Function that cleans EEPROM:
void ClearEeprom(int startByte, int endByte) {
  Serial.println("Clearing EEPROM from " + String(startByte) + " to " + String(endByte));
  for (int i = startByte; i < endByte; ++i) {
    EEPROM.write(i, 0);
  }
}

void loop() // For operating in alternative mode
{
  server.handleClient();
}