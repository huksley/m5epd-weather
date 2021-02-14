#undef __APPLE__
#include <M5EPD.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <FS.h>
#include <LITTLEFS.h>
#include <ezTime.h>
#include <regex>
#include "M5PanelWidget.h"
#include "defs.h"

#define ERR_WIFI_NOT_CONNECTED "ERROR: Wifi not connected"
#define ERR_HTTP_ERROR "ERROR: HTTP code "
#define ERR_GETITEMSTATE "ERROR in getItemState"

#define DEBUG true

#define WIDGET_COUNT 6
#define FONT_CACHE_SIZE 256

// Global vars
M5EPD_Canvas canvas(&M5.EPD);
M5PanelWidget *widgets = new M5PanelWidget[WIDGET_COUNT];
HTTPClient httpClient;
int loopIndex = 0;
unsigned long upTime;

DynamicJsonDocument jsonDoc(30000);

int previousSysInfoMillis = 0;
int currentSysInfoMillis;

int previousRefreshMillis = 0;
int currentRefreshMillis;

uint16_t _last_pos_x = 0xFFFF, _last_pos_y = 0xFFFF;

Timezone openhabTZ;

/* Reminders
    EPD canvas library https://docs.m5stack.com/#/en/api/m5paper/epd_canvas
    Text aligment https://github.com/m5stack/M5Stack/blob/master/examples/Advanced/Display/TFT_Float_Test/TFT_Float_Test.ino
*/

void debug(String function, String message)
{
    if (DEBUG)
    {
        Serial.print(F("DEBUG (function "));
        Serial.print(function);
        Serial.print(F("): "));
        Serial.println(message);
    }
}

bool fetch(String &url, String &response)
{
    HTTPClient http;
    debug(F("httpRequest"), "HTTP request to " + String(url));
    if (WiFi.status() != WL_CONNECTED)
    {
        Serial.println(ERR_WIFI_NOT_CONNECTED);
        response = String(ERR_WIFI_NOT_CONNECTED);
        return false;
    }
    http.useHTTP10(true);
    http.setReuse(false);
    http.begin(url);
    int httpCode = http.GET();
    if (httpCode != HTTP_CODE_OK)
    {
        Serial.println(String(ERR_HTTP_ERROR) + String(httpCode));
        response = String(ERR_HTTP_ERROR) + String(httpCode);
        http.end();
        return false;
    }
    response = http.getString();
    http.end();
    debug(F("httpRequest"), F("HTTP request done"));
    return true;
}

void postWidgetValue(const String &itemName, const String &newValue)
{
}

void setTimeZone() // Gets timezone from OpenHAB
{
    String timezone = TIMEZONE;
    debug("setTimeZone", "OpenHAB timezone= " + timezone);
    openhabTZ.setLocation(timezone);
}

void displaySysInfo()
{
    // Display system information
    canvas.setTextSize(FONT_SIZE_STATUS_CENTER);
    canvas.setTextDatum(TC_DATUM);

    canvas.drawString(openhabTZ.dateTime("H:i"), 80, 40);

    canvas.setTextSize(FONT_SIZE_LABEL);
    canvas.setTextDatum(TL_DATUM);

    canvas.drawString("Free Heap:", 0, 250);
    canvas.drawString(String(ESP.getFreeHeap()) + " B", 0, 290);

    canvas.drawString("Voltage: ", 0, 340);
    canvas.drawString(String(M5.getBatteryVoltage()) + " mV", 0, 380);

    upTime = millis() / (60000);
    canvas.drawString("Uptime: ", 0, 430);
    canvas.drawString(String(upTime) + " min", 0, 470);
    canvas.pushCanvas(780, 0, UPDATE_MODE_A2);
}

void updateSiteMap(int widget);

void setup()
{
    M5.begin(true, false, true, true, false); // bool touchEnable = true, bool SDEnable = false, bool SerialEnable = true, bool BatteryADCEnable = true, bool I2CEnable = false
    M5.disableEXTPower();
    M5.EPD.Clear(true);
    M5.RTC.begin();
    M5.SHT30.Begin();

    // FS Setup
    Serial.println(F("Inizializing FS..."));
    if (SPIFFS.begin())
    {
        Serial.println(F("SPIFFS mounted correctly."));
    }
    else
    {
        Serial.println(F("!An error occurred during SPIFFS mounting"));
    }

    Serial.println(F("Inizializing LITTLEFS FS..."));
    if (LITTLEFS.begin())
    {
        Serial.println(F("LITTLEFS mounted correctly."));
    }
    else
    {
        Serial.println(F("!An error occurred during LITTLEFS mounting"));
    }

    // Get all information of LITTLEFS
    unsigned int totalBytes = LITTLEFS.totalBytes();
    unsigned int usedBytes = LITTLEFS.usedBytes();

    // TODO : Should fail and stop if SPIFFS error

    Serial.println("===== File system info =====");

    Serial.print("Total space:      ");
    Serial.print(totalBytes);
    Serial.println("byte");

    Serial.print("Total space used: ");
    Serial.print(usedBytes);
    Serial.println("byte");

    Serial.println();

    canvas.createCanvas(160, 540);
    canvas.loadFont("/FreeSansBold.ttf", LITTLEFS);
    // TODO : Should fail and stop if font not found

    canvas.setTextSize(FONT_SIZE_LABEL);
    canvas.createRender(FONT_SIZE_LABEL, FONT_CACHE_SIZE);
    canvas.createRender(FONT_SIZE_STATUS_CENTER, FONT_CACHE_SIZE);
    canvas.createRender(FONT_SIZE_STATUS_BOTTOM, FONT_CACHE_SIZE);

    // Setup Wifi
    Serial.println(F("Starting Wifi"));
    WiFi.begin(WIFI_SSID, WIFI_PSK);
    while (WiFi.status() != WL_CONNECTED)
    {
        delay(500);
        Serial.print(".");
    }
    Serial.println();
    Serial.println(F("WiFi connected"));
    Serial.println(F("IP address: "));
    Serial.println(WiFi.localIP());

    // Init widgets
    for (byte i = 0; i < WIDGET_COUNT; i++)
    {
        int x = i % BUTTONS_X;
        int y = i / BUTTONS_X;
        widgets[i].init(i, 0, 40 + x * (40 + BUTTON_SIZE), 40 + y * (40 + BUTTON_SIZE));
    }

    // NTP stuff
    setInterval(3600);
    waitForSync();
    setTimeZone();
    displaySysInfo();
    updateSiteMap(-1);
}

void updateSiteMap(int widget)
{

    M5.SHT30.UpdateData();
    String url = WEATHER_URL;
    String response;
    bool success = fetch(url, response);
    DynamicJsonDocument json(50000);
    deserializeJson(json, response);
    debug(F("updateSiteMap"), "fetch API:" + String(success ? 1 : 0) + ", size " + String(response.length()));

    String type = "type";
    String itemState = "S2";
    String itemName = "IName";
    String itemType = "Ipype";
    char buf[64];
    float temp = M5.SHT30.GetTemperature() - 7;
    snprintf(buf, 63, "%2.0f *C", temp);

    Serial.println("Item type=" + type + " itemName=" + itemName + " itemType=" + itemType + " itemState=" + itemState);
    int index = 0;
    widgets[index++].update("Home", buf, itemState, "blinds", type, itemName, itemType);

    float hum = M5.SHT30.GetRelHumidity();
    snprintf(buf, 63, "%0.2f%%", hum);
    widgets[index++].update("Humidity", buf, itemState, "humidity", type, itemName, itemType);

    float nowTemp = json["current"]["temp"].as<float>();
    snprintf(buf, 63, "%2.0f *C", nowTemp);
    widgets[index++].update("Now", buf, itemState, "temperature", type, itemName, itemType);

    tmElements_t tm;
    JsonArray hourly = json["hourly"].as<JsonArray>();
    int indexes[] = {-1, -1, -1};
    String names[] = {"?", "?", "?"};
    int next = 0;
    int max = 3;
    bool haveMorning = false;
    bool haveEvening = false;
    bool haveDay = false;

    for (int i = 0; i < hourly.size(); i++)
    {
        JsonObject hour = hourly[i].as<JsonObject>();
        long time = openhabTZ.tzTime(hour["dt"], UTC_TIME);
        breakTime(time, tm);
        int hr = tm.Hour;
        Serial.printf("%i %i:%i\n", i, hr, tm.Minute);

        if (hr >= 7 && hr < 11 && next < max && !haveMorning)
        {
            names[next] = "Morning";
            indexes[next] = i;
            next++;
            haveMorning = true;
            Serial.printf("%i morning\n", i);
        }

        if (hr >= 12 && hr < 18 && next < max && !haveDay)
        {
            names[next] = "Day";
            indexes[next] = i;
            next++;
            haveDay = true;
            Serial.printf("%i day\n", i);
        }

        if (hr >= 18 && hr < 21 && next < max && !haveEvening)
        {
            names[next] = "Evening";
            indexes[next] = i;
            next++;
            haveEvening = true;
            Serial.printf("%i evening\n", i);
        }
    }

    debug(F("updateSiteMap"), "Created indexes");

    float morningTemp = json["hourly"][indexes[0]]["temp"].as<float>();
    snprintf(buf, 63, "%2.0f *C", morningTemp);
    widgets[index++].update(names[0], buf, itemState, "climate", type, itemName, itemType);

    float dayTemp = json["hourly"][indexes[1]]["temp"].as<float>();
    snprintf(buf, 63, "%2.0f *C", dayTemp);
    widgets[index++].update(names[1], buf, itemState, "climate", type, itemName, itemType);

    float eveningTemp = json["hourly"][indexes[2]]["temp"].as<float>();
    snprintf(buf, 63, "%2.0f *C", eveningTemp);
    widgets[index++].update(names[2], buf, itemState, "climate", type, itemName, itemType);

    debug(F("updateSiteMap"), "Updated widgets");

    index = 0;
    if (widget == -1 || widget == index)
    {
        widgets[index].draw(UPDATE_MODE_GC16);
    }
    index++;

    if (widget == -1 || widget == index)
    {
        widgets[index].draw(UPDATE_MODE_GC16);
    }
    index++;

    if (widget == -1 || widget == index)
    {
        widgets[index].draw(UPDATE_MODE_GC16);
    }
    index++;

    if (widget == -1 || widget == index)
    {
        widgets[index].draw(UPDATE_MODE_GC16);
    }
    index++;

    if (widget == -1 || widget == index)
    {
        widgets[index].draw(UPDATE_MODE_GC16);
    }
    index++;

    if (widget == -1 || widget == index)
    {
        widgets[index].draw(UPDATE_MODE_GC16);
    }
    index++;

    debug(F("updateSiteMap"), "Done refreshes");
    debug(F("updateSiteMap"), "Done, free heap" + String(ESP.getFreeHeap()));
}

int forceRefresh = -1;

// Loop
void loop()
{
    if (loopIndex % 1000)
    {
        Serial.printf("Loop %i\n", loopIndex++);
    }

    // Check touch
    if (M5.TP.avaliable())
    {
        M5.TP.update();
        Serial.printf("Touch available\n");
        bool is_finger_up = M5.TP.isFingerUp();
        if (is_finger_up || (_last_pos_x != M5.TP.readFingerX(0)) || (_last_pos_y != M5.TP.readFingerY(0)))
        {
            _last_pos_x = M5.TP.readFingerX(0);
            _last_pos_y = M5.TP.readFingerY(0);
            if (!is_finger_up)
            {
                for (byte i = 0; i < WIDGET_COUNT; i++)
                    if (widgets[i].testIfTouched(_last_pos_x, _last_pos_y))
                    {
                        debug("loop", "Widget touched: " + String(i));
                        forceRefresh = i;
                    }
            }
        }
        M5.TP.flush();
    }

    currentSysInfoMillis = millis();
    if ((currentSysInfoMillis - previousSysInfoMillis) > 10000)
    {
        previousSysInfoMillis = currentSysInfoMillis;
        displaySysInfo();
    }

    currentRefreshMillis = millis();
    if (forceRefresh >= 0 || (currentRefreshMillis - previousRefreshMillis) > REFRESH_INTERVAL * 1000)
    {
        previousRefreshMillis = currentRefreshMillis;
        debug("Loop", "Full refresh forceRefresh = " + String(forceRefresh));
        updateSiteMap(forceRefresh);
        M5.EPD.UpdateFull(UPDATE_MODE_GL16);
        debug("Loop", "Full refresh done");
        forceRefresh = -1;
    }

    events(); // for ezTime
}