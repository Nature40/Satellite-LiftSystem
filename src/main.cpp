
#include <SSD1306.h>
#include <WiFi.h>
#include <esp_wifi.h>

// connections to L298N
#define EN_A 21
#define IN1 13
#define IN2 12

#define BUTTON_DOWN 32
#define BUTTON_UP 33

// motor timeout (security fallback)
int timeout_ms = 500;

// PWM motor configuration
const int freq = 255;
const int chan = 0;
const int resolution = 8;

// Network configuration
char ssid[30];
const char *pass = "supersicher";
const int port = 35037;
const IPAddress ip = IPAddress(192, 168, 3, 254);
const IPAddress gateway = IPAddress(192, 168, 3, 254);
const IPAddress subnet = IPAddress(255, 255, 255, 0);

WiFiServer server;
WiFiUDP udp;

#define MAX_UDP_SIZE 65536
char buffer[MAX_UDP_SIZE];

// timeout for motor command
int timeout = 0;

// parameters of last client
IPAddress remoteIP(0, 0, 0, 0);
uint16_t remotePort = 0;

// display configuration
#define OLED_ADDRESS 0x3c
#define OLED_SDA 4
#define OLED_SCL 15
#define OLED_RST 16
SSD1306 display(OLED_ADDRESS, OLED_SDA, OLED_SCL);

int display_speed = 0;
int display_redraw = 0;

void sendResponse(int payload_len) {
    udp.beginPacket(remoteIP, remotePort);
    udp.write((const uint8_t *)buffer, payload_len);
    udp.endPacket();
}

void setTimeout(int new_timeout_ms) {
    Serial.printf("Setting timeout_ms to %i\n", new_timeout_ms);
    timeout_ms = new_timeout_ms;

    int payload_len =
        snprintf(buffer, MAX_UDP_SIZE, "timeout %i\n", new_timeout_ms);
    sendResponse(payload_len);
}

void setSpeed(int speed, int timeout_ms) {
    // cutoff extreme values
    if (speed > 255)
        speed = 255;
    if (speed < -255)
        speed = -255;

    bool send_response = (display_speed != speed);

    Serial.printf("Setting speed to %i\n", speed);
    display_speed = speed;
    timeout = millis() + timeout_ms;

    // write 255 if standing still or pwm value
    if (speed == 0)
        ledcWrite(chan, 255);
    else
        ledcWrite(chan, abs(speed));

    // write direction (double zero to break)
    digitalWrite(IN1, (speed < 0));
    digitalWrite(IN2, (speed > 0));

    // send packet to last controller, if changed
    if (send_response) {
        int payload_len = snprintf(buffer, MAX_UDP_SIZE, "speed %i\n", speed);
        sendResponse(payload_len);
    }
}

void parsePacket() {
    // separate cmd string
    char *cmd = strtok(buffer, " ");
    if (cmd == NULL) {
        Serial.printf("Error: empty command\n");
        return;
    }

    char *arg1 = strtok(NULL, " ");

    if (!strcmp(cmd, "speed")) {
        if (arg1 == NULL) {
            Serial.printf("Error: argument <speed> missing\n");
        } else {
            int speed = atoi(arg1);
            setSpeed(speed, timeout_ms);
        }
    } else if (!strcmp(cmd, "timeout")) {
        if (arg1 == NULL) {
            Serial.printf("Error: argument <ms> missing\n");
        } else {
            int new_timeout_ms = atoi(arg1);
            setTimeout(new_timeout_ms);
        }
    } else {
        Serial.printf("Command '%s' is unknown, skipping\n", cmd);
    }
}

bool handlePacket() {
    int packetSize = udp.parsePacket();

    // sometimes udp.parsePacket() gets stuck in a loop, a reboot helps here.
    if (errno == 5)
        ESP.restart();

    if (packetSize) {
        remoteIP = udp.remoteIP();
        remotePort = udp.remotePort();

        size_t payload_len = udp.read(buffer, MAX_UDP_SIZE);
        buffer[payload_len] = 0;

        Serial.printf("Received %i bytes from %s:%i: '%s'\n", payload_len,
                      remoteIP.toString().c_str(), remotePort, buffer);

        parsePacket();
    }

    return (packetSize > 0);
}

bool handleButtons() {
    if (digitalRead(BUTTON_UP) && digitalRead(BUTTON_DOWN)) {
        Serial.println("Both buttons pressed, stopping lift.");
        setSpeed(0, 10);
        return true;
    }

    if (digitalRead(BUTTON_UP)) {
        Serial.println("Button up pressed.");
        setSpeed(255, 10);
        return true;
    }

    if (digitalRead(BUTTON_DOWN)) {
        Serial.println("Button down pressed.");
        setSpeed(-255, 10);
        return true;
    }

    return false;
}

void redraw() {
    // get station count
    wifi_sta_list_t stations;
    esp_wifi_ap_get_sta_list(&stations);
    char stations_str[16];
    char speed_str[16];
    char timeout_str[16];
    snprintf(stations_str, 16, "%i", stations.num);
    snprintf(speed_str, 16, "%i", display_speed);
    double timeout_s = ((double)millis() - (double)timeout) / 1000;
    snprintf(timeout_str, 16, "%.1f", timeout_s);

    display.clear();
    display.flipScreenVertically();
    display.setTextAlignment(TEXT_ALIGN_LEFT);
    display.setFont(ArialMT_Plain_24);
    display.drawString(0, 0, ssid + 20);

    display.setFont(ArialMT_Plain_10);
    display.drawString(0, 24, "WiFi Stations");
    display.drawString(0, 36, "Speed");
    display.drawString(0, 48, "Last Movement");

    display.setTextAlignment(TEXT_ALIGN_RIGHT);
    display.drawString(128, 24, stations_str);
    display.drawString(128, 36, speed_str);
    display.drawString(128, 48, timeout_str);
    display.display();

    display_redraw = millis() + 100;
}

void setup() {
    // setup serial console output
    Serial.begin(115200);
    Serial.setDebugOutput(true);

    // read chip id
    uint64_t chipid = ESP.getEfuseMac();
    Serial.printf("ESP32 Chip ID: %04x\n", (uint16_t)(chipid >> 32));
    snprintf(ssid, 30, "nature40-liftsystem-%04x", (uint16_t)(chipid >> 32));

    // Reset OLED
    pinMode(OLED_RST, OUTPUT);
    digitalWrite(OLED_RST, LOW);
    delay(50);
    digitalWrite(OLED_RST, HIGH);

    // Init OLED
    display.init();
    redraw();
    Serial.println("OLED display initialised.");

    // setup WiFi access point
    WiFi.softAP(ssid, pass, 11, false, 2);
    WiFi.softAPConfig(ip, gateway, subnet);
    server.begin();

    // start udp server
    if (!udp.begin(WiFi.softAPIP(), port)) {
        Serial.println("Error: Failed to start UDP server");
        while (true) {
            delay(1000);
        }
    }

    // print configuration
    Serial.printf("SSID: %s\n", ssid);
    Serial.printf("Password: %s\n", pass);
    Serial.printf("IP: %s\n", WiFi.softAPIP().toString().c_str());
    Serial.printf("Port: %i\n", port);

    // setup manual control buttons
    pinMode(BUTTON_DOWN, INPUT);
    pinMode(BUTTON_UP, INPUT);

    // setup motor control pins
    pinMode(EN_A, OUTPUT);
    pinMode(IN1, OUTPUT);
    pinMode(IN2, OUTPUT);

    // setup PWM channel
    ledcSetup(chan, freq, resolution);
    ledcAttachPin(EN_A, chan);

    // set initial break
    setSpeed(0, timeout_ms);
}

void loop() {
    bool button_pressed = handleButtons();
    bool packet_received = false;

    if (!button_pressed) {
        packet_received = handlePacket();
    }

    // if a timeout occures and the lift moves
    if ((timeout < millis()) && (digitalRead(IN1) || digitalRead(IN2))) {
        setSpeed(0, 0);
    }

    if (display_redraw < millis()) {
        redraw();
    }

    // if a packet is received, check for the next, else sleep.
    if (packet_received) {
        return;
    } else {
        delay(10);
    }
}
