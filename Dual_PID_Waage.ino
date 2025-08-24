// ##### WAAGEN-ESP (TTGO T-Display - SENDER V17 - Web-Konfiguration & USB-Neustart) #####

// --- NEUE BIBLIOTHEKEN ---
#include <WebServer.h>
#include <Preferences.h>

// --- BESTEHENDE BIBLIOTHEKEN ---
#include <esp_now.h>
#include <esp_sleep.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_bt.h>
#include <HX711.h>
#include <TFT_eSPI.h>
#include <SPI.h>
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "soc/rtc_cntl_reg.h"
#include "soc/soc.h"

// --- NEUE GLOBALE OBJEKTE FÜR KONFIGURATION ---
WebServer server(80);
Preferences preferences;

// --- KONFIGURATIONSVARIABLEN (werden aus dem Speicher geladen) ---
// Diese ersetzen die alten, hardcodierten Werte.
uint8_t coffeeMachineAddress[6];
int wifi_kanal = 1; // Standardwert, falls nichts geladen wird
unsigned long inactivitySleepTimeout = 120 * 1000UL; // Standardwert 120s, in ms

// --- Pin-Konfiguration (unverändert) ---
const gpio_num_t HX711_DOUT_PIN           = GPIO_NUM_26;
const gpio_num_t HX711_SCK_PIN            = GPIO_NUM_27;
const gpio_num_t BUTTON_1_PIN_TOGGLE_MODE = GPIO_NUM_0;    // Kurzer Druck für Toggle, langer Druck für Tiefschlaf, beim Booten für Config
const gpio_num_t BUTTON_2_PIN_TARE        = GPIO_NUM_35;   // Kurzer Druck für Tara

// --- RTC-Daten (unverändert) ---
RTC_DATA_ATTR float calibration_factor = 4213.0;
RTC_DATA_ATTR bool rtcFirstBootAfterWake = true;

// --- Batterie-Konfiguration (unverändert) ---
const int BATTERY_ADC_PIN              = 34;
const float VOLTAGE_DIVIDER_RATIO      = 2.27;
const float ESP32_ADC_VREF             = 3.3;
const float CHARGING_VOLTAGE_THRESHOLD = 4.22;

// --- Button-Handling (unverändert) ---
uint8_t  lastBtn1RawState = 1;
unsigned long lastBtn1DebounceTime = 0;
uint8_t  currentBtn1DebouncedState = 1;
uint8_t  lastBtn1DebouncedState = 1;
unsigned long btn1PressStartTime = 0;
bool     btn1IsCurrentlyPressed = false;
uint8_t  lastBtn2RawState = 1;
unsigned long lastBtn2DebounceTime = 0;
uint8_t  currentBtn2DebouncedState = 1;
uint8_t  lastBtn2DebouncedState = 1;
const unsigned long debounceDelayBtn = 50;
const unsigned long longPressThreshold = 1000;
bool     tareAfterDelay = false;
unsigned long tareDelayStartTime = 0;
const unsigned long TARE_DELAY_DURATION = 1000;
unsigned long lastButtonActivityTime = 0;

// --- Globale Objekte (unverändert) ---
HX711 scale;
TFT_eSPI tft = TFT_eSPI();
TFT_eSprite weightSprite = TFT_eSprite(&tft);
uint16_t ACCENT_COLOR;

// --- ESP-NOW Datenstruktur (unverändert) ---
typedef struct struct_espnow_scale_message {
    float weight_g;
    uint8_t status_flags;
    uint8_t battery_percentage;
} struct_espnow_scale_message;
struct_espnow_scale_message currentScaleData;

// --- ESP-NOW Flags (unverändert) ---
#define ESPNOW_SCALE_FLAG_JUST_TARED (1 << 0)
#define ESPNOW_SCALE_FLAG_TOGGLE_MODE (1 << 1)
#define ESPNOW_SCALE_FLAG_AWOKE      (1 << 2)

// --- Display-Optimierung (unverändert) ---
float lastDisplayedNumericWeight = -9999.0;
String lastDisplayedTareMessage = "";
int   lastDisplayedBatteryPercentage = -1;
bool  forceFullDisplayRedraw = true;
const int ADC_SAMPLES = 20;

// --- Akku-Caching & USB-Neustart-Logik ---
unsigned long lastBatteryUpdateTime = 0;
int           cachedBatteryPercentage = -1;
bool          cachedIsCharging = false;
bool          wasPreviouslyCharging = false; // NEU: Für die Erkennung der USB-Trennung

// ####################################################################
// ### NEUER ABSCHNITT: Konfigurations-Management (Speichern/Laden) ###
// ####################################################################

/**
 * @brief Speichert die Konfiguration im permanenten NVS-Speicher.
 * @param mac Die MAC-Adresse als String (Format: "XX:XX:XX:XX:XX:XX").
 * @param kanal Der WLAN-Kanal.
 * @param sleep Die Zeit bis zum Deep Sleep in Sekunden.
 */
void saveConfiguration(String mac, int kanal, int sleep) {
    preferences.begin("waage-cfg", false); // "waage-cfg" ist der Namespace, false = read/write
    preferences.putString("mac_addr", mac);
    preferences.putUInt("wifi_kanal", kanal);
    preferences.putUInt("sleep_time", sleep);
    preferences.end();
}

/**
 * @brief Lädt die Konfiguration aus dem NVS.
 * @return true, wenn eine gültige Konfiguration geladen wurde, sonst false.
 */
bool loadConfiguration() {
    if (!preferences.begin("waage-cfg", true)) { // true = read-only
        Serial.println("Konnte Konfiguration nicht im read-only Modus öffnen.");
        return false;
    }

    String macStr = preferences.getString("mac_addr", "");
    if (macStr.length() != 17) { // Gültige MAC-Adresse hat 17 Zeichen (inkl. Trennzeichen)
        preferences.end();
        Serial.println("Keine gültige MAC-Adresse in Konfiguration gefunden.");
        return false;
    }

    // Wandelt den MAC-String in das benötigte uint8_t-Array um
    sscanf(macStr.c_str(), "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
           &coffeeMachineAddress[0], &coffeeMachineAddress[1], &coffeeMachineAddress[2],
           &coffeeMachineAddress[3], &coffeeMachineAddress[4], &coffeeMachineAddress[5]);

    wifi_kanal = preferences.getUInt("wifi_kanal", 1);
    inactivitySleepTimeout = preferences.getUInt("sleep_time", 120) * 1000UL; // In s speichern, in ms umrechnen

    preferences.end();
    Serial.println("Konfiguration erfolgreich geladen.");
    Serial.print("  > Ziel-MAC: "); Serial.println(macStr);
    Serial.print("  > WLAN-Kanal: "); Serial.println(wifi_kanal);
    Serial.print("  > Sleep nach (s): "); Serial.println(inactivitySleepTimeout / 1000);
    return true;
}

/**
 * @brief Sendet die HTML-Seite für das Konfigurationsformular.
 * NEU: Füllt das Formular mit den bereits gespeicherten Werten vor.
 */
void handleRoot() {
    // Puffer für die Umwandlung der MAC-Adresse in einen String
    char mac_str[18];
    sprintf(mac_str, "%02X:%02X:%02X:%02X:%02X:%02X",
            coffeeMachineAddress[0], coffeeMachineAddress[1], coffeeMachineAddress[2],
            coffeeMachineAddress[3], coffeeMachineAddress[4], coffeeMachineAddress[5]);

    // Umwandlung der Sleep-Zeit von Millisekunden in Sekunden für die Anzeige
    String sleep_sec = String(inactivitySleepTimeout / 1000);

    // HTML-Vorlage mit Platzhaltern
    String html = R"rawliteral(
<!DOCTYPE HTML><html><head>
<title>Brew-By-Weight Konfiguration</title><meta name="viewport" content="width=device-width, initial-scale=1">
<style>
  body{font-family:Arial,sans-serif;background-color:#f0f0f0;margin:0;padding:0;display:flex;justify-content:center;align-items:center;min-height:100vh}
  .container{max-width:500px;width:90%;padding:20px;background-color:#fff;border-radius:8px;box-shadow:0 2px 4px rgba(0,0,0,0.1)}
  h2{color:#333;text-align:center}label{display:block;margin-top:15px;font-weight:bold;color:#555}
  input{width:100%;padding:10px;margin-top:5px;box-sizing:border-box;border:1px solid #ccc;border-radius:4px}
  button{background-color:#d89904;color:white;padding:12px 20px;border:none;border-radius:4px;cursor:pointer;margin-top:20px;font-size:16px;width:100%}
  button:hover{background-color:#b57e03}
</style></head><body>
<div class="container">
  <h2>Waagen-Konfiguration</h2>
  <form action="/save" method="POST">
    <label for="mac">MAC-Adresse des Haupt-Controllers:</label>
    <input type="text" id="mac" name="mac" placeholder="z.B. E4:65:B8:71:B1:60" pattern="^([0-9A-Fa-f]{2}[:-]){5}([0-9A-Fa-f]{2})$" value="_MAC_" required>
    
    <label for="kanal">WLAN-Kanal (1-13):</label>
    <input type="number" id="kanal" name="kanal" min="1" max="13" value="_KANAL_" required>
    
    <label for="sleep">Zeit bis Deep Sleep (in Sekunden):</label>
    <input type="number" id="sleep" name="sleep" min="30" value="_SLEEP_" required>
    
    <button type="submit">Speichern & Neustarten</button>
  </form>
</div></body></html>)rawliteral";

    // Ersetze die Platzhalter mit den tatsächlichen Werten
    html.replace("_MAC_", mac_str);
    html.replace("_KANAL_", String(wifi_kanal));
    html.replace("_SLEEP_", sleep_sec);

    server.send(200, "text/html", html);
}

/**
 * @brief Verarbeitet die Formulardaten, speichert sie und startet den ESP neu.
 */
void handleSave() {
    String mac = server.arg("mac");
    int kanal = server.arg("kanal").toInt();
    int sleep = server.arg("sleep").toInt();

    saveConfiguration(mac, kanal, sleep);

    String response = "<h1>Speichern erfolgreich!</h1><p>Ger&auml;t wird neu gestartet...</p><script>setTimeout(function(){window.location.href='/';}, 3000);</script>";
    server.send(200, "text/html", response);

    delay(1000);
    ESP.restart();
}

/**
 * @brief Startet den AP und den Webserver. Diese Funktion blockiert den normalen Ablauf.
 */
void runConfigPortal() {
    Serial.println("Starte Konfigurations-Portal...");
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString("Konfigurationsmodus", tft.width() / 2, tft.height() / 2 - 25, 2);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString("SSID: Brew-By-Weight", tft.width() / 2, tft.height() / 2 + 10, 2);
    tft.drawString("(Kein Passwort)", tft.width() / 2, tft.height() / 2 + 30, 2);
    tft.drawString("IP: 192.168.4.1", tft.width() / 2, tft.height() / 2 + 50, 2);

    // Starte den Access Point ohne Passwort
    WiFi.softAP("Brew-By-Weight", nullptr);
    IPAddress ip = WiFi.softAPIP();
    Serial.print("AP IP address: ");
    Serial.println(ip);

    server.on("/", HTTP_GET, handleRoot);
    server.on("/save", HTTP_POST, handleSave);
    server.begin();

    // Endlosschleife, um Web-Anfragen zu bearbeiten
    while (true) {
        server.handleClient();
        delay(1);
    }
}


// ####################################################################
// ### Unveränderte Hilfsfunktionen (Batterie, ESP-NOW Senden etc.) ###
// ####################################################################

float getBatteryVoltage() {
    if (BATTERY_ADC_PIN < 0) return 0.0;
    uint32_t millivolt_sum = 0;
    for (int i = 0; i < ADC_SAMPLES; i++) {
        millivolt_sum += analogReadMilliVolts(BATTERY_ADC_PIN);
        delayMicroseconds(50);
    }
    float mv_avg = (float)millivolt_sum / ADC_SAMPLES;
    float actual_battery_voltage = (mv_avg / 1000.0) * VOLTAGE_DIVIDER_RATIO;
    return actual_battery_voltage;
}

int voltageToPercent(float voltage) {
    const float voltages[] = {4.20, 4.15, 4.11, 4.08, 4.02, 3.98, 3.92, 3.87, 3.82, 3.78,
                              3.74, 3.70, 3.65, 3.61, 3.58, 3.55, 3.51, 3.48, 3.44, 3.41, 3.30};
    const int percents[]    = {100, 95, 90, 85, 80, 75, 70, 65, 60, 55,
                               50, 45, 40, 35, 30, 25, 20, 15, 10, 5, 0};
    if (voltage >= voltages[0]) return 100;
    for (size_t i = 0; i < (sizeof(voltages) / sizeof(voltages[0])) - 1; i++) {
        if (voltage >= voltages[i + 1]) {
            float vHigh = voltages[i];
            float vLow  = voltages[i + 1];
            int pHigh   = percents[i];
            int pLow    = percents[i + 1];
            return pLow + (int)((voltage - vLow) * (pHigh - pLow) / (vHigh - vLow));
        }
    }
    return 0;
}

void updateBatteryStatus() {
    float voltage = getBatteryVoltage();
    cachedIsCharging = (voltage > CHARGING_VOLTAGE_THRESHOLD);
    cachedBatteryPercentage = voltageToPercent(voltage);
}

void drawBatteryIcon(int x, int y, int percentage, bool isChargingFlag = false) {
    if (!(tft.width() > 0)) return;
    int iconWidth = 28; int iconHeight = 14; int nippleWidth = 3; int nippleHeight = 6;
    int borderWidth = 1; int padding = 2;
    uint16_t borderColor = TFT_WHITE;
    uint16_t backgroundColor = TFT_BLACK;
    uint16_t fillColor;
    uint16_t emptyColor = TFT_DARKGREY;
    if (percentage < 0) percentage = 0;
    if (percentage > 100) percentage = 100;
    if (isChargingFlag) {
        fillColor = ACCENT_COLOR;
    } else {
        if (percentage <= 15) fillColor = TFT_RED;
        else if (percentage <= 40) fillColor = ACCENT_COLOR;
        else fillColor = TFT_GREEN;
    }
    tft.fillRect(x - 2, y - 2, 70, 18, backgroundColor);
    tft.drawRect(x, y, iconWidth, iconHeight, borderColor);
    tft.fillRect(x + iconWidth, y + (iconHeight / 2) - (nippleHeight / 2), nippleWidth, nippleHeight, borderColor);
    int innerWidth = iconWidth - (2 * borderWidth) - (2 * padding);
    int innerHeight = iconHeight - (2 * borderWidth) - (2 * padding);
    int innerX = x + borderWidth + padding;
    int innerY = y + borderWidth + padding;
    if (innerWidth < 1) innerWidth = 1;
    if (innerHeight < 1) innerHeight = 1;
    tft.fillRect(innerX, innerY, innerWidth, innerHeight, emptyColor);
    if (isChargingFlag) {
        tft.fillRect(innerX, innerY, innerWidth, innerHeight, ACCENT_COLOR);
    } else {
        int fillW = map(percentage, 0, 100, 0, innerWidth);
        if (fillW > 0) {
            tft.fillRect(innerX, innerY, fillW, innerHeight, fillColor);
        }
    }
    tft.setTextDatum(ML_DATUM);
    char percentStr[6];
    if (isChargingFlag) {
        sprintf(percentStr, "");
        tft.setTextColor(ACCENT_COLOR, backgroundColor);
        tft.drawString(percentStr, x + iconWidth + nippleWidth + 5, y + iconHeight / 2, 2);
    } else {
        sprintf(percentStr, "%3d%%", percentage);
        tft.setTextColor(fillColor, backgroundColor);
        tft.drawString(percentStr, x + iconWidth + nippleWidth + 5, y + iconHeight / 2, 2);
    }
}

void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
    if (status != ESP_NOW_SEND_SUCCESS) {
        Serial.print("ESP-NOW Sendefehler: ");
        Serial.println(status);
    }
}

void sendDataViaEspNow(bool forceSend = false) {
    static float lastActuallySentWeightForComparison = -99999.0;
    static unsigned long lastRegularSendTime = 0;
    const unsigned long regularSendInterval = 333;
    bool shouldSend = false;
    struct_espnow_scale_message msgToSend;
    msgToSend.status_flags = 0;
    msgToSend.weight_g = currentScaleData.weight_g;
    if (tareAfterDelay && (millis() - tareDelayStartTime >= TARE_DELAY_DURATION)) {
        Serial.println("Verzögerung abgelaufen. Führe Tara auf Waage aus.");
        scale.tare(10);
        currentScaleData.weight_g = scale.get_units(5);
        msgToSend.weight_g = currentScaleData.weight_g;
        msgToSend.status_flags |= ESPNOW_SCALE_FLAG_JUST_TARED;
        tareAfterDelay = false;
        lastButtonActivityTime = millis();
        Serial.print("Waage lokal tariert. Neues Gewicht: ");
        Serial.println(currentScaleData.weight_g);
        shouldSend = true;
        forceFullDisplayRedraw = true;
    }
    if (currentScaleData.status_flags & ESPNOW_SCALE_FLAG_TOGGLE_MODE) {
        msgToSend.status_flags |= ESPNOW_SCALE_FLAG_TOGGLE_MODE;
        currentScaleData.status_flags &= ~ESPNOW_SCALE_FLAG_TOGGLE_MODE;
        shouldSend = true;
    }
    if (currentScaleData.status_flags & ESPNOW_SCALE_FLAG_AWOKE) {
        msgToSend.status_flags |= ESPNOW_SCALE_FLAG_AWOKE;
        currentScaleData.status_flags &= ~ESPNOW_SCALE_FLAG_AWOKE;
        shouldSend = true;
    }
    currentScaleData.battery_percentage = cachedBatteryPercentage;
    msgToSend.battery_percentage = currentScaleData.battery_percentage;
    if (abs(msgToSend.weight_g - lastActuallySentWeightForComparison) > 0.05 || forceSend || msgToSend.status_flags != 0) {
        shouldSend = true;
    }
    if (millis() - lastRegularSendTime >= regularSendInterval && !tareAfterDelay) {
        shouldSend = true;
    }
    if (shouldSend) {
        esp_err_t result = esp_now_send(coffeeMachineAddress, (uint8_t *)&msgToSend, sizeof(msgToSend));
        if (result == ESP_OK) {
            lastActuallySentWeightForComparison = msgToSend.weight_g;
        }
        lastRegularSendTime = millis();
    }
}

void processButton1_ToggleMode() {
    pinMode(BUTTON_1_PIN_TOGGLE_MODE, INPUT_PULLUP);
    uint8_t currentRawState = digitalRead(BUTTON_1_PIN_TOGGLE_MODE);
    if (currentRawState != lastBtn1RawState) {
        lastBtn1DebounceTime = millis();
    }
    lastBtn1RawState = currentRawState;
    if ((millis() - lastBtn1DebounceTime) > debounceDelayBtn) {
        if (currentRawState != currentBtn1DebouncedState) {
            currentBtn1DebouncedState = currentRawState;
            if (currentBtn1DebouncedState == LOW) { // Gedrückt
                btn1PressStartTime = millis();
                btn1IsCurrentlyPressed = true;
                Serial.println(F("[BUTTON 1] Gedrückt"));
                lastButtonActivityTime = millis();
            } else { // Losgelassen
                if (btn1IsCurrentlyPressed) {
                    unsigned long pressDuration = millis() - btn1PressStartTime;
                    Serial.print(F("[BUTTON 1] Losgelassen. Dauer: "));
                    Serial.println(pressDuration);
                    if (pressDuration >= longPressThreshold) {
                        Serial.println(F(">> Langer Druck (Button 1): Tiefschlaf-Anfrage."));
                        forceFullDisplayRedraw = true;
                        goToSleep();
                    } else {
                        Serial.println(F(">> Kurzer Druck (Button 1): Toggle Mode Anfrage."));
                        currentScaleData.status_flags |= ESPNOW_SCALE_FLAG_TOGGLE_MODE;
                        forceFullDisplayRedraw = true;
                    }
                }
                btn1IsCurrentlyPressed = false;
            }
        }
    }
    lastBtn1DebouncedState = currentBtn1DebouncedState;
}

void processButton2_Tare() {
    pinMode(BUTTON_2_PIN_TARE, INPUT_PULLUP);
    uint8_t currentRawState = digitalRead(BUTTON_2_PIN_TARE);
    if (currentRawState != lastBtn2RawState) {
        lastBtn2DebounceTime = millis();
    }
    lastBtn2RawState = currentRawState;
    if ((millis() - lastBtn2DebounceTime) > debounceDelayBtn) {
        if (currentRawState != currentBtn2DebouncedState) {
            currentBtn2DebouncedState = currentRawState;
            if (currentBtn2DebouncedState == LOW) {
                if (!tareAfterDelay) {
                    Serial.println(F("[BUTTON 2] Tara Anfrage, starte 1 Sek. Verzögerung."));
                    tareAfterDelay = true;
                    tareDelayStartTime = millis();
                    lastButtonActivityTime = millis();
                    forceFullDisplayRedraw = true;
                }
            }
        }
    }
    lastBtn2DebouncedState = currentBtn2DebouncedState;
}

void goToSleep() {
    if (cachedIsCharging) {
        Serial.println("USB verbunden, Tiefschlaf verhindert.");
        lastButtonActivityTime = millis();
        return;
    }
    Serial.print(inactivitySleepTimeout/1000);
    Serial.println("s Inaktivität. Gehe in Tiefschlaf...");
    WiFi.mode(WIFI_OFF);
    esp_wifi_stop();
    esp_bt_controller_disable();
    if (tft.width() > 0) {
        tft.fillScreen(TFT_BLACK);
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        tft.drawString("Standby wird aktiviert", tft.width() / 2, tft.height() / 2 - 10, 4);
        tft.drawString("Reaktivieren durch Tastendruck", tft.width() / 2, tft.height() / 2 + 20, 2);
        delay(2000);
        tft.fillScreen(TFT_BLACK);
        tft.writecommand(TFT_DISPOFF);
        tft.writecommand(TFT_SLPIN);
        delay(5);
        #ifdef TFT_BL
            pinMode(TFT_BL, OUTPUT);
            digitalWrite(TFT_BL, LOW);
        #endif
    }
    scale.power_down();
    Serial.println("HX711 im Power-Down. ESP32 geht schlafen.");
    pinMode(BUTTON_1_PIN_TOGGLE_MODE, INPUT_PULLUP);
    pinMode(BUTTON_2_PIN_TARE, INPUT_PULLUP);
    delay(1);
    // RTC-Peripherie abschalten, damit der Akku im Tiefschlaf kaum entladen wird
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_OFF);
    // Beide Tasten als Wakeup-Quelle konfigurieren (aktive Low-Pegel)
    gpio_wakeup_enable(BUTTON_1_PIN_TOGGLE_MODE, GPIO_INTR_LOW_LEVEL);
    gpio_wakeup_enable(BUTTON_2_PIN_TARE, GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();
    Serial.println("ESP32 geht jetzt schlafen in Deep Sleep.");
    delay(100);
    esp_deep_sleep_start();
}

void setup_waage(); // Forward declaration

// ####################################################################
// ### ANGEPASSTE `setup()` FUNKTION (MIT SICHEREM CONFIG-TRIGGER)  ###
// ####################################################################
void setup() {
    Serial.begin(115200);
    pinMode(BUTTON_1_PIN_TOGGLE_MODE, INPUT_PULLUP);
    
    // Initialisiere TFT früh, um Meldungen ausgeben zu können
    tft.init();
    tft.setRotation(1);
    #ifdef TFT_BL
        pinMode(TFT_BL, OUTPUT);
        digitalWrite(TFT_BL, HIGH);
    #endif

    bool enterConfigMode = false;
    
    // --- NEUE LOGIK: Abfragefenster für Config-Modus ---
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString("Untere Taste: Konfig.-Modus.", tft.width() / 2, tft.height() / 2, 2);
    Serial.println("Pruefe auf manuellen Start des Konfigurationsmodus (3s Zeitfenster)...");

    unsigned long startTime = millis();
    while (millis() - startTime < 3000) { // 3 Sekunden Zeitfenster
        if (digitalRead(BUTTON_1_PIN_TOGGLE_MODE) == LOW) {
            Serial.println("Manueller Start des Konfigurationsmodus erkannt!");
            tft.fillScreen(TFT_BLACK);
            tft.drawString("Konfiguration wird gestartet...", tft.width() / 2, tft.height() / 2, 2);
            delay(1000);
            enterConfigMode = true;
            break; // Schleife verlassen
        }
        delay(10);
    }
    
    // Wenn der Config-Modus nicht manuell gestartet wurde, lade die Konfiguration
    if (!enterConfigMode) {
        if (!loadConfiguration()) {
            Serial.println("Keine gueltige Konfiguration gefunden. Starte Konfigurationsmodus automatisch.");
            enterConfigMode = true;
        }
    }

    // Entscheide basierend auf dem Flag, welcher Modus gestartet wird
    if (enterConfigMode) {
        runConfigPortal(); // Diese Funktion blockiert den Code und startet nach dem Speichern neu.
    } else {
        // Konfiguration ist geladen, starte den normalen Waagenbetrieb.
        Serial.println("Gültige Konfiguration vorhanden. Starte normalen Waagenbetrieb.");
        setup_waage();
    }
}

// ####################################################################
// ### ANGEPASSTE `setup_waage()` FUNKTION (Normaler Start)         ###
// ####################################################################
void setup_waage() {
    Serial.println(F("\n\nTTGO T-Display Waagen-Sender V17"));
    Serial.println(F("======================================================"));
    setCpuFrequencyMhz(80);
    esp_bt_controller_mem_release(ESP_BT_MODE_BTDM);

    ACCENT_COLOR = tft.color565(216, 153, 4);
    lastButtonActivityTime = millis();
    pinMode(BUTTON_1_PIN_TOGGLE_MODE, INPUT_PULLUP);
    pinMode(BUTTON_2_PIN_TARE, INPUT_PULLUP);

    if (BATTERY_ADC_PIN >= 0) {
        analogSetPinAttenuation(BATTERY_ADC_PIN, ADC_11db);
        updateBatteryStatus();
        lastBatteryUpdateTime = millis();
        wasPreviouslyCharging = cachedIsCharging; // NEU: Initialen Ladestatus setzen
    }

    tft.writecommand(TFT_SLPOUT); delay(120);
    tft.writecommand(TFT_DISPON); delay(20);
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(ACCENT_COLOR, TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("Brew-By-Weight", tft.width() / 2, tft.height() / 2 - 15, 4);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString("SCALE", tft.width() / 2, tft.height() / 2 + 20, 2);
    delay(1000);

    weightSprite.createSprite(200, 80);
    weightSprite.setColorDepth(8);
    weightSprite.setTextDatum(MC_DATUM);
    forceFullDisplayRedraw = true;
    currentScaleData.status_flags = 0;

    esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
    if (wakeup_reason == ESP_SLEEP_WAKEUP_GPIO) {
        Serial.println("Aufgewacht durch Button-Druck.");
        rtc_gpio_hold_dis(BUTTON_1_PIN_TOGGLE_MODE);
        rtc_gpio_hold_dis(BUTTON_2_PIN_TARE);
        currentScaleData.status_flags |= ESPNOW_SCALE_FLAG_AWOKE;
        tft.fillScreen(TFT_BLACK);
        tareAfterDelay = true;
        tareDelayStartTime = millis();
        forceFullDisplayRedraw = true;
    }

    Serial.println("Initialisiere HX711...");
    scale.begin(HX711_DOUT_PIN, HX711_SCK_PIN);
    scale.power_up(); delay(300);
    if (scale.is_ready()) {
        Serial.println("HX711 bereit.");
        scale.set_scale(calibration_factor);
        if (wakeup_reason != ESP_SLEEP_WAKEUP_GPIO) {
            tareAfterDelay = true;
            tareDelayStartTime = millis();
        }
    } else {
        Serial.println("HX711 nicht gefunden!");
        tft.fillScreen(TFT_BLACK);
        tft.setTextColor(TFT_RED, TFT_BLACK);
        tft.drawString("HX711 FEHLER!", tft.width() / 2, tft.height() / 2, 4);
        while (1);
    }
    
    // ESP-NOW mit den geladenen Werten initialisieren
    WiFi.mode(WIFI_STA);
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    esp_wifi_set_channel(wifi_kanal, WIFI_SECOND_CHAN_NONE); // Verwendet geladene Variable
    if (esp_now_init() != ESP_OK) {
        Serial.println("ESP-NOW Init Error"); return;
    }
    esp_now_register_send_cb(OnDataSent);
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, coffeeMachineAddress, 6); // Verwendet geladene Variable
    peerInfo.channel = wifi_kanal; // Verwendet geladene Variable
    peerInfo.encrypt = false;
    if (esp_now_add_peer(&peerInfo) != ESP_OK) {
        Serial.println("Peer Add Error"); return;
    }
    Serial.println("ESP-NOW Peer hinzugefügt.");
    currentScaleData.battery_percentage = cachedBatteryPercentage;
}


void displayWeightAndStatus() {
    if (!(tft.width() > 0)) return;

    unsigned long now = millis();
    // Akku-Status nur alle 30 Sekunden aktualisieren
    if (now - lastBatteryUpdateTime >= 5000UL) {
        updateBatteryStatus();
        lastBatteryUpdateTime = now;
        Serial.printf("Batterie neu eingelesen: %d%%, Charging: %s\n",
                      cachedBatteryPercentage, cachedIsCharging ? "ja" : "nein");
    }

    bool isCurrentlyCharging = cachedIsCharging;
    static bool lastChargingState = !cachedIsCharging;

    if (isCurrentlyCharging) {
        if (!lastChargingState || forceFullDisplayRedraw) {
            tft.fillScreen(TFT_BLACK);
            tft.setTextDatum(MC_DATUM);
            tft.setTextColor(ACCENT_COLOR, TFT_BLACK);
            tft.drawString("Akku wird geladen", tft.width() / 2, tft.height() / 2 - 10, 4);
            tft.setTextColor(TFT_WHITE, TFT_BLACK);
            tft.drawString("(USB verbunden)", tft.width() / 2, tft.height() / 2 + 20, 2);
            drawBatteryIcon(tft.width() - 70, 5, cachedBatteryPercentage, true);
        }
        lastChargingState = true;
        forceFullDisplayRedraw = false;
        return;
    }

    if (lastChargingState) {
        tft.fillScreen(TFT_BLACK);
        forceFullDisplayRedraw = true;
    }
    lastChargingState = false;

    char buf[30];
    String currentTareUIMsg = "";
    bool isCurrentlyShowingTareMsg = false;
    float weightToDisplay = currentScaleData.weight_g;
    int batteryPercent = cachedBatteryPercentage;

    if (tareAfterDelay) {
        isCurrentlyShowingTareMsg = true;
        if (millis() - tareDelayStartTime < TARE_DELAY_DURATION) {
            sprintf(buf, "Tarieren: %.1fs", (TARE_DELAY_DURATION - (millis() - tareDelayStartTime)) / 1000.0);
            currentTareUIMsg = String(buf);
        } else {
            currentTareUIMsg = "Tariere jetzt!";
        }
    }

    bool weightHasChanged      = abs(weightToDisplay - lastDisplayedNumericWeight) >= 0.05;
    bool tareMessageHasChanged = (currentTareUIMsg != lastDisplayedTareMessage);
    bool displayModeHasChanged = (isCurrentlyShowingTareMsg != (lastDisplayedTareMessage != ""));
    bool batteryHasChanged     = (batteryPercent != lastDisplayedBatteryPercentage);

    if (!weightHasChanged && !tareMessageHasChanged && !batteryHasChanged &&
        !forceFullDisplayRedraw && !displayModeHasChanged) {
        return;
    }

    if (weightHasChanged || tareMessageHasChanged || forceFullDisplayRedraw || displayModeHasChanged) {
        weightSprite.fillSprite(TFT_BLACK);

        if (isCurrentlyShowingTareMsg) {
            weightSprite.setTextColor(ACCENT_COLOR, TFT_BLACK);
            weightSprite.drawString(currentTareUIMsg, weightSprite.width() / 2, weightSprite.height() / 2, 4);
        } else {
            weightSprite.setTextColor(TFT_SILVER, TFT_BLACK);
            weightSprite.setTextDatum(BR_DATUM);
            weightSprite.drawString("g", weightSprite.width() - 2, weightSprite.height() - 2, 4);

            weightSprite.setTextColor(ACCENT_COLOR, TFT_BLACK);
            weightSprite.setTextDatum(MC_DATUM);
            
            // ####################################################################
            // ### NEUE LOGIK: "Dead Zone" um Nullpunkt, um "-0.0" zu verhindern ###
            // ####################################################################
            float displayValue = weightToDisplay;
            // Wenn das Gewicht sehr nah an Null ist (z.B. durch Rauschen), zeige exakt "0.0" an.
            // fabs() ist die Fließkomma-Version von abs() (Absolutwert).
            if (fabs(displayValue) < 0.05) {
                displayValue = 0.0;
            }
            // ####################################################################

            dtostrf(displayValue, 6, 1, buf); // Verwende den ggf. angepassten Wert
            char* p = buf; while (*p == ' ') p++;

            int integralPart = abs((int)weightToDisplay);
            uint8_t fontNumber = 7;
            if (integralPart >= 1000) fontNumber = 4;
            else if (integralPart >= 100) fontNumber = 6;

            weightSprite.drawString(p, weightSprite.width() / 2 - 15, weightSprite.height() / 2, fontNumber);
        }
        weightSprite.pushSprite(tft.width() / 2 - weightSprite.width() / 2,
                                tft.height() / 2 - weightSprite.height() / 2);
    }

    if (batteryHasChanged || forceFullDisplayRedraw) {
        drawBatteryIcon(tft.width() - 70, 5, batteryPercent, false);
    }

    lastDisplayedNumericWeight   = weightToDisplay;
    lastDisplayedTareMessage     = currentTareUIMsg;
    lastDisplayedBatteryPercentage = batteryPercent;
    forceFullDisplayRedraw = false;
}

void loop_waage(); // Forward declaration

// ####################################################################
// ### ANGEPASSTE `loop()` FUNKTION (Nur noch der Aufruf)           ###
// ####################################################################
void loop() {
    loop_waage();
}

// ####################################################################
// ### ANGEPASSTE `loop_waage()` FUNKTION (Mit USB-Neustart-Logik)  ###
// ####################################################################
void loop_waage() {
    // --- NEU: Logik für Neustart bei USB-Trennung ---
    if (wasPreviouslyCharging && !cachedIsCharging) {
        Serial.println("USB-Verbindung getrennt. Starte Controller neu...");
        tft.fillScreen(TFT_BLACK);
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        tft.drawString("Neustart...", tft.width() / 2, tft.height() / 2, 4);
        delay(2000);
        ESP.restart();
    }
    wasPreviouslyCharging = cachedIsCharging; // Zustand für nächsten Durchlauf merken

    if (cachedIsCharging) {
        displayWeightAndStatus();
        lastButtonActivityTime = millis(); // Verhindert Schlaf bei angeschlossenem USB
        delay(500);
        return;
    }
    
    // Normaler Betrieb
    processButton1_ToggleMode();
    processButton2_Tare();
    if (!tareAfterDelay) {
        currentScaleData.weight_g = scale.get_units(1);
    }
    sendDataViaEspNow();
    displayWeightAndStatus();

    // In den Schlaf gehen bei Inaktivität
    if (millis() - lastButtonActivityTime > inactivitySleepTimeout) {
        goToSleep();
    }
    delay(50);
}