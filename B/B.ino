//Joseph Hewitt 2023
//This code is for the ESP32 "Side B" of the wardriver hardware revision 3.

//Serial = PC, 115200
//Serial1 = ESP32 (side A), 115200
//Serial2 = SIM800L module, 9600

#include <WiFi.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

//LCD stuff. Side B does not normally have an LCD but this allows us to print an error if you flash the firmware onto the wrong ESP32.
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#define SCREEN_WIDTH 128 // OLED display width, in pixels
#define SCREEN_HEIGHT 32 // OLED display height, in pixels
#define OLED_RESET     -1 // Reset pin # (or -1 if sharing Arduino reset pin)
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

#include <OneWire.h>
OneWire  ds(22); //DS18B20 data pin is 22.
byte addr[8]; //The DS18B20 address

boolean serial_lock = false; //Set to true when the serial with side A is in active use.
boolean temperature_sensor_ok = true; //Set to false automatically if a DS18B20 is not detected.

#define mac_history_len 256

struct mac_addr {
   unsigned char bytes[6];
};

struct mac_addr mac_history[mac_history_len];
unsigned int mac_history_cursor = 0;

int wifi_scan_channel = 1; //The channel to scan (increments automatically)

void setup_wifi(){
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
}

BLEScan* pBLEScan;

void await_serial(){
  while(serial_lock){
    Serial.println("await");
    delay(1);
  }
}

class MyAdvertisedDeviceCallbacks: public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice advertisedDevice) {
      unsigned char mac_bytes[6];
      int values[6];
  
      if (6 == sscanf(advertisedDevice.getAddress().toString().c_str(), "%x:%x:%x:%x:%x:%x%*c", &values[0], &values[1], &values[2], &values[3], &values[4], &values[5])){
        for(int i = 0; i < 6; ++i ){
            mac_bytes[i] = (unsigned char) values[i];
        }
      
        if (!seen_mac(mac_bytes)){
          save_mac(mac_bytes);
          
          await_serial();
          serial_lock = true;
          Serial.printf("Advertised Device: %s \n", advertisedDevice.toString().c_str());
          Serial1.print("BL,");
          Serial1.print(advertisedDevice.getRSSI());
          Serial1.print(",");
          Serial1.print(advertisedDevice.getAddress().toString().c_str());
          Serial1.print(",");
          Serial1.println(advertisedDevice.getName().c_str());
          serial_lock = false;
        }
      }
    }
};

TaskHandle_t loop2handle;

void request_temperature(){
  if (!temperature_sensor_ok){
    return;
  }
  Serial.println("Requesting temperature");
  ds.reset();
  ds.select(addr);
  ds.write(0x44, 1);
  //A delay of 750ms is required now before the temperature is ready.
}

void read_temperature(){
  if (!temperature_sensor_ok){
    return;
  }
  byte present = 0;
  byte data[12];
  present = ds.reset();
  ds.select(addr);    
  ds.write(0xBE);         // Read Scratchpad

  for (int i = 0; i < 9; i++) {           // we need 9 bytes
    data[i] = ds.read();
  }

  int16_t raw = (data[1] << 8) | data[0];  
  byte cfg = (data[4] & 0x60);
  // at lower res, the low bits are undefined, so let's zero them
  if (cfg == 0x00) raw = raw & ~7;  // 9 bit resolution, 93.75 ms
  else if (cfg == 0x20) raw = raw & ~3; // 10 bit res, 187.5 ms
  else if (cfg == 0x40) raw = raw & ~1; // 11 bit res, 375 ms
  //// default is 12 bit resolution, 750 ms conversion time
  
  float celsius = (float)raw / 16.0;
  
  Serial.print("Temperature = ");
  Serial.println(celsius);
  await_serial();
  serial_lock = true;
  Serial1.print("TEMP,");
  Serial1.println(celsius);
  serial_lock = false;
}

void setup() {
  setup_wifi();
  delay(5000);
  Serial.begin(115200); //PC, if connected.
  Serial.println("Starting");

  int sensor_attempts = 0;
  while ( !ds.search(addr)) {
    Serial.println("No more addresses.");
    Serial.println();
    ds.reset_search();
    delay(250);
    sensor_attempts++;
    if (sensor_attempts > 5){
      temperature_sensor_ok = false;
      break;
    }
  }

  if (temperature_sensor_ok){
    Serial.print("DS18B20 detected with ID =");
    for(int i = 0; i < 8; i++) {
      Serial.write(' ');
      Serial.print(addr[i], HEX);
    }
  
    Serial.println();
    if (OneWire::crc8(addr, 7) != addr[7]) {
        Serial.println("DS18B20 CRC is not valid!");
        temperature_sensor_ok = false;
    }
  
    request_temperature();
  } else {
    Serial.println("Unable to detect DS18B20 temperature sensor!");
  }

  Serial1.begin(115200,SERIAL_8N1,27,14); //ESP A, pins 27/14
  Serial1.println("STARTING");

  Serial2.begin(9600); //SIM800L
  delay(50);
  Serial.println("Requesting data from SIM");
  Serial2.print("AT+CNETSCAN=1\r\n");
  Serial2.flush();
  int i = 0;
  boolean response = false;
  while (i < 2000){
    if (Serial2.available()){
      char c = Serial2.read();
      Serial.write(c);
      response = true;
    } else {
      delay(1);
    }
    i++;
  }
  Serial.println();

  if (!response){
    Serial.println("SIM800L did not respond.");
    delay(3000);
    Serial2.print("AT+CNETSCAN=1\r\n");
  }

  Serial.println("Setting up Bluetooth scanning");
  BLEDevice::init("");
  pBLEScan = BLEDevice::getScan(); //create new scan
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
  pBLEScan->setActiveScan(false); //active scan uses more power, but get results faster
  pBLEScan->setInterval(50);
  pBLEScan->setWindow(40);  // less or equal setInterval value


  Serial.println("Setting up multithreading");
  xTaskCreatePinnedToCore(
      loop2, /* Function to implement the task */
      "loop2", /* Name of the task */
      10000,  /* Stack size in words */
      NULL,  /* Task input parameter */
      0,  /* Priority of the task */
      &loop2handle,  /* Task handle. */
      0); /* Core where the task should run */

  Serial.println("Started");
  if (!temperature_sensor_ok){
    //If there's no temperature sensor, attempt to put a warning on the LCD.
    //This is side B so there should be no LCD. This should only be visible if side A is flashed with this code.
    //The display.begin() line kills the DS18B20 communication (bug), hence why we check for the sensor.
    //Side A does not have a DS18B20, so if we detect one then we clearly aren't running on A and the warning isn't needed.
    if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) { // Address 0x3C for 128x32
        Serial.println(F("SSD1306 allocation failed (this is completely okay)"));
    }
    display.setRotation(2);
    display.clearDisplay();
    display.setTextSize(1);      // Normal 1:1 pixel scale
    display.setTextColor(WHITE); // Draw white text
    display.setCursor(0, 0);     // Start at top-left corner
    display.cp437(true);         // Use full 256 char 'Code Page 437' font
    display.println("SIDE B CODE RUNNING ON SIDE A?");
    display.println("Check documentation @ wardriver.uk");
    display.display();
  }
}

unsigned long last_sim_request;
unsigned long last_temperature;

void loop() {
  clear_mac_history();
  BLEScanResults foundDevices = pBLEScan->start(2.5, false);
  await_serial();
  serial_lock = true;
  Serial1.print("BLC,");
  Serial1.println(mac_history_cursor);
  serial_lock = false;
  Serial.print("Devices found: ");
  Serial.println(mac_history_cursor);
  Serial.println("Scan done!");
  pBLEScan->clearResults();   // delete results fromBLEScan buffer to release memory
  if (last_temperature == 0 || millis() - last_temperature > 15000){
    read_temperature();
    request_temperature();
    last_temperature = millis();
  }
  
  //This side will only scan the primary non-overlapping channels; most scan time is dedicated to Bluetooth here.
  for (int y = 0; y < 4; y++){
    switch(wifi_scan_channel){
      case 1:
        wifi_scan_channel = 6;
        break;
      case 6:
        wifi_scan_channel = 11;
        break;
      case 11:
        wifi_scan_channel = 14;
        break;
      default:
        wifi_scan_channel = 1;
    }
  
    //scanNetworks(bool async, bool show_hidden, bool passive, uint32_t max_ms_per_chan, uint8_t channel)
    int n = WiFi.scanNetworks(false,true,false,110,wifi_scan_channel);
    Serial.print("Scan of channel ");
    Serial.print(wifi_scan_channel);
    Serial.print(" returned ");
    Serial.println(n);
    if (n > 0){
      for (int i = 0; i < n; i++) {
        String ssid = WiFi.SSID(i);
        ssid.replace(",","_");
        //Normally we would use "WIx," as the first value where x is the value of i+1. We're using "WI0," here to make it easier to parse on side A.
        Serial.printf("WI%d,%s,%d,%d,%d,%s\n", 0, ssid.c_str(), WiFi.channel(i), WiFi.RSSI(i), WiFi.encryptionType(i), WiFi.BSSIDstr(i).c_str());
        await_serial();
        serial_lock = true;
        Serial1.printf("WI%d,%s,%d,%d,%d,%s\n", 0, ssid.c_str(), WiFi.channel(i), WiFi.RSSI(i), WiFi.encryptionType(i), WiFi.BSSIDstr(i).c_str());
        serial_lock = false;
      }
    }
  }
}

void loop2( void * parameter) {
  boolean had_gsm_data = false;
  while (true) {
    if (Serial2.available()){
      char linebuf[120];
      int i = 0;
      int linelen = 0;
      int no_data_times = 0;
      while (i < 120){
        if (Serial2.available()){
          char c = Serial2.read();
          if (c == '\n'){
            Serial.print("Read from SIM: ");
            Serial.print(i);
            Serial.println(" bytes");
            linebuf[i] = '\0';
            break;
          }
          linebuf[i] = c;
          i++;
        } else {
          no_data_times++;
          if (no_data_times > 500){
            break;
          }
          delay(2);
        }
      }
     linelen = i;
     i = 0;
     if (linelen > 30){
       await_serial();
       serial_lock = true;
       Serial1.print("GSM,");
       while (i < linelen){
        char c = linebuf[i];
        if (c == '\0' || c == '\r' || c == '\n'){
          break;
        }
        Serial1.write(c);
        i++;
       }
       had_gsm_data = true;
       Serial1.println();
       serial_lock = false;
      }
    } else {
      if (last_sim_request == 0 || millis() - last_sim_request > 15000 || had_gsm_data == true){
        Serial2.print("AT+CNETSCAN\r\n");
        Serial.println("Requesting data from SIM");
        last_sim_request = millis();
        had_gsm_data = false;
      }
    }
  }
}

void save_mac(unsigned char* mac){
  if (mac_history_cursor >= mac_history_len){
    mac_history_cursor = 0;
  }
  struct mac_addr tmp;
  for (int x = 0; x < 6 ; x++){
    tmp.bytes[x] = mac[x];
  }

  mac_history[mac_history_cursor] = tmp;
  mac_history_cursor++;
}

boolean seen_mac(unsigned char* mac){

  struct mac_addr tmp;
  for (int x = 0; x < 6 ; x++){
    tmp.bytes[x] = mac[x];
  }

  for (int x = 0; x < mac_history_len; x++){
    if (mac_cmp(tmp, mac_history[x])){
      return true;
    }
  }
  return false;
}

void print_mac(struct mac_addr mac){
  for (int x = 0; x < 6 ; x++){
    Serial.print(mac.bytes[x],HEX);
    Serial.print(":");
  }
}

boolean mac_cmp(struct mac_addr addr1, struct mac_addr addr2){
  for (int y = 0; y < 6 ; y++){
    if (addr1.bytes[y] != addr2.bytes[y]){
      return false;
    }
  }
  return true;
}

void clear_mac_history(){
  struct mac_addr tmp;
  for (int x = 0; x < 6 ; x++){
    tmp.bytes[x] = 0;
  }
  
  for (int x = 0; x < mac_history_len; x++){
    mac_history[x] = tmp;
  }

  mac_history_cursor = 0;
}
