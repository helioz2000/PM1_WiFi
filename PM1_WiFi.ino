/*
 * Board: LOLIN(WeMos) D1 R1
 */

#include <ESP8266WiFi.h>
#include <SoftwareSerial.h>
#include "Network.h"

WiFiServer server(502);
WiFiClient client, client2;

#define WIFI_BUF_SIZE 128
uint8_t wifi_rx_buf[WIFI_BUF_SIZE];
int wifi_rx_size = 0;

#define SERIAL_BUF_SIZE 128
uint8_t serial_rx_buf[SERIAL_BUF_SIZE];
int serial_rx_size = 0;

#define SERIAL_BAUD 115200

#define SERIAL_EOT 50       // [ms] time after last RX where we consider RX complete
uint32_t serial_EOT_timeout;

bool connected = false;   // true during a WiFi client connection

/* SSID and Password defined in their own file
char ssid[] = "";   //  your network SSID (name)
char pass[] = "";   // your network password
*/

// USE serial is on GPIO1 (TX) and GPIO3 (RX)
const byte TX_pin = 5;   // D1 (GPIO 5)
const byte RX_pin = 4;   // D2 (GPIO 4)

SoftwareSerial data_serial(RX_pin, TX_pin); 

void setup() {
  // put your setup code here, to run once:
  Serial.begin(74880);
  Serial.println();
  // Pin modes for soft serial
  pinMode(RX_pin, INPUT);
  pinMode(TX_pin, OUTPUT);
  // Start soft serial
  data_serial.begin(SERIAL_BAUD);
  
  Serial.print("Started");
  
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
  data_serial.print("IP=");
  data_serial.println(WiFi.localIP());
}

void loop() {
  int nBytes, i;
  uint8_t c;
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
      // do we have data from WiFi client ?
      nBytes = client.available();
      if (nBytes){
        Serial.print(nBytes);
        Serial.println(" Bytes received on WiFi");
        //data_serial.println(" Bytes received on WiFi");
        for (i=0; i<nBytes; i++) {
          wifi_rx_buf[wifi_rx_size++] = client.read();
        }
        for (i=0; i<nBytes; i++) { 
          data_serial.write(wifi_rx_buf[i]);
        }
        wifi_rx_size = 0;
      }

      // receive and accumulate serial data 
      while (data_serial.available()) {
        serial_rx_buf[serial_rx_size] = data_serial.read();
        serial_rx_size++;
        serial_EOT_timeout = millis() + SERIAL_EOT;
      }

      // write serial data to WiFi client after end of transmission
      if (serial_EOT_timeout > millis()) {
        Serial.print(serial_rx_size);
        Serial.println(" Bytes received on Serial");
        serial_EOT_timeout = 0;
        client.write_P((const char *) &serial_rx_buf, serial_rx_size);
        serial_rx_size = 0;
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