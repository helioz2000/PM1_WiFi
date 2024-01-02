#include <ESP8266WiFi.h>

WiFiServer server(502);

char ssid[] = "homeapx";          //  your network SSID (name)
char pass[] = "lang42.Seine";   // your network password

void setup() {
  // put your setup code here, to run once:
  Serial.begin(115200);
  Serial.println();
  //Serial.setDebugOutput(true);
  delay(10); 
  WiFi.begin(ssid, pass);

  Serial.print("Connecting");
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  server.begin();
  Serial.print("Connected, IP address: ");
  Serial.println(WiFi.localIP());
}

void loop() {
  // put your main code here, to run repeatedly:
  // listen for incoming clients
  WiFiClient client = server.available();
  if (!client) {
    return;
  }

  // Wait until the client sends some data
  Serial.println("new client");
  while(!client.available()){
    delay(1);
  }
   
  // Read the first line of the request
  String request = client.readStringUntil('\r');
  Serial.println(request);
  client.flush();
  client.stopAll();
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