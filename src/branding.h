#pragma once

// -----------------------------------------------------------------------------
// branding.h — white-label product identity (compile-time). See CLAUDE.md.
//
// Change BRAND_NAME (and rebuild) to ship the firmware under a different brand.
// Everything user-visible — display, web UI, serial banner — must derive from
// here; never hardcode a brand string elsewhere.
//
// The string is UTF-8; the rendering layer (OLED / web) is responsible for
// drawing accented characters.
//
// Network identity is a separate, ASCII-safe pair (no spaces/accents): SSID and
// mDNS hostnames cannot carry the accented BRAND_NAME, so they are their own
// defines. The mDNS host stays user-overridable during provisioning (PRD F6).
// -----------------------------------------------------------------------------

#define BRAND_NAME "Café & Aço"

// --- Network identity (ASCII-safe; default brand: torrador) ---
#define BRAND_MDNS_HOST   "torrador"        // -> http://torrador.local (overridable at provisioning)
#define BRAND_AP_SSID     "Torrador"         // {Brand} network (config + permanent AP mode)
#define BRAND_AP_PASSWORD "torrador"        // default AP WPA2 passphrase (>=8 chars); change/print per device
