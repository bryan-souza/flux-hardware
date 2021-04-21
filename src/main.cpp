#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <WiFi.h>

#define SCANNER_UUID "0909c9fc-a6a4-4cbe-8520-1377e6b45d11"
#define SERVICE_UUID "4782188b-8c6a-4ed1-8984-7a9a1467da56"
#define SETTER_UUID "c5bf2cdc-6d1d-48fc-aa1c-b3a1202a4a88"
#define STATUS_LED 2

// *********************** GLOBALS **************************
// Set network preferences namespace
Preferences netman;

// Async Web Server instance
AsyncWebServer server(80);

// Async WebSocket instance;
AsyncWebSocket ws("/ws");
// **********************************************************

// ************************* BLE ****************************
class ServerCallbacks : public NimBLEServerCallbacks
{
    void onConnect(NimBLEServer *pServer)
    {
        digitalWrite(STATUS_LED, HIGH);
        delay(500);
        digitalWrite(STATUS_LED, LOW);
    };

    void onDisconnect(NimBLEServer *pServer)
    {
        digitalWrite(STATUS_LED, HIGH);
        delay(500);
        digitalWrite(STATUS_LED, LOW);
    };
};

class NetworkingCallbacks : public NimBLECharacteristicCallbacks
{
    /*
        This function should receive a JSON object
        {"ssid": "network_ssid", "pwd":""}
            -> set network credentials
        {anything else}
            -> scan for local APs
    */
    void onWrite(NimBLECharacteristic *netChar)
    {
        DynamicJsonDocument js_input(1024); // JSON Object for parsing opcodes
        deserializeJson(js_input, netChar->getValue());

        if (js_input.containsKey("ssid"))
        {
        /*
        Accepts:
        {"ssid": "some_ssid", "pwd": "passwrd"}
        {"ssid": "some_ssid", "pwd": ""}
        {"ssid": "some_ssid"}
        */
            // Parse JSON parameters
            const char *ssid = js_input["ssid"];
            const char *pwd = (!js_input["pwd"].isNull()) ? (const char*)js_input["pwd"] : "";

            /*
            If at least the network SSID
            is given, save credentials
            */
            if (ssid != "")
            {
                // Save permanently for auto-reconnection
                netman.putString("ssid", ssid);
                netman.putString("pwd", pwd);

                // Notify success
                netChar->setValue("Success");
                netChar->notify(true);
            } // If
            else
            {
                // Notify success
                netChar->setValue("[E] SSID must not be empty!");
                netChar->notify(true);
            } // Else
        } // If
        else
        {
            // This will ensure that the user will
            // get something as return

            // Scan for wireless APs
            DynamicJsonDocument networks(1024); // JSON object to store all network objects
            const char *authTypes[5] = {"Open", "WEP", "WPA", "WPA2", "Auto"};

            // scan for nearby networks:
            int contSSID = WiFi.scanNetworks();

            // print the network number and name for each network found:
            DynamicJsonDocument network(256); // JSON object to store network characteristics
            for (int i = 0; i < contSSID; i++)
            {
                network["ssid"] = WiFi.SSID(i);
                network["rssi"] = WiFi.RSSI(i);
                int auth = WiFi.encryptionType(i);
                network["auth"] = (auth <= 4) ? authTypes[auth] : "Unknown";
                networks["networks"][i] = network;
            }; // For

            std::string output;
            serializeJson(networks, output);

            // Return detected networks
            netChar->setValue(output);
            netChar->notify(true);
        }; // Else
    };     // onWrite
};         // scannerCallbacks
// **********************************************************

// ********************** HTTP SERVER ***********************
void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len)
{
    switch (type)
    {
    case WS_EVT_CONNECT:
        // On client connect
        Serial.println("Client connected");
        client->printf("Hello Client %u :)", client->id());
        client->ping(); // Send ping request
        break;
    case WS_EVT_DISCONNECT:
        Serial.println("Client disconnected");
        break;
    default:
        break;
    }
}
// **********************************************************

void setup()
{
    Serial.begin(115200);
    pinMode(STATUS_LED, OUTPUT);

    // Open the network namespace
    netman.begin("netman", false);
    netman.clear(); // Clear the current credentials

    while (true)
    {
        // Try to connect to AP
        if (netman.getString("ssid") != "" && netman.getString("pwd") != "")
        {
            for (size_t i = 0; i <= 6; i++)
            {
                int status = WiFi.begin(
                    netman.getString("ssid").c_str(),
                    netman.getString("pwd").c_str());

                if (status == WL_CONNECTED)
                {
                    digitalWrite(STATUS_LED, HIGH);
                    break;
                }

                // Wait 10s to retry
                delay(10000);
            }
        }

        // Exit loop if connected
        if (WiFi.status() == WL_CONNECTED)
            break;

        // Start the BLE server if not already running
        if (NimBLEDevice::getInitialized() == false)
        {
            // Start BLE server
            NimBLEDevice::init("ESP32");
            static NimBLEServer *pServer = NimBLEDevice::createServer();
            pServer->setCallbacks(new ServerCallbacks());

            // Assign service
            NimBLEService *netService = pServer->createService(SERVICE_UUID);

            // Networking characteristic
            NimBLECharacteristic *netChar = netService->createCharacteristic(
                SCANNER_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::NOTIFY);
            netChar->setCallbacks(new NetworkingCallbacks());

            // Start service
            netService->start();

            // Set advertising parameters
            NimBLEAdvertising *pAdvertising = NimBLEDevice::getAdvertising();
            pAdvertising->addServiceUUID(SERVICE_UUID);
            pAdvertising->setMinPreferred(0x06); // functions that help with iPhone connections issue
            pAdvertising->setMaxPreferred(0x12);

            // Broadcast server to clients
            NimBLEDevice::startAdvertising();
        }
    }

    // Show local IP address for connection
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());

    // HTTP Server section
    ws.onEvent(onWsEvent);  // Assing WebSocket event handler
    server.addHandler(&ws); // Assign to Server
    server.begin();
}

void loop() {}