#pragma once

#include <Arduino.h>
#include <SPIFFS.h>
#include <time.h>

// ============================================================================
// Debug Logger Module - Enable/disable logging with this flag
// ============================================================================
static const bool DEBUG_LOGGING_ENABLED = true;  // Set to false to disable all logging

// Log file on SPIFFS
static const char DEBUG_LOG_FILE[] = "/debug_log.txt";
static const uint32_t MAX_LOG_SIZE = 65536;  // 64KB max log file (circular)

// ============================================================================
// Internal helpers (only used when DEBUG_LOGGING_ENABLED is true)
// ============================================================================

static void debug_log_trim_if_needed() {
  if (!DEBUG_LOGGING_ENABLED) return;
  
  File logFile = SPIFFS.open(DEBUG_LOG_FILE, "r");
  if (!logFile) return;
  
  uint32_t fileSize = logFile.size();
  logFile.close();
  
  if (fileSize > MAX_LOG_SIZE) {
    // Circular buffer: trim oldest 25% of entries
    logFile = SPIFFS.open(DEBUG_LOG_FILE, "r");
    if (!logFile) return;
    
    uint32_t skipBytes = fileSize / 4;  // Skip first 25%
    logFile.seek(skipBytes);
    
    // Find next newline to avoid corrupting an entry
    while (logFile.available()) {
      if (logFile.read() == '\n') break;
    }
    
    // Read rest of file into memory
    String remaining;
    while (logFile.available()) {
      remaining += (char)logFile.read();
    }
    logFile.close();
    
    // Rewrite file with only newest entries
    logFile = SPIFFS.open(DEBUG_LOG_FILE, "w");
    if (logFile) {
      logFile.print(remaining);
      logFile.close();
    }
  }
}

static String debug_log_timestamp() {
  time_t now = time(nullptr);
  struct tm* timeinfo = localtime(&now);
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", timeinfo);
  return String(buf);
}

// ============================================================================
// Public logging functions (safe to call; will no-op if disabled)
// ============================================================================

void debug_log_init() {
  if (!DEBUG_LOGGING_ENABLED) return;
  
  if (!SPIFFS.begin(false)) {
    Serial.println("[DEBUG] SPIFFS already mounted or unavailable");
  }
  debug_log_trim_if_needed();
  Serial.println("[DEBUG] Debug logging initialized");
}

void debug_log_event(const char* event, const char* details = nullptr) {
  if (!DEBUG_LOGGING_ENABLED) return;
  
  debug_log_trim_if_needed();
  
  File logFile = SPIFFS.open(DEBUG_LOG_FILE, "a");
  if (!logFile) {
    Serial.printf("[DEBUG] Failed to open log file\n");
    return;
  }
  
  String timestamp = debug_log_timestamp();
  logFile.printf("[%s] %s", timestamp.c_str(), event);
  
  if (details) {
    logFile.printf(" | %s", details);
  }
  logFile.printf("\n");
  logFile.close();
  
  Serial.printf("[LOG] %s\n", event);
}

void debug_log_connect_start(const char* ssid) {
  if (!DEBUG_LOGGING_ENABLED) return;
  
  char buf[128];
  snprintf(buf, sizeof(buf), "WiFi connect attempt to '%s'", ssid);
  debug_log_event(buf);
}

void debug_log_connect_success(const char* ip, int rssi) {
  if (!DEBUG_LOGGING_ENABLED) return;
  
  char buf[128];
  snprintf(buf, sizeof(buf), "WiFi connected | IP=%s RSSI=%d", ip, rssi);
  debug_log_event(buf);
}

void debug_log_connect_failed(int status_code, int attempts) {
  if (!DEBUG_LOGGING_ENABLED) return;
  
  char buf[128];
  snprintf(buf, sizeof(buf), "WiFi connection failed | status=%d attempts=%d", 
           status_code, attempts);
  debug_log_event(buf);
}

void debug_log_disconnect() {
  if (!DEBUG_LOGGING_ENABLED) return;
  
  int rssi = WiFi.RSSI();
  int status = WiFi.status();
  char buf[128];
  snprintf(buf, sizeof(buf), "WiFi disconnected | status=%d last_rssi=%d", 
           status, rssi);
  debug_log_event(buf);
}

void debug_log_reconnect_attempt() {
  if (!DEBUG_LOGGING_ENABLED) return;
  debug_log_event("WiFi background reconnect attempt");
}

void debug_log_http_error(const char* url, int http_code) {
  if (!DEBUG_LOGGING_ENABLED) return;
  
  char buf[256];
  snprintf(buf, sizeof(buf), "HTTP error | URL=%.200s code=%d", url, http_code);
  debug_log_event(buf);
}

void debug_log_stream_error(const char* error_msg, uint32_t bytes_received) {
  if (!DEBUG_LOGGING_ENABLED) return;
  
  char buf[256];
  snprintf(buf, sizeof(buf), "Stream error: %s | bytes_received=%u", 
           error_msg, bytes_received);
  debug_log_event(buf);
}

// ============================================================================
// Retrieve logs (for debugging via serial command)
// ============================================================================

void debug_log_dump_to_serial() {
  if (!DEBUG_LOGGING_ENABLED) {
    Serial.println("[DEBUG] Logging is disabled");
    return;
  }
  
  File logFile = SPIFFS.open(DEBUG_LOG_FILE, "r");
  if (!logFile) {
    Serial.println("[DEBUG] No log file found");
    return;
  }
  
  Serial.println("\n=== Debug Log Contents ===");
  while (logFile.available()) {
    Serial.print((char)logFile.read());
  }
  logFile.close();
  Serial.println("=== End of Log ===\n");
}

void debug_log_clear() {
  if (!DEBUG_LOGGING_ENABLED) return;
  
  if (SPIFFS.remove(DEBUG_LOG_FILE)) {
    Serial.println("[DEBUG] Log file cleared");
  }
}

uint32_t debug_log_file_size() {
  if (!DEBUG_LOGGING_ENABLED) return 0;
  
  File logFile = SPIFFS.open(DEBUG_LOG_FILE, "r");
  if (!logFile) return 0;
  
  uint32_t size = logFile.size();
  logFile.close();
  return size;
}
