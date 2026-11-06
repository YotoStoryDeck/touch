// In-Memory Circular Log Buffer for Live Web Streaming (/logs)
#pragma once
#include <Arduino.h>
#include <time.h>

#define MAX_LOG_ENTRIES 96
#define LOG_LINE_MAX 160

struct LogEntry {
  uint32_t ms;       // Milliseconds since boot
  time_t epoch;      // POSIX epoch timestamp (if NTP synced), else 0
  char msg[LOG_LINE_MAX];
};

static LogEntry s_log_ring[MAX_LOG_ENTRIES];
static int s_log_head = 0;
static int s_log_total = 0;

inline void log_msg(const char* fmt, ...) {
  char buf[LOG_LINE_MAX];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);

  time_t now = time(nullptr);
  char ts[32] = "";
  if(now > 100000000){
    struct tm tm_val;
    localtime_r(&now, &tm_val);
    strftime(ts, sizeof(ts), "[%Y-%m-%d %H:%M:%S] ", &tm_val);
  } else {
    snprintf(ts, sizeof(ts), "[+%lu.%02lus] ", (unsigned long)(millis() / 1000), (unsigned long)((millis() % 1000) / 10));
  }

  Serial.print(ts);
  Serial.println(buf);

  int idx = s_log_head;
  s_log_ring[idx].ms = millis();
  s_log_ring[idx].epoch = (now > 100000000) ? now : 0;
  strncpy(s_log_ring[idx].msg, buf, LOG_LINE_MAX - 1);
  s_log_ring[idx].msg[LOG_LINE_MAX - 1] = '\0';

  s_log_head = (s_log_head + 1) % MAX_LOG_ENTRIES;
  if(s_log_total < MAX_LOG_ENTRIES) s_log_total++;
}
