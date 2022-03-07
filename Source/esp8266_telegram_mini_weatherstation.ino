extern "C" {
#include "user_interface.h"
}

#include <stdint.h>
#include <ESP8266WiFi.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <DHTesp.h>
#include <AsyncTelegram.h>
#include <RTCVars.h>

// Pins
#define pin_led 2
#define pin_onewire 14
#define pin_dht 13
#define pin_analog A0
#define pin_sensor_supply 12

const int msg_ln = 512;
const float undervoltage = 3.3;
const int uSinSecs = 1000000;

const int SecsInMin = 60;
const int MinInHour = 60;
const int HoursInDay = 24;
const int read_every_mins = 10;

const int READINGS = 10;
const int READINGS_PER_HOUR = MinInHour / read_every_mins;

const char* ssid     = "WLAN-Name";
const char* password = "WLAN-Password";
const char* token = "123456789-Replace with your own Telegram-Bot token"; //Infos how to optain one are going to be added
const int32_t userid = 0; //UserID to send the infos to, groups often start with a "-"

DHTesp dht;
OneWire oneWire0(pin_onewire);
DallasTemperature dallas_sensors(&oneWire0);
AsyncTelegram myBot;
char text[msg_ln] = {0};

//stored in rtc_ram
struct { //divide everything by 10 to get the real value (for storing the first digit in an int)
  uint32_t crc32;
  uint8_t elapsed_min;  
  uint8_t bat_voltage;
  int16_t temp_air;
  int16_t temp_ground;
  int16_t humidity_air;
  int16_t temp_air_max;
  int16_t temp_air_min;
  int16_t temp_ground_max;
  int16_t temp_ground_min;
  int16_t humidity_air_max;
  int16_t humidity_air_min;
  int16_t history_air[HoursInDay] = {0};
  int16_t history_ground[HoursInDay] = {0};
  int16_t history_humidity_air[HoursInDay] = {0};
  uint8_t index_history;
  int16_t last_hour_air[READINGS_PER_HOUR] = {0};
  int16_t last_hour_ground[READINGS_PER_HOUR] = {0};
  int16_t last_hour_humidity_air[READINGS_PER_HOUR] = {0};
  uint8_t index_last_hour;
  int32_t last_msg;    
} rtcm;

uint32_t calculateCRC32(const uint8_t *data, size_t length);
int16_t avg(int16_t *dat, int count);
void read_values();
void print_values();
void generate_msg();
void init_wifi();
void init_telegram();
void reset_min_max();
void recv_msg();
void reinit_memory();

void setup()
{
  pinMode(pin_led, OUTPUT);
  pinMode(pin_sensor_supply, OUTPUT);
  digitalWrite(pin_sensor_supply, LOW);
  digitalWrite(pin_led, HIGH);

  dht.setup(pin_dht, DHTesp::DHT22);

  Serial.begin(9600);
  Serial.setTimeout(2000);
  while (!Serial) {}
   
  ESP.rtcUserMemoryRead(0, (uint32_t*) &rtcm, sizeof(rtcm));
  uint32_t crcOfData = calculateCRC32((uint8_t*) &rtcm+sizeof(rtcm.crc32), sizeof(rtcm)-sizeof(rtcm.crc32));
  
  if (crcOfData != rtcm.crc32)
  {
    Serial.println("CRC32 doesn't match. Data is invalid.");
    reinit_memory();   
    
    TBMessage msg;
    strncpy(text, "CRC-Error: As a result, the stored data has now been discarded", msg_ln);
    init_wifi();
    init_telegram();
    msg.sender.id = userid;
    myBot.sendMessage(msg, text, "");
    WiFi.mode(WIFI_OFF);    
  } else 
  {
    Serial.println("CRC32 check ok.");
  } 
}

void loop()
{
  delay(500);

  rtcm.elapsed_min += read_every_mins;
  rtcm.index_last_hour++;
    
  if(rtcm.index_last_hour >= READINGS_PER_HOUR)
  {
    rtcm.index_last_hour = 0;
  }

  read_values();
  
  rtcm.last_hour_air[rtcm.index_last_hour] = rtcm.temp_air;
  rtcm.last_hour_ground[rtcm.index_last_hour] = rtcm.temp_ground;
  rtcm.last_hour_humidity_air[rtcm.index_last_hour] = rtcm.humidity_air;

  rtcm.crc32 = calculateCRC32((uint8_t*) &rtcm+sizeof(rtcm.crc32), sizeof(rtcm)-sizeof(rtcm.crc32));
  ESP.rtcUserMemoryWrite(0, (uint32_t*) &rtcm, sizeof(rtcm));
   
  print_values(); 
    
  if (rtcm.bat_voltage / 10.0 < undervoltage)
  {
    TBMessage msg;
    strncpy(text, "Battery low - Shutdown initiated.", msg_ln);
    init_wifi();
    init_telegram();
    msg.sender.id = userid;
    myBot.sendMessage(msg, text, "");
    ESP.deepSleep(0);
  }

  //Send every hour
  if (rtcm.elapsed_min >= MinInHour)
  {
    TBMessage msg;
    
    rtcm.elapsed_min = 0;
    rtcm.index_last_hour = 0;     
    rtcm.index_history++;
    
    if(rtcm.index_history >= HoursInDay)
    {
      rtcm.index_history = 0;
    }
    
    read_values();

    rtcm.history_air[rtcm.index_history] = avg((int16_t*)&rtcm.last_hour_air, READINGS_PER_HOUR);
    rtcm.history_ground[rtcm.index_history] = avg((int16_t*)&rtcm.last_hour_ground, READINGS_PER_HOUR);
    rtcm.history_humidity_air[rtcm.index_history] = avg((int16_t*)&rtcm.last_hour_humidity_air, READINGS_PER_HOUR);
    
    rtcm.crc32 = calculateCRC32((uint8_t*) &rtcm+sizeof(rtcm.crc32), sizeof(rtcm)-sizeof(rtcm.crc32));
    ESP.rtcUserMemoryWrite(0, (uint32_t*) &rtcm, sizeof(rtcm));
        
    init_wifi();
    init_telegram();

    recv_msg();
        
    generate_msg();
    msg.sender.id = userid;
    myBot.sendMessage(msg, text, "");    
    WiFi.mode(WIFI_OFF);
  }
  
  ESP.deepSleep(read_every_mins * SecsInMin * uSinSecs); 
}


uint32_t calculateCRC32(const uint8_t *data, size_t length) {
  uint32_t crc = 0xffffffff;
  while (length--) {
    uint8_t c = *data++;
    for (uint32_t i = 0x80; i > 0; i >>= 1) {
      bool bit = crc & 0x80000000;
      if (c & i) {
        bit = !bit;
      }
      crc <<= 1;
      if (bit) {
        crc ^= 0x04c11db7;
      }
    }
  }
  return crc;
}

int16_t avg(int16_t *dat, int count)
{
  int64_t sum = 0;

  for (int i = 0; i < count; i++)
    sum += dat[i];
  
  return (int16_t)(sum / count);
}

void read_values()
{
  int32_t buffer0 = 0;
  float readbuff0 = 0;
  uint16_t valid_readings0 = 0;

  digitalWrite(pin_sensor_supply, HIGH);
  digitalWrite(pin_led, LOW);

  delay(500);

  //dallas_sensors.requestTemperatures(); (bei Einbau von Originalsensor kann wahrscheinlich das 10x auslesen entfallen)
  
  rtcm.bat_voltage = (analogRead(pin_analog) * 450) / 10230;
  rtcm.humidity_air = dht.getHumidity() * 10.0;
  rtcm.temp_air = dht.getTemperature() * 10.0;
    
  //rtcm.temp_ground = dallas_sensors.getTempCByIndex(0);
  
  for (int i = 0; i < READINGS; i++)
  {
    dallas_sensors.requestTemperatures();
    readbuff0 = dallas_sensors.getTempCByIndex(0);    

    if (readbuff0 > -100.0)
    {
      buffer0 += readbuff0 * 10.0;
      valid_readings0++;
    }
    
    if (valid_readings0 > 0)
    {
      rtcm.temp_ground = buffer0 / valid_readings0;
      if (rtcm.temp_ground > rtcm.temp_ground_max)
        rtcm.temp_ground_max = rtcm.temp_ground;
      if (rtcm.temp_ground < rtcm.temp_ground_min)
        rtcm.temp_ground_min = rtcm.temp_ground;
    }
    else
      rtcm.temp_ground = -999;

    delay(10);
  }

  if (rtcm.temp_air > rtcm.temp_air_max)
    rtcm.temp_air_max = rtcm.temp_air;
  if (rtcm.temp_air < rtcm.temp_air_min)
    rtcm.temp_air_min = rtcm.temp_air;

  if (rtcm.humidity_air > rtcm.humidity_air_max)
    rtcm.humidity_air_max = rtcm.humidity_air;
  if (rtcm.humidity_air < rtcm.humidity_air_min)
    rtcm.humidity_air_min = rtcm.humidity_air;

  digitalWrite(pin_sensor_supply, LOW);
  digitalWrite(pin_led, HIGH);  
}

void print_values()
{
  Serial.println();
  Serial.print("Elaspsed_Mins:");
  Serial.print(rtcm.elapsed_min);
  Serial.print(", ");
  Serial.print("Voltage:");
  Serial.print(rtcm.bat_voltage / 10.0, 1);
  Serial.print(", ");
  Serial.print("Temp.Air:");
  Serial.print(rtcm.temp_air / 10.0, 1);
  Serial.print(", ");
  Serial.print("Temp.Ground:");
  Serial.print(rtcm.temp_ground / 10.0, 1);
  Serial.print(", ");
  Serial.print("Humidity:");
  Serial.print(rtcm.humidity_air / 10.0, 1);
  Serial.println();  
}

void generate_msg()
{
  char float_buf[7] = {0};

  dtostrf(rtcm.bat_voltage / 10.0, 6, 1, float_buf);
  strncpy(text, "Battery: ", msg_ln);
  strncat(text, float_buf, msg_ln);
  strncat(text, " V\n", msg_ln);
  
  dtostrf(rtcm.temp_air / 10.0, 6, 1, float_buf);
  strncat(text, "Air temperature: ", msg_ln);
  strncat(text, float_buf, msg_ln);
  strncat(text, " C\nMin/Max: ", msg_ln);
  dtostrf(rtcm.temp_air_min / 10.0, 6, 1, float_buf);
  strncat(text, float_buf, msg_ln);
  strncat(text, " / ", msg_ln);
  dtostrf(rtcm.temp_air_max / 10.0, 6, 1, float_buf);
  strncat(text, float_buf, msg_ln);
  strncat(text, "\nAVG 1h/24h: ", msg_ln);
  dtostrf(avg((int16_t*)&rtcm.last_hour_air, READINGS_PER_HOUR) / 10.0, 6, 1, float_buf);
  strncat(text, float_buf, msg_ln);
  strncat(text, " / ", msg_ln);
  dtostrf(avg((int16_t*)&rtcm.history_air, HoursInDay) / 10.0, 6, 1, float_buf);
  strncat(text, float_buf, msg_ln);
  strncat(text, "\n\n", msg_ln);

  dtostrf(rtcm.temp_ground / 10.0, 6, 1, float_buf);
  strncat(text, "Ground temperature: ", msg_ln);
  strncat(text, float_buf, msg_ln);
  strncat(text, " C\nMin/Max: ", msg_ln);
  dtostrf(rtcm.temp_ground_min / 10.0, 6, 1, float_buf);
  strncat(text, float_buf, msg_ln);
  strncat(text, " / ", msg_ln);
  dtostrf(rtcm.temp_ground_max / 10.0, 6, 1, float_buf);
  strncat(text, float_buf, msg_ln);
  strncat(text, "\nAVG 1h/24h: ", msg_ln);
  dtostrf(avg((int16_t*)&rtcm.last_hour_ground, READINGS_PER_HOUR) / 10.0, 6, 1, float_buf);
  strncat(text, float_buf, msg_ln);
  strncat(text, " / ", msg_ln);
  dtostrf(avg((int16_t*)&rtcm.history_ground, HoursInDay) / 10.0, 6, 1, float_buf);
  strncat(text, float_buf, msg_ln);
  strncat(text, "\n\n", msg_ln);

  dtostrf(rtcm.humidity_air / 10.0, 6, 1, float_buf);
  strncat(text, "Humidity: ", msg_ln);
  strncat(text, float_buf, msg_ln);
  strncat(text, " %\nMin/Max: ", msg_ln);
  dtostrf(rtcm.humidity_air_min / 10.0, 6, 1, float_buf);
  strncat(text, float_buf, msg_ln);
  strncat(text, " / ", msg_ln);
  dtostrf(rtcm.humidity_air_max / 10.0, 6, 1, float_buf);
  strncat(text, float_buf, msg_ln);
  strncat(text, "\nAVG 1h/24h: ", msg_ln);
  dtostrf(avg((int16_t*)&rtcm.last_hour_humidity_air, READINGS_PER_HOUR) / 10.0, 6, 1, float_buf);
  strncat(text, float_buf, msg_ln);
  strncat(text, " / ", msg_ln);
  dtostrf(avg((int16_t*)&rtcm.history_humidity_air, HoursInDay) / 10.0, 6, 1, float_buf);
  strncat(text, float_buf, msg_ln);
  strncat(text, "\n\n", msg_ln);
}

void init_wifi()
{
  const uint32_t pause_between_retries = 500;
  uint32_t connection_timeout = (1000/pause_between_retries) * 60;
  wifi_set_sleep_type(LIGHT_SLEEP_T);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  while (connection_timeout && WiFi.status() != WL_CONNECTED) {
    --connection_timeout;
    delay(pause_between_retries);    
  }
}

void init_telegram()
{
  myBot.setUpdateTime(2000);
  myBot.setTelegramToken(token);  
}

void reset_min_max()
{
  rtcm.temp_air_max = rtcm.temp_air;
  rtcm.temp_air_min = rtcm.temp_air;
  rtcm.temp_ground_max = rtcm.temp_ground;
  rtcm.temp_ground_min = rtcm.temp_ground;
  rtcm.humidity_air_max = rtcm.humidity_air;
  rtcm.humidity_air_min = rtcm.humidity_air;
  
  rtcm.crc32 = calculateCRC32((uint8_t*) &rtcm+sizeof(rtcm.crc32), sizeof(rtcm)-sizeof(rtcm.crc32));
  ESP.rtcUserMemoryWrite(0, (uint32_t*) &rtcm, sizeof(rtcm));
}

void recv_msg()
{
  TBMessage msg;
  
  while (myBot.getNewMessage(msg)) 
  {
    if (msg.date != rtcm.last_msg)
    {
      if (msg.text.equalsIgnoreCase("RESET MIN MAX")) 
      {
        reset_min_max();
        
        rtcm.last_msg = msg.date;
        rtcm.crc32 = calculateCRC32((uint8_t*) &rtcm+sizeof(rtcm.crc32), sizeof(rtcm)-sizeof(rtcm.crc32));
        ESP.rtcUserMemoryWrite(0, (uint32_t*) &rtcm, sizeof(rtcm));
        
        myBot.sendMessage(msg, "Reset confirmed.");
      }             
    }    
  }  
}

void reinit_memory()
{
  rtcm.elapsed_min = 0;  
  rtcm.index_history = 0;  
  rtcm.index_last_hour = 0;
    
  for (int i = 0; i < HoursInDay;i++)
  {
    rtcm.history_air[i] = 0;
    rtcm.history_ground[i] = 0;
    rtcm.history_humidity_air[i] = 0;
  }

    for (int i = 0; i < READINGS_PER_HOUR;i++)
  {
    rtcm.last_hour_air[i] = 0;
    rtcm.last_hour_ground[i] = 0;
    rtcm.last_hour_humidity_air[i] = 0;
  }
  
  read_values();
  reset_min_max();
  rtcm.crc32 = calculateCRC32((uint8_t*) &rtcm+sizeof(rtcm.crc32), sizeof(rtcm)-sizeof(rtcm.crc32));
  ESP.rtcUserMemoryWrite(0, (uint32_t*) &rtcm, sizeof(rtcm));
}
