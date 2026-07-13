#pragma once

// -----------------------------------------------------------------------------
// branding.h — white-label product identity (compile-time). See CLAUDE.md.
//
// Change BRAND_NAME (and rebuild) to ship the firmware under a different brand.
// Everything user-visible — display, web UI, serial banner — must derive from
// here; never hardcode a brand string elsewhere.
//
// The string is UTF-8; the rendering layer (OLED / web) is responsible for
// drawing accented characters. The mDNS host and AP SSID defaults will join
// this file when Wi-Fi provisioning is added.
// -----------------------------------------------------------------------------

#define BRAND_NAME "Café & Aço"
