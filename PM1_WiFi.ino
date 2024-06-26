/*
 * WiFi to Serial bridge
 * for use with Power Meter PM1
 * Board: LOLIN(WeMos) D1 R1
 *
 * Note: The data received on the serial port is expected to be a modbus response
 * the modbus byte length is interrogated and compared against the actual number
 * of bytes received on serial, if the number of bytes doesn't match, the modbus request
 * received on WiFi is resent to the serial port with another response expected from PM1 
 * Retries will occur until MAX_RETRY is reached.
 * 
 * Sequence:
 * 1 - received modbus TCP packet by WiFi -> wifi_rx_buf[]
 * 2 - convert TCP to RTU packet -> serial_tx_buf[]
 * 3 - send RTU packet (serial_tx_buf) to interface
 * 4 - received sRTU packet -> serial_rx_buf[]
 * 5 - check RTU packet CRC, on CRC mismatch go back to 3
 * 6 - convert RTU to TCP packet
 * 7 - transmit TCP packet on WiFi
 * 
 * Pins
 * D1 Serial TX
 * D2 Serial RX
 * RST Board reset
 */

#include <ESP8266WiFi.h>
#include <SoftwareSerial.h>
#include "Network.h"

#define SERIAL_BAUD 38400
#define SERIAL_EOT 10       // [ms] time after last RX where we consider RX complete
uint32_t serial_EOT_timeout;

WiFiServer server(502);
WiFiClient client, client2;

#define WIFI_BUF_SIZE 256
uint8_t wifi_rx_buf[WIFI_BUF_SIZE];
uint8_t wifi_tx_buf[WIFI_BUF_SIZE];
int wifi_rx_size = 0;
int wifi_tx_size = 0;
int retry_attempts = 0;
#define MAX_RETRY 2

#define SERIAL_BUF_SIZE 256
uint8_t serial_rx_buf[SERIAL_BUF_SIZE];
uint8_t serial_tx_buf[SERIAL_BUF_SIZE];
int serial_rx_size = 0;
int serial_tx_size = 0;

uint16_t transID;   // Modbus MBAP transaction ID
uint16_t protoID;   // Modbus MBAP protocol ID

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

void retry() {
  // resend wifi rx buffer
  for (int i=0; i<serial_tx_size; i++) { 
    data_serial.write(serial_tx_buf[i]);
  }
  retry_attempts++;
}

/*
 * Convert the modbus TCP buffer to RTU buffer
 * returns the number of bytes placed in rtu_buf
 */
int tcp_to_rtu(const uint8_t *tcp_buf, int tcp_len, uint8_t *rtu_buf, int rtu_max_len) {
  int i, len;
  uint16_t crc;
  uint8_t *rtu_ptr = rtu_buf;

  // store MBAP infor for reply
  transID = tcp_buf[0]*256 + tcp_buf[1];
  protoID = tcp_buf[2]*256 + tcp_buf[3];
  //length = tcp_buf[4]*256 + tcp_buf[5];

  // copy data into RTU buffer
  for (i=6; i<tcp_len; i++) {
    *rtu_ptr++ = tcp_buf[i];
  }
  // append CRC
  crc = modbus_crc16(rtu_buf, tcp_len-6);
  *rtu_ptr++ = (uint8_t) (crc >> 8);    // CRC hi
  *rtu_ptr++ = (uint8_t) (crc & 0x00FF);// CRC lo
  return tcp_len - 4;
}

/*
 * Check modbus RTU buffer CRC and
 * convert RTU buffer to TCP buffer
 * returns the number of bytes placed in the TCP buffer or 0 on CRC mismatch
 */
int rtu_to_tcp(const uint8_t *rtu_buf, int rtu_len, uint8_t *tcp_buf, int tcp_max_len) {
  int tcp_idx = 0;
  uint16_t len = rtu_len - 2;
  // CRC contained in RTU packet
  uint16_t crc = rtu_buf[rtu_len-2] * 256 + rtu_buf[rtu_len-1];
  // calculate CRC
  uint16_t crc_calculated = modbus_crc16(rtu_buf, rtu_len-2);
  // if CRC is incorrect abort here
  if (crc != crc_calculated) { return 0; }
  // CRC is OK proceed with TCP
  tcp_buf[tcp_idx++] = transID >> 8;
  tcp_buf[tcp_idx++] = transID & 0x00FF;
  tcp_buf[tcp_idx++] = protoID >> 8;
  tcp_buf[tcp_idx++] = protoID & 0x00FF;
  tcp_buf[tcp_idx++] = len >> 8;
  tcp_buf[tcp_idx++] = len & 0x00FF;
  for (int i=0; i<rtu_len-2; i++) {
    tcp_buf[tcp_idx++] = rtu_buf[i];
  }
  return tcp_idx;
}

void loop() {
  int nBytes, i, len;
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
      if (!serial_EOT_timeout) {  // ignore WiFi activity during serial RX
        // do we have data from WiFi client ?
        nBytes = client.available();
        if (nBytes){
          Serial.print(nBytes);
          Serial.println(" Bytes received on WiFi");
          //data_serial.println(" Bytes received on WiFi");
          // read modbus TCP packet
          for (i=0; i<nBytes; i++) {
            wifi_rx_buf[wifi_rx_size++] = client.read();
          }
          // convert to RTU packet
          serial_tx_size = tcp_to_rtu(wifi_rx_buf, wifi_rx_size, serial_tx_buf ,SERIAL_BUF_SIZE);
          // send RTU packet on serial interface
          for (i=0; i<serial_tx_size; i++) { 
            data_serial.write(serial_tx_buf[i]);
          }
          wifi_rx_size = 0;
        }
      }

      // receive and accumulate serial data 
      while (data_serial.available()) {
        serial_rx_buf[serial_rx_size] = data_serial.read();
        serial_rx_size++;
        serial_EOT_timeout = millis() + SERIAL_EOT;
      }

      // write serial data to WiFi client after end of transmission
      if (serial_EOT_timeout && (millis() > serial_EOT_timeout)){
        Serial.print(serial_rx_size);
        Serial.println(" Bytes received on Serial");
        serial_EOT_timeout = 0;
        len = rtu_to_tcp(serial_rx_buf, serial_rx_size, wifi_tx_buf, WIFI_BUF_SIZE);
        if (len != 0) {      // no error
          client.write_P((const char *) &wifi_tx_buf, len);  // write data to wifi client
          retry_attempts = 0;
        } else {                          // incorrect number of bytes
          if (retry_attempts < MAX_RETRY) {   // max retires reached?
            Serial.println("Attempting retry, resending on serial port");
            retry();
          } else {        // max retires have been reached
            retry_attempts = 0;   // reset attempts
          }
        }
        serial_rx_size = 0;
      }

    } else {
      Serial.println("Client Disconnected");
      connected = false;
    }
  }
  delay(3);   // RossW has experienced random board resets without a 5ms delay
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

static uint16_t modbus_crc16( const unsigned char *buf, unsigned int len )
{
	static const uint16_t crc_table[256] = {
	0x0000, 0xC0C1, 0xC181, 0x0140, 0xC301, 0x03C0, 0x0280, 0xC241,
	0xC601, 0x06C0, 0x0780, 0xC741, 0x0500, 0xC5C1, 0xC481, 0x0440,
	0xCC01, 0x0CC0, 0x0D80, 0xCD41, 0x0F00, 0xCFC1, 0xCE81, 0x0E40,
	0x0A00, 0xCAC1, 0xCB81, 0x0B40, 0xC901, 0x09C0, 0x0880, 0xC841,
	0xD801, 0x18C0, 0x1980, 0xD941, 0x1B00, 0xDBC1, 0xDA81, 0x1A40,
	0x1E00, 0xDEC1, 0xDF81, 0x1F40, 0xDD01, 0x1DC0, 0x1C80, 0xDC41,
	0x1400, 0xD4C1, 0xD581, 0x1540, 0xD701, 0x17C0, 0x1680, 0xD641,
	0xD201, 0x12C0, 0x1380, 0xD341, 0x1100, 0xD1C1, 0xD081, 0x1040,
	0xF001, 0x30C0, 0x3180, 0xF141, 0x3300, 0xF3C1, 0xF281, 0x3240,
	0x3600, 0xF6C1, 0xF781, 0x3740, 0xF501, 0x35C0, 0x3480, 0xF441,
	0x3C00, 0xFCC1, 0xFD81, 0x3D40, 0xFF01, 0x3FC0, 0x3E80, 0xFE41,
	0xFA01, 0x3AC0, 0x3B80, 0xFB41, 0x3900, 0xF9C1, 0xF881, 0x3840,
	0x2800, 0xE8C1, 0xE981, 0x2940, 0xEB01, 0x2BC0, 0x2A80, 0xEA41,
	0xEE01, 0x2EC0, 0x2F80, 0xEF41, 0x2D00, 0xEDC1, 0xEC81, 0x2C40,
	0xE401, 0x24C0, 0x2580, 0xE541, 0x2700, 0xE7C1, 0xE681, 0x2640,
	0x2200, 0xE2C1, 0xE381, 0x2340, 0xE101, 0x21C0, 0x2080, 0xE041,
	0xA001, 0x60C0, 0x6180, 0xA141, 0x6300, 0xA3C1, 0xA281, 0x6240,
	0x6600, 0xA6C1, 0xA781, 0x6740, 0xA501, 0x65C0, 0x6480, 0xA441,
	0x6C00, 0xACC1, 0xAD81, 0x6D40, 0xAF01, 0x6FC0, 0x6E80, 0xAE41,
	0xAA01, 0x6AC0, 0x6B80, 0xAB41, 0x6900, 0xA9C1, 0xA881, 0x6840,
	0x7800, 0xB8C1, 0xB981, 0x7940, 0xBB01, 0x7BC0, 0x7A80, 0xBA41,
	0xBE01, 0x7EC0, 0x7F80, 0xBF41, 0x7D00, 0xBDC1, 0xBC81, 0x7C40,
	0xB401, 0x74C0, 0x7580, 0xB541, 0x7700, 0xB7C1, 0xB681, 0x7640,
	0x7200, 0xB2C1, 0xB381, 0x7340, 0xB101, 0x71C0, 0x7080, 0xB041,
	0x5000, 0x90C1, 0x9181, 0x5140, 0x9301, 0x53C0, 0x5280, 0x9241,
	0x9601, 0x56C0, 0x5780, 0x9741, 0x5500, 0x95C1, 0x9481, 0x5440,
	0x9C01, 0x5CC0, 0x5D80, 0x9D41, 0x5F00, 0x9FC1, 0x9E81, 0x5E40,
	0x5A00, 0x9AC1, 0x9B81, 0x5B40, 0x9901, 0x59C0, 0x5880, 0x9841,
	0x8801, 0x48C0, 0x4980, 0x8941, 0x4B00, 0x8BC1, 0x8A81, 0x4A40,
	0x4E00, 0x8EC1, 0x8F81, 0x4F40, 0x8D01, 0x4DC0, 0x4C80, 0x8C41,
	0x4400, 0x84C1, 0x8581, 0x4540, 0x8701, 0x47C0, 0x4680, 0x8641,
	0x8201, 0x42C0, 0x4380, 0x8341, 0x4100, 0x81C1, 0x8081, 0x4040 };

	uint8_t nTemp = 0;
	uint16_t crcWord = 0xFFFF;

	while( len-- )
	{
		nTemp = (*buf++) ^ crcWord;
		crcWord >>= 8;
		crcWord ^= crc_table[nTemp];
	}

	return crcWord;
}

/*
 * 2REM studio alert module
 * Uses the V2 DTMF decoder boards.
 * 
 * Settings:
 * Board: WeMos D1 R1
 * Shouldn't matter which file system split is selected, I used 0.
 * This version made major changes, including:
 *   - keep-alive for http, to eliminate all the FIN-WAIT processes on the server(s)
 *   - listen for WiFi networks and choose the best signal one if multiple match
 *   - stop the WAP after 5 minutes once the device connects
 */

/*

#define Version "1.6"       //firmware version.  17Nov2022

#define led 2           // onboard LED is on GPIO2 (pin D4)
#define ledselect 5     // GPIO5 (D1) selects leds 1-5 or 6-10
#define led3 12         // GPIO12 (D6) is led 3/7
#define beeper 15         // GPIO15 (D8) is beeper.    led 5/10

#include <ESP8266WiFi.h>
#include <EEPROM.h>
#include <ESP8266WebServer.h>


#define configcount 4           // how many configs can we store?

const unsigned memchk=1234;     // just a check digit that the memory is valid
int profile=-1;                 // which wifi profile is in use

struct {                        // config data stored in eeprom (or emulated)
    unsigned int memchk;        // memory check ID
    char id[20];                // Identity string
    struct {
      char ssid[32];            // wifi SSID is a max of 30 chars
      char password[32];        // wifi passphrase
      char host[64];            // hostname (without protocol or port)
      char port[6];             // destination server port
    } ap[configcount];          // each "access-point" blocks.
    unsigned int waptime;       // time in seconds for WAP to remain active after connection
    byte flash1on;    // "on" interrupt cycles in flash-only mode
    byte flash1off;   // "off" interrupt cycles in flash-only mode
    byte flash2on;    // "on" cycles in flash+beep mode
    byte flash2off;   // "off" cycles in flash+beep mode
    byte greenflash;  // alive pulse in milliseconds
} eeprom;


uint8_t macAddr[6];             // MAC address of this device

ESP8266WebServer server(80);


int wap=0;                      // 0 for normal operation, 1 if we are a WAP
byte alarm=0;                   // alarm state 1=flash, 2=flash+beep
byte alarmcount=0;    // alarm ISR uses this to keep track of where it is up to
byte alarmstate=0;    // alarm ISR uses this to keep track of the LED state

void ICACHE_RAM_ATTR onTimerISR()  //Code needs to be in IRAM because its a ISR
{
    switch(alarm)
    {
  case 0:       // no alarms, turn everything off
          digitalWrite(talent,0);
          digitalWrite(beeper,0);
    alarmcount=255; alarmstate=0;
    break;
  case 1:       // flash only
    if(alarmstate)    // if light is on now
    {
      if(alarmcount++ >= eeprom.flash1on) // been on long enough?
      {
        alarmstate=0;
        alarmcount=0;
      }
    } else {
      if(alarmcount++ >= eeprom.flash1off)  // been off long enough?
      {
        alarmstate=1;
        alarmcount=0;
      }
    }
    break;
  case 2:
    if(alarmstate)    // if light is on now
    {
      if(alarmcount++ >= eeprom.flash2on) // been on long enough?
      {
        alarmstate=0;
        alarmcount=0;
      }
    } else {
      if(alarmcount++ >= eeprom.flash2off)  // been off long enough?
      {
        alarmstate=2;
        alarmcount=0;
      }
    }
    break;
    }
    if(alarmstate) digitalWrite(talent,1); else digitalWrite(talent,0);
    if(alarmstate>1) digitalWrite(beeper,1); else digitalWrite(beeper,0);
}   



char buf[4096];           // buffer used to receive stuff, and for wifi sending buffer.
int readline(int readch) {
    static int pos = 0;
    if (readch > 0) 
    {
        switch (readch) {
            case '\r':      // Ignore CR
                break;
            case '\n':      // Return on new-line
                buf[pos]=0; // terminate the string to be sure
                pos = 0;    // Reset position index ready for next time
                return 1;
            default:
                if (pos < 512-1)
                    buf[pos++] = readch;
        }
    }
    return 0;
}

unsigned long nextsample;     // time for next sample (in milliseconds)

WiFiClient client;                        // instance for client comms

void default_eeprom() {     // write defaults to eeprom
      WiFi.macAddress(macAddr);     // get MAC address and build default device ID
      eeprom.memchk=memchk;
      Serial.printf("ID:%s\n",eeprom.id);
      sprintf(eeprom.id, "DISPLAY%02X%02X%02X", macAddr[3], macAddr[4], macAddr[5]);
      int i=0;
      strcpy(eeprom.ap[i].ssid,"2 REM");          // Station
      strcpy(eeprom.ap[i].password,"xxxxxx");
      strcpy(eeprom.ap[i].host,"10.1.1.12");
      strcpy(eeprom.ap[i++].port,"80");
      strcpy(eeprom.ap[i].ssid,"xxxxxx");         // John
      strcpy(eeprom.ap[i].password,"xxxxxx");
      strcpy(eeprom.ap[i].host,"rural.net.au");
      strcpy(eeprom.ap[i++].port,"13032");
      strcpy(eeprom.ap[i].ssid,"xxxxxx");         // Ross
      strcpy(eeprom.ap[i].password,"xxxxxx");
      strcpy(eeprom.ap[i].host,"rural.net.au");
      strcpy(eeprom.ap[i++].port,"13032");
      strcpy(eeprom.ap[i].ssid,"xxxxxx");  // Bart
      strcpy(eeprom.ap[i].password,"xxxxxx");
      strcpy(eeprom.ap[i].host,"rural.net.au");
      strcpy(eeprom.ap[i++].port,"13032");
      while(i<configcount)
      {
        Serial.printf("Config%d\n",i);
        strcpy(eeprom.ap[i].ssid,"");
        strcpy(eeprom.ap[i].password,"");
        strcpy(eeprom.ap[i].host,"rural.net.au");
        strcpy(eeprom.ap[i++].port,"13032");
      }
      eeprom.waptime=300;    // 5 minutes
      eeprom.flash1on=20;     // 20:30 on:off ratio for slow flash
      eeprom.flash1off=80;    // 
      eeprom.flash2on=10;     // 10:10 on:off ratio for slow flash
      eeprom.flash2off=10;    //
      eeprom.greenflash=20;   // green "alive" flash
}

void setup() {
  pinMode(beeper, OUTPUT);          // set beeper pin as output
  digitalWrite(beeper,0);           // and turn it off
  pinMode(led, OUTPUT);             // set led pin as output
  digitalWrite(led,1);              // led is active-low, so 1=off
  pinMode(talent, OUTPUT);          // set talent led pin as output
  digitalWrite(talent,0);           // talent led is active-high
  pinMode(led3, OUTPUT);            // "green" led pin as output
  digitalWrite(led3,0);
  pinMode(ledselect, OUTPUT);       // set bank led pin as output
  digitalWrite(ledselect,0);
  
  EEPROM.begin(sizeof(eeprom));
  EEPROM.get(0,eeprom);
  if(eeprom.memchk != memchk)            // if eeprom is uninitialized
  {
    default_eeprom();   // initialize eeprom (but don't actually write it)
  }  
  if(eeprom.waptime>3600) eeprom.waptime=300;       // can't be more than an hour
  Serial.begin(115200);
  delay(10);
  Serial.printf("\n"); 

  WiFi.mode(WIFI_AP_STA);
  WiFi.hostname(eeprom.id);                             // set hostname
  wapmode();                                            // become a WAP.



//---------------------------------




  // We start by (trying to) connect to a WiFi network
  // set hostname
  Serial.printf("Host %s\n", eeprom.id);
  WiFi.hostname(eeprom.id);
  WiFi.persistent(false);     // don't wear out flash

  // Explicitly set the ESP8266 to be a WiFi-client, otherwise, it by default,
  //   would try to act as both a client and an access-point and could cause
  //   network-issues with your other WiFi-devices on your WiFi-network.
  WiFi.setSleepMode(WIFI_NONE_SLEEP,0);     // see if this fixes network disconnects

  struct {                          // "candidate" stations
      int wap;
      int eeprom;
  } possible[configcount];
  int foundcounter=0;               // count/pointer to found ssid's
  WiFi.disconnect();  delay(100);   // not sure this is required



  WiFi.setSleepMode(WIFI_NONE_SLEEP,0);     // see if this fixes network disconnects
  while (WiFi.status() != WL_CONNECTED)   // wait for a connection
  {
    digitalWrite(led3,1);       // turn on the green LED
    Serial.println("Scanning wifi");
    int n = WiFi.scanNetworks();
    digitalWrite(led3,0);       // turn off the green LED
    for (int i = 0; i < n; i++)
    {
      for(int j=0; j<configcount; j++)
      {
                Serial.printf("scan[%d](%s) + conf[%d](%s) : ",i,WiFi.SSID(i).c_str(),j,eeprom.ap[j].ssid);
                if(!strcmp(WiFi.SSID(i).c_str(), eeprom.ap[j].ssid)) 
          { 
              possible[foundcounter].wap=i; 
              possible[foundcounter++].eeprom=j; 
                    Serial.printf(" Found at index %d\n",j);
                } else Serial.printf(" not found\n");
      }
      Serial.print("\n");  
    }
    // Sort candidate WAPs
    int sort=1;
    while(sort)
    {
      sort=0;
      for(int i=0; i<foundcounter-1; i++)
      {
         if(WiFi.RSSI(possible[i].wap) < WiFi.RSSI(possible[i+1].wap))
         {
            int j=possible[i].wap; possible[i].wap=possible[i+1].wap; possible[i+1].wap=j;
            j=possible[i].eeprom; possible[i].eeprom=possible[i+1].eeprom; possible[i+1].eeprom=j;
            sort++;
         }       
      }
    }
    profile=possible[0].eeprom;
    Serial.printf("Sorted (%d)\n",foundcounter); for(int i=0; i<foundcounter; i++) Serial.printf("%d: %d %d\n", i, possible[i].wap, possible[i].eeprom);
  
    Serial.printf("Time: %d. ",millis());
    if(foundcounter>0)
    {
       Serial.printf("Connecting to %s/%s\n",eeprom.ap[profile].ssid,eeprom.ap[profile].password);
       WiFi.begin(eeprom.ap[profile].ssid, eeprom.ap[profile].password);
    } else {
       Serial.println("No known network to connect to.");
    }
  
    nextsample=millis();                // timer for how long to wait for a connection
    while (WiFi.status() != WL_CONNECTED && (millis()-nextsample)<15000)    // wait for a connection
    {
        server.handleClient();              // service wifi requests if someone hits us in WAP mode 
        digitalWrite(led3,!(millis()&0x70));          // fast blink green led while waiting for connection
              switch(Serial.read())
              {
                 case 'd':               // if 'd' then default EEPROM 
               default_eeprom();
                   break;
                 case 's':               // if 's' then show EEPROM
                   Serial.printf("\n"); 
                   for(int i=0; i<configcount; i++) Serial.printf("%d -> %s (%s)\n",i, eeprom.ap[i].ssid,eeprom.ap[i].password);
                   break;
             }
           digitalWrite(talent,!(millis()&0x3ff));
    }
    digitalWrite(led3,0);               // Done
  } 
  


//---------------------------------





  if(WiFi.status() == WL_CONNECTED)
  {
    Serial.printf("Time: %d. WiFi connected to %s at %ddBm on ",millis(),eeprom.ap[profile].ssid,WiFi.RSSI());
    Serial.println(WiFi.localIP());
    WiFi.setAutoReconnect(1);                       // automatically try to reconnect if lost
    nextsample=millis()+5000;                       // schedule nex reading for 5 seconds after connecting to wifi
    WiFi.setAutoReconnect(1);             // automatically try to reconnect if lost
  



    // create TCP connection to server
    unsigned int httpPort = atoi(eeprom.ap[profile].port);
    if (client.connect(eeprom.ap[profile].host, httpPort))     // If we connect ok, procede. Otherwise just ignore and retry later
    {
        // We now create a URI for the request
        String url = "/start.htm?";
        url += eeprom.id;
        url += "+";
        url += eeprom.ap[profile].ssid;
        // Send the request to the server
        url.replace(" ", "%20");
        client.print(String("GET ") + url + "+" + WiFi.RSSI() + "dBm+" + WiFi.localIP().toString() + " HTTP/1.1\r\n" +
        "Host: " + eeprom.ap[profile].host + "\r\n" +
        "Connection: keep-alive\r\n\r\n");    
    }
  }
  timer1_attachInterrupt(onTimerISR);              //Attach our sampling ISR
  timer1_enable(TIM_DIV256, TIM_EDGE, TIM_LOOP);   // set up the timer
  timer1_write(3125);                             // Start the ISR 1 tick per 3.2 microsecond. 3125=10ms ?


}




const char *password="warningdisplay";
void wapmode() {
  Serial.println("");
  Serial.print("WAP ");
  Serial.print(eeprom.id);
  Serial.print("/");
  Serial.print(password);
  WiFi.softAP(eeprom.id, password);
  IPAddress myIP = WiFi.softAPIP();
  Serial.print(" on ");
  Serial.print(myIP);
  server.on("/", handleRoot);
  server.on("/set", handleset);
  server.begin();
  Serial.println(" started.");
  wap++;
}

void handleRoot() {
  strcpy(buf,"<h1>Monitor Setup</h1>");
  strcat(buf,"<br><form action=\"/set\"><table border=\"0\">");
  strcat(buf,"<tr><td>MAC:</td><td>");
  WiFi.macAddress(macAddr);   // get MAC address and build default device ID
  sprintf(buf+strlen(buf), "%02X%02X%02X%02X%02X%02X", macAddr[0], macAddr[1], macAddr[2], macAddr[3], macAddr[4], macAddr[5]);
  strcat(buf,"</td></tr>");

  if(WiFi.status() == WL_CONNECTED)
  {
    strcat(buf,"<tr><td>Connected to:</td><td>");
    strcat(buf,eeprom.ap[profile].ssid);
    sprintf(buf+strlen(buf)," (%ddBm)",WiFi.RSSI());
    strcat(buf,"</td></tr><tr><td>IP address:</td><td>");
    IPAddress x=WiFi.localIP(); 
    sprintf(buf+strlen(buf),"%d.%d.%d.%d</td></tr>",x[0],x[1],x[2],x[3]);
  } else {
    strcat(buf,"<tr><td>WiFi</td><td>Not Connected</td></tr>");    
  }

  sprintf(buf+strlen(buf),"<tr><td>Version:</td><td>%s</td></tr>",Version);
  int sec=millis()/1000;
  sprintf(buf+strlen(buf),"<tr><td>Up:</td><td>%0d:%02d:%02d</td></tr>",sec/3600,(sec%3600)/60,sec%60);
  sprintf(buf+strlen(buf),"<tr><td>WAP:</td><td>%s</td></tr>",(wap) ? "Active" : "Inactive");
  sprintf(buf+strlen(buf),"<tr><td>WAPtime:</td><td><input type=\"text\" name=\"wapt\" value=\"%d\"></td></tr>\n",eeprom.waptime);
  strcat(buf,"<tr><td>ID:</td><td><input type=\"text\" name=\"id\" value=\"");
  strcat(buf, eeprom.id);
  strcat(buf,"\"></td></tr>\n");

  sprintf(buf+strlen(buf),"<tr><td>Warn dutycycle:</td><td><input type=\"text\" name=\"w_on\" value=\"%d\"> : <input type=\"text\" name=\"w_off\" value=\"%d\"></td></tr>\n", eeprom.flash1on, eeprom.flash1off);
  sprintf(buf+strlen(buf),"<tr><td>Alarm dutycycle:</td><td><input type=\"text\" name=\"a_on\" value=\"%d\"> : <input type=\"text\" name=\"a_off\" value=\"%d\"></td></tr>\n", eeprom.flash2on, eeprom.flash2off);
  sprintf(buf+strlen(buf),"<tr><td>Alive Flash:</td><td><input type=\"text\" name=\"flash\" value=\"%d\"></td></tr>\n", eeprom.greenflash);


  for(int c=0; c<configcount; c++)
  {
    sprintf(buf+strlen(buf),"<tr><td colspan=2>Connection %d</td></tr>\n",c+1);  
    sprintf(buf+strlen(buf),"<tr><td>SSID:</td><td><input type=\"text\" name=\"ssid%d\" value=\"%s\"></td></tr>\n",c+1,eeprom.ap[c].ssid);  
    sprintf(buf+strlen(buf),"<tr><td>Password:</td><td><input type=\"text\" name=\"pw%d\" value=\"\"></td></tr>\n",c+1);
    sprintf(buf+strlen(buf),"</tr><tr><td>Host:</td><td><input type=\"text\" name=\"host%d\" value=\"%s\"></td></tr>\n",c+1,eeprom.ap[c].host);
    sprintf(buf+strlen(buf),"<tr><td>Port:</td><td><input type=\"text\" name=\"port%d\" value=\"%s\"></td></tr>\n",c+1,eeprom.ap[c].port);
  }
  strcat(buf,"</table><input type=\"submit\" value=\"Save and restart.\"></form></html>");
  server.send(200, "text/html", buf);
}


void handleset() {
  int x;
  strcpy(buf,"Received. ");
  
  for (int i=0; i<server.args(); i++) 
  {
    String temp=server.arg(i);
    temp.trim();

    if(temp.length())
    {
      if(server.argName(i).startsWith("ssid")) {x=atoi(server.argName(i).c_str()+4)-1; strcpy(eeprom.ap[x].ssid,temp.c_str()); }
      if(server.argName(i).startsWith("pw"))   {x=atoi(server.argName(i).c_str()+2)-1; strcpy(eeprom.ap[x].password,temp.c_str()); }
      if(server.argName(i).startsWith("host")) {x=atoi(server.argName(i).c_str()+4)-1; strcpy(eeprom.ap[x].host,temp.c_str()); }
      if(server.argName(i).startsWith("port")) {x=atoi(server.argName(i).c_str()+4)-1; strcpy(eeprom.ap[x].port,temp.c_str()); }
      if(server.argName(i)=="id") strcpy(eeprom.id,temp.c_str());
      if(server.argName(i)=="wapt") eeprom.waptime=server.arg(i).toInt();
      if(server.argName(i)=="w_on") eeprom.flash1on=server.arg(i).toInt();
      if(server.argName(i)=="w_off") eeprom.flash1off=server.arg(i).toInt();
      if(server.argName(i)=="a_on") eeprom.flash2on=server.arg(i).toInt();
      if(server.argName(i)=="a_off") eeprom.flash2off=server.arg(i).toInt();
      if(server.argName(i)=="flash") eeprom.greenflash=server.arg(i).toInt();
    }
  }
  sprintf(buf+strlen(buf),"Save and restart!\n");
  server.send(200, "text/plain", buf);
  EEPROM.put(0,eeprom);
  EEPROM.commit();
  delay(1000);
  ESP.restart();
}

int count=-1;                       // sample counter.  -1 is "wait state".
unsigned long statusupdate=0;       // give status updates periodically
unsigned long resettimeout=0;       // reset timer

void loop() 
{  
  server.handleClient(); 
  if(wap && millis()>=(eeprom.waptime*1000) && resettimeout>0)   // if wap is active AND its more than 5 mins AND we are active
  {
      Serial.printf("Time: %d. Shutdown wifi\n",millis()); 
      WiFi.softAPdisconnect(true); 
      wap=0;
  }

  if(resettimeout>0 && (millis()-resettimeout) > 60000)       // if we have been active but not had a valid reply in 60 seconds
  {
        ESP.restart();                      // reboot
  }

  if(WiFi.status() == WL_CONNECTED)
  {
    if(millis()>statusupdate)
    {
          statusupdate=millis()+(1*1000);
          if(!client.connected())
          {
              unsigned int httpPort = atoi(eeprom.ap[profile].port);
              client.connect(eeprom.ap[profile].host, httpPort);     // try to connect
          }
          if(client.connected())      // If we connect ok, procede. Otherwise just ignore and retry later
          {
              String url = "/now.htm?";
              url += eeprom.id;
            
              // Send the request to the server
              //digitalWrite(led,0);       // blue LED while requesting status
              //digitalWrite(led3,1);      // green LED while requesting status
              client.print(String("GET ") + url + " HTTP/1.1\r\n" +
                 "Host: " + eeprom.ap[profile].host + "\r\n" +
                 "Connection: keep-alive\r\n\r\n");
              unsigned long timeout = millis()+300;
              while (millis()<timeout) 
              {
                  if(readline(client.read()))
                  {
                      if(strstr(buf,"alarm="))
                      {
                          alarm=atoi(buf+6);         // warning status
                          digitalWrite(led3,!alarm); // green LED only if no alarm
                          delay(eeprom.greenflash);
                          digitalWrite(led3,0);      // green LED off when request completed
                          resettimeout=millis();     // reset timer every time we get a valid response.
                      }
                      if(strstr(buf,"ssid=")) 
                      {
                              eeprom.id[0]=0xff;
                              EEPROM.put(0,eeprom);
                              EEPROM.commit();
                              delay(1000);
                              ESP.restart();
                      }
                      timeout=millis()+10;
                  }
            }
        }
    }
  }      
}

 * 
 */
