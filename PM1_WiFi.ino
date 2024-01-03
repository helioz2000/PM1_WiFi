/*
 * Board: LOLIN(WeMos) D1 R1
 */

#include <ESP8266WiFi.h>
#include <SoftwareSerial.h>
#include "Network.h"

WiFiServer server(502);
WiFiClient client, client2;

bool connected = false;   // true during a client connection

/* SSID and Password defined in their own file
char ssid[] = "";   //  your network SSID (name)
char pass[] = "";   // your network password
*/

// USE serial is on GPIO1 (TX) and GPIO3 (RX)
const char SERTX = D1;   // D1 (GPIO 5)
const char SERRX = D2;   // D2 (GPIO 4)

SoftwareSerial data_serial(SERTX, SERRX); 

void setup() {
  // put your setup code here, to run once:
  Serial.begin(74880);
  Serial.println();
  data_serial.begin(115200);
  
  //Serial.print("Started");
  //Serial.setDebugOutput(true);
  delay(10);
  esp_info();
  delay(10);
  WiFi.begin(ssid, pass);

  Serial.print("Connecting");
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  server.begin();   // Start server
  Serial.print("Connected to WiFi. My IP address: ");
  Serial.println(WiFi.localIP());
}

void loop() {
  int nBytes, i;
  char c;
  // listen for incoming clients
  if (!connected) {
    client = server.available();
    if (client) {
      connected = true;
      Serial.print("connection from ");
      Serial.println(client.remoteIP().toString());
    }
  }
    
  if (connected) {
    // look for second connection
    client2 = server.available();
    if (client2) {
      Serial.print("Connection attempt from ");
      Serial.print(client2.remoteIP().toString());
      Serial.println(" - Disconnected");
      client2.stop();   // disconnect
    }
    if (client.connected()) {
      nBytes = client.available();
      if (nBytes){
        Serial.print(nBytes);
        Serial.println(" Bytes available");
        for (i=0; i<nBytes; i++) {
          c = client.read();
          data_serial.write((uint8_t) c);
          if (c >= ' ') {
            Serial.print(c);
          } else {
            Serial.print("<");
            Serial.print((uint8_t)c, HEX);
            Serial.print(">");
          }
          
        }
        Serial.println();
      }

      // write serial data to client
      while (data_serial.available()) {
        c = data_serial.read();
        client.write(c);
      }

    } else {
      Serial.println("Client Disconnected");
      connected = false;
    }
  }
  delay(5);   // RossW has experienced random board resets without this delay
}

void esp_info() {
  mylog("\nESP Chip info:\n");
  mylog("Reset reason: %s\n", ESP.getResetReason().c_str() );
  mylog("Chip ID: %u\n", ESP.getChipId() );
  mylog("Core Version: %s\n", ESP.getCoreVersion().c_str() );
  mylog("SDK Version: %s\n", ESP.getSdkVersion() );
  mylog("CPU Frequency: %uMHz\n", ESP.getCpuFreqMHz() );
  mylog("Sketch size: %u\n", ESP.getSketchSize() );
  mylog("Free Sketch space: %u\n", ESP.getFreeSketchSpace() );
  mylog("Flash Chip ID: %u\n", ESP.getFlashChipId() );
  mylog("Flash Chip size: %u (as seen by SDK)\n", ESP.getFlashChipSize() );
  mylog("Flash Chip size: %u (physical)\n", ESP.getFlashChipRealSize() );
  mylog("Flash Chip speed: %uHz\n", ESP.getFlashChipSpeed() );
  mylog("VCC: %.2f\n", (float)ESP.getVcc() / 896 );
}

void mylog(const char *sFmt, ...)
{
  char acTmp[128];       // place holder for sprintf output
  va_list args;          // args variable to hold the list of parameters
  va_start(args, sFmt);  // mandatory call to initilase args 

  vsprintf(acTmp, sFmt, args);
  Serial.print(acTmp);
  // mandatory tidy up
  va_end(args);
  return;
}