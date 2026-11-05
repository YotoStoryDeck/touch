// In-Memory Circular Log Buffer for Live Web Streaming (/logs)
#pragma once
#include <Arduino.h>

#define MAX_LOG_ENTRIES 64
#define LOG_LINE_MAX 160

struct LogEntry {
  uint32_t ms;
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

  Serial.println(buf);

  int idx = s_log_head;
  s_log_ring[idx].ms = millis();
  strncpy(s_log_ring[idx].msg, buf, LOG_LINE_MAX - 1);
  s_log_ring[idx].msg[LOG_LINE_MAX - 1] = '\0';

  s_log_head = (s_log_head + 1) % MAX_LOG_ENTRIES;
  if(s_log_total < MAX_LOG_ENTRIES) s_log_total++;
}
