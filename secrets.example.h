#pragma once

// ==========================================
// Credentials template
// ==========================================
// 1. Copy this file to `secrets.h` (same folder as the .ino).
// 2. Fill in your own values.
// 3. `secrets.h` is listed in .gitignore and never leaves your machine.

// Primary Wi-Fi network
#define SECRET_PRIMARY_SSID     "your-primary-ssid"
#define SECRET_PRIMARY_PASSWORD "your-primary-password"

// Backup Wi-Fi network (failover). Leave the SSID empty ("") to disable.
#define SECRET_BACKUP_SSID      ""
#define SECRET_BACKUP_PASSWORD  ""

// healthchecks.io ping UUID - the last part of your check's ping URL:
// https://hc-ping.com/<UUID>
// Treat it like a password: anyone who has it can ping (or spoof) your check.
#define SECRET_HC_UUID          "00000000-0000-0000-0000-000000000000"
