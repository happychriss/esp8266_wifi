

#include <ESP8266WiFi.h>
//#include <WiFiClientSecureBearSSL.h>
#include <WiFiClientSecureAxTLS.h> // force use of AxTLS (BearSSL is now default)
#include "string"
#include "main_esp8266_wifi.h"
#include "time.h"
#include "softser_old.h"
#include "EEPROM.h"
#include "cal_comm.h"
#include <support.h>
#include "oauth.h"

// to get serial working: disconnect serial, press reset and shortly afterwards connect serial

//receivePin, transmitPin,
//SoftwareSerial swSer(12, 13, false, 256); //STM32: Weiß muss an RX, Grun an TX

SoftwareSerial swSer(12, 13, false, 256); //STM32: Weiß muss an RX, Grun an TX


char my_ssid[20] = {0};
char my_pwd[20] = {0};

char str_time[22];
char *global_access_token;
char * error_msg;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored  "-Wdeprecated-declarations"


tm global_time;


#pragma GCC diagnostic pop


rtcDataOauthStruct rtcOAuth;
rtcDataWakeupStruct rtcWakeUp;

// char global_access_token[150] = "ya29.GluPBv80YVsTmc5uCIDt5AoEDbdoP-srm1zk7zzfF-1qRCmOHwcTlXsIgGGKJmNufn5XaRSAdh4B-T4gkizphT20SFW54IlyMZD7wepBC-2OW9W1elL8Z6swZTcO\0";


// Set global variable attributes.
uint8_t global_status;
bool b_reset_authorization = false;






void setup() {

    pinMode(PIN_POWER_CAL, OUTPUT);
    pinMode(PIN_LED, OUTPUT);
    digitalWrite(PIN_POWER_CAL, LOW);
    delay(500);
    swSer.begin(19200);
    EEPROM.begin(250);
    delay(500);
    BLINK(0);

#if defined(MYDEBUG) || defined(MYDEBUG_CORE)
    Serial.begin(19200);
    delay(200);
#endif

#if defined(WAIT_SERIAL) && (defined(MYDEBUG) || defined(MYDEBUG_CORE))
    int c;
    DPL("Key to start, 'x' to reset authentication!!");
    c = SerialKeyWait();
    DP("Typed:");
    DPL(c);
    if (c == 120) { //x
        b_reset_authorization = true;
    }
#endif
    global_status = WAKE_UP_FROM_SLEEP; // normal operations
//    global_status = CAL_QUICK_INIT; // debugging
}


void loop() {
    char ssid[20] = {0};
    char pwd[20] = {0};

   // ESP.deepSleep(0);

    CP("******************* State:");
    CPD(global_status);
    CPL(" *****");


    bool b_send_ok = false;

    switch (global_status) {

        case CAL_QUICK_INIT:
            CPL("** QUICK INIT **");
            RTC_WakeUpRead();
            global_status = CAL_WAKEUP;
            break;

        case WAKE_UP_FROM_SLEEP:
            CPL("** GOOD MORNING! **");
            if (RTC_WakeUpRead()) {

                if (rtcWakeUp.wakeup_count > 1) {
                    rtcWakeUp.wakeup_count = rtcWakeUp.wakeup_count - 1;
                    RTC_WakeUpWrite();
                    CP("DeepSleep again for Cycles:");
                    CPL(rtcWakeUp.wakeup_count);
                    MyDeepSleep(MAX_SLEEP_MIN, RF_NO_CAL);
                    delay(1500);
                }

                if (rtcWakeUp.wakeup_count == 1) {
                    rtcWakeUp.wakeup_count = rtcWakeUp.wakeup_count - 1;
                    RTC_WakeUpWrite();
                    CP("DeepSleep again for min:");
                    CPL(rtcWakeUp.remaining_sleep_min);
                    MyDeepSleep(rtcWakeUp.remaining_sleep_min, RF_CAL);
                    delay(1500);
                }

                if (rtcWakeUp.wakeup_count == 0) {
                    global_status = CAL_WAKEUP;;
                    CPL("DeepSleep DONE - Start Calendar Flow");
                }
            } else {
                global_status = CAL_WAKEUP;
                CPL("DeepSleep *** No Valid RTC found - Start Calendar Flow **");
            }

            break;

        case CAL_WAKEUP:
            BLINK(2);
            CPL("Wake up the calendar");
            digitalWrite(PIN_POWER_CAL, HIGH);
            delay(3000);
            WriteCommandToCalendar(CMD_AWAKE_TEST);
            WaitForCalendarStatus();
            CPL("Calendar I am awake!");
            BLINK(0);
            delay(5000);
            global_status = CAL_WIFI_GET_CONFIG;
            break;

        case CAL_WIFI_GET_CONFIG:
            CPL("Request Wifi Config from Calendar..");
            WriteCommandToCalendar(CMD_SEND_WIFI);
            ReadFromCalendar(my_ssid);CP("SSID:");CPL(my_ssid);
            ReadFromCalendar(my_pwd);CP("PWD: ");CPL(my_pwd);

            global_status = WIFI_INIT;


            break;

        case WIFI_INIT:

            if (SetupMyWifi(my_ssid, my_pwd)) {
                error_msg = strdup(" - WIFI Connection ERROR!");
                global_status = ESP_SEND_ERROR_MSG;
                break;
            }

            SetupTimeSNTP(&global_time);

            BLINK(3);

            if (RTC_OAuthRead()) {
                CPL("** OAUTH RTC Status");
                CP("Status: ");
                CPL(rtcOAuth.status);
                CP("Device-Code: ");
                CPL(rtcOAuth.device_code);
                CP("Refresh-Token: ");
                CPL(rtcOAuth.refresh_token);

                if (b_reset_authorization) {
                    CPL("*Reset Google User access - set initial state*");
                    global_status = WIFI_INITIAL_STATE;
                } else {
                    global_status = rtcOAuth.status;
                }

            } else {
                CPL("** Invalid RTC Status - Reauth");;
                global_status = WIFI_INITIAL_STATE;
            }

            break;

//********************************************************************************************

        case WIFI_INITIAL_STATE:

            const char *user_code;
            user_code = request_user_and_device_code();

            char *my_user_code;
            my_user_code = strdup(user_code);

            WriteCommandToCalendar(CMD_SHOW_USERCODE_CALENDAR);
            WriteToCalendar(my_user_code);

            CPL("*********************");
            CP("USER-CODE:  ");
            CPL(user_code);
            CP("*********************");

            global_status = WIFI_AWAIT_CHALLENGE;
            break;

        case WIFI_AWAIT_CHALLENGE:
            CP("Await challenge for Device:");
            CPL(rtcOAuth.device_code);
            global_status = poll_authorization_server();
            break;

        case WIFI_CHECK_ACCESS_TOKEN:
            CPL("Refresh Access Token");
            global_status = request_access_token();
            break;


        case CAL_PAINT_UPDATE:

            WriteCommandToCalendar(CMD_READ_CALENDAR);

            strftime(str_time, sizeof(str_time), "%Y %m %d %H:%M:%S", &global_time);
            CP("******* Request Calendar Update with time-stamp:");
            CPL(str_time);
            WriteToCalendar(str_time);

            while (true) {
                BLINK(0);CP("Wait Calendar Ready, heap is ");CPL(ESP.getFreeHeap());

                if (WaitForCalendarStatus() != CALENDAR_STATUS_MORE) break;

                // Read the Google Calendar URL from STM32, request to Goolge and send back to STM32
                char request[250] = {0};
                ReadSWSer(request);
                b_send_ok = calendarGetRequest(request);
                if (!b_send_ok) break;
                CPL("******* SUCCESS: Calendar Update *******");
//                break;

            }

            if (!b_send_ok) {
                CPL("******* ERROR: Calendar Update *******");
                ErrorToDisplay ("Ugggh, problem!!");
                global_status = ESP_SEND_ERROR_MSG;
            } else {
                CPL("******* SUCCESS: ALL  Requests done ************");
                global_status = CAL_PAINT_DONE;
            }

            break;

        case CAL_PAINT_DONE:
            delay(500);
            WriteCommandToCalendar(CMD_SHUTDOWN_CALENDAR);
            delay(500);
            digitalWrite(PIN_POWER_CAL, LOW);
            global_status = ESP_START_SLEEP;
            break;

        case ESP_START_SLEEP:

            CP("*** PREPARE DEEP SLEEP with Warp-Factor: ");
            CP(WARP_FACTOR);
            CPL("***");

#ifdef CYCLE_SLEEP_HOURS
            CalculateSleepUntil(global_time.tm_hour+CYCLE_SLEEP_HOURS, global_time.tm_min);
#else
            CalculateSleepUntil(WAKE_UP_HOUR, WAKE_UP_MIN);
#endif

            if (rtcWakeUp.wakeup_count == 0) {
                CP("*** GOOD-NIGHT - Send to DeepSleep for min: ");
                CPL(rtcWakeUp.remaining_sleep_min);
                delay(500);
                MyDeepSleep(rtcWakeUp.remaining_sleep_min, RF_CAL);
                delay(1500);
            } else {
                CP("*** GOOD-NIGHT - Send to DeepSleep for cycles: ");
                CPL(rtcWakeUp.wakeup_count);
                delay(500);
                MyDeepSleep(MAX_SLEEP_MIN, RF_NO_CAL);
                delay(1500);
            }
            // here we will not arrive :-)
            delay(1500);

            break;

        case ESP_SEND_ERROR_MSG:
            CPL("************* ERROR *******");
            CPL("Shutdown Calendar and Reboot.....");
            delay(500);
            swSer.write(CMD_SHUTDOWN_CALENDAR);
            delay(500);
            digitalWrite(PIN_POWER_CAL, LOW);
            delay(500);
            digitalWrite(PIN_POWER_CAL, HIGH);
            delay(1000);

            WriteCommandToCalendar(CMD_SHOW_ERROR_MSG);
            WriteToCalendar(error_msg);
            delay(500);
            digitalWrite(PIN_POWER_CAL, LOW);
            delay(500);
            MyDeepSleep(0, RF_NO_CAL);
            delay(1500);
            break;

        default:
            CPL("ERROR");
            break;
    }

    yield();

}


