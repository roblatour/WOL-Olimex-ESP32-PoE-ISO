/*
    This sketch was developed by Rob Latour Copyright (c) 2019 - 2020

    license: MIT

    usage: wakeup your computer (WOL) triggered by a Pushbullet note

    Additional boards manager
      https://dl.espressif.com/dl/package_esp32_dev_index.json

    board: OLKMEX ESP32-POE
      esp32       1.0.2   https://github.com/espressif/arduino-esp32

    with special thanks to those that developed these libraries:
      JSON        6.12     by Benoit Blachon    https://arduinojson.org/?utm_source=meta&utm_medium=library.properties
      WakeOnLAN   1.1.6    by a7md0             https://github.com/a7md0/WakeOnLan
      Websockets  2.1.4    by Markus Sattle     https://github.com/Links2004/arduinoWebSockets
      Time        1.6.0    by Michael Margolis  https://playground.arduino.cc/code/time/

*/

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFiClientSecure.h>
#include <WebSocketsClient.h>

#include <ETH.h>

// Type of the Ethernet PHY (LAN8720 or TLK110)
// #define ETH_TYPE        ETH_PHY_LAN8720
// #define ETH_TYPE        ETH_PHY_TLK110

#include <WiFi.h>
#include <WiFiUdp.h>
#include <WakeOnLan.h>

#include <time.h>
#include <TimeLib.h>

// User button on the OLKMEX ESP32-POE-ISO board
const int User_Button = 34;

// LED
// NOTE: The use of a LED is optional
//       However, if you use it be very sure to connect it by putting an appropriate resistor between the
//       LED and the ESP32-PoE-ISO pin that controls the LED.  I did not do this and ended up rendering
//       my ESP32-PoE-ISO card permanently useless :-(
//
// If you use the LED, it will blink:
//    - quickly until the card connects to the LAN
//    - once every 30 seconds or so for a second when Pushbullet returns a 'nop' indicating the connection is still good
//    - once for about two and a half seconds when a WOL magic packet is sent
//
// ESP32-PoE-ISO pin to control the LED

const int LED = 32;

// Default MAC Address to wake up when user button is pressed
// Note: This will be updated to the last MAC update sent via Pushbullet each time a Pushbullet note is sent
// it can be either be set to the default MAC Address of PC to wake up or left blank
// format is:  xx:xx;xx:xx:xx;xx
String Default_MAC_Address = "A1:B2:C3:D4:E5:F6";

// Pushbullet
const String Pushbullet_Note_Title_To_React_To = "Wakeup On LAN";
const String My_PushBullet_Access_Token = "**********************************";

const String PushBullet_Server = "stream.pushbullet.com";
const String PushBullet_Server_Directory = "/websocket/";
const String PushBullet_KeepAlive_ID = "rob_wol_active";
const int PushBullet_Server_Port = 443;
const char* host = "api.pushbullet.com";
const char* KeepAliveHost = "zebra.pushbullet.com";
const int https_Port = 443;
bool PushBullet_connected = false;
String My_PushBullet_Client_ID = "";

//Time stuff
unsigned long  StartupTime;
unsigned long  LastNOPTime;
unsigned long  LastKeepAliveRequest;

unsigned long  secondsSinceStartup;
unsigned long  secondsSinceLastNop;
unsigned long  secondsSinceLastKeepAliveRequest;

const unsigned long  RebootAfterThisManySecondsSinceLastStartup = 300;  // 5 minutes
const unsigned long  RebootAfterThisManySecondsWithoutANOP = 180;       // 3 minutes

const unsigned long  TwentyFourHours = 86400; // 24 hours * 60 minutes * 60 seconds

//*****************  LED

void Setup_LED() {

  pinMode(LED, OUTPUT);
  LedOn(true);

}

void flashLED(int FlashTime) {

  LedOn(true);
  delay(FlashTime / 2);
  LedOn(false);
  delay(FlashTime / 2);

}

//*****************  button used to manually trigger wol
void Setup_Button() {

  Serial.println("Setting up user button");
  pinMode (User_Button, INPUT);
  Serial.println("User button has been setup");
  Serial.println(" ");

}

void Check_Button() {

  Check_a_Button(User_Button, LOW);  // User button reads LOW when pressed

}

void Check_a_Button(int Button_Number, int Pressed) {

  // if the button is pressed, toggle the relay
  if (digitalRead(Button_Number) == Pressed)
  {
    delay(20); // debounce time
    if (digitalRead(Button_Number) == Pressed)
    {

      Serial.println(" ");
      Serial.println("User button pressed");
      SendMagicPacket(Default_MAC_Address);  // wakeup
      Serial.println(" ");

      delay(1000); // more debounce time

      while (digitalRead(Button_Number) == Pressed) {
        // wait until the button is released
      }

    }
  }

}

//*****************  ethernet

//Ethernet - based on Olimex ESP32-POE example

static bool eth_connected = false;

void WiFiEvent(WiFiEvent_t event)
{
  switch (event) {

    case SYSTEM_EVENT_ETH_START:
      Serial.println(" ");
      Serial.println("ETH Started");
      ETH.setHostname("WOL-server");
      break;

    case SYSTEM_EVENT_ETH_CONNECTED:
      Serial.println(" ");
      Serial.println("ETH Connected");
      break;

    case SYSTEM_EVENT_ETH_GOT_IP:
      Serial.print("ETH MAC: ");
      Serial.print(ETH.macAddress());
      Serial.print(", IPv4: ");
      Serial.print(ETH.localIP());
      if (ETH.fullDuplex()) {
        Serial.print(", FULL_DUPLEX");
      }
      Serial.print(", ");
      Serial.print(ETH.linkSpeed());
      Serial.println("Mbps");
      eth_connected = true;
      break;

    case SYSTEM_EVENT_ETH_DISCONNECTED:
      Serial.println("ETH Disconnected");
      eth_connected = false;
      break;

    case SYSTEM_EVENT_ETH_STOP:
      Serial.println("ETH Stopped");
      eth_connected = false;
      break;

    default:
      break;
  }
}

//*****************  LED

void LedOn(bool TurnLEDOn) {

  if (TurnLEDOn) {
    //Serial.println("turn LED on");
    digitalWrite(LED, HIGH);
  }
  else
  {
    //Serial.println("turn LED off");
    digitalWrite(LED, LOW);
  }

}

//*****************  Time

void Setup_Time() {

  StartupTime = now();
  LastNOPTime = now();
  LastKeepAliveRequest = now();

}

//*****************  Failsafe check to see if reset is needed

void CheckForReset()
{
  // Failsafe: If a nop was not received in the specified time (default 2 minutes)
  // and its been over a specified time since startup (default 5 minutes)
  // then restart the system

  secondsSinceLastNop = now() - LastNOPTime;

  if ( secondsSinceLastNop > RebootAfterThisManySecondsWithoutANOP ) {

    secondsSinceStartup = now() - StartupTime;
    if ( secondsSinceStartup > RebootAfterThisManySecondsSinceLastStartup) {

      Serial.println("Restart!");
      ESP.restart();

    }
  }

}


//*****************  Pushbullet

//*****************  every 24 hours send a request to keep the Pushbullet account alive (with out this it would expire every 30 days)

void KeepPushBulletAccountAlive()
{

  secondsSinceLastKeepAliveRequest = now() - LastKeepAliveRequest;

  if ( secondsSinceLastKeepAliveRequest > TwentyFourHours ) {

    PushbulletStayAlive();
    LastKeepAliveRequest = now();

  }

}

WebSocketsClient webSocket;

void Setup_PushBullet() {

  Serial.println("Setting up Pushbullet");

  String PushBullet_Server_DirectoryAndAccessToken = PushBullet_Server_Directory + My_PushBullet_Access_Token;
  webSocket.beginSSL(PushBullet_Server, PushBullet_Server_Port, PushBullet_Server_DirectoryAndAccessToken);

  webSocket.setReconnectInterval(5000);   // try ever 5000 again if connection has failed
  webSocket.onEvent(webSocketEvent);      // event handler

  Serial.println("Connecting to Pushbullet ");
  while (!PushBullet_connected) {
    Serial.print(".");
    webSocket.loop();
    delay(250);
  }

  GetPushbulletClientID();
  PushbulletStayAlive();

  Serial.println("Pushbullet has been setup");
  Serial.println(" ");

}

void webSocketEvent(WStype_t type, uint8_t * payload, size_t length) {

  static String Last_Pushbullet_Iden;

  switch (type) {

    case WStype_DISCONNECTED:
      Serial.println(" ");
      Serial.println("Pushbullet disconnected!");
      PushBullet_connected = false;
      break;

    case WStype_CONNECTED:
      Serial.println(" ");
      Serial.println("Pushbullet connection successful!");
      PushBullet_connected = true;
      break;

    case WStype_TEXT:
      {
        Serial.printf("Incoming: % s\n", payload);

        DynamicJsonDocument jsonDocument(4096);
        deserializeJson(jsonDocument, payload);


        if (jsonDocument["type"] == "nop") {

          Serial.println("nop");
          LastNOPTime = now();

          // flash the LED to show pushbullet connection is still alive
          flashLED(2000);

        }

        if ((jsonDocument["type"] == "tickle") && (jsonDocument["subtype"] == "push")) {

          WiFiClientSecure client1;
          if (!client1.connect(host, https_Port)) {
            Serial.println("Connection failed");
            return;
          }

          client1.println("GET /v2/pushes?limit=1 HTTP/1.1");
          client1.println("Host: " + String(host));
          client1.println("Authorization: Bearer " + My_PushBullet_Access_Token);
          client1.println("Content-Type : application/json");
          client1.println("Content-Length : 0");
          client1.println();

          // Serial.print(" waiting for the details ");
          int WaitLimit = 0;
          while ((!client1.available()) && (WaitLimit < 250)) {
            delay(50);
            WaitLimit++;
          }

          WaitLimit = 0;
          while ( (client1.connected()) && (WaitLimit < 250) ) {
            String line = client1.readStringUntil('\n');
            if (line == "\r") {
              // retrieved header lines can be ignored
              break;
            }
            WaitLimit++;
          }

          String Response = "";
          while (client1.available()) {
            char c = client1.read();
            Response += c;
          }

          // Serial.println(Response);

          client1.stop();

          deserializeJson(jsonDocument, Response);
          String Current_Pushbullet_Iden = jsonDocument["pushes"][0]["iden"];

          if ( Current_Pushbullet_Iden == Last_Pushbullet_Iden) {

            Serial.println(" duplicate - ignoring it");

          }
          else
          {

            Serial.println(" new push : ");
            Last_Pushbullet_Iden = Current_Pushbullet_Iden;

            String Title_Of_Incoming_Push = jsonDocument["pushes"][0]["title"];
            String Body_Of_Incoming_Push = jsonDocument["pushes"][0]["body"];
            bool Dismissed = jsonDocument["pushes"][0]["dismissed"];

            Serial.print(" title = '" + Title_Of_Incoming_Push + "'");
            Serial.print(" ; body = '" + Body_Of_Incoming_Push + "'");

            if (Dismissed) {
              Serial.println(" ; dismissed = true");
            } else {
              Serial.println(" ; dismissed = false");
            }

            if (Pushbullet_Note_Title_To_React_To == Title_Of_Incoming_Push) {

              Serial.print(" ");
              // Serial.println(Response);

              // wake up on lan
              SendMagicPacket(Body_Of_Incoming_Push);
              flashLED(5000);

              // Dismiss push if required
              if (!Dismissed) {

                PushbulletDismissPush(Current_Pushbullet_Iden);

              }

            }
            else {
              Serial.println("Pushbullet note title did not match trigger; ignoring note");
            }

          }

        }

        break;

      };

    case WStype_BIN:
      Serial.println("Incoming binary data");
      break;

    case WStype_ERROR:
      Serial.println("Error");
      break;

    case WStype_FRAGMENT_TEXT_START:
      Serial.println("Fragment Text Start");
      break;

    case WStype_FRAGMENT_BIN_START:
      Serial.println("Fragment Bin Start");
      break;

    case WStype_FRAGMENT:
      Serial.println("Fragment");
      break;

    case WStype_FRAGMENT_FIN:
      Serial.println("Fragment finish");
      break;

  }

}

void PushbulletDismissPush(String Push_Iden) {

  Serial.println(" dismissing push");

  WiFiClientSecure client;

  if (!client.connect(host, https_Port)) {
    Serial.println(" connection failed");
    return;
  }

  String Pushbullet_Message_Out = " { \"dismissed\": true }";
  client.println("POST /v2/pushes/" + Push_Iden + " HTTP/1.1");
  client.println("Host: " + String(host));
  client.println("Authorization: Bearer " + My_PushBullet_Access_Token);
  client.println("Content-Type: application/json");
  client.println("Content-Length: " + String(Pushbullet_Message_Out.length()));
  client.println();
  client.println(Pushbullet_Message_Out);

  int WaitLimit = 0;
  while ((!client.available()) && (WaitLimit < 250)) {
    delay(50); //
    WaitLimit++;
  }

  String Response = "";
  WaitLimit = 0;
  while ( (client.connected()) && (WaitLimit < 250) ) {
    String line = client.readStringUntil('\n');
    if (line == "\r") {
      // retrieved header lines can be ignored
      break;
    }
    WaitLimit++;
  }

  while (client.available()) {
    char c = client.read();
    Response += c;
  }

  client.stop();
  DynamicJsonDocument jsonDocument(4096);
  deserializeJson(jsonDocument, Response);

  String Dismissed_Pushbullet_Iden = jsonDocument["iden"];
  bool Dismissed_Pushbullet_Status = jsonDocument["dismissed"];

  if ((Dismissed_Pushbullet_Iden == Push_Iden) && (Dismissed_Pushbullet_Status)) {
    Serial.println(" dismiss successful!");
  }
  else {
    Serial.println(" dismiss not successful");
    Serial.println(Response);
  }

}


void GetPushbulletClientID() {

  WiFiClientSecure client1;
  if (!client1.connect(host, https_Port)) {
    Serial.println("Connection failed");
    return;
  }

  client1.println("GET /v2/users/me HTTP/1.1");
  client1.println("Host: " + String(host));
  client1.println("Authorization: Bearer " + My_PushBullet_Access_Token);
  client1.println("Content-Type : application/json");
  client1.println("Content-Length : 0");
  client1.println();

  // Serial.print(" waiting for the details ");
  int WaitLimit = 0;
  while ((!client1.available()) && (WaitLimit < 250)) {
    delay(50);
    WaitLimit++;
  }

  WaitLimit = 0;
  while ( (client1.connected()) && (WaitLimit < 250) ) {
    String line = client1.readStringUntil('\n');
    if (line == "\r") {
      // retrieved header lines can be ignored
      break;
    }
    WaitLimit++;
  }

  String Response = "";
  while (client1.available()) {
    char c = client1.read();
    Response += c;
  }

  // Serial.println(Response);

  client1.stop();

  DynamicJsonDocument jsonDocument(4096);
  deserializeJson(jsonDocument, Response);
  String cid = jsonDocument["iden"];
  My_PushBullet_Client_ID = cid;

}

void PushbulletStayAlive() {

  Serial.print("stay alive request ");

  WiFiClientSecure client;

  if (!client.connect(KeepAliveHost, https_Port)) {
    Serial.println(" connection failed");
    return;
  }

  String Pushbullet_Message_Out = " { \"name\": \"" + PushBullet_KeepAlive_ID + "\", \"user_iden\": \"" + My_PushBullet_Client_ID  + "\" }";

  client.println("POST / HTTP/1.1");
  client.println("Host: " + String(host));
  client.println("Authorization: Bearer " + My_PushBullet_Access_Token);
  client.println("Content-Type: application/json");
  client.println("Content-Length: " + String(Pushbullet_Message_Out.length()));
  client.println();
  client.println(Pushbullet_Message_Out);

  int WaitLimit = 0;
  while ((!client.available()) && (WaitLimit < 250)) {
    delay(50); //
    WaitLimit++;
  }

  String Response = "";
  WaitLimit = 0;
  while ( (client.connected()) && (WaitLimit < 250) ) {
    String line = client.readStringUntil('\n');
    if (line == "\r") {
      // retrieved header lines can be ignored
      break;
    }
    WaitLimit++;
  }

  while (client.available()) {
    char c = client.read();
    Response += c;
  }

  client.stop();

  if (Response == "{}") {
    Serial.println("succeeded");
  }
  else {
    Serial.println("failed");
    Serial.println(Response);
  }


}

//*****************  wol
// with thanks to https://github.com/a7md0/WakeOnLan

WiFiUDP UDP;
WakeOnLan WOL(UDP);

void Setup_WOL() {

  Serial.println("Setting up WOL");
  WOL.setRepeat(3, 100); // Optional, repeat the packet three times with 100ms between. WARNING delay() is used between send packet function.
  WOL.calculateBroadcastAddress(WiFi.localIP(), WiFi.subnetMask()); // Optional  => To calculate the broadcast address, otherwise 255.255.255.255 is used (which is denied in some networks).
  Serial.println("WOL has been setup");
  Serial.println(" ");

}

void SendMagicPacket(String MAC_Address) {

  Serial.print("Sending Wakeup on LAN request to ");
  Serial.println(MAC_Address);

  WOL.sendMagicPacket(MAC_Address); // Send Wake On Lan packet with the above MAC address. Default to port 9.
  Default_MAC_Address = MAC_Address;

}


void setup()
{

  Serial.begin(115200);

  Serial.println("Startup underway");

  Setup_LED();
  Setup_Button();
  Setup_Time();

  WiFi.onEvent(WiFiEvent);
  ETH.begin();

  while (!eth_connected) {
    Serial.print(".");
    flashLED(250);
    CheckForReset();
  }

  Serial.println(" ");
  Serial.println("Ethernet connected!");
  Serial.println(" ");

  Setup_WOL();
  Setup_PushBullet();

};


void loop()
{

  Check_Button();

  webSocket.loop();

  if ( eth_connected ) {

    if ( !PushBullet_connected ) {
      Setup_PushBullet();
    }

    KeepPushBulletAccountAlive();

    CheckForReset();

  }

};
