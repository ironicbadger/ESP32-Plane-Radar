#include "services/weather_time.h"

#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include <ArduinoJson.h>

#include <cmath>
#include <cstdio>
#include <ctime>
#include <sys/time.h>

#include "config.h"
#include "services/display_settings.h"

namespace services::weather {
namespace {

constexpr time_t kMinimumValidEpoch = 1609459200;  // 2021-01-01 UTC

bool s_started = false;
bool s_valid = false;
float s_temperature_c = 0.0f;
int s_humidity_percent = 0;
int s_weather_code = -1;
int32_t s_utc_offset_seconds = 0;
unsigned long s_last_attempt_ms = 0;
double s_last_latitude = 999.0;
double s_last_longitude = 999.0;
PollFn s_poll_fn = nullptr;

bool clockValid() { return time(nullptr) >= kMinimumValidEpoch; }

void pollNetwork() {
  if (s_poll_fn != nullptr) {
    s_poll_fn();
  }
}

const char* conditionLabel(int code) {
  if (code == 0) return "CLEAR";
  if (code == 1) return "MOSTLY CLEAR";
  if (code == 2) return "PARTLY CLOUDY";
  if (code == 3) return "OVERCAST";
  if (code == 45 || code == 48) return "FOG";
  if (code >= 51 && code <= 57) return "DRIZZLE";
  if (code >= 61 && code <= 67) return "RAIN";
  if (code >= 71 && code <= 77) return "SNOW";
  if (code >= 80 && code <= 82) return "SHOWERS";
  if (code == 85 || code == 86) return "SNOW";
  if (code >= 95 && code <= 99) return "STORM";
  return "WEATHER";
}

int64_t daysFromCivil(int year, unsigned month, unsigned day) {
  year -= month <= 2;
  const int era = (year >= 0 ? year : year - 399) / 400;
  const unsigned year_of_era = static_cast<unsigned>(year - era * 400);
  const unsigned day_of_year =
      (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const unsigned day_of_era =
      year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
  return static_cast<int64_t>(era) * 146097 +
         static_cast<int64_t>(day_of_era) - 719468;
}

void seedClockFromApiTime(const char* local_iso_time, int32_t utc_offset) {
  if (clockValid() || local_iso_time == nullptr) {
    return;
  }

  int year = 0;
  int month = 0;
  int day = 0;
  int hour = 0;
  int minute = 0;
  if (sscanf(local_iso_time, "%d-%d-%dT%d:%d", &year, &month, &day, &hour,
             &minute) != 5) {
    return;
  }

  const time_t local_epoch = static_cast<time_t>(
      daysFromCivil(year, static_cast<unsigned>(month),
                    static_cast<unsigned>(day)) *
          86400 +
      hour * 3600 + minute * 60);
  if (local_epoch < kMinimumValidEpoch) {
    return;
  }

  timeval value = {};
  value.tv_sec = local_epoch - utc_offset;
  settimeofday(&value, nullptr);
}

bool fetch(double latitude, double longitude) {
  String url = config::kWeatherApiBase;
  url += "?latitude=";
  url += String(latitude, 6);
  url += "&longitude=";
  url += String(longitude, 6);
  url +=
      "&current=temperature_2m,relative_humidity_2m,weather_code,is_day"
      "&temperature_unit=celsius&timezone=auto&forecast_days=1";

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  if (!http.begin(client, url)) {
    Serial.println("weather: http.begin failed");
    return false;
  }
  http.setConnectTimeout(config::kWeatherRequestTimeoutMs);
  http.setTimeout(config::kWeatherRequestTimeoutMs);

  pollNetwork();
  const int code = http.GET();
  pollNetwork();
  if (code != HTTP_CODE_OK) {
    Serial.printf("weather: HTTP %d\n", code);
    http.end();
    return false;
  }

  const String payload = http.getString();
  http.end();
  pollNetwork();

  JsonDocument doc;
  const DeserializationError error = deserializeJson(doc, payload);
  if (error) {
    Serial.printf("weather: JSON parse error: %s\n", error.c_str());
    return false;
  }

  JsonObject current = doc["current"].as<JsonObject>();
  if (current.isNull() || !current["temperature_2m"].is<float>() ||
      !current["relative_humidity_2m"].is<int>() ||
      !current["weather_code"].is<int>()) {
    Serial.println("weather: incomplete response");
    return false;
  }

  s_temperature_c = current["temperature_2m"].as<float>();
  s_humidity_percent = current["relative_humidity_2m"].as<int>();
  s_weather_code = current["weather_code"].as<int>();
  s_utc_offset_seconds = doc["utc_offset_seconds"] | 0;
  seedClockFromApiTime(current["time"] | nullptr, s_utc_offset_seconds);
  s_valid = true;
  Serial.printf("weather: %.1f C, %d%%, code %d, UTC%+ld\n", s_temperature_c,
                s_humidity_percent, s_weather_code,
                static_cast<long>(s_utc_offset_seconds));
  return true;
}

}  // namespace

void begin() {
  if (!s_started) {
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    s_started = true;
  }
}

void setPollFn(PollFn fn) { s_poll_fn = fn; }

bool refreshIfDue(double latitude, double longitude, bool force) {
  begin();
  const unsigned long now = millis();
  const bool location_changed =
      fabs(latitude - s_last_latitude) > 0.0001 ||
      fabs(longitude - s_last_longitude) > 0.0001;
  if (!force && !location_changed && s_last_attempt_ms != 0 &&
      now - s_last_attempt_ms < config::kWeatherFetchIntervalMs) {
    return false;
  }
  s_last_attempt_ms = now;
  s_last_latitude = latitude;
  s_last_longitude = longitude;
  return fetch(latitude, longitude);
}

bool valid() { return s_valid; }

void formatWeatherLine(char* out, size_t out_len) {
  if (out_len == 0) {
    return;
  }
  if (!s_valid) {
    snprintf(out, out_len, "WEATHER --");
    return;
  }

  float temperature = s_temperature_c;
  const char unit =
      settings::temperatureFahrenheit() ? 'F' : 'C';
  if (settings::temperatureFahrenheit()) {
    temperature = temperature * 9.0f / 5.0f + 32.0f;
  }
  snprintf(out, out_len, "%s %d%c RH%d%%", conditionLabel(s_weather_code),
           static_cast<int>(lroundf(temperature)), unit, s_humidity_percent);
}

void formatDateTimeLine(char* out, size_t out_len) {
  if (out_len == 0) {
    return;
  }

  const time_t utc_now = time(nullptr);
  if (utc_now < kMinimumValidEpoch) {
    snprintf(out, out_len, "--:-- -- ---");
    return;
  }

  const time_t local_now = utc_now + s_utc_offset_seconds;
  tm local = {};
  gmtime_r(&local_now, &local);
  constexpr const char* kMonths[] = {"JAN", "FEB", "MAR", "APR",
                                     "MAY", "JUN", "JUL", "AUG",
                                     "SEP", "OCT", "NOV", "DEC"};
  const char* month =
      local.tm_mon >= 0 && local.tm_mon < 12 ? kMonths[local.tm_mon] : "---";

  if (settings::use24HourClock()) {
    snprintf(out, out_len, "%02d:%02d %02d %s", local.tm_hour, local.tm_min,
             local.tm_mday, month);
    return;
  }

  int hour = local.tm_hour % 12;
  if (hour == 0) {
    hour = 12;
  }
  snprintf(out, out_len, "%d:%02d%c %02d %s", hour, local.tm_min,
           local.tm_hour >= 12 ? 'P' : 'A', local.tm_mday, month);
}

}  // namespace services::weather
