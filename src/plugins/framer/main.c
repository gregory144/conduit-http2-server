#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <inttypes.h>

#include <uv.h>

#include "server.h"
#include "plugin.h"

#include "log.h"
#include "util.h"
#include "http/http.h"
#include "http/h2/h2.h"
#include "http/request.h"

static void framer_plugin_start(plugin_t * plugin)
{
  log_append(plugin->log, LOG_INFO, "Framer plugin started");
}

static void framer_plugin_stop(plugin_t * plugin)
{
  log_append(plugin->log, LOG_INFO, "Framer plugin stopped");
}

/**
 * Caller should not free the returned string
 */
static char * frame_type_to_string(enum frame_type_e t)
{
  switch (t) {
    case FRAME_TYPE_DATA:
      return "DATA";
    case FRAME_TYPE_HEADERS:
      return "HEADERS";
    case FRAME_TYPE_PRIORITY:
      return "PRIORITY";
    case FRAME_TYPE_RST_STREAM:
      return "RST_STREAM";
    case FRAME_TYPE_SETTINGS:
      return "SETTINGS";
    case FRAME_TYPE_PUSH_PROMISE:
      return "PUSH_PROMISE";
    case FRAME_TYPE_PING:
      return "PING";
    case FRAME_TYPE_GOAWAY:
      return "GOAWAY";
    case FRAME_TYPE_WINDOW_UPDATE:
      return "WINDOW_UPDATE";
    case FRAME_TYPE_CONTINUATION:
      return "CONTINUATION";
  }
  return "UNKNOWN";
}

static char * error_code_to_string(enum h2_error_code_e e)
{
  switch (e) {
    case H2_ERROR_NO_ERROR:
      return "NO_ERROR";
    case H2_ERROR_PROTOCOL_ERROR:
      return "PROTOCOL_ERROR";
    case H2_ERROR_INTERNAL_ERROR:
      return "INTERNAL_ERROR";
    case H2_ERROR_FLOW_CONTROL_ERROR:
      return "FLOW_CONTROL_ERROR";
    case H2_ERROR_SETTINGS_TIMEOUT:
      return "SETTINGS_TIMEOUT";
    case H2_ERROR_STREAM_CLOSED:
      return "STREAM_CLOSED";
    case H2_ERROR_FRAME_SIZE_ERROR:
      return "FRAME_SIZE_ERROR";
    case H2_ERROR_REFUSED_STREAM:
      return "REFUSED_STREAM";
    case H2_ERROR_CANCEL:
      return "CANCEL";
    case H2_ERROR_COMPRESSION_ERROR:
      return "COMPRESSION_ERROR";
    case H2_ERROR_CONNECT_ERROR:
      return "CONNECT_ERROR";
    case H2_ERROR_ENHANCE_YOUR_CALM:
      return "ENHANCE_YOUR_CALM";
    case H2_ERROR_INADEQUATE_SECURITY:
      return "INADEQUATE_SECURITY";
  }
  return "UNKNOWN";
}

#define FLAGS_TO_STRING_BUF_LEN 128

static void flags_to_string(char * buf, size_t buf_len, h2_frame_t * frame)
{
  buf[0] = '\0';
  uint8_t flags = frame->flags;
  switch (frame->type) {
    case FRAME_TYPE_DATA:
      if (flags & FLAG_END_STREAM) { strncat(buf, "END_STREAM, ", buf_len); }
      if (flags & FLAG_END_SEGMENT) { strncat(buf, "END_SEGMENT, ", buf_len); }
      if (flags & FLAG_PADDED) { strncat(buf, "END_PADDED, ", buf_len); }
      break;
    case FRAME_TYPE_HEADERS:
      if (flags & FLAG_END_STREAM) { strncat(buf, "END_STREAM, ", buf_len); }
      if (flags & FLAG_END_SEGMENT) { strncat(buf, "END_SEGMENT, ", buf_len); }
      if (flags & FLAG_END_HEADERS) { strncat(buf, "END_HEADERS, ", buf_len); }
      if (flags & FLAG_PADDED) { strncat(buf, "PADDED, ", buf_len); }
      if (flags & FLAG_PRIORITY) { strncat(buf, "PRIORITY, ", buf_len); }
      break;
    case FRAME_TYPE_SETTINGS:
    case FRAME_TYPE_PING:
      if (flags & FLAG_ACK) { strncat(buf, "ACK, ", buf_len); }
      break;
    case FRAME_TYPE_PUSH_PROMISE:
      if (flags & FLAG_END_HEADERS) { strncat(buf, "END_HEADERS, ", buf_len); }
      if (flags & FLAG_PADDED) { strncat(buf, "PADDED, ", buf_len); }
      break;
    case FRAME_TYPE_CONTINUATION:
      if (flags & FLAG_END_HEADERS) { strncat(buf, "END_HEADERS, ", buf_len); }
      break;
    default:
      break;
  }
  if (buf[0] == '\0') {
    strncat(buf, "none", buf_len);
  } else {
    // remove the last ", "
    buf[strlen(buf) - 2] = '\0';
  }
}

static void log_frame(plugin_t * plugin, client_t * client,
    h2_frame_t * frame, char * frame_options)
{
  char frame_flags[FLAGS_TO_STRING_BUF_LEN];
  flags_to_string(frame_flags, FLAGS_TO_STRING_BUF_LEN, frame);

  char * type = frame_type_to_string(frame->type);

  log_append(plugin->log, LOG_INFO, "> %s [client: %" PRIu64 ", length: %" PRIu16
      ", stream id: %" PRIu32 ", flags: %s (%" PRIu8 ", 0x%02x)%s%s]",
      type, client->id, frame->length, frame->stream_id, frame_flags, frame->flags, frame->flags,
      frame_options ? ", " : "", frame_options ? frame_options : ""
  );
}

static void framer_plugin_incoming_frame_data(plugin_t * plugin, client_t * client,
    h2_frame_data_t * frame)
{
  bool padded = frame->flags & FLAG_PADDED;
  size_t buf_len = 128;
  char padded_buf[buf_len];
  if (padded) {
    snprintf(padded_buf, buf_len, "padding: %" PRIu32 " octets",
        ((uint32_t)frame->padding_length) + 1);
    log_frame(plugin, client, (h2_frame_t *) frame, padded_buf);
  } else {
    log_frame(plugin, client, (h2_frame_t *) frame, NULL);
  }
}

static void framer_plugin_incoming_frame_headers(plugin_t * plugin, client_t * client,
    h2_frame_headers_t * frame)
{
  bool padded = frame->flags & FLAG_PADDED;
  bool priority = frame->flags & FLAG_PRIORITY;
  size_t buf_len = 128;
  char padded_buf[buf_len];
  char priority_buf[buf_len];
  if (padded) {
    snprintf(padded_buf, buf_len, "padding: %" PRIu32 " octets",
        ((uint32_t)frame->padding_length) + 1);
  }
  if (priority) {
    snprintf(priority_buf, buf_len, "priority: (dependency: %" PRIu32", weight: %"
      PRIu32 ", exclusive: %s)", frame->priority_stream_dependency,
      ((uint32_t)frame->priority_weight) + 1,
      frame->priority_exclusive ? "yes" : "no");
  }
  if (padded && priority) {
    char buf[buf_len * 2];
    snprintf(buf, buf_len * 2, "%s, %s", padded_buf, priority_buf);
    log_frame(plugin, client, (h2_frame_t *) frame, buf);
  } else if (padded) {
    log_frame(plugin, client, (h2_frame_t *) frame, padded_buf);
  } else if (priority) {
    log_frame(plugin, client, (h2_frame_t *) frame, priority_buf);
  } else {
    log_frame(plugin, client, (h2_frame_t *) frame, NULL);
  }
}

static void framer_plugin_incoming_frame_priority(plugin_t * plugin, client_t * client,
    h2_frame_priority_t * frame)
{
  size_t buf_len = 128;
  char priority_buf[buf_len];
  snprintf(priority_buf, buf_len, "priority: (dependency: %" PRIu32", weight: %"
    PRIu32 ", exclusive: %s)", frame->priority_stream_dependency,
    ((uint32_t)frame->priority_weight) + 1,
    frame->priority_exclusive ? "yes" : "no");
  log_frame(plugin, client, (h2_frame_t *) frame, priority_buf);
}

static void framer_plugin_incoming_frame_rst_stream(plugin_t * plugin,
    client_t * client, h2_frame_rst_stream_t * frame)
{
  size_t buf_len = 128;
  char buf[buf_len];
  snprintf(buf, buf_len, "error: %s", error_code_to_string(frame->error_code));
  log_frame(plugin, client, (h2_frame_t *) frame, buf);
}

static void framer_plugin_incoming_frame_settings(plugin_t * plugin,
    client_t * client, h2_frame_settings_t * frame)
{
  size_t buf_len = 1024;
  char buf[buf_len];
  buf[0] = '\0';

  if (!frame->flags & FLAG_ACK) {
    h2_t * h2 = client->connection->handler;
    size_t setting_buf_len = 128;
    char setting_buf[setting_buf_len];
    bool first = true;
    for (size_t i = 0; i < frame->num_settings; i++) {
      h2_setting_t * setting = &frame->settings[i];
      switch (setting->id) {
        case SETTINGS_HEADER_TABLE_SIZE:
          snprintf(setting_buf, setting_buf_len, "%sheader table size: %zd -> %" PRIu32,
              !first ? ", " : "", h2->header_table_size, setting->value);
          break;
        case SETTINGS_ENABLE_PUSH:
          snprintf(setting_buf, setting_buf_len, "%senable push: %s -> %s",
              !first ? ", " : "", h2->enable_push ? "yes" : "no", setting->value ? "yes" : "no");
          break;
        case SETTINGS_MAX_CONCURRENT_STREAMS:
          snprintf(setting_buf, setting_buf_len, "%smax concurrent streams: %zd -> %" PRIu32,
              !first ? ", " : "", h2->max_concurrent_streams, setting->value);
          break;
        case SETTINGS_INITIAL_WINDOW_SIZE:
          snprintf(setting_buf, setting_buf_len, "%sinitial window size: %zd -> %" PRIu32,
              !first ? ", " : "", h2->initial_window_size, setting->value);
          break;
        case SETTINGS_MAX_FRAME_SIZE:
          snprintf(setting_buf, setting_buf_len, "%smax frame size: %zd -> %" PRIu32,
              !first ? ", " : "", h2->max_frame_size, setting->value);
          break;
        case SETTINGS_MAX_HEADER_LIST_SIZE:
          snprintf(setting_buf, setting_buf_len, "%smax header list size: %zd -> %" PRIu32,
              !first ? ", " : "", h2->max_header_list_size, setting->value);
          break;
        default:
          snprintf(setting_buf, setting_buf_len, "%sunknown setting: %d: %" PRIu32,
              !first ? ", " : "", setting->id, setting->value);
      }
      strncat(buf, setting_buf, buf_len);
      first = false;
    }
  }
  log_frame(plugin, client, (h2_frame_t *) frame, buf);
}

static void framer_plugin_incoming_frame_ping(plugin_t * plugin,
    client_t * client, h2_frame_ping_t * frame)
{
  size_t buf_len = 64;
  char buf[buf_len];
  snprintf(buf, buf_len, "opaque data: 0x%02x%02x%02x%02x%02x%02x%02x%02x",
    frame->opaque_data[0],
    frame->opaque_data[1],
    frame->opaque_data[2],
    frame->opaque_data[3],
    frame->opaque_data[4],
    frame->opaque_data[5],
    frame->opaque_data[6],
    frame->opaque_data[7]
  );
  log_frame(plugin, client, (h2_frame_t *) frame, buf);
}

static void framer_plugin_incoming_frame_goaway(plugin_t * plugin,
    client_t * client, h2_frame_goaway_t * frame)
{
  size_t buf_len = 64 + frame->debug_data_length;
  char buf[buf_len];
  snprintf(buf, buf_len, "last stream ID: %" PRIu32 ", error: %s",
    frame->last_stream_id,
    error_code_to_string(frame->error_code)
  );
  if (frame->debug_data_length) {
    strncat(buf, ", debug data: <", buf_len);
    for (size_t i = 0; i < frame->debug_data_length; i++) {
      char dd_buf[2];
      snprintf(dd_buf, 2, " %02x", frame->debug_data[i]);
      strncat(buf, dd_buf, buf_len);
    }
    strncat(buf, ">", buf_len);
  }
  log_frame(plugin, client, (h2_frame_t *) frame, buf);
}

static void framer_plugin_incoming_frame_window_update(plugin_t * plugin,
    client_t * client, h2_frame_window_update_t * frame)
{
  size_t buf_len = 64;
  char buf[buf_len];
  h2_t * h2 = client->connection->handler;
  if (frame->stream_id) {
    if (!h2_stream_closed(h2, frame->stream_id)) {
      h2_stream_t * stream = h2_stream_get(h2, frame->stream_id);
      long window = stream->outgoing_window_size;
      snprintf(buf, buf_len, "increment outgoing stream window: %ld -> %ld by %" PRIu32,
        window, window + frame->increment, frame->increment
      );
    }
  } else {
    long window = h2->outgoing_window_size;
    snprintf(buf, buf_len, "increment outgoing connection window: %ld -> %ld by %" PRIu32,
      window, window + frame->increment, frame->increment
    );
  }
  log_frame(plugin, client, (h2_frame_t *) frame, buf);
}

static void framer_plugin_incoming_frame_continuation(plugin_t * plugin,
    client_t * client, h2_frame_continuation_t * frame)
{
  log_frame(plugin, client, (h2_frame_t *) frame, NULL);
}

static bool framer_plugin_handler(plugin_t * plugin, client_t * client, enum plugin_callback_e cb, va_list args)
{
  switch (cb) {
    case INCOMING_FRAME_DATA:
    {
      h2_frame_data_t * frame = va_arg(args, h2_frame_data_t *);
      framer_plugin_incoming_frame_data(plugin, client, frame);
      return false;
    }
    case INCOMING_FRAME_HEADERS:
    {
      h2_frame_headers_t * frame = va_arg(args, h2_frame_headers_t *);
      framer_plugin_incoming_frame_headers(plugin, client, frame);
      return false;
    }
    case INCOMING_FRAME_PRIORITY:
    {
      h2_frame_priority_t * frame = va_arg(args, h2_frame_priority_t *);
      framer_plugin_incoming_frame_priority(plugin, client, frame);
      return false;
    }
    case INCOMING_FRAME_RST_STREAM:
    {
      h2_frame_rst_stream_t * frame = va_arg(args, h2_frame_rst_stream_t *);
      framer_plugin_incoming_frame_rst_stream(plugin, client, frame);
      return false;
    }
    case INCOMING_FRAME_SETTINGS:
    {
      h2_frame_settings_t * frame = va_arg(args, h2_frame_settings_t *);
      framer_plugin_incoming_frame_settings(plugin, client, frame);
      return false;
    }
    case INCOMING_FRAME_PING:
    {
      h2_frame_ping_t * frame = va_arg(args, h2_frame_ping_t *);
      framer_plugin_incoming_frame_ping(plugin, client, frame);
      return false;
    }
    case INCOMING_FRAME_GOAWAY:
    {
      h2_frame_goaway_t * frame = va_arg(args, h2_frame_goaway_t *);
      framer_plugin_incoming_frame_goaway(plugin, client, frame);
      return false;
    }
    case INCOMING_FRAME_WINDOW_UPDATE:
    {
      h2_frame_window_update_t * frame = va_arg(args, h2_frame_window_update_t *);
      framer_plugin_incoming_frame_window_update(plugin, client, frame);
      return false;
    }
    case INCOMING_FRAME_CONTINUATION:
    {
      h2_frame_continuation_t * frame = va_arg(args, h2_frame_continuation_t *);
      framer_plugin_incoming_frame_continuation(plugin, client, frame);
      return false;
    }
    default:
      return false;
  }
}

void plugin_initialize(plugin_t * plugin, server_t * server)
{
  UNUSED(server);

  plugin->handlers->start = framer_plugin_start;
  plugin->handlers->stop = framer_plugin_stop;
  plugin->handlers->handle = framer_plugin_handler;
}

