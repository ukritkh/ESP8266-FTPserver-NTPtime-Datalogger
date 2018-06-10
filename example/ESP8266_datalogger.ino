#include <SPI.h>
#include <SD.h>
#include "SdList.h"
#include "FtpServer.h"

#include "MAX17043.h"

#include <OneWire.h>
#include <DallasTemperature.h>

#include <ESP8266WiFi.h>
#include "WiFiManager.h"
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <WiFiClient.h>
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <PubSubClient.h>
#include <Ticker.h>
#include <pgmspace.h>
#include <Wire.h>  // must be incuded here so that Arduino library object file references work
#include <RtcDS3231.h>
RtcDS3231 Rtc;

#define countof(a) (sizeof(a) / sizeof(a[0]))

#define TRIGGER_SLEEP_PIN 4
bool sleepStatus;
//==== temperature sensor ===//
#define ONE_WIRE_BUS 10
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

//==== range finder =====//
const int pwPin = 5; 
//variables needed to store values
long pulse, inches, cm;

//======= SD card =======//
const uint8_t chipSelect = 15;
char fileName[] = "Bush.csv";
File file;
  //===== NTP stuff =====//
  unsigned int localPort = 8888;      // local port to listen for UDP packets
  const char timeServer[] = "3.nz.pool.ntp.org";  // NTP server 
  const int NTP_PACKET_SIZE = 48; // NTP time stamp is in the first 48 bytes of the message
  byte packetBuffer[ NTP_PACKET_SIZE]; //buffer to hold incoming and outgoing packets
  WiFiUDP udp; // A UDP instance to let us send and receive packets over UDP
  // Update these with values suitable for your network.
  IPAddress server(10, 13, 0, 136);
  PubSubClient client(server);
    
  //rtc timer variables
  unsigned long rtc_start_time = millis(); //used for the count down timer
  unsigned long current_time = millis();

  
volatile int watchdogCount = 0;
const int sleepSeconds = 10;

MAX17043 batteryMonitor;

SdList sdl;
FtpServer ftpSrv;
 String webString = "";   // String to display

 /*------------ RTC----------- */

void printDateTime(const RtcDateTime& dt)
{Serial.print(returnDateTime(dt));
}


String returnDateTime(const RtcDateTime& dt)
{
  String datestring = "";
  if (dt.Month() < 10) {datestring += "0";}datestring += dt.Month();datestring += "/";
  if (dt.Day() < 10) {datestring += "0";} datestring += dt.Day();datestring += "/";datestring += dt.Year(); datestring += " ";
  if (dt.Hour() < 10) {datestring += "0";}datestring += dt.Hour(); datestring += ":";
  if (dt.Minute() < 10) {datestring += "0";}datestring += dt.Minute();datestring += ":";
  if (dt.Second() < 10) {datestring += "0";}datestring += dt.Second();
  return datestring;
}

// Callback function
void callback(const MQTT::Publish& pub) {
  if(pub.payload_string().equals("rtc set"))
  {
    setRTC();
    webString = "RTC Set. Time: ";
    webString += getTime();
    client.publish("outTopic",webString);// send to someones browser when asked
    } 
  else if(pub.payload_string().equals("rtc get"))
  { 
    webString = "Time: ";
    webString += getTime();
    client.publish("outTopic",webString);               // send to someones browser when asked
  }   
    Serial.print(pub.topic()); 
    Serial.print(" => ");
    Serial.println(pub.payload_string());
}

void configModeCallback (WiFiManager *myWiFiManager) {
  WiFiManager wifiManager;
  Serial.println("Entered config mode");
  Serial.println(WiFi.softAPIP()); 
  // need to reset every time if you cant find to prevent error
  wifiManager.resetSettings(); 
  //if you used auto generated SSID, print it
  Serial.println(myWiFiManager->getConfigPortalSSID());
}
void FTP_WiFiConfig(){
   batteryMonitor.reset();
  batteryMonitor.quickStart();
  delay(100);
    
  WiFi.forceSleepWake();
  WiFi.mode(WIFI_STA); 
  
  //===== WiFiManager ======//
  WiFiManager wifiManager;
  wifiManager.setAPCallback(configModeCallback);
  wifiManager.setMinimumSignalQuality();
  wifiManager.setConfigPortalTimeout(110);
 
  //===== AP name & Password
  if(!wifiManager.autoConnect("ESP8266", "1234567890")) {
    Serial.println("failed to connect and hit timeout");
    stopWiFiAndSleep();
  } 
  
  //====IP Address===//
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
   
  //---- set rtc time first ----///  
  udp.begin(localPort);
  RtcDateTime compiled = RtcDateTime(__DATE__, __TIME__);  
  printDateTime(compiled); Serial.println();

  if (!Rtc.IsDateTimeValid())
  { 
   Serial.println("RTC lost confidence in the DateTime!");
   Rtc.SetDateTime(compiled);
  }

  RtcDateTime now = Rtc.GetDateTime();
  
  if (now < compiled)
  {
    Serial.println("RTC is older than compile time!  (Updating DateTime)");
    Rtc.SetDateTime(compiled);
  }
 
  else if (now > compiled)
  {
    Serial.println("RTC is newer than compile time. (this is expected)");
  }
  
  else if (now == compiled)
  {
    Serial.println("RTC is the same as compile time! (not expected but all is fine)");
  }

    Rtc.Enable32kHzPin(false);
    Rtc.SetSquareWavePin(DS3231SquareWavePin_ModeNone);
    client.set_callback(callback); 
  
  if (client.connect("arduinoClient")) { 
    client.publish("outTopic","rtc boot up"); 
    client.subscribe("inTopic"); 
  }  
  setRTC();
  
  ftpSrv.init();
  const unsigned long ftp_time_out = 1*60*3*100000; //3*60*3*100000 = 4.30 min timeout for the knob turn in thousandths of a second (60*1000 = 10 mins)
  unsigned long ftp_start_time = 0; //used for the count down timer
  while(ftp_start_time<=ftp_time_out){
    ftpSrv.service();ftp_start_time++; 
  }  
   
}
void setup()
{ 
 stopWiFi();
 Serial.begin(9600);
 pinMode(TRIGGER_SLEEP_PIN, INPUT);
  //--------RTC SETUP ------------
  Rtc.Begin();
#if defined(ESP8266)
  Wire.begin(0, 2); //SDA,SCL
#endif

  sensors.begin();

   pinMode(15, OUTPUT);
   if (!SD.begin(chipSelect)){
        Serial.println("Card failed, or not present");
        return;
   }
   if(sdl.begin(15) == 0){
     Serial.println(("SD init fail"));
   }  

   // Create a file and header
   if(! SD.exists(fileName)){
     file = SD.open(fileName, FILE_WRITE);
     if(file){
      file.println(", , ,"); 
      file.println("Water Height (cm), Water Temperature (C), Date, Time");
      file.close();
      return; }
   }
  while(digitalRead(TRIGGER_SLEEP_PIN) ==  LOW) {
    if(! SD.exists(fileName)){
    file = SD.open(fileName, FILE_WRITE);
    if(file){file.println(", , ,"); 
      file.println("Water Height (cm), Water Temperature (C), Date, Time");
      file.close();}
      FTP_WiFiConfig();
   }else{ FTP_WiFiConfig();}
  }
}

void loop()
{
    Serial.println("Wake up");
    if (!Rtc.IsDateTimeValid()) 
    {Serial.println("RTC lost confidence in the DateTime!");}
   
   // RTC
      RtcDateTime now = Rtc.GetDateTime();  
      char datestring[11];
      char timestring[9];
      snprintf_P(datestring, countof(datestring),PSTR("%02u/%02u/%04u"),now.Month(),now.Day(),now.Year());
      snprintf_P(timestring, countof(timestring),PSTR("%02u:%02u:%02u"),now.Hour(),now.Minute(),now.Second() );
    Serial.print("Time");
    Serial.print(datestring);
    Serial.println(timestring);
  //==== Maxbotix sensor ====//
  float height = cm;
  pulse = pulseIn(pwPin, HIGH);//147uS per inch
  //inches = pulse/147; //change inches to centimetres
  cm = (pulse/147) * 2.54;
    
    //===== DS18B20====//
    float temperature = (sensors.getTempCByIndex(0));
    sensors.requestTemperatures(); // Send the command to get temperatures
  
   String dataString = String(cm) + ", " + String(temperature) + ", " + String(datestring) + "," + String(timestring);
  
    file = SD.open(fileName, FILE_WRITE);
  if (!file) {
    Serial.println(F("open failed"));
    return;
  }
  Serial.println(dataString);
  file.println(dataString);
  file.close();

  stopWiFiAndSleep();
}
void stopWiFi() {
    WiFi.mode(WIFI_OFF); 
    WiFi.forceSleepBegin();
    delay(1);
}
void stopWiFiAndSleep() {// Sleep for 10 seconds
    WiFi.mode(WIFI_OFF);
    WiFi.forceSleepBegin();
    delay(1); 
    ESP.deepSleep(10*1000000, WAKE_RF_DEFAULT); 
    delay(100);
}


String getTime()
{
  if (!Rtc.IsDateTimeValid())
  {
    return "RTC lost confidence in the DateTime!";
  }
  RtcDateTime now = Rtc.GetDateTime();
  return returnDateTime(now);
}
void setRTC()
{
    Serial.print("RTC before : ");
    printDateTime(Rtc.GetDateTime());    
    Serial.println();
  rtc_start_time = current_time;
  sendNTPpacket(timeServer); // send an NTP packet to a time server
  // wait to see if a reply is available
  delay(1000);

  int cb = udp.parsePacket();
  if (!cb) {Serial.println("no packet yet");}
  else {// We've received a packet, read the data from it
       udp.read(packetBuffer, NTP_PACKET_SIZE); // read the packet into the buffer
    unsigned long t1, t2, t3, t4;
    t1 = t2 = t3 = t4 = 0;
    for (int i = 0; i < 4; i++)
    {t1 = t1 << 8 | packetBuffer[16 + i];t2 = t2 << 8 | packetBuffer[24 + i];t3 = t3 << 8 | packetBuffer[32 + i];t4 = t4 << 8 | packetBuffer[40 + i];}
   
    float f1, f2, f3, f4;
    f1 = ((long)packetBuffer[20] * 256 + packetBuffer[21]) / 65536.0;f2 = ((long)packetBuffer[28] * 256 + packetBuffer[29]) / 65536.0;f3 = ((long)packetBuffer[36] * 256 + packetBuffer[37]) / 65536.0;f4 = ((long)packetBuffer[44] * 256 + packetBuffer[45]) / 65536.0;
#define SECONDS_FROM_1970_TO_2000 946684800
    const unsigned long seventyYears = 2208988800UL + 946684800UL; //library differences, it wants seconds since 2000 not 1970
    t1 -= seventyYears;t2 -= seventyYears;
    t3 -= seventyYears;t4 -= seventyYears;
    t4 -= ((-13) * 3600L);     // Notice the L for long calculations!!
    t4 += 1;               // adjust the delay(1000) at begin of loop!
    if (f4 > 0.4) t4++;    // adjust fractional part, see above
    Rtc.SetDateTime(t4);
    Serial.print("RTC after : "); 
    printDateTime(Rtc.GetDateTime());
    Serial.println();
  }
}

unsigned long sendNTPpacket(const char*)
{
  memset(packetBuffer, 0, NTP_PACKET_SIZE);
  // Initialize values needed to form NTP request
  // (see URL above for details on the packets)
  packetBuffer[0] = 0b11100011;   // LI, Version, Mode
  packetBuffer[1] = 0;     // Stratum, or type of clock
  packetBuffer[2] = 6;     // Polling Interval
  packetBuffer[3] = 0xEC;  // Peer Clock Precision
  // 8 bytes of zero for Root Delay & Root Dispersion
  packetBuffer[12]  = 49;packetBuffer[13]  = 0x4E;packetBuffer[14]  = 49;packetBuffer[15]  = 52;
  //NTP requests are to port 123
  udp.beginPacket(timeServer, 123); 
  udp.write(packetBuffer, NTP_PACKET_SIZE);
  udp.endPacket();
}
