//Joseph Hewitt 2023
//This code is for the ESP32 "Side B" of the wardriver hardware revision 3.

//Serial = PC, 115200
//Serial1 = ESP32 (side A), 115200
//Serial2 = SIM800L module, 9600

#include <WiFi.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <Update.h>
#include "mbedtls/sha256.h"
#include "mbedtls/md.h"

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
boolean ota_mode = false; //Set to true automatically when doing OTA update
String ota_hash = ""; //SHA256 of the OTA update, set automatically.

boolean using_bw16 = false; //Set when advanced config is sb_bw16=yes https://wardriver.uk/advanced_config

#define mac_history_len 1024

struct mac_addr {
   unsigned char bytes[6];
};

struct mac_addr mac_history[mac_history_len];
unsigned int mac_history_cursor = 0;

int ble_found = 0; //The number of BLE devices found in a single scan, sent to side A.
int wifi_scan_channel = 1; //The channel to scan (increments automatically)

void setup_wifi(){
  //Gets the WiFi ready for scanning by disconnecting from networks and changing mode.
  //Turn off entirely to cleanup any references to active networks
  WiFi.mode(WIFI_OFF);
  delay(250);
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
      
        ble_found++;
        if (!seen_mac(mac_bytes)){
          save_mac(mac_bytes);

          String ble_name = advertisedDevice.getName().c_str();
          ble_name.replace(",","_");
          
          Serial1.print("BL,");
          Serial1.print(advertisedDevice.getRSSI());
          Serial1.print(",");
          Serial1.print(advertisedDevice.getAddress().toString().c_str());
          Serial1.print(",");
          Serial1.println(ble_name);
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

String hex_str(const unsigned char buf[], size_t len)
{
    String outstr;
    char outchr[6];
    for (size_t i = 0; i < len; i++) {
        if (buf[i] <= 0xF) {
            sprintf(outchr, "0%x", buf[i]);
        } else {
            sprintf(outchr, "%x", buf[i]);
        }
        outstr = outstr + outchr;
    }
    return outstr;
}

void setup() {
  setup_wifi();
  delay(5000);
  int reset_reason = esp_reset_reason();
  Serial.begin(115200); //PC, if connected.
  Serial.println("Starting");
  
  Serial1.begin(115200,SERIAL_8N1,27,14); //ESP A, pins 27/14
  Serial1.println("REV3!");
  Serial1.print("RESET=");
  Serial1.println(reset_reason);

  setup_id_pins();
  byte board_id = read_id_pins();

  switch(board_id){
    case 1:                 // CoD_Segfault Mini Wardriver Rev2
      using_bw16 = true;    // All units have BW16
      break;
    default:                // Any boards not using ID pins will be assumed 
      break;                // to rely on config files for all parameters
  }

  Serial.println("Waiting for config vars");
  Serial1.println("SEND_CONF");
  Serial1.flush();
  while (millis() < 11000){
    String buff = Serial1.readStringUntil('\n');
    Serial.print("IN:");
    Serial.println(buff);
    if (!buff.startsWith("PUSH:")){
      continue;
    }
    buff.replace("PUSH:","");
    //Lets make this a bit nicer in the future.
    if (buff.indexOf("sb_bw16=yes") > -1){
      using_bw16 = true;
    }
  }

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

  int baud_rate = 9600;
  if (using_bw16){
    baud_rate = 38400;
    Serial.println("Using BW16 instead of SIM800L");
  }

  Serial2.begin(baud_rate,SERIAL_8N1,16,17); //SIM800L/BW16
  delay(50);
  if (!using_bw16){
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
  } else {
    Serial.println("Waking BW16");
    Serial2.print("AT\r\n");
    Serial2.flush();
  }

  Serial1.print("RESET=");
  Serial1.println(reset_reason);

  Serial.println("Setting up Bluetooth scanning");
  BLEDevice::init("");
  pBLEScan = BLEDevice::getScan(); //create new scan
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
  pBLEScan->setActiveScan(false); //active scan uses more power, but get results faster
  pBLEScan->setInterval(100);
  pBLEScan->setWindow(50);  // less or equal setInterval value


  Serial.println("Setting up multithreading");
  xTaskCreatePinnedToCore(
      loop2, /* Function to implement the task */
      "loop2", /* Name of the task */
      10000,  /* Stack size in words */
      NULL,  /* Task input parameter */
      3,  /* Priority of the task */
      &loop2handle,  /* Task handle. */
      0); /* Core where the task should run */

  Serial.println("Started");
  Serial1.println("REV3!");
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
  if (ota_mode){
    boolean preamble_started = false;
    boolean binary_started = false;
    Serial.println("Core1 OTA");
    Serial1.println(ota_hash);
    Serial1.flush();
    Update.begin(UPDATE_SIZE_UNKNOWN);
    #define binbuflen 4096
    uint8_t binbuf[binbuflen] = { 0x00 };
    int counter = 0;

    //Setup a hash context, and somewhere to keep the output.
    unsigned char genhash[32];
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    unsigned long fw_last_byte = millis();
    
    while (ota_mode){
      if (Serial1.available()){
        byte c = Serial1.read();
        fw_last_byte = millis();
        if (c == 0xFF){
          //Do a flush on the first byte of the preamble, just in case. A side does too.
          if (!preamble_started){
            Serial1.flush();
            Serial.println("OTA preamble");
          }
          preamble_started = true;
        }
        //0xE9 is the magic, but we won't use it. Maybe one day.
        if (c != 0xFF && preamble_started){
          if (!binary_started){
            Serial.println("OTA preamble end");
            Serial.flush();
          }
          binary_started = true;
        }
        if (binary_started){
          
          binbuf[counter] = c;
          counter++;
          fw_last_byte = millis();
          if (counter == binbuflen){
            Update.write(binbuf,counter);
            mbedtls_sha256_update(&ctx, binbuf, counter);
            counter = 0;
            memset(binbuf, 'f', binbuflen);
          }
        }
      } else { //Serial1 available
        Serial1.write(0xFF);
      }
      
      if (millis() - fw_last_byte > 4000){
        Serial.println("Upload complete");
        if (counter > 0){
          Update.write(binbuf,counter);
          mbedtls_sha256_update(&ctx, binbuf, counter);
        }
        mbedtls_sha256_finish(&ctx, genhash);
        String actual_hash = hex_str(genhash, sizeof genhash);
        if (actual_hash == ota_hash){
          Update.end(true);
          Serial.println("Update OK and verified");
          Serial.flush();
          Serial1.println(actual_hash);
          delay(500);
          Serial1.println(actual_hash);
          Serial1.flush();
          delay(500);
          ESP.restart();
        } else {
          Serial.println("HASH MISMATCH:");
          Serial.println(actual_hash);
          Serial.println(ota_hash);
          Update.abort();
          Serial1.println("FAILURE");
          ESP.restart();
        }
        
      }
    }
  }

  await_serial();
  serial_lock = true;
  BLEScanResults* foundDevices = pBLEScan->start(1.8, false);
  Serial1.print("BLC,");
  Serial1.println(ble_found);
  serial_lock = false;
  Serial.print("Devices found: ");
  Serial.println(ble_found);
  Serial.println("Scan done!");
  pBLEScan->clearResults();   // delete results fromBLEScan buffer to release memory
  yield();
  ble_found = 0;
  if (last_temperature == 0 || millis() - last_temperature > 15000){
    read_temperature();
    request_temperature();
    last_temperature = millis();
  }
  
  //This side will only scan a subset of channels defined below; most scan time is dedicated to Bluetooth here.
  for (int y = 0; y < 6; y++){
    switch(wifi_scan_channel){
      case 1:
        wifi_scan_channel = 6;
        break;
      case 6:
        wifi_scan_channel = 11;
        break;
      case 11:
        wifi_scan_channel = 12;
        break;
      case 12:
        wifi_scan_channel = 13;
        break;
      case 13:
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
        uint8_t *this_bssid_raw = WiFi.BSSID(i);
        char this_bssid[18] = {0};
        sprintf(this_bssid, "%02X:%02X:%02X:%02X:%02X:%02X", this_bssid_raw[0], this_bssid_raw[1], this_bssid_raw[2], this_bssid_raw[3], this_bssid_raw[4], this_bssid_raw[5]);
        if (seen_mac(this_bssid_raw)){
          continue;
        }
        save_mac(this_bssid_raw);

        String ssid = WiFi.SSID(i);
        ssid.replace(",","_");

        await_serial();
        serial_lock = true;
        Serial1.printf("WI%d,%s,%d,%d,%d,%s\n", 0, ssid.c_str(), WiFi.channel(i), WiFi.RSSI(i), WiFi.encryptionType(i), this_bssid);
        serial_lock = false;
      }
    }
  }
}

void loop2( void * parameter) {
  boolean had_gsm_data = false;
  int count_5ghz = 0;
  while (true) {
    yield();
    delay(10);
    while (Serial1.available()){
      //ESP A rarely talks to us, but it's usually important
      String a_buff = Serial1.readStringUntil('\n');
      if (a_buff.startsWith("FWUP:")){
        Serial.println("Core2 OTA prep");
        ota_hash = a_buff.substring(5);
        ota_mode = true;
        while (ota_mode){
          //Keep this core busy
          yield();
        }
      }
      
    }
    String s2buf = Serial2.readStringUntil('\n');
    if (s2buf.length() >= 2){
      
     if (s2buf.length() > 30){
       if (!using_bw16){
         await_serial();
         serial_lock = true;
         Serial1.print("GSM,");
         Serial1.print(s2buf);
         had_gsm_data = true;
         Serial1.println();
         serial_lock = false;
       } else {
        //Parse BW16 line here.
        String ssid = "";
        int channel = 0;
        int rssi = 0;
        int enc_type = 0;
        String mac = "";

        #define mac_len 18

        mac = s2buf.substring(s2buf.length()-mac_len);
        mac.toUpperCase();

        int pos = mac_len+1;
        int previous_pos = pos;
        int counter = 0;
        while (pos <= s2buf.length()){
          pos++;
          if (s2buf.charAt(s2buf.length()-pos) != ','){
            continue;
          }

          counter++;
          String match = s2buf.substring(s2buf.length()-pos+1, s2buf.length()-previous_pos);
          

          if (counter == 1){
            //RSSI
            rssi = match.toInt();
          }
          if (counter == 2){
            //Security type
            if (match.indexOf("WPA2 AES") > -1 || match.indexOf("WPA2 TKIP") > -1 || match.indexOf("WPA2 PSK") > -1) {
              enc_type = WIFI_AUTH_WPA2_PSK;
            }
            if ((match.indexOf("WPA2/WPA3 PSK") > -1) && enc_type == 0) {
              enc_type = WIFI_AUTH_WPA2_WPA3_PSK;
            }
            if ((match.indexOf("WPA3") > -1 || match.indexOf("WPA3 PSK") > -1) && enc_type == 0) {
              enc_type = WIFI_AUTH_WPA3_PSK;
            }
            if ((match.indexOf("WPA/WPA2 PSK") > -1) && enc_type == 0) {
              enc_type = WIFI_AUTH_WPA_WPA2_PSK;
            }
            if ((match.indexOf("WPA Enterprise") > -1 || match.indexOf("WPA2 Enterprise") > -1 || match.indexOf("WPA/WPA2 Enterprise") > -1) && enc_type == 0) {
              enc_type = WIFI_AUTH_ENTERPRISE;
            }
            if ((match.indexOf("WPA2") > -1 || match.indexOf("WPA2 Enterprise") > -1) && enc_type == 0) {
              enc_type = WIFI_AUTH_WPA2_ENTERPRISE;
            }
            if ((match.indexOf("WPA") > -1 || match.indexOf("WPA PSK") > -1) && enc_type == 0) {
              enc_type = WIFI_AUTH_WPA_PSK;
            }
            if ((match.indexOf("None") > -1 || match.indexOf("Open") > -1) && enc_type == 0) {
              enc_type = WIFI_AUTH_OPEN;
            }
            if ((match.indexOf("WEP") > -1) && enc_type == 0) {
              enc_type = WIFI_AUTH_WEP;
            }



          }
          if (counter == 3){
            //Channel
            channel = match.toInt();
            if (channel > 14){
              count_5ghz++;
            }

            int comma_pos = s2buf.indexOf(",")+1;

            ssid = s2buf.substring(comma_pos, s2buf.length()-pos);
          }
          if (counter >= 4){
            break;
          }

          previous_pos = pos;
          
        }
        
        await_serial();
        serial_lock = true;
        Serial.printf("WI%d,%s,%d,%d,%d,%s\n", 0, ssid.c_str(), channel, rssi, enc_type, mac.c_str());
        Serial1.printf("WI%d,%s,%d,%d,%d,%s\n", 0, ssid.c_str(), channel, rssi, enc_type, mac.c_str());
        serial_lock = false;
       }
      } else {
        //Short line, normally we discard this
        if (using_bw16){
          if (s2buf.indexOf("[ATWS]") > -1){
            had_gsm_data = true;
            await_serial();
            serial_lock = true;
            Serial1.print("5G,");
            Serial1.print(count_5ghz);
            Serial1.print("\n");
            serial_lock = false;
            Serial.print("BW16 done, 5GHz count: ");
            Serial.println(count_5ghz);
            count_5ghz = 0;
          }
        }
      }
    } else {
      if (last_sim_request == 0 || millis() - last_sim_request > 15000 || had_gsm_data == true){
        if (!using_bw16){
          Serial2.print("AT+CNETSCAN\r\n");
          Serial.println("Requesting data from SIM");
        } else {
          Serial2.print("ATWS\r\n");
          Serial.println("Requesting data from BW16");
        }
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

void setup_id_pins(){
  pinMode(13, INPUT_PULLUP); // IO13 is A/B identifier pin
  pinMode(25, INPUT_PULLDOWN); // All other pins are board identifers
  pinMode(26, INPUT_PULLDOWN);
  pinMode(32, INPUT_PULLDOWN);
  pinMode(33, INPUT_PULLDOWN);
}

byte read_id_pins(){
  byte board_id = 0;
  board_id = digitalRead(25);                     // shift bits to get a board ID
  board_id = (board_id << 1) + digitalRead(26);
  board_id = (board_id << 1) + digitalRead(32);
  board_id = (board_id << 1) + digitalRead(33);

  return board_id;
}

