#include <algorithm>

extern "C" {
#include <osapi.h>
#include <os_type.h>
}

#include <ESP8266WiFi.h>
#include <ESPAsyncTCP.h>
#include <EEPROM.h>

#include "config.h"
#include "PacketProcessor.h"
#include "MsgParser.h"
#include "GPIO.h"
#include "WifiScan.h"
#include "OLED.h"

#define LOG_PRINTF_IMPL OLED_printf
#include "log.h"

static AsyncClient* client;

static PacketProcessor packetProcessor(true);
static MsgParser msgParser;

static gpio::OUT relay(PIN_RELAY);

static WiFiScan wiFiScan;

const size_t MAX_SSIDRE_LEN = 50 + 1;
const size_t MAX_PASSWD_LEN = 16 + 1;
const uint16_t HOST_INFO_CONFIGED = 0x5AA5;
struct HostInfo {
    uint16_t configed;
    char ssidRE[MAX_SSIDRE_LEN];
    char passwd[MAX_PASSWD_LEN];
};

static HostInfo* hostInfo;

static void sendRaw(const std::string& data) {
    client->write(data.data(), data.size());
}

static void sendRaw(const void* data, size_t size) {
    client->write((char*)data, size);
}

static void sendJasonMsg(const std::string& jsonMsg) {
    auto payload = packetProcessor.pack(jsonMsg);
    sendRaw(payload.data(), payload.size());
}

static void sendMsgByTemplate(const std::string& type, const std::string& msg) {
    auto jsonMsg = msgParser.makeMsg(type, msg);
    sendJasonMsg(jsonMsg);
}

static void handleData(void* arg, AsyncClient* client, void *data, size_t len) {
    LOGD("handleData: from: %s, len: %u, data: %s"
            , client->remoteIP().toString().c_str()
            , len
            , std::string((char*)data, std::min(len, 10U)).c_str()
            );
    packetProcessor.feed((uint8_t*)data, len);
}

static void onConnect(void* arg, AsyncClient* client) {
    LOGD("onConnect: host_ip: %s host_port:%d", WiFi.gatewayIP().toString().c_str(), TCP_PORT);
    sendMsgByTemplate(Msg::Type::MSG, "hello");
}

static void initHostFromEEPROM() {
    EEPROM.begin(sizeof(HostInfo));
    hostInfo = reinterpret_cast<HostInfo*>(EEPROM.getDataPtr());

#if TRY_USE_EEPROM_INFO
    if (hostInfo->configed != HOST_INFO_CONFIGED) {
        hostInfo->configed = HOST_INFO_CONFIGED;
        strcpy(hostInfo->ssidRE, SSID_RE_DEFAULT);
        strcpy(hostInfo->passwd, PASSWORD_DEFAULT);
        EEPROM.commit();
        LOGD("init hostInfo to EEPROM: ssidRE: %s, passwd: %s", hostInfo->ssidRE, hostInfo->passwd);
    } else {
        LOGD("use hostInfo from EEPROM: ssidRE: %s, passwd: %s", hostInfo->ssidRE, hostInfo->passwd);
    }
#else
    strcpy(hostInfo->ssidRE, SSID_RE_DEFAULT);
    strcpy(hostInfo->passwd, PASSWORD_DEFAULT);
    LOGD("use default hostInfo: ssidRE: %s, passwd: %s", hostInfo->ssidRE, hostInfo->passwd);
#endif
}

int	OLED_printf(const char *fmt, ...) {
    va_list _va_list;
    va_start(_va_list, fmt);

    const int bufferSize = 1024;
    char logBuf[bufferSize];
    int size = vsnprintf(logBuf, bufferSize, fmt, _va_list);
    Serial.print(logBuf);

    auto make_empty = [](char& c) {
        if (c == '\r' || c == '\n') {
            c = 0;
        }
    };
    make_empty(logBuf[size-1]);
    make_empty(logBuf[size-2]);
    make_empty(logBuf[size-3]);

    OLED_Fill(0x00);
    OLED_ShowChar(0, 0, (unsigned char*)logBuf, 1);

    va_end(_va_list);
    return size;
}

void setup() {
    Serial.begin(115200);
    delay(20);
    OLED_Init();

    std::set_new_handler([] {
        FATAL("out of memory");
        for(;;) {
            delay(100);
        }
    });

    initHostFromEEPROM();

    LOGD("init wiFiScan");
    wiFiScan.setSSIDEnds(hostInfo->ssidRE);

    LOGD("init client");
    client = new AsyncClient;
    client->onData(&handleData, client);
    client->onConnect(&onConnect, client);
    client->onDisconnect([](void*, AsyncClient*){
        LOGD("client disconnect");
    }, client);

    LOGD("init packetProcessor");
    packetProcessor.setMaxBufferSize(1024);
    packetProcessor.setOnPacketHandle([](uint8_t* data, size_t size) {
        msgParser.parser(std::string((char*)data, size));//todo: performance
    });

    LOGD("init msgParser");
    msgParser.setRelayCb([](bool on, MsgParser::ID_t id) {
        LOGD("relay pin set to: %d", on);
        relay.set(on);

        sendJasonMsg(MsgParser::makeRsp(id));
    });

    LOGD("init message callback");
    msgParser.setHostRegexCb([](const std::string& hostRegex, MsgParser::ID_t id) {
        LOGD("HostRegexCb: %s", hostRegex.c_str());
        if (hostRegex.length() > MAX_SSIDRE_LEN - 1) {
            LOGD("hostRegex too long");
            sendJasonMsg(MsgParser::makeRsp(id, false));
        } else {
            wiFiScan.setSSIDEnds(hostRegex);
            strcpy(hostInfo->ssidRE, hostRegex.c_str());
            EEPROM.commit();
            sendJasonMsg(MsgParser::makeRsp(id, true));
        }
    });
    msgParser.setHostPasswdCb([](const std::string& passwd, MsgParser::ID_t id) {
        LOGD("HostPasswdCb: %s", passwd.c_str());
        if (passwd.length() > MAX_SSIDRE_LEN - 1) {
            LOGD("passwd too long");
            sendJasonMsg(MsgParser::makeRsp(id, false));
        } else {
            strcpy(hostInfo->passwd, passwd.c_str());
            EEPROM.commit();
            sendJasonMsg(MsgParser::makeRsp(id, true));
        }
    });
}

void loop() {
    if (not WiFi.isConnected()) {
        WiFi.mode(WIFI_STA);

#if TEST_WITH_DESKTOP
        WiFi.begin(TEST_WITH_DESKTOP_SSID, TEST_WITH_DESKTOP_PASSWD);
#else
        SCAN:
        auto ssid = wiFiScan.scan();
        if (ssid.empty()) {
            LOGD("scan empty");
            delay(1000);
            goto SCAN;
        }

        LOGD("try connect to: %s", ssid.c_str());

        auto isConfigAp = ssid == CONFIG_AP_SSID;
        WiFi.begin(ssid.c_str(), isConfigAp ? CONFIG_AP_PASSWD : hostInfo->passwd);
#endif
        while (WiFi.status() != WL_CONNECTED) {
            LOGD("WiFi connecting...");
            delay(500);
        }
        LOGD("gatewayIP: %s", WiFi.gatewayIP().toString().c_str());
    }

    if (not client->connected()) {
#if TEST_WITH_DESKTOP
        client->connect(TEST_WITH_DESKTOP_IP, TEST_WITH_DESKTOP_PORT);
#else
        client->connect(WiFi.gatewayIP(), TCP_PORT);
#endif
        delay(500);
    }
}
