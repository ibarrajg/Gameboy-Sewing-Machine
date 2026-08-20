/**
 * Phase 2 step 1: ESP-NOW radio proof alongside SoftAP streaming.
 * Not a Game Boy cable yet — ping/echo + GPIO + stats only.
 */
#pragma once

#include <Arduino.h>
#include <ESPAsyncWebServer.h>

static const int LINK_GPIO_SEND = 4; // toggle on ping/pong transmit
static const int LINK_GPIO_RECV = 5; // toggle on ESP-NOW receive
static const uint8_t LINK_WIFI_CHANNEL = 1;

enum class LinkRole : uint8_t { Idle = 0, Ping = 1, Echo = 2 };

bool linkRadioBegin();
void linkRadioRegisterRoutes(AsyncWebServer &server);

// Called from Core 0 ~1 ms pump. Sends a ping when role is Ping and running.
void linkRadioPoll();
