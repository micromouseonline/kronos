// ----------------------------------------------------------------------------
//  mdns.h — Advertises this device as http://cerberus.local/ via mDNS, so
//  spectators/gates don't need to know its DHCP-assigned IP. mdns_start()
//  must only be called once Wi-Fi actually has an IP (wired into main.cpp's
//  combined wifi_on_connected handler, alongside net/http-server.h's
//  http_server_restart() -- see wifi-manager.h's hook comment) -- it
//  previously ran inline in setup() before Wi-Fi was necessarily connected,
//  which is suspected (unconfirmed; no serial capture was taken at the
//  time) to have caused a full device hang after a reboot, since everything
//  after it in setup() would never run if it blocked.
// ----------------------------------------------------------------------------
#pragma once

#include <ESPmDNS.h>

#include "debug-log.h"

inline void mdns_start() {
  if (!MDNS.begin("cerberus")) {
    debug_println("[SYSTEM] mDNS failed to start");
  } else {
    debug_println("[SYSTEM] mDNS started: http://cerberus.local/");
  }
}
