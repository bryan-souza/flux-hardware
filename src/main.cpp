#include <NimBLEDevice.h>
#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <WiFi.h>

#define SERVICE_UUID "4782188b-8c6a-4ed1-8984-7a9a1467da56"
#define SETTER_UUID "c5bf2cdc-6d1d-48fc-aa1c-b3a1202a4a88"
#define SCANNER_UUID "0909c9fc-a6a4-4cbe-8520-1377e6b45d11"
#define STATUS_LED 2

// Set network preferences namespace
Preferences netman;

std::string getEncryptionType(int encType) {
    // read the encryption type and print out the name:
    switch (encType) {
        case WIFI_AUTH_WEP:
            return "WEP";
            break;
        case WIFI_AUTH_WPA_PSK:
            return "WPA";
            break;
        case WIFI_AUTH_WPA2_PSK:
            return "WPA2";
            break;
        case WIFI_AUTH_OPEN:
            return "None";
            break;
        case WIFI_AUTH_WPA_WPA2_PSK:
            return "Auto";
            break;
        default:
            return "Unknown";
            break;
    };
};

std::string netDiscover() {
    DynamicJsonDocument doc(1024);

    // scan for nearby networks:
    int contSSID = WiFi.scanNetworks();

    // print the network number and name for each network found:
    DynamicJsonDocument obj(256);
    for (int i = 0; i < contSSID; i++) {
        obj["ssid"] = WiFi.SSID(i);
        obj["rssi"] = WiFi.RSSI(i);
        obj["auth"] = getEncryptionType(WiFi.encryptionType(i));
        doc["networks"][i] = obj;
    };

    std::string output;
    serializeJson(doc, output);
    return output;
};

class ServerCallbacks: public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* pServer) {
        digitalWrite(STATUS_LED, HIGH);
    };

    void onDisconnect(NimBLEServer* pServer) {
        digitalWrite(STATUS_LED, LOW);
    };
};

class scannerCallbacks: public NimBLECharacteristicCallbacks {
    /*
    This function should receive a JSON string
    {"opcode": 76} -> Scan for Wireless APs
    */
    void onWrite(NimBLECharacteristic* netScanner) {
        std::string json = netScanner->getValue();
        DynamicJsonDocument doc(1024);
        DeserializationError error = deserializeJson(doc, json);
        
        if (error) {
            // JSON parsing error
            netScanner->setValue("JSON parsing error");
            netScanner->notify();
        };

        try {
            // Parse JSON parameters
            int opcode = doc["opcode"];

            /* 
            NOTE TO SELF:
                If you intend to use more than one opcode
                for this service, please refactor this section
                for better code performance and readiness
            */
            if (opcode == 76) {
                // Scan for WiFi APs
                netScanner->setValue( netDiscover() );
                netScanner->notify();
            } else {
                /*
                Notify the user that the given opcode
                either got deprecated or wasn't
                implemented yet
                */
               netScanner->setValue("OPCODE either got deprecated or not implemented yet");
               netScanner->notify();
            };
            
        } catch(const std::exception& e) {
            // Wrong JSON format
            netScanner->setValue("Wrong JSON format");
            netScanner->notify();
        };
    };
};

class setterCallbacks: public NimBLECharacteristicCallbacks {
    /*
    This function should receive a JSON string
    {"ssid": "AP SSID", "pwd": "password"}
    */
    void onWrite(NimBLECharacteristic* netSetter) {
        std::string json = netSetter->getValue();
        DynamicJsonDocument doc(1024);
        DeserializationError error = deserializeJson(doc, json);
        
        if (error) {
            // JSON parsing error
            netSetter->setValue("JSON parsing error");
            netSetter->notify();
        };

        try {
            const char* ssid = doc["ssid"];
            const char* pwd = doc["pwd"];

            // Parse JSON parameters
            Serial.println(ssid);
            Serial.println(pwd);

            // Save permanently for auto-reconnection
            netman.putString("ssid", ssid);
            netman.putString("pwd", pwd);

            // TODO: Insert WiFi connection snippet here
            netman.end(); // Close the network namespace

            // Notify success
            netSetter->setValue("Success");
            netSetter->notify();
        } catch(const std::exception& e) {
            // Wrong JSON format
            netSetter->setValue("Wrong JSON format");
            netSetter->notify();
        };
    };
};

void setup() {
    Serial.begin(115200);
    pinMode(STATUS_LED, OUTPUT);

    // Open the network namespace
    netman.begin("netman", false);
    // netman.clear(); // Clear the current credentials

    // Check for existing WiFi credentials
    if ( (netman.getString("ssid") != "") && (netman.getString("pwd") != "") ) {
        // Credentials exist
        Serial.println("Credentials detected!");

        // Insert HTTP Server code here
    } else {
        // Credentials either doesn't exist or are corrupted
        Serial.println("Credentials either doesn't exist or are corrupted");
        // Start BLE server
        Serial.print("Starting BLE Server...");
        NimBLEDevice::init("ESP32");
        static NimBLEServer* pServer = NimBLEDevice::createServer();
        pServer->setCallbacks(new ServerCallbacks());

        // Assign service
        NimBLEService* netService = pServer->createService(SERVICE_UUID);

        // Network scanner output
        NimBLECharacteristic* netScanner = netService->createCharacteristic(SCANNER_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::NOTIFY);
        netScanner->setCallbacks(new scannerCallbacks());

        // Network configuration setter
        NimBLECharacteristic* netSetter = netService->createCharacteristic(SETTER_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::NOTIFY);
        netSetter->setCallbacks(new setterCallbacks());

        // Start service
        netService->start();

        // Set advertising parameters
        NimBLEAdvertising* pAdvertising = NimBLEDevice::getAdvertising();
        pAdvertising->addServiceUUID(SERVICE_UUID);
        pAdvertising->setMinPreferred(0x06);  // functions that help with iPhone connections issue
        pAdvertising->setMaxPreferred(0x12);

        // Broadcast server to clients
        NimBLEDevice::startAdvertising();
    }
}

void loop() {

}