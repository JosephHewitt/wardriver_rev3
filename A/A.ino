//Joseph Hewitt 2023
//This code is for the ESP32 "Side A" of the wardriver hardware revision 3.

const String VERSION = "1.2.0b1";

#include <GParser.h>
#include <MicroNMEA.h>
#include "FS.h"
#include "SD.h"
#include "SPI.h" 
#include <WiFi.h>
#include <Preferences.h>
#include <time.h>
#include <Update.h>
#include "mbedtls/sha256.h"
#include "mbedtls/md.h"

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#define SCREEN_WIDTH 128 // OLED display width, in pixels
#define SCREEN_HEIGHT 32 // OLED display height, in pixels
#define OLED_RESET     -1 // Reset pin # (or -1 if sharing Arduino reset pin)
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

//These variables are used for buffering/caching GPS data.
char nmeaBuffer[100];
MicroNMEA nmea(nmeaBuffer, sizeof(nmeaBuffer));
unsigned long lastgps = 0;
String last_dt_string = "";
String last_lats = "";
String last_lons = "";

//Automatically set to true if a blocklist was loaded.
boolean use_blocklist = false;
//millis() when the last block happened.
unsigned long ble_block_at = 0;
unsigned long wifi_block_at = 0;

//These variables are used to populate the LCD with statistics.
float temperature;
unsigned int ble_count;
unsigned int gsm_count;
unsigned int wifi_count;
unsigned int disp_gsm_count;
unsigned int disp_wifi_count;

uint32_t chip_id;

File filewriter;

Preferences preferences;
unsigned long bootcount = 0;

String default_ssid = "wardriver.uk";
const char* default_psk = "wardriver.uk";

/* The recently seen MAC addresses and cell towers are saved into these arrays so that the 
 * wardriver can detect if they have already been written to the Wigle CSV file.
 * These _len definitions define how large those arrays should be. Larger is better but consumes more RAM.
 */
#define mac_history_len 512
#define cell_history_len 128
#define blocklist_len 20
//Max blocklist entry length. 32 = max SSID len.
#define blocklist_str_len 32

struct mac_addr {
   unsigned char bytes[6];
};

struct coordinates {
  double lat;
  double lon;
  int acc;
};

struct cell_tower {
  int mcc;
  int mnc;
  int lac;
  int cellid;
  unsigned long seenat;
  int strength;
  struct coordinates pos;
};

struct block_str {
  char characters[blocklist_str_len];
};

struct mac_addr mac_history[mac_history_len];
unsigned int mac_history_cursor = 0;

struct cell_tower cell_history[cell_history_len];
unsigned int cell_history_cursor = 0;

struct block_str block_list[blocklist_len];

unsigned long lcd_last_updated;

#define YEAR_2020 1577836800 //epoch value for 1st Jan 2020; dates older than this are considered wrong (this code was written after 2020).
unsigned long epoch;
unsigned long epoch_updated_at;
const char* ntpServer = "pool.ntp.org";

TaskHandle_t primary_scan_loop_handle;

boolean b_working = false; //Set to true when we receive some valid data from side B.

#define DEVICE_UNKNOWN 254
#define DEVICE_CUSTOM  0
#define DEVICE_REV3    1
#define DEVICE_REV3_5  2
#define DEVICE_REV4    3
byte DEVICE_TYPE = DEVICE_UNKNOWN;

#define HTTP_TIMEOUT_MS 750

//Change these in cfg.txt instead of editing this source code.
int gps_baud_rate = 9600;
boolean rotate_display = false;
boolean block_resets = false;
boolean block_reconfigure = false;
int web_timeout = 60000; //ms to spend hosting the web interface before booting.
int gps_allow_stale_time = 60000;
boolean enforce_valid_binary_checksums = true; //Lookup OTA binary checksums online, prevent installation if no match found

void setup_wifi(){
  //Gets the WiFi ready for scanning by disconnecting from networks and changing mode.
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
}

void clear_display(){
  //Clears the LCD and resets the cursor.
  display.clearDisplay();
  display.setCursor(0, 0);
}

int get_config_int(String key, int def=0){
  String res = get_config_option(key);
  if (res == ""){
    return def;
  }
  return res.toInt();
}

String file_hash(String filename, boolean update_lcd=true, String lcd_prompt="Wardriver busy"){
  File reader = SD.open(filename, FILE_READ);
  //Setup a hash context, and somewhere to keep the output.
  unsigned char genhash[32];
  byte bbuf[2] = {0x00, 0x00};
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);

  int i = 0;

  while (reader.available()){
    byte c = reader.read();
    bbuf[0] = c;
    mbedtls_sha256_update(&ctx, bbuf, 1);
    i++;
    if (i > 1800){
      i = 0;
      if (update_lcd){
        clear_display();
        display.println(lcd_prompt);
        float percent = ((float)reader.position() / (float)reader.size()) * 100;
        display.print(percent);
        display.println("%");
        display.display();
      }
    }
    
  }
  mbedtls_sha256_finish(&ctx, genhash);
  return hex_str(genhash, sizeof genhash);
}

static void print_hex(const char *title, const unsigned char buf[], size_t len)
{
    Serial.printf("%s: ", title);

    for (size_t i = 0; i < len; i++) {
        if (buf[i] < 0xF) {
            Serial.printf("0%x", buf[i]);
        } else {
            Serial.printf("%x", buf[i]);
        }
    }

    Serial.println();
}

boolean install_firmware(String filepath, String expect_hash = "") {
  //Install a .bin binary to the local device.
  //If expect_hash is not empty, the hash will be validated first.
  
  if (!SD.exists(filepath)) {
    Serial.print("File not found: ");
    Serial.println(filepath);
    return false;
  }

  Serial.println("Validating firmware");
  String actual_hash = file_hash(filepath, true, "Validating firmware");
  if (expect_hash.length() > 0) {
    if (expect_hash != actual_hash) {
      Serial.println("Local checksum mismatch");
      return false;
    }
  }

  if (enforce_valid_binary_checksums) {
    //At this point, make a HTTPS request to an API which can validate the .bin checksum.
    //Fail here if the checksum is a mismatch.
  }

  if (filepath == "/A.bin"){

    clear_display();
    display.println("Installing update");
    display.display();
  
    File binreader = SD.open(filepath, FILE_READ);
    #define binbuflen 4096
    uint8_t binbuf[binbuflen] = { 0x00 };
  
    Update.begin(binreader.size());
    int counter = 0;
    
    while (binreader.available()) {
      byte c = binreader.read();
      binbuf[counter] = c;
      counter++;
      if (counter == binbuflen){
        Update.write(binbuf,counter);
        counter = 0;
        memset(binbuf, 'f', binbuflen);
        clear_display();
        display.print("Installing: ");
        float percent = ((float)binreader.position() / (float)binreader.size()) * 100;
        display.print(percent);
        display.println("%");
        display.println("DO NOT POWER OFF");
        display.display();
      }
      
    }
    
    if (counter != 0){
      Update.write(binbuf,counter);
    }
    Update.end(true);
  
    clear_display();
    display.println("Update installed");
    display.println("Restarting now");
    display.display();
    delay(1000);
    ESP.restart();
  
    return true;
  }

  if (filepath == "/B.bin"){
    Serial.println("Firmware update for side B");
    boolean update_ready = false;
    Serial1.flush();
    String b_buff = "";
    int ready_failures = 0;
    clear_display();
    display.println("Getting B ready");
    display.display();
    
    while (!update_ready){
      Serial.println("Getting B ready");
      Serial1.print("FWUP:");
      Serial1.println(actual_hash);
      Serial1.flush();
      int linecounter = 0;
      while (linecounter < 5){
        String buff = Serial1.readStringUntil('\n');
        if (buff.indexOf(actual_hash) > -1){
          update_ready = true;
          break;
        }
        Serial.print("Unwanted: ");
        Serial.println(buff);
        buff = "";
        linecounter++;
      }
      if (!update_ready){
        ready_failures++;
      }
      if (ready_failures > 9){
        Serial.println("Unable to configure B!");
        Serial.println("Likely B is outdated, does not support OTA, and must be updated manually");
        clear_display();
        display.println("FAILURE");
        display.println("Update B manually!");
        display.println("OTA not supported");
        display.display();
        delay(10000);
        return false;
      }
    }
    //At this point, B side is in update mode.
    Serial.println("B is ready");
    //0xE9 is the binary header, let's spam something else to be sure we're clear of junk
    for(int i=0; i<5000; i++){
      Serial1.write(0xFF);
      Serial1.flush();
      delay(1);
    }
    //B will sense 0xFF -> 0xE9 and start the update.

    //START UPDATE

    File binreader = SD.open(filepath, FILE_READ);
    int counter = 0;
    
    while (binreader.available()) {
      byte c = binreader.read();
      Serial1.write(c);
      while (!Serial1.available()){
        yield();
      }
      while (Serial1.available()){
        Serial1.read();
      }
      counter++;
      

      if (counter > 7000){
        counter = 0;
        clear_display();
        display.print("Installing: ");
        float percent = ((float)binreader.position() / (float)binreader.size()) * 100;
        display.print(percent);
        display.println("%");
        display.println("DO NOT POWER OFF");
        display.display();
      }
      
    }
    Serial1.flush();
    
    clear_display();
    display.println("Completing install");
    display.println("Please wait");
    display.println("DO NOT POWER OFF");
    display.display();
    int tocounter = 0;
    boolean did_update = false;
    boolean transfer_success = false;

    while (!did_update){
      String buff = Serial1.readStringUntil('\n');
      clear_display();
      if (!transfer_success){
        display.println("Verifying..");
      } else {
        display.println("Finalizing..");
      }
      display.println("DO NOT POWER OFF");
      display.print("Count:");
      tocounter++;
      display.println(tocounter);
      if (buff.indexOf(actual_hash) > -1){
        Serial.println("Update transfer verified");
        transfer_success = true;
        tocounter = 0;
      }
      if (transfer_success == true && tocounter > 3){
        Serial.println("Update complete");
        clear_display();
        display.println("Update complete");
        display.display();
        delay(4000);
        did_update = true;
        return true;
      }
      if (transfer_success == false && tocounter > 30){
        Serial.println("Update failed");
        clear_display();
        display.println("!FAILURE!");
        display.println("Try again");
        display.display();
        delay(7500);
        return false;
      }
      
    }
    
  }

  return true;
}

String hex_str(const unsigned char buf[], size_t len)
{
    String outstr;
    char outchr[6];
    for (size_t i = 0; i < len; i++) {
        if (buf[i] < 0xF) {
            sprintf(outchr, "0%x", buf[i]);
        } else {
            sprintf(outchr, "%x", buf[i]);
        }
        outstr = outstr + outchr;
    }
    return outstr;
}

boolean get_config_bool(String key, boolean def=false){
  String res = get_config_option(key);
  if (res == "true" || res == "yes"){
    return true;
  }
  if (res == "false" || res == "no"){
    return false;
  }
  return def;
}

String get_config_string(String key, String def=""){
  String res = get_config_option(key);
  if (res == ""){
    return def;
  }
  return res;
}

void boot_config(){
  //Load configuration variables and perform first time setup if required.
  Serial.println("Setting/loading boot config..");

  gps_baud_rate = get_config_int("gps_baud_rate", gps_baud_rate);
  rotate_display = get_config_bool("rotate_display", rotate_display);
  block_resets = get_config_bool("block_resets", block_resets);
  block_reconfigure = get_config_bool("block_reconfigure", block_reconfigure);
  web_timeout = get_config_int("web_timeout", web_timeout);
  gps_allow_stale_time = get_config_int("gps_allow_stale_time", gps_allow_stale_time);
  enforce_valid_binary_checksums = get_config_bool("enforce_checksums", enforce_valid_binary_checksums);

  preferences.begin("wardriver", false);
  bool firstrun = preferences.getBool("first", true);
  if (block_reconfigure){
    firstrun = false;
  }
  bool doreset = preferences.getBool("reset", false);
  if (block_resets){
    doreset = false;
  }
  bootcount = preferences.getULong("bootcount", 0);
  
  Serial.println("Loaded variables");

  DEVICE_TYPE = preferences.getShort("model", DEVICE_UNKNOWN);
  DEVICE_TYPE = identify_model();
  preferences.putShort("model", DEVICE_TYPE);
  
  if (doreset){
    Serial.println("resetting");
    clear_display();
    display.println("RESET");
    display.display();
    preferences.clear();
    delay(2000);
    firstrun = true;
  }

  if (!firstrun && !block_resets){
    preferences.putBool("reset", true);
    preferences.end();
    clear_display();
    display.println("Power cycle now");
    display.println("to factory reset");
    display.display();
    delay(1250);
    preferences.begin("wardriver", false);
    preferences.putBool("reset", false);
    clear_display();
  }

  if (firstrun){
    Serial.println("Begin first time setup..");
    int n = WiFi.scanNetworks(false,false,false,150);
    Serial.print("Scan result is ");
    Serial.println(n);
    Serial.print("Connect to: ");
    Serial.println(default_ssid);
    WiFi.softAP(default_ssid.c_str(), default_psk);
    IPAddress IP = WiFi.softAPIP();
    clear_display();
    display.println("Connect to:");
    display.println(default_ssid);
    display.println(IP);
    display.display();
    WiFiServer server(80);
    server.begin();
    boolean newline = false;
    String buff;

    while (firstrun){
      WiFiClient client = server.available();
      if (client){
        Serial.println("client connected");
        clear_display();
        display.println("Client connected");
        display.display();
      }
      
      while (client.connected()){
        if (client.available()){
          char c = client.read();
          Serial.write(c);
          buff += c;
          if (c == '\n'){
            if (newline){
              Serial.println("End of message");
              Serial.println(buff);
              client.println("HTTP/1.1 200 OK");
              client.println("Content-type: text/html");
              client.println("Connection: close");
              client.println();

              if (buff.indexOf("GET / HTTP") > -1) {
                Serial.println("Sending FTS homepage");
                client.print("<style>html{font-size:21px;text-align:center;padding:20px}input,select{padding:5px;width:100%;max-width:1000px}form{padding-top:10px}br{display:block;margin:5px 0}</style>");
                client.print("<html><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1, maximum-scale=1\"><h1>wardriver.uk " + device_type_string() + " by Joseph Hewitt</h1><h2>First time setup</h2>");
                client.print("Please provide the credentials of your WiFi network to get started.<br>");
                if (n > 0){
                  client.println("<script>function ssid_selected(obj){");
                  client.println("document.getElementById(\"ssid\").value = obj.value;");
                  client.println("}</script>");
                  client.println("<select onchange=\"ssid_selected(this)\" name=\"ssid\" id=\"ssid_select\">");
                  client.println("<option value=\"Select your network\">Select your network</option>");
                  for (int i = 0; i < n; i++) {
                    client.print("<option value=\"");
                    client.print(WiFi.SSID(i));
                    client.print("\">");
                    client.print(WiFi.SSID(i));
                    client.println("</option>");
                  }
                  client.println("</select>");
                }
                client.print("<form method=\"get\" action=\"/wifi\">SSID:<input type=\"text\" name=\"ssid\" id=\"ssid\"><br>PSK:<input type=\"password\" name=\"psk\" id=\"psk\"><br><input type=\"submit\" value=\"Submit\"></form>");
                client.print("<a href=\"/wifi?ssid=&psk=\">Continue without network</a>");
                client.print("<br><hr>Additional help is available at http://wardriver.uk<br>v");
                client.print(VERSION);
              }

              if (buff.indexOf("GET /wifi?") > -1){
                Serial.println("Got WiFi config");
                int startpos = buff.indexOf("?ssid=")+6;
                int endpos = buff.indexOf("&");
                String new_ssid = GP_urldecode(buff.substring(startpos,endpos));
                startpos = buff.indexOf("&psk=")+5;
                endpos = buff.indexOf(" HTTP");
                String new_psk = GP_urldecode(buff.substring(startpos,endpos));

                Serial.println(new_ssid);
                preferences.putString("ssid", new_ssid);
                Serial.println(new_psk);
                preferences.putString("psk", new_psk);

                client.print("<h1>Thanks!</h1>Please wait. <meta http-equiv=\"refresh\" content=\"1; URL=/step2\" />");
                
              }

              if (buff.indexOf("GET /step2 HTTP") > -1){
                Serial.println("Starting step2");
                client.print("<style>html{font-size:21px;text-align:center;padding:20px}input,select{padding:5px;width:100%;max-width:1000px}form{padding-top:10px}br{display:block;margin:5px 0}</style>");
                client.print("<html><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1, maximum-scale=1\"><h1>wardriver.uk " + device_type_string() + " by Joseph Hewitt</h1><h2>Fallback network setup</h2>");
                client.print("If your wardriver is unable to connect to your main network it will create a network which your device can join. Please provide some credentials for this fallback network.<br>");
                client.print("<form method=\"get\" action=\"/fbwifi\">SSID:<input type=\"text\" name=\"ssid\" id=\"ssid\"><br>PSK:<input type=\"password\" name=\"psk\" id=\"psk\"><br><input type=\"submit\" value=\"Submit\"></form>");
                client.print("<a href=\"/fbwifi?ssid=&psk=\">Continue without fallback network</a>");
                client.print("<br><hr>Additional help is available at http://wardriver.uk<br>v");
                client.print(VERSION);
              }

              if (buff.indexOf("GET /fbwifi?") > -1){
                Serial.println("Got WiFi (fallback) config");
                int startpos = buff.indexOf("?ssid=")+6;
                int endpos = buff.indexOf("&");
                String new_ssid = GP_urldecode(buff.substring(startpos,endpos));
                startpos = buff.indexOf("&psk=")+5;
                endpos = buff.indexOf(" HTTP");
                String new_psk = GP_urldecode(buff.substring(startpos,endpos));

                Serial.println(new_ssid);
                preferences.putString("fbssid", new_ssid);
                Serial.println(new_psk);
                preferences.putString("fbpsk", new_psk);
                client.print("<h1>Thanks!</h1>Your wardriver is now getting ready. <meta http-equiv=\"refresh\" content=\"1; URL=/done\" />");
              }

              if (buff.indexOf("GET /done HTTP") > -1){
                Serial.println("Setup complete");
                client.print("<h1>Setup complete!</h1>Your wardriver will now boot normally.");
                client.print("\n\r\n\r");
                client.flush();
                delay(800);
                client.stop();
                preferences.putBool("first", false);
                firstrun = false;
                break;
              }

              client.print("\n\r\n\r");
              client.flush();
              delay(5);
              client.stop();
              buff = "";
            }
            newline = true;
          } else {
            if (c != '\r'){
              newline = false;
            }
          }
          
        }
        
      }//client
    } //firstrun
    
  }
  setup_wifi();

  bootcount++;
  preferences.putULong("bootcount", bootcount);

  String con_ssid = GP_urldecode(preferences.getString("ssid",""));
  String con_psk = GP_urldecode(preferences.getString("psk",""));
  con_ssid = get_config_string("con_ssid", con_ssid);
  con_psk = get_config_string("con_psk", con_psk);
  String fb_ssid = GP_urldecode(preferences.getString("fbssid",""));
  String fb_psk = GP_urldecode(preferences.getString("fbpsk",""));
  fb_ssid = get_config_string("fb_ssid", fb_ssid);
  fb_psk = get_config_string("fb_psk", fb_psk);
  boolean created_network = false; //Set to true automatically when the fallback network is created.

  if (con_ssid != "" || fb_ssid != ""){
    Serial.print("Attempting to connect to WiFi");
    clear_display();
    display.print("Connecting");
    display.display();
    if (con_ssid != ""){
      WiFi.begin(con_ssid.c_str(), con_psk.c_str());
      
      int fcount = 0;
      while (WiFi.status() != WL_CONNECTED) {
        display.print(".");
        display.display();
        delay(100);
        Serial.print(".");
        fcount++;
        if (fcount > 50){
          clear_display();
          display.println("Cannot connect to WiFi");
          display.display();
          delay(500);
          if (fb_ssid != ""){
            WiFi.softAP(fb_ssid.c_str(), fb_psk.c_str());
            created_network = true;
          }
          break;
        }
      }
    } else {
      WiFi.softAP(fb_ssid.c_str(), fb_psk.c_str());
      created_network = true;
    }
    Serial.println();
    if (WiFi.status() == WL_CONNECTED || created_network == true){
      IPAddress fb_IP = WiFi.softAPIP();
      clear_display();
      if (!created_network){
        display.println("Attempting NTP sync");
        display.display();
        Serial.println("Connected, getting the time");
        configTime(0, 0, ntpServer);
        epoch = getTime();
        epoch_updated_at = millis();
        Serial.print("Time is now set to ");
        Serial.println(epoch);
        Serial.println("Continuing..");
      }
      unsigned long disconnectat = millis() + web_timeout;
      String buff;
      boolean newline = false;
      WiFiServer server(80);
      server.begin();
      while (WiFi.status() == WL_CONNECTED || created_network == true){
        clear_display();
        if (created_network){
          display.print("SSID:");
          display.println(fb_ssid);
          display.println(fb_IP);
        } else {
          display.println("Connected");
          display.println(WiFi.localIP());
        }
        display.print((disconnectat - millis())/1000);
        display.println("s until boot");
        display.print(device_type_string());
        display.display();
        
        if (millis() > disconnectat){
          Serial.println("Disconnecting");
          clear_display();
          display.println("Disconnecting");
          display.display();
          setup_wifi();
          delay(250);
          break;
        }
        WiFiClient client = server.available();
        if (client){
          unsigned long client_last_byte_at = millis();
          Serial.println("client connected, awaiting request");
          clear_display();
          display.println("Client connected");
          display.println("Awaiting request..");
          display.display();
          boolean first_byte = true;
          while (client.connected()){
            if (millis() - client_last_byte_at > HTTP_TIMEOUT_MS){
              Serial.println("HTTP client timeout, stopping");
              client.stop();
            }
            if (client.available()){
              client_last_byte_at = millis();
              if (first_byte){
                first_byte = false;
                Serial.println("Got first byte of request");
                display.println("..got one");
                display.println("Handling..");
                display.display();
              }
              char c = client.read();
              Serial.write(c);
              buff += c;
              if (c == '\n'){
                if (newline){
                  Serial.println("End of message");
                  Serial.println(buff);
                  client.println("HTTP/1.1 200 OK");
                  client.println("Connection: close");
                  
                  disconnectat = millis() + web_timeout;
    
                  if (buff.indexOf("GET / HTTP") > -1) {
                    client.println("Content-type: text/html");
                    client.println();
                    Serial.println("Sending homepage");
                    client.println("<style>html,td,th{font-size:21px;text-align:center;padding:20px }table{padding:5px;width:100%;max-width:1000px;}td, th{border: 1px solid #999;padding: 0.5rem;}</style>");
                    client.println("<html><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1, maximum-scale=1\"><h1>wardriver.uk " + device_type_string() + " by Joseph Hewitt</h1></head><table>");
                    client.println("<tr><th>Filename</th><th>File Size</th><th>Finish Date</th><th>Opt</th></tr>");
                    Serial.println("Scanning for files");
                    File dir = SD.open("/");
                    while (true) {
                      File entry =  dir.openNextFile();
                      if (!entry) {
                        break;
                      }
                      if (!entry.isDirectory()) {
                        String filename = entry.name();
                        if (filename.charAt(0) != '/'){
                          filename = "/";
                          filename.concat(entry.name());
                        }
                        Serial.print(filename);
                        Serial.print(" is ");
                        Serial.print(entry.size());
                        Serial.println(" bytes");
                        client.print("<tr><td>");
                        client.print("<a href=\"/download?fn=");
                        client.print(filename);
                        client.print("\">");
                        client.print(filename);
                        client.print("</a></td><td>");
                        client.print(entry.size()/1024);
                        client.print(" kb</td><td>");
                        client.print(get_latest_datetime(filename, false));
                        client.print("</td>");
                        client.print("<td>");
                        client.print("<a href=\"/delete?fn=");
                        client.print(filename);
                        client.print("\">");
                        client.print("DEL</a></td>");
                        client.println("</tr>");
                      }
                    }
                    client.print("</table><br><hr>");
                    client.print("<input type=\"file\" id=\"file\" /><button id=\"read-file\">Read File</button>");
                    client.print("<br><br>v");
                    client.println(VERSION);
                    //The very bottom of the homepage contains this JS snippet to send the current epoch value from the browser to the wardriver
                    //Also a snippet to force binary uploads instead of multipart.
                    client.println("<script>const ep=Math.round(Date.now()/1e3);var x=new XMLHttpRequest;x.open(\"GET\",\"time?v=\"+ep,!1),x.send(null); document.querySelector(\"#read-file\").addEventListener(\"click\",function(){if(\"\"==document.querySelector(\"#file\").value){alert(\"no file selected\");return}var e=document.querySelector(\"#file\").files[0],n=new FileReader;n.onload=function(n){let t=new XMLHttpRequest;var l=e.name;t.open(\"POST\",\"/fw?n=\"+l,!0),t.onload=e=>{window.location.href=\"/fwup\"};let r=new Blob([n.target.result],{type:\"application/octet-stream\"});t.send(r)},n.readAsArrayBuffer(e)});</script>");
                  }

                  if (buff.indexOf("GET /fwup") > -1){
                    client.println("Content-type: text/html");
                    client.println();
                    Serial.println("Sending FW update page");
                    client.println("<style>html,td,th{font-size:21px;text-align:center;padding:20px }table{padding:5px;width:100%;max-width:1000px;}td, th{border: 1px solid #999;padding: 0.5rem;}</style>");
                    client.println("<html><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1, maximum-scale=1\"><h1>wardriver.uk updater</h1></head><table>");
                    client.println("<tr><th>Filename</th><th>SHA256</th><th>Opt</th></tr>");
                    client.flush();
                    //In future lets iterate *.bin
                    if (SD.exists("/A.bin")){
                      String filehash = file_hash("/A.bin");
                      client.println("<tr><td>A.bin</td><td>" + filehash + "</td><td><a href=\"/fwins?h=" + filehash + "&n=/A.bin\">Install</a></td></tr>");
                    }
                    if (SD.exists("/B.bin")){
                      String filehash = file_hash("/B.bin");
                      client.println("<tr><td>B.bin</td><td>" + filehash + "</td><td><a href=\"/fwins?h=" + filehash + "&n=/B.bin\">Install</a></td></tr>");
                    }
                    client.println("</tr>");
                  }

                  if (buff.indexOf("GET /fwins") > -1) {
                    int startpos = buff.indexOf("?h=") + 3;
                    int endpos = buff.indexOf("&");
                    String expect_hash = GP_urldecode(buff.substring(startpos, endpos));
                    startpos = buff.indexOf("&n=") + 3;
                    endpos = buff.indexOf(" HTTP");
                    String fw_filename = GP_urldecode(buff.substring(startpos, endpos));

                    client.println("Content-type: text/html");
                    client.println();
                    Serial.print("Firmware install requested: ");
                    Serial.print(fw_filename);
                    Serial.print(expect_hash);

                    if (expect_hash.length() > 0 && SD.exists(fw_filename)) {
                      Serial.println("Will install firmware");
                      client.print("<h1>Firmware will now be installed. Check the wardriver LCD for progress</h1>");
                      client.print("\n\r\n\r");
                      client.flush();
                      delay(5);
                      client.stop();
                      install_firmware(fw_filename, expect_hash);
                    } else {
                      client.print("<h1>Error verifying update</h1>");
                    }
                  }

                  if (buff.indexOf("POST /fw") > -1){
                    Serial.println("Incoming firmware");
                    int startpos = buff.indexOf("?n=")+3;
                    int endpos = buff.indexOf(" ",startpos);
                    String bin_filename = buff.substring(startpos,endpos);
                    Serial.println(bin_filename);
                    String newname = "/other.bin";
                    if (bin_filename.startsWith("A")){
                      newname = "/A.bin";
                    }
                    if (bin_filename.startsWith("B")){
                      newname = "/B.bin";
                    }

                    Serial.println(newname);
                    if (SD.exists(newname)){
                      SD.remove(newname);
                    }
                    File binwriter = SD.open(newname, FILE_WRITE);

                    //Setup a hash context, and somewhere to keep the output.
                    unsigned char genhash[32];
                    mbedtls_sha256_context ctx;
                    mbedtls_sha256_init(&ctx);
                    mbedtls_sha256_starts(&ctx, 0);

                    unsigned long fw_last_byte = millis();
                    byte bbuf[2] = {0x00, 0x00};
                    while (1) {
                      if (client.available()){
                        byte c = client.read();
                        binwriter.write(c);
                        bbuf[0] = c;
                        mbedtls_sha256_update(&ctx, bbuf, 1);
                        
                        fw_last_byte = millis();
                      }
                      if (millis() - fw_last_byte > 4000){
                        Serial.println("Done");
                        mbedtls_sha256_finish(&ctx, genhash);
                        print_hex("HASHED", genhash, sizeof genhash);
                        binwriter.flush();
                        binwriter.close();
                        break;
                      }
                    } //Firmware update loop
                  }

                  if (buff.indexOf("GET /time?") > -1){
                    client.println("Content-type: text/html");
                    Serial.println("Got time from browser");
                    int startpos = buff.indexOf("?v=")+3;
                    int endpos = buff.indexOf(" ",startpos);
                    String newtime_str = buff.substring(startpos,endpos);
                    unsigned long newtime = atol(newtime_str.c_str());
                    if (epoch < YEAR_2020){
                      //if the epoch value is set to something before 2020, we can be quite sure it is inaccurate.
                      if (newtime > YEAR_2020){
                        //A very basic validity test for the datetime value issued by the client.
                        Serial.println("Current clock is inaccurate, using time from client");
                        epoch = newtime;
                        Serial.print("epoch is now ");
                        Serial.println(epoch);
                        epoch_updated_at = millis();
                      }
                    }
                  }

                  if (buff.indexOf("GET /delete?") > -1) {
                    Serial.println("File delete pre-request");
                    int startpos = buff.indexOf("?fn=")+4;
                    int endpos = buff.indexOf(" ",startpos);
                    String filename = buff.substring(startpos,endpos);
                    Serial.println(filename);
                    if (!SD.exists(filename)){
                      Serial.println("file does not exist");
                      client.println("Content-type: text/html");
                      client.println();
                      client.print("<h1>File not found </h1>");
                      client.println("<meta http-equiv=\"refresh\" content=\"1; URL=/\" />");
                    } else {
                      client.println("Content-type: text/html");
                      client.println();
                      client.print("<style>html,td,th{font-size:21px;text-align:center;padding:20px}</style><html>");
                      client.print("<h1>Confirm delete of ");
                      client.print(filename);
                      client.print("<br><a href=\"/\">Cancel</a></h1><br><h2><a href=\"/confirmdelete?fn=");
                      client.print(filename);
                      client.print("\">DELETE</a></h2></html>");
                    }
                  }

                  if (buff.indexOf("GET /confirmdelete?") > -1) {
                    Serial.println("File delete request");
                    int startpos = buff.indexOf("?fn=")+4;
                    int endpos = buff.indexOf(" ",startpos);
                    String filename = buff.substring(startpos,endpos);
                    Serial.println(filename);
                    SD.remove(filename);
                    client.println("Content-type: text/html");
                    client.println();
                    client.print("<h1>Deleted ");
                    client.print(filename);
                    client.println("</h1>");
                    client.println("<meta http-equiv=\"refresh\" content=\"1; URL=/\" />");
                    
                  }

                  if (buff.indexOf("GET /download?") > -1) {
                    Serial.println("File download request");
                    int startpos = buff.indexOf("?fn=")+4;
                    int endpos = buff.indexOf(" ",startpos);
                    String filename = buff.substring(startpos,endpos);
                    Serial.println(filename);

                    File reader = SD.open(filename, FILE_READ);
                    if (!reader){
                      client.println("Content-type: text/html");
                      client.println();
                      client.print("Invalid file");
                      client.flush();
                      delay(5);
                      client.stop();
                      buff = "";
                      break;
                    }
                    if (reader){
                      client.println("Content-type: text/csv");
                      Serial.println("Sending file");
                      client.print("Content-Disposition: attachment; filename=\"");
                      client.print(get_latest_datetime(filename, true));
                      client.print("_");
                      client.print(chip_id);
                      client.print("_");
                      client.print(filename);
                      client.println("\"");
                      client.print("Content-Length: ");
                      client.print(reader.size());
                      client.println();
                      client.println();
                      client.flush();
                      delay(2);
                      client.write(reader);
                    }
                  }
    
                  client.print("\n\r\n\r");
                  client.flush();
                  delay(5);
                  client.stop();
                  buff = "";
                  disconnectat = millis() + web_timeout;
                }
                newline = true;
              } else {
                if (c != '\r'){
                  newline = false;
                }
              }
              
            } else {
              if (created_network){
                display.print("SSID:");
                display.println(fb_ssid);
                display.println(fb_IP);
              } else {
                display.println("Connected");
                display.println(WiFi.localIP());
              }
              display.print((disconnectat - millis())/1000);
              display.println("s until boot");
              display.print(device_type_string());
              display.display();
            }
          } //while client connected
        } //if client
      } //while wifi
    } //if wifi
  }
  preferences.end();
}

void setup() {
    setup_wifi();
    delay(500);
    
    Serial.begin(115200);
    Serial.print("Starting v");
    Serial.println(VERSION);

    for(int i=0; i<17; i=i+8) {
      chip_id |= ((ESP.getEfuseMac() >> (40 - i)) & 0xff) << i;
    }

    Serial.print("Chip ID: ");
    Serial.println(chip_id);

    default_ssid.concat(" - ");
    default_ssid.concat(chip_id);
    default_ssid.remove(default_ssid.length()-3);
    
    Serial1.begin(115200,SERIAL_8N1,27,14);

    if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) { // Address 0x3C for 128x32
      Serial.println(F("SSD1306 allocation failed"));
    }
    if (!rotate_display){
      display.setRotation(2);
    } else {
      display.setRotation(0);
    }
    display.clearDisplay();
    display.setTextSize(1);      // Normal 1:1 pixel scale
    display.setTextColor(WHITE); // Draw white text
    display.setCursor(0, 0);     // Start at top-left corner
    display.cp437(true);         // Use full 256 char 'Code Page 437' font
    display.println("Starting");
    display.print("Version ");
    display.println(VERSION);
    display.display();
    
    int reset_reason = esp_reset_reason();
    if (reset_reason != ESP_RST_POWERON){
      clear_display();
      display.println("Unexpected reset");
      display.print("Code ");
      display.println(reset_reason);
      display.print("Version ");
      display.println(VERSION);
      display.display();
      delay(4000);
    }
    delay(1500);
  
    if(!SD.begin()){
        Serial.println("SD Begin failed!");
        clear_display();
        display.println("SD Begin failed!");
        display.display();
        delay(4000);
    }
    uint8_t cardType = SD.cardType();
    if(cardType == CARD_NONE){
        Serial.println("No SD card attached!");
        clear_display();
        display.println("No SD Card!");
        display.display();
        delay(10000);
    }
  
    Serial.print("SD Card Type: ");
    if(cardType == CARD_MMC){
        Serial.println("MMC");
    } else if(cardType == CARD_SD){
        Serial.println("SDSC");
    } else if(cardType == CARD_SDHC){
        Serial.println("SDHC");
    } else {
        Serial.println("UNKNOWN");
    }
  
    uint64_t cardSize = SD.cardSize() / (1024 * 1024);
    Serial.printf("SD Card Size: %lluMB\n", cardSize);

    while (!filewriter){
      filewriter = SD.open("/test.txt", FILE_APPEND);
      if (!filewriter){
        Serial.println("Failed to open file for writing.");
        clear_display();
        display.println("SD File open failed!");
        display.display();
        delay(1000);
      }
    }
    int wrote = filewriter.print("\n_BOOT_");
    filewriter.print(VERSION);
    filewriter.print(", ut=");
    filewriter.print(micros());
    filewriter.print(", rr=");
    filewriter.print(reset_reason);
    filewriter.print(", id=");
    filewriter.print(chip_id);
    filewriter.flush();
    if (wrote < 1){
      while(true){
        Serial.println("Failed to write to SD card!");
        clear_display();
        display.println("SD Card write failed!");
        display.display();
        delay(4000);
      }
    }
    
    boot_config();
    setup_wifi();

    Serial2.begin(gps_baud_rate);

    Serial.print("This device: ");
    Serial.println(device_type_string());
    
    filewriter.print(", bc=");
    filewriter.print(bootcount);
    filewriter.print(", ep=");
    filewriter.print(epoch);
    filewriter.flush();
    filewriter.close();

    if (SD.exists("/bl.txt")){
      Serial.println("Opening blocklist");
      File blreader;
      blreader = SD.open("/bl.txt", FILE_READ);
      byte i = 0;
      byte ci = 0;
      while (blreader.available()){
        char c = blreader.read();
        if (c == '\n' || c == '\r'){
          use_blocklist = true;
          if (ci != 0){
            i += 1;
          }
          ci = 0;
        } else {
          block_list[i].characters[ci] = c;
          ci += 1;
          if (ci >= blocklist_str_len){
            Serial.println("Blocklist line too long!");
            ci = 0;
          }
        }
      }
      blreader.close();
    }

    if (!use_blocklist){
      Serial.println("Not using a blocklist");
    }
    
    Serial.println("Opening destination file for writing");

    String filename = "/wd3-";
    filename = filename + bootcount;
    filename = filename + ".csv";
    Serial.println(filename);
    filewriter = SD.open(filename, FILE_APPEND);
    filewriter.print("WigleWifi-1.4,appRelease=" + VERSION + ",model=wardriver.uk " + device_type_string() + ",release=1.0.0,device=wardriver.uk " + device_type_string() + ",display=i2c LCD,board=wardriver.uk " + device_type_string() + ",brand=JHewitt\nMAC,SSID,AuthMode,FirstSeen,Channel,RSSI,CurrentLatitude,CurrentLongitude,AltitudeMeters,AccuracyMeters,Type\n");
    filewriter.flush();
    
    clear_display();
    display.println("Starting main..");
    display.display();

    xTaskCreatePinnedToCore(
      primary_scan_loop, /* Function to implement the task */
      "primary_scan_loop", /* Name of the task */
      10000,  /* Stack size in words */
      NULL,  /* Task input parameter */
      3,  /* Priority of the task */
      &primary_scan_loop_handle,  /* Task handle. */
      0); /* Core where the task should run */
}

void primary_scan_loop(void * parameter){
  //This core will be dedicated entirely to WiFi scanning in an infinite loop.
  while (true){
    disp_wifi_count = wifi_count;
    wifi_count = 0;
    setup_wifi();
    for(int scan_channel = 1; scan_channel < 14; scan_channel++){
      yield();
      //scanNetworks(bool async, bool show_hidden, bool passive, uint32_t max_ms_per_chan, uint8_t channel)
      int n = WiFi.scanNetworks(false,true,false,110,scan_channel);
      if (n > 0){
        wifi_count = wifi_count + n;
        for (int i = 0; i < n; i++) {
          if (seen_mac(WiFi.BSSID(i))){
            //Skip any APs which we've already logged.
            continue;
          }
          //Save the AP MAC inside the history buffer so we know it's logged.
          save_mac(WiFi.BSSID(i));

          String ssid = WiFi.SSID(i);
          ssid.replace(",","_");
          if (use_blocklist){
            if (is_blocked(ssid)){
              Serial.print("BLOCK: ");
              Serial.println(ssid);
              wifi_block_at = millis();
              continue;
            }
            String tmp_mac_str = WiFi.BSSIDstr(i).c_str();
            tmp_mac_str.toUpperCase();
            if (is_blocked(tmp_mac_str)){
              Serial.print("BLOCK: ");
              Serial.println(tmp_mac_str);
              wifi_block_at = millis();
              continue;
            }
          }
          
          filewriter.printf("%s,%s,%s,%s,%d,%d,%s,WIFI\n", WiFi.BSSIDstr(i).c_str(), ssid.c_str(), security_int_to_string(WiFi.encryptionType(i)).c_str(), dt_string().c_str(), WiFi.channel(i), WiFi.RSSI(i), gps_string().c_str());
         
        }
      }
      filewriter.flush();
    }
    yield();
  }
}

void lcd_show_stats(){
  //Clear the LCD then populate it with stats about the current session.
  boolean ble_did_block = false;
  boolean wifi_did_block = false;
  if (millis() - wifi_block_at < 30000){
    wifi_did_block = true;
  }
  if (millis() - ble_block_at < 30000){
    ble_did_block = true;
  }
  clear_display();
  display.print("WiFi:");
  display.print(disp_wifi_count);
  if (wifi_did_block){
    display.print("X");
  }
  if (int(temperature) != 0){
    display.print(" Temp:");
    display.print(temperature);
    display.print("c");
  }
  display.println();
  if (nmea.getHDOP() < 250 && nmea.getNumSatellites() > 0){
    display.print("HDOP:");
    display.print(nmea.getHDOP());
    display.print(" Sats:");
    display.print(nmea.getNumSatellites());
    display.println(nmea.getNavSystem());
  } else {
    display.print("No GPS: ");
    struct coordinates gsm_loc = gsm_get_current_position();
    if (gsm_loc.acc > 0){
      display.println("GSM pos OK");
    } else {
      display.println("No GSM pos");
    }
  }
  if (b_working){
  display.print("BLE:");
  display.print(ble_count);
  if (ble_did_block){
    display.print("X");
  }
  display.print(" GSM:");
  display.println(disp_gsm_count);
  } else {
    display.println("ESP-B NO DATA");
  }
  display.println(dt_string());
  display.display();
  if (gsm_count > 0){
    disp_gsm_count = gsm_count;
    gsm_count = 0;
  }
}

void loop(){
  //The main loop for the second core; handles GPS, "Side B" communication, and LCD refreshes.
  update_epoch();
  while (Serial2.available()){
    char c = Serial2.read();
    if (nmea.process(c)){
      if (nmea.isValid()){
        lastgps = millis();
        update_epoch();
      }
    }
  }

  if (Serial1.available()){
    String bside_buffer = "";
    while (true){
      char c;
      if (Serial1.available()){
        c = Serial1.read();
        if (c != '\n'){
          bside_buffer += c;
        } else {
          break;
        }
      }
    }
    Serial.println(bside_buffer);
    String towrite = "";
    towrite = parse_bside_line(bside_buffer);
    if (towrite.length() > 1){
      Serial.println(towrite);
      filewriter.print(towrite);
      filewriter.print("\n");
      filewriter.flush();
    }
    
  }
  if (gsm_count > 0){
    disp_gsm_count = gsm_count;
  }
  if (lcd_last_updated == 0 || millis() - lcd_last_updated > 1000){
    lcd_show_stats();
    lcd_last_updated = millis();
  }
}

void save_cell(struct cell_tower tower){
  //Save a cell_tower struct into the recently seen array.
  if (cell_history_cursor >= cell_history_len){
    cell_history_cursor = 0;
  }
  cell_history[cell_history_cursor] = tower;

  cell_history_cursor++;

  Serial.print("Tower len ");
  Serial.println(cell_history_cursor);
}

boolean is_blocked(String test_str){
  if (!use_blocklist){
    return false;
  }
  unsigned int test_str_len = test_str.length();
  if (test_str_len == 0){
    return false;
  }
  if (test_str_len > blocklist_str_len){
    Serial.print("Refusing to blocklist check due to length: ");
    Serial.println(test_str);
    return false;
  }
  for (byte i=0; i<blocklist_len; i++){
    boolean matched = true;
    for (byte ci=0; ci<test_str_len; ci++){
      if (test_str.charAt(ci) != block_list[i].characters[ci]){
        matched = false;
        break;
      }
    }
    if (matched){
      Serial.print("Blocklist match: ");
      Serial.println(test_str);
      return true;
    }
  }
  return false;
}

void replace_cell(struct cell_tower tower1, struct cell_tower tower2){
  //Provide two cell_tower structs; the first will be looked up in the history buffer and the second will replace the found object.
  //Only mcc, mnc, lac, and cellid are used for comparisons. Use this function to update the coordinates part of the tower object.

  for (int x = 0; x < cell_history_len; x++){
    if (cell_cmp(tower1, cell_history[x])){
      cell_history[x] = tower2;
    }
  }
}

void print_cell(struct cell_tower tower){
  //Provide a cell_tower struct to be printed over serial
  Serial.print(tower.mcc);
  Serial.print(":");
  Serial.print(tower.mnc);
  Serial.print(":");
  Serial.print(tower.lac);
  Serial.print(":");
  Serial.print(tower.cellid);
  Serial.print("@");
  Serial.print(tower.pos.lat, 6);
  Serial.print(",");
  Serial.print(tower.pos.lon, 6);
  Serial.print(",");
  Serial.println(tower.pos.acc);
  //printf ;)
}

boolean cell_cmp(struct cell_tower tower, struct cell_tower tower2){
  //Provide 2 cell_tower structs to return a boolean indicating if they match or not (based on mcc, mnc, cellid, and lac)
  if (tower.mcc == tower2.mcc and tower.mnc == tower2.mnc and tower.cellid == tower2.cellid and tower.lac == tower2.lac){
    return true;
  }
  return false;
}

boolean seen_cell(struct cell_tower tower){
  //Return true if the cell_tower provided is in the recently seen array.
  //This will also update the seenat and strength values within the recently seen array.
  for (int x = 0; x < cell_history_len; x++){
    if (cell_cmp(tower, cell_history[x])){
      cell_history[x].seenat = millis();
      cell_history[x].strength = tower.strength;
      return true;
    }
  }
  return false;
}

struct cell_tower get_tower(struct cell_tower tower){
  //Provide a cell_tower object (mcc,mnc,lac,cellid) and the full object in RAM will be returned including seenat and pos.
  //Will return a zero'd object if the tower isn't in RAM. Call seen_cell first to be sure the object you need is actually in RAM.

  struct coordinates empty_pos;
  empty_pos = (coordinates){.lat = 0, .lon = 0, .acc = 0};
  struct cell_tower toreturn;
  toreturn = (cell_tower){.mcc = 0, .mnc = 0, .lac = 0, .cellid = 0, .seenat = 0, .strength = 0, .pos = empty_pos};

  for (int x = 0; x < cell_history_len; x++){
    if (cell_cmp(tower, cell_history[x])){
      toreturn = cell_history[x];
    }
  }
  
  return toreturn;
}

void save_mac(unsigned char* mac){
  //Save a MAC address into the recently seen array.
  if (mac_history_cursor >= mac_history_len){
    mac_history_cursor = 0;
  }
  struct mac_addr tmp;
  for (int x = 0; x < 6 ; x++){
    tmp.bytes[x] = mac[x];
  }

  mac_history[mac_history_cursor] = tmp;
  mac_history_cursor++;
  Serial.print("Mac len ");
  Serial.println(mac_history_cursor);
}

boolean seen_mac(unsigned char* mac){
  //Return true if this MAC address is in the recently seen array.

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
  //Print a mac_addr struct nicely.
  for (int x = 0; x < 6 ; x++){
    Serial.print(mac.bytes[x],HEX);
    Serial.print(":");
  }
}

boolean mac_cmp(struct mac_addr addr1, struct mac_addr addr2){
  //Return true if 2 mac_addr structs are equal.
  for (int y = 0; y < 6 ; y++){
    if (addr1.bytes[y] != addr2.bytes[y]){
      return false;
    }
  }
  return true;
}

String parse_bside_line(String buff){
  //Provide a String which contains a line from ESP32 side B.
  //A String will be returned which should be written to the Wigle CSV.

  /*
  I am aware that this code isn't great..
  I'm not a huge fan of Strings, especially not this many temporary Strings but it's a quick way to get things working.
  Hopefully the large amount of RAM on the ESP32 will prevent heap fragmentation issues but I'll do extended uptime tests to be sure.
  
  This code sucessfully ran for 48 hours straight starting on the 1st September 2021.
  Multiple tests have since completed with no crashes or issues. While it looks scary, it seems to be stable.
  */
  
  String out = "";
  if (buff.indexOf("BL,") > -1) {
    
    int startpos = buff.indexOf("BL,")+3;
    int endpos = buff.indexOf(",",startpos);
    String rssi = buff.substring(startpos,endpos);

    startpos = endpos+1;
    endpos = buff.indexOf(",",startpos);
    String mac_str = buff.substring(startpos,endpos);

    startpos = endpos+1;
    String ble_name = buff.substring(startpos,buff.length()-1);

    unsigned char mac_bytes[6];
    int values[6];

    if (is_blocked(ble_name) || is_blocked(mac_str)){
      out = "";
      Serial.print("BLOCK: ");
      Serial.print(ble_name);
      Serial.print(" / ");
      Serial.println(mac_str);
      ble_block_at = millis();
      return out;
    }

    if (6 == sscanf(mac_str.c_str(), "%x:%x:%x:%x:%x:%x%*c", &values[0], &values[1], &values[2], &values[3], &values[4], &values[5])){
      for(int i = 0; i < 6; ++i ){
          mac_bytes[i] = (unsigned char) values[i];
      }
    
      if (!seen_mac(mac_bytes)){
        save_mac(mac_bytes);
        //Save to SD?
        Serial.print("NEW BLE DEVICE: ");
        Serial.println(buff);

        mac_str.toUpperCase();
        out = mac_str + "," + ble_name + "," + "[BLE]," + dt_string() + ",0," + rssi + "," + gps_string() + ",BLE";
      }
    }
  }

  if (buff.indexOf("WI0,") > -1) {
    //WI0,SSID,6,-88,5,00:00:00:00:00:00
    int startpos = buff.indexOf("WI0,")+4;
    int endpos = buff.indexOf(",",startpos);
    String ssid = buff.substring(startpos,endpos);

    startpos = endpos+1;
    endpos = buff.indexOf(",",startpos);
    String channel = buff.substring(startpos,endpos);

    startpos = endpos+1;
    endpos = buff.indexOf(",",startpos);
    String rssi = buff.substring(startpos,endpos);

    startpos = endpos+1;
    endpos = buff.indexOf(",",startpos);
    String security_raw = buff.substring(startpos,endpos);

    startpos = endpos+1;
    endpos = startpos+17;
    String mac_str = buff.substring(startpos,endpos);
    mac_str.toUpperCase();

    if (is_blocked(ssid) || is_blocked(mac_str)){
      out = "";
      Serial.print("BLOCK: ");
      Serial.print(ssid);
      Serial.print(" / ");
      Serial.println(mac_str);
      wifi_block_at = millis();
      return out;
    }

    unsigned char mac_bytes[6];
    int values[6];

    if (6 == sscanf(mac_str.c_str(), "%x:%x:%x:%x:%x:%x%*c", &values[0], &values[1], &values[2], &values[3], &values[4], &values[5])){
      for(int i = 0; i < 6; ++i ){
          mac_bytes[i] = (unsigned char) values[i];
      }
    
      if (!seen_mac(mac_bytes)){
        save_mac(mac_bytes);
        //Save to SD?
        Serial.print("NEW WIFI (SIDE B): ");
        Serial.println(buff);

        String authtype = security_int_to_string((int) security_raw.toInt());
        
        out = mac_str + "," + ssid + "," + authtype + "," + dt_string() + "," + channel + "," + rssi + "," + gps_string() + ",WIFI";
      }
    }
    
  }

  if (buff.indexOf("GSM,") > -1) {
    //GSM,Operator:"vodafone",MCC:000,MNC:00,Rxlev:12,Cellid:FF00,Arfcn:4,Lac:0000,Bsic:00
    //^Some values censored for my privacy^ :)

    gsm_count++;
    
    int startpos = buff.indexOf("Operator:\"")+10;
    int endpos = buff.indexOf("\"",startpos);
    String cell_operator = buff.substring(startpos,endpos);

    startpos = buff.indexOf("MCC:")+4;
    endpos = buff.indexOf(",",startpos);
    String mcc = buff.substring(startpos,endpos);

    startpos = buff.indexOf("MNC:")+4;
    endpos = buff.indexOf(",",startpos);
    String mnc = buff.substring(startpos,endpos);

    startpos = buff.indexOf("Rxlev:")+6;
    endpos = buff.indexOf(",",startpos);
    String rxlev = buff.substring(startpos,endpos);

    startpos = buff.indexOf("Cellid:")+7;
    endpos = buff.indexOf(",",startpos);
    String cellid = buff.substring(startpos,endpos);

    startpos = buff.indexOf("Arfcn:")+6;
    endpos = buff.indexOf(",",startpos);
    String arfcn = buff.substring(startpos,endpos);

    startpos = buff.indexOf("Lac:")+4;
    endpos = buff.indexOf(",",startpos);
    String lac = buff.substring(startpos,endpos);

    startpos = buff.indexOf("Bsic:")+5;
    endpos = buff.length()-1;
    String bsic = buff.substring(startpos,endpos);

    int lac_int;
    int cellid_int;
    int rssi_int;
    int arfcn_int;

    lac_int = (int) strtol(lac.c_str(), 0, 16);
    cellid_int = (int) strtol(cellid.c_str(), 0, 16);
    rssi_int = (int) rxlev.toInt();
    rssi_int = rssi_int - 80;
    arfcn_int = (int) arfcn.toInt();

    String mccmnc = mcc + mnc;
    String wigle_cell_key = mccmnc.substring(0,7);
    wigle_cell_key += "_";
    wigle_cell_key += lac_int;
    wigle_cell_key += "_";
    wigle_cell_key += cellid_int;

    struct coordinates cell_pos;
    cell_pos = (coordinates){.lat = 0, .lon = 0, .acc = -255};

    struct cell_tower tower;
    tower = (cell_tower){.mcc = (int) mcc.toInt(), .mnc = (int) mnc.toInt(), .lac = (int) lac.toInt(), .cellid = cellid_int, .seenat = millis(), .strength = rssi_int, .pos = cell_pos};
    
    if (!seen_cell(tower)){
      //Get the location for this newly-seen tower if the GPS is almost stale.
      if (millis() > lastgps + gps_allow_stale_time/2 || lastgps == 0){
        cell_pos = get_cell_pos(wigle_cell_key);
        tower.pos = cell_pos;
      }
      
      out = wigle_cell_key + "," + cell_operator + ",GSM;" + mccmnc + "," + dt_string() + "," + arfcn + "," + rssi_int + "," + gps_string() + ",GSM";
      save_cell(tower);
    } else {
      //We've seen this tower, get the full object so we can see if anything is missing
      struct cell_tower ram_tower = get_tower(tower);
      if (ram_tower.pos.acc == -255){
        //We've never tried to get the location of this tower. Consider doing that now.
        if (millis() > lastgps + gps_allow_stale_time/2 || lastgps == 0){
          //Get the position for this tower and replace what is currently in RAM.
          cell_pos = get_cell_pos(wigle_cell_key);
          tower.pos = cell_pos;
          replace_cell(ram_tower, tower);
        }
      }
    }
  }

  if (buff.indexOf("TEMP,") > -1) {
    int startpos = buff.indexOf("TEMP,")+5;
    String temp = buff.substring(startpos);
    temperature = temp.toFloat();
    Serial.print("Temperature = ");
    Serial.println(temperature);
  }

  if (buff.indexOf("BLC,") > -1) {
    int startpos = buff.indexOf("BLC,")+4;
    String blc = buff.substring(startpos);
    ble_count = blc.toInt();
    Serial.print("Bluetooth count = ");
    Serial.println(ble_count);
    b_working = true;
  }
  
  return out;
}

String dt_string(){
  //Return a datetime String using local timekeeping and GPS data.
  time_t now = epoch;
  struct tm ts;
  char buf[80];

  ts = *localtime(&now);
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &ts);
  String out = String(buf);
  
  Serial.print("New dt_str: ");
  Serial.println(out);
  
  return out;
}

String dt_string_from_gps(){
  //Return a datetime String using GPS data only.
  String datetime = "";
  if (nmea.isValid() && nmea.getYear() > 0){
    datetime += nmea.getYear();
    datetime += "-";
    datetime += nmea.getMonth();
    datetime += "-";
    datetime += nmea.getDay();
    datetime += " ";
    datetime += nmea.getHour();
    datetime += ":";
    datetime += nmea.getMinute();
    datetime += ":";
    datetime += nmea.getSecond();
    last_dt_string = datetime;
  } else if (lastgps + gps_allow_stale_time > millis()) {
    datetime = last_dt_string;
  }
  return datetime;
}

String gps_string(){
  //Return a String which can be used in a Wigle CSV line to show the current position.
  //This uses data from GPS and GSM tower locations.
  //output: lat,lon,alt,acc
  String out = "";
  long alt = 0;
  if (!nmea.getAltitude(alt)){
    alt = 0;
  }
  float altf = (float)alt / 1000;

  String lats = String((float)nmea.getLatitude()/1000000, 7);
  String lons = String((float)nmea.getLongitude()/1000000, 7);
  if (nmea.isValid() && nmea.getHDOP() <= 250){
    last_lats = lats;
    last_lons = lons;
  }

  if (nmea.getHDOP() > 250){
    lats = "";
    lons = "";
  }

  //HDOP returned here is in tenths and needs dividing by 10 to make it 'true'.
  //We're using this as a very basic estimate of accuracy by multiplying HDOP with the precision of the GPS module (2.5)
  //This isn't precise at all, but is a very rough estimate to your GPS accuracy.
  float accuracy = ((float)nmea.getHDOP()/10);
  accuracy = accuracy * 2.5;

  if (!nmea.isValid()){
    lats = "";
    lons = "";
    accuracy = 1000;
    if (lastgps + gps_allow_stale_time > millis()){
      lats = last_lats;
      lons = last_lons;
      accuracy = 5 + (millis() - lastgps) / 100;
    } else {
      Serial.println("Bad GPS, using GSM location");
      struct coordinates pos = gsm_get_current_position();
      if (pos.acc > 0){
        lats = String(pos.lat, 6);
        lons = String(pos.lon, 6);
        accuracy = pos.acc;
      }
    }
  }

  //The module we are using has a precision of 2.5m, accuracy can never be better than that.
  if (accuracy <= 2.5){
    accuracy = 2.5;
  }

  out = lats + "," + lons + "," + altf + "," + accuracy;
  return out;
}

String security_int_to_string(int security_type){
  //Provide a security type int from WiFi.encryptionType(i) to convert it to a String which Wigle CSV expects.
  String authtype = "";
  switch (security_type){
    case WIFI_AUTH_OPEN:
      authtype = "[OPEN]";
      break;
  
    case WIFI_AUTH_WEP:
      authtype = "[WEP]";
      break;
  
    case WIFI_AUTH_WPA_PSK:
      authtype = "[WPA_PSK]";
      break;
  
    case WIFI_AUTH_WPA2_PSK:
      authtype = "[WPA2_PSK]";
      break;
  
    case WIFI_AUTH_WPA_WPA2_PSK:
      authtype = "[WPA_WPA2_PSK]";
      break;
  
    case WIFI_AUTH_WPA2_ENTERPRISE:
      authtype = "[WPA2]";
      break;

    //Requires at least v2.0.0 of https://github.com/espressif/arduino-esp32/
    case WIFI_AUTH_WPA3_PSK:
      authtype = "[WPA3_PSK]";
      break;

    case WIFI_AUTH_WPA2_WPA3_PSK:
      authtype = "[WPA2_WPA3_PSK]";
      break;

    case WIFI_AUTH_WAPI_PSK:
      authtype = "[WAPI_PSK]";
      break;
        
    default:
      authtype = "[UNDEFINED]";
  }

  return authtype;
}

String get_latest_datetime(String filename, boolean date_only){
  //Provide a filename to get the highest datetime from that Wigle CSV file on the SD card.
  Serial.print("Getting latest dt from ");
  Serial.println(filename);
  String buff = "";
  
  File reader = SD.open(filename, FILE_READ);
  int seekto = reader.size()-512;
  if (seekto < 1){
    seekto = 0;
  }
  reader.seek(seekto);
  int ccount = 0;
  while (reader.available()){
    char c = reader.read();
    if (c == '\n' || c == '\r'){
      if (ccount == 10){
        int startpos = buff.indexOf("],2");
        int endpos = buff.indexOf(",",startpos+3);
        if (startpos > 0 && endpos > 0){
          String dt = buff.substring(startpos+2,endpos);
          Serial.print("Got: ");
          Serial.println(dt);
          if (date_only){
            int spacepos = dt.indexOf(" ");
            String new_dt = dt.substring(0,spacepos);
            dt = new_dt;
            Serial.print("Stripped to: ");
            Serial.println(dt);
          }
          return dt;
        } 
      }
      ccount = 0;
      buff = "";
    } else {
      buff.concat(c);
      if (c == ','){
        ccount++;
      }
    }
  }
  return "";
}

void update_epoch(){
  //Update the global epoch variable using the GPS time source.
  String gps_dt = dt_string_from_gps();
  if (!nmea.isValid() || lastgps == 0 || gps_dt.length() < 5){
    unsigned int tdiff_sec = (millis()-epoch_updated_at)/1000;
    if (tdiff_sec < 1){
      return;
    }
    epoch += tdiff_sec;
    epoch_updated_at = millis();
    Serial.print("Added ");
    Serial.print(tdiff_sec);
    Serial.println(" seconds to epoch");
    return;
  }
  
  struct tm tm;

  strptime(gps_dt.c_str(), "%Y-%m-%d %H:%M:%S", &tm );
  epoch = mktime(&tm);
  epoch_updated_at = millis();
}

unsigned long getTime() {
  //Use NTP to get the current epoch value.
  
  time_t now;
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return(0);
  }
  time(&now);
  Serial.print("Got time from NTP: ");
  Serial.println(now);
  return now;
}

struct coordinates get_cell_pos(String wigle_key){
  //Convert a GSM wigle_key to a coordinates struct using CSV files stored at cells/ on the SD card.
  //This location should be the approximate location of the cell tower.
  struct coordinates toreturn;
  toreturn = (coordinates){.lat = 0, .lon = 0, .acc = -127};

  Serial.print("locating ");
  Serial.println(wigle_key);
  int endpos = wigle_key.indexOf("_");
  String mccmnc = wigle_key.substring(0,endpos);
  
  int startpos = endpos+1;
  endpos = wigle_key.indexOf("_", startpos);
  String lac = wigle_key.substring(startpos,endpos);
  Serial.print("LAC = ");
  Serial.println(lac);

  String mccmnclac = mccmnc + "_" + lac;

  File filereader = SD.open("/cells/" + mccmnc + ".csv", FILE_READ);
  if (!filereader){
    Serial.print("file not found for ");
    Serial.println(mccmnc);
    return toreturn;
  }

  boolean index_read = false;
  boolean in_position = false;
  int lines = 0;
  String buff = "";
  while (filereader.available()){
    char c = filereader.read();
    if (c != '\n'){
      buff.concat(c);
    } else {
      if (buff.length() < 17){
        //Index lines are the only lines which are under 17 chars in these files
        if (buff.indexOf(lac + ",") == 0){
          Serial.print("Found relevant index ");
          Serial.println(buff);
          //This index entry is what we need.
          startpos = buff.indexOf(",")+1;
          String offset_str = buff.substring(startpos);
          int offset = offset_str.toInt()-64;
          if (offset > filereader.position()){
            Serial.print("Jump to ");
            Serial.println(offset);
            filereader.seek(offset);
            lines = 0;
            buff = "";
            index_read = true;
            continue;
          }
        }
      }
      if (buff.indexOf(wigle_key) > -1){
        Serial.print("FOUND: ");
        Serial.println(buff);

        startpos = buff.indexOf(",")+1;
        endpos = buff.indexOf(",",startpos);
        String lat_str = buff.substring(startpos,endpos);

        startpos = endpos+1;
        endpos = buff.indexOf(",",startpos);
        String lon_str = buff.substring(startpos,endpos);

        startpos = endpos+1;
        String acc_str = buff.substring(startpos);

        double lat = lat_str.toDouble();
        double lon = lon_str.toDouble();
        int acc = acc_str.toInt();

        toreturn.lat = lat;
        toreturn.lon = lon;
        toreturn.acc = acc;
        
        break;
      } else {
        if (in_position == false){
          if (buff.indexOf(mccmnclac) > -1){
            in_position = true;
            Serial.print("Now in position at ");
            Serial.print(filereader.position());
            Serial.print(" ");
            Serial.println(lines);
          }
        }
        if (buff.indexOf(mccmnclac) < 0 && index_read == true && in_position == true){
          Serial.print("read ");
          Serial.println(buff);
          Serial.println("^^^ wrong section, stopping");
          break;
        }
      }
      lines++;
      buff = "";
      if (lines >= 5000){
        Serial.println("Gave up");
        break;
      }
    }
  }
  Serial.print("read lines: ");
  Serial.println(lines);
  return toreturn;
}

struct coordinates gsm_get_current_position(){
  //Get our current position using recently seen cell towers.

  Serial.println("GSM get position");

  struct coordinates toreturn = (coordinates){.lat = 0, .lon = 0, .acc = -255};
   
  double lat_total = 0;
  double lon_total = 0;
  unsigned int total = 0;

  int max_accuracy = -127;
  
  for (int x = 0; x < cell_history_len; x++){
    struct cell_tower tower = cell_history[x];
    if (tower.pos.acc < 1){
      continue;
    }
    if (tower.pos.lat == 0 && tower.pos.lon == 0){
      continue;
    }
    if (tower.seenat+20000 < millis()){
      Serial.print("seen too long ago: ");
      print_cell(tower);
      continue;
    }
    
    //This is a (negative) RSSI value, make it positive where high numbers are stronger signals.
    int strength = (tower.strength + 100);

    if (tower.pos.acc > max_accuracy){
      max_accuracy = tower.pos.acc;
    }
    
    if (strength < 1){
      strength = 1;
    }
    Serial.print("Using tower: ");
    print_cell(tower);
    Serial.print(" with strength ");
    Serial.print(strength);
    Serial.print(", raw: ");
    Serial.println(tower.strength);
    for (int y = 0; y <= strength; y++){
      lat_total += tower.pos.lat;
      lon_total += tower.pos.lon;
      total++;
    }
  }

  Serial.print("GSM Position from ");
  Serial.print(total);
  Serial.println(" towers:");

  double lat = lat_total / total;
  double lon = lon_total / total;

  Serial.print(lat, 6);
  Serial.print(",");
  Serial.println(lon, 6);

  if (max_accuracy > 0){
    toreturn.lat = lat;
    toreturn.lon = lon;
    toreturn.acc = max_accuracy;
  }

  return toreturn;
}

String device_type_string(){
  String ret = "";
  switch (DEVICE_TYPE){
    case DEVICE_REV3:
      ret = "rev3";
      break;
      
    case DEVICE_REV3_5:
      ret = "rev3 5GHz";
      break;

    case DEVICE_REV4:
      ret = "rev4";
      break;

    case DEVICE_CUSTOM:
      ret = "generic";
      break;

    default:
      ret = "generic";
  }
  
  return ret;
}

byte identify_model(){
  //Block until we know for sure what hardware model this is. Can take a while so cache the response.
  //Return a byte indicating the model, such as DEVICE_REV3.
  //Only call *before* the main loops start, otherwise multiple threads could be trying to access the serial.

  if (DEVICE_TYPE != DEVICE_UNKNOWN){
    //We already know.
    Serial.print("Device already identified as ");
    Serial.println(device_type_string());
    return DEVICE_TYPE;
  }
  
  Serial.println("Identifying hardware..");
  
  //TODO: For models without "side B" serial, detect their respective bus here and do an immediate return.

  //For anything which is rev 3-ish, listen to the "side B" serial for a while.
  //Timeout after a while in case "side B" is dead/missing, it's technically optional.
  int i = 0;
  String buff = "";
  int bufflen = 0;
  while (i < 10000){
    if (Serial1.available()){
      char c = Serial1.read();
      if (c == '\n' || c == '\r'){
        //Handle buff.
        if (buff.indexOf("BLC,") > -1){
          Serial.println("Identified Rev3 (cm)");
          return DEVICE_REV3;
        }
        if (buff.indexOf("REV3!") > -1){
          Serial.println("Identified Rev3");
          return DEVICE_REV3;
        }
        if (buff.indexOf("5GHz,") > -1){
          Serial.println("Identified Rev3 5Ghz (cm)");
          return DEVICE_REV3_5;
        }
        if (buff.indexOf("!REV3.5") > -1){
          Serial.println("Identified Rev3 5Ghz");
          return DEVICE_REV3_5;
        }

        buff = "";
      }
      buff.concat(c);
      bufflen++;
      if (bufflen > 70){
        bufflen = 0;
        buff = "";
      }
      
    }
    delay(1);
    i++;
  }
  Serial.println("Failed to identify hardware");
  return DEVICE_UNKNOWN;
}

String get_config_option(String key){
  #define max_line_len 64
  
  char linebuf[max_line_len];
  File filereader = SD.open("/cfg.txt", FILE_READ);
  if (!filereader){
    Serial.println("cfg.txt could not be opened");
    return "";
  }

  //Unlikely to be needed but a nice safety net.
  filereader.setTimeout(500);

  while (filereader.available()){
    int buflen = filereader.readBytesUntil('\n', linebuf, max_line_len-1);
    if (buflen < 1){
      Serial.println("Failed to read cfg line");
      continue;
    }
    
    String cfgkey = "";
    String value = "";
    bool reading_key = true;
    
    for (int i = 0; i < buflen; i++){
      if (linebuf[i] == '='){
        reading_key = false;
        continue;
      }
      if (linebuf[i] == '\n' || linebuf[i] == '\r'){
        break;
      }
      if (reading_key){
        cfgkey.concat(linebuf[i]);
      } else {
        value.concat(linebuf[i]);
      }
    }
    Serial.print("cfgread: ");
    Serial.print(cfgkey);
    Serial.print(" eq ");
    Serial.println(value);

    if (cfgkey == key){
      Serial.print("cfg got: ");
      Serial.print(cfgkey);
      Serial.print(" equal to ");
      Serial.println(value);
    
      return value;
    }
    
  }
  
  Serial.print("Did not find ");
  Serial.println(key);
  
  return "";
  
}
