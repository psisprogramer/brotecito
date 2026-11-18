/*
 * Comandos por Serial:
 *   sim <temp> <hum>   -> simula esos valores (ej: sim 36 70)
 *   sim fallo          -> simula sensor desconectado
 *   sim off            -> vuelve al sensor real
 *   reporte            -> envía el reporte a Telegram ahora
 */

#include <Arduino.h>
#include <DHT.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>
#include <esp_task_wdt.h>
#include <esp_system.h>
#include "secrets.h"


// --- Pines (poner -1 en un LED que no tengas) ---
const int8_t PIN_DHT       = 22;
const int8_t PIN_BUZZER    = 23;
const int8_t PIN_LED_VERDE = 26;
const int8_t PIN_LED_ROJO  = 25;
const int8_t PIN_BOTON     = 0;     // botón BOOT de la placa

const bool BUZZER_ACTIVO_EN_ALTO = true;   // false si suena con LOW

// --- Muestreo y validación ---
const uint32_t INTERVALO_LECTURA_MS = 2000;      // el DHT22 permite 1 lectura cada 2 s
const uint8_t  FALLOS_PARA_ERROR    = 5;         // fallos seguidos -> sensor en falla
const uint8_t  ACIERTOS_PARA_RECUPERAR = 3;      // aciertos seguidos -> sensor recuperado
const uint32_t REINTENTO_SENSOR_MS  = 30000;     // reinicializar el DHT mientras esté en falla
const uint32_t CONGELADO_MS         = 20UL * 60 * 1000;
const float    MAX_SALTO_T          = 8.0f;      // °C entre dos muestras (2 s)
const float    MAX_SALTO_H          = 20.0f;     // % entre dos muestras
const uint8_t  MAX_SALTOS_SEGUIDOS  = 3;         // al 3er salto seguido se acepta como real

// --- Umbrales de alarma (EJEMPLO: ajustar según la especie del vivero) ---
const float T_ALTA_ADV = 30.0f, T_ALTA_CRIT = 35.0f;
const float T_BAJA_ADV = 12.0f, T_BAJA_CRIT =  5.0f;
const float H_ALTA_ADV = 85.0f, H_ALTA_CRIT = 95.0f;
const float H_BAJA_ADV = 50.0f, H_BAJA_CRIT = 35.0f;
const float HIST_T = 1.0f;       // histéresis de temperatura (°C)
const float HIST_H = 3.0f;       // histéresis de humedad (%)
const float LIM_RATE_T = 3.0f;   // °C por minuto  (advertencia; crítica = el doble)
const float LIM_RATE_H = 10.0f;  // % por minuto
const uint8_t CONFIRMACIONES_ALARMA = 2;   // muestras seguidas para activar

// --- Alarma sonora ---
const uint32_t ADV_REPETIR_MS = 60000;           // advertencia: suena 1 vez por minuto
const uint32_t SILENCIO_MS    = 10UL * 60 * 1000;

// --- Notificaciones ---
const uint32_t REC_ADV_MS     = 30UL * 60 * 1000;   // recordatorio si sigue la advertencia
const uint32_t REC_CRIT_MS    = 10UL * 60 * 1000;   // recordatorio si sigue la crítica
const uint32_t REPORTE_MS     = 6UL * 60 * 60 * 1000;
const uint8_t  MAX_INTENTOS_ENVIO = 5;
const long     ZONA_HORARIA_S = -5L * 3600;         // Colombia (UTC-5, sin horario de verano)

const uint8_t TAM_BUFFER = 30;     // 30 muestras x 2 s = 1 minuto de historia


typedef uint8_t Sev;
const Sev SEV_NINGUNA = 0, SEV_ADVERTENCIA = 1, SEV_CRITICA = 2;


enum Alarma : uint8_t {
  AL_SENSOR_FALLA, AL_TEMP_ALTA, AL_TEMP_BAJA, AL_HUM_ALTA, AL_HUM_BAJA,
  AL_CAMBIO_BRUSCO, AL_SENSOR_CONGELADO, AL_TOTAL
};

enum EstadoSensor : uint8_t { SENS_INICIANDO, SENS_OK, SENS_FALLA };

struct Muestra { float t; float h; uint32_t ms; };
struct Patron  { const uint16_t* d; uint8_t n; };    // alternado: encendido, apagado, ...
struct Notif   { uint32_t ms; char texto[220]; };

#define PAT(x) { x, (uint8_t)(sizeof(x) / sizeof(x[0])) }

// Patrones del buzzer (ms): ON, OFF, ON, OFF... el último OFF es la pausa de repetición
const uint16_t P_SENSOR_FALLA[]      = {1500, 1000};
const uint16_t P_TEMP_ALTA[]         = {150, 150, 150, 150, 150, 1200};      // 3 cortos
const uint16_t P_TEMP_BAJA[]         = {600, 300, 600, 1200};                // 2 largos
const uint16_t P_HUM_ALTA[]          = {150, 150, 600, 1200};                // corto-largo
const uint16_t P_HUM_BAJA[]          = {600, 150, 150, 1200};                // largo-corto
const uint16_t P_CAMBIO_BRUSCO[]     = {80, 80, 80, 80, 80, 80, 80, 1200};   // 4 rápidos
const uint16_t P_SENSOR_CONGELADO[]  = {300, 2000};

const Patron PATRONES[AL_TOTAL] = {
  PAT(P_SENSOR_FALLA), PAT(P_TEMP_ALTA), PAT(P_TEMP_BAJA), PAT(P_HUM_ALTA),
  PAT(P_HUM_BAJA), PAT(P_CAMBIO_BRUSCO), PAT(P_SENSOR_CONGELADO)
};

const char* const NOMBRE_ALARMA[AL_TOTAL] = {
  "SENSOR SIN RESPUESTA", "TEMPERATURA ALTA", "TEMPERATURA BAJA", "HUMEDAD ALTA",
  "HUMEDAD BAJA", "CAMBIO BRUSCO", "SENSOR CONGELADO"
};
const char* const DETALLE_ALARMA[AL_TOTAL] = {
  "No hay lecturas validas. Revisar cableado y alimentacion.",
  "Riesgo de estres termico en las plantulas.",
  "Riesgo de dano por frio.",
  "Riesgo de hongos y enfermedades.",
  "Riesgo de deshidratacion.",
  "Variacion demasiado rapida de temperatura o humedad.",
  "Lecturas identicas por mucho tiempo: posible sensor trabado."
};
const char* const NOMBRE_SEV[3] = {"NORMAL", "ADVERTENCIA", "CRITICA"};

// ---- Estado global ----
DHT dht(PIN_DHT, DHT22);

Muestra  buf[TAM_BUFFER];
uint8_t  bufN = 0, bufIni = 0;

EstadoSensor estadoSensor = SENS_INICIANDO;
bool     hayDatos = false;
uint8_t  fallosSeguidos = 0, aciertosSeguidos = 0, saltosSeguidos = 0;
uint32_t ultimaLectura = 0, ultimoReinicioSensor = 0, igualesDesde = 0;
float    ultT = 0, ultH = 0;           // última lectura válida (cruda)
float    tFilt = 0, hFilt = 0;         // filtradas (mediana de 3)
float    tMin = 0, tMax = 0, hMin = 0, hMax = 0;

uint32_t totalLecturas = 0, totalFallos = 0, totalDescartadas = 0;
uint32_t latLecturaUs = 0, maxLoopUs = 0;

Sev      sev[AL_TOTAL];
uint8_t  confirmaciones[AL_TOTAL];
uint32_t ultimoAviso[AL_TOTAL];

// Buzzer / silencio / botón
int8_t   bzAlarma = -1;
Sev      bzSev = SEV_NINGUNA;
uint8_t  bzIdx = 0;
uint32_t bzT0 = 0, bzProxAdv = 0;
bool     bzSonando = false;
bool     silencioActivo = false;
uint32_t silencioT0 = 0, botonT = 0;
bool     botonPrev = false;

uint32_t ultimoReporte = 0;

// Simulación
bool  simActiva = false, simFallo = false;
float simT = 0, simH = 0;
char  lineaSerial[40];
uint8_t lineaN = 0;

// Red
QueueHandle_t colaNotif;
volatile bool wifiOk = false;


inline void led(int8_t pin, bool on) { if (pin >= 0) digitalWrite(pin, on ? HIGH : LOW); }

inline void buzzerSet(bool on) {
  digitalWrite(PIN_BUZZER, (on == BUZZER_ACTIVO_EN_ALTO) ? HIGH : LOW);
}

float mediana3(float a, float b, float c) {
  return fmaxf(fminf(a, b), fminf(fmaxf(a, b), c));
}

const Muestra& bufGet(uint8_t i) { return buf[(bufIni + i) % TAM_BUFFER]; }

void bufAgregar(float t, float h, uint32_t ms) {
  uint8_t pos = (bufIni + bufN) % TAM_BUFFER;
  if (bufN < TAM_BUFFER) bufN++; else bufIni = (bufIni + 1) % TAM_BUFFER;
  buf[pos].t = t; buf[pos].h = h; buf[pos].ms = ms;
}

void bufLimpiar() { bufN = 0; bufIni = 0; }

void uptimeTexto(uint32_t ms, char* out, size_t cap) {
  uint32_t s = ms / 1000;
  snprintf(out, cap, "%lud %02luh %02lum", (unsigned long)(s / 86400),
           (unsigned long)((s % 86400) / 3600), (unsigned long)((s % 3600) / 60));
}

void datosTexto(char* out, size_t cap) {
  if (!hayDatos) snprintf(out, cap, "Sin lecturas validas todavia");
  else snprintf(out, cap, "Temp: %.1f C | Hum: %.1f %%", tFilt, hFilt);
}

const char* motivoReinicio() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "encendido normal";
    case ESP_RST_SW:       return "reinicio por software";
    case ESP_RST_PANIC:    return "ERROR del programa (panic)";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:      return "WATCHDOG (el programa se colgo)";
    case ESP_RST_BROWNOUT: return "BAJO VOLTAJE (brownout)";
    default:               return "otro";
  }
}

void encolar(const char* texto) {
  Notif n;
  n.ms = millis();
  strncpy(n.texto, texto, sizeof(n.texto) - 1);
  n.texto[sizeof(n.texto) - 1] = 0;
  if (xQueueSend(colaNotif, &n, 0) != pdTRUE) {          // cola llena: descartar la más vieja
    Notif vieja;
    xQueueReceive(colaNotif, &vieja, 0);
    xQueueSend(colaNotif, &n, 0);
    Serial.println("[RED] cola llena: se descarto el mensaje mas antiguo");
  }
}

void jsonEscape(const char* in, char* out, size_t cap) {
  size_t j = 0;
  for (size_t i = 0; in[i] && j + 7 < cap; i++) {
    char c = in[i];
    if (c == '"')       { out[j++] = '\\'; out[j++] = '"'; }
    else if (c == '\\') { out[j++] = '\\'; out[j++] = '\\'; }
    else if (c == '\n') { out[j++] = '\\'; out[j++] = 'n'; }
    else if ((uint8_t)c >= 0x20) out[j++] = c;
  }
  out[j] = 0;
}

// Convierte el instante (millis) del evento a hora real, si el reloj ya se sincronizó
void prefijoHora(uint32_t msEvento, char* out, size_t cap) {
  time_t ahora = time(nullptr);
  if (ahora < 1700000000) { out[0] = 0; return; }
  time_t evento = ahora - (time_t)((uint32_t)(millis() - msEvento) / 1000);
  struct tm tmv;
  localtime_r(&evento, &tmv);
  strftime(out, cap, "[%d/%m %H:%M:%S] ", &tmv);
}

bool enviarTelegram(const Notif& n) {
  char pref[28];  prefijoHora(n.ms, pref, sizeof(pref));
  char texto[260]; snprintf(texto, sizeof(texto), "%s%s", pref, n.texto);
  char esc[640];  jsonEscape(texto, esc, sizeof(esc));
  char body[720];
  int len = snprintf(body, sizeof(body), "{\"chat_id\":\"%s\",\"text\":\"%s\"}", TELEGRAM_CHAT_ID, esc);
  char url[160];
  snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/sendMessage", TELEGRAM_TOKEN);

  WiFiClientSecure client;
  client.setInsecure();     // prototipo: no valida el certificado (en producción usar setCACert)
  HTTPClient http;
  if (!http.begin(client, url)) return false;
  http.setTimeout(8000);
  http.addHeader("Content-Type", "application/json");
  int code = http.POST((uint8_t*)body, len);
  http.end();
  Serial.printf("[TELEGRAM] HTTP %d\n", code);
  return code == 200;
}

void tareaRed(void*) {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t ultimoIntento = millis();
  bool ntpIniciado = false;
  uint8_t intentos = 0;

  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      if (wifiOk) Serial.println("[WIFI] conexion perdida");
      wifiOk = false;
      if (millis() - ultimoIntento > 15000) {
        WiFi.disconnect();
        WiFi.begin(WIFI_SSID, WIFI_PASS);
        ultimoIntento = millis();
      }
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }
    if (!wifiOk) { wifiOk = true; Serial.printf("[WIFI] conectado, IP %s\n", WiFi.localIP().toString().c_str()); }
    if (!ntpIniciado) { configTime(ZONA_HORARIA_S, 0, "pool.ntp.org", "time.google.com"); ntpIniciado = true; }

    Notif n;
    if (xQueuePeek(colaNotif, &n, 0) == pdTRUE) {
      if (enviarTelegram(n)) {
        xQueueReceive(colaNotif, &n, 0);
        intentos = 0;
      } else if (++intentos >= MAX_INTENTOS_ENVIO) {
        xQueueReceive(colaNotif, &n, 0);
        intentos = 0;
        Serial.println("[RED] mensaje descartado tras varios intentos");
      } else {
        vTaskDelay(pdMS_TO_TICKS(5000));
      }
    } else {
      vTaskDelay(pdMS_TO_TICKS(250));
    }
  }
}

void notificarAlarma(Alarma a, Sev ant, Sev nueva, const char* etiqueta) {
  (void)ant;
  char dat[64]; datosTexto(dat, sizeof(dat));
  char msg[220];
  if (nueva == SEV_NINGUNA) {
    snprintf(msg, sizeof(msg), "✅ NORMALIZADO: %s\n%s", NOMBRE_ALARMA[a], dat);
  } else {
    snprintf(msg, sizeof(msg), "%s %s: %s%s\n%s\n%s",
             nueva == SEV_CRITICA ? "🚨" : "⚠️", NOMBRE_SEV[nueva], NOMBRE_ALARMA[a],
             etiqueta, DETALLE_ALARMA[a], dat);
  }
  encolar(msg);
}

void enviarReporte(uint32_t now) {
  char up[24]; uptimeTexto(now, up, sizeof(up));
  char msg[220];
  if (hayDatos) {
    snprintf(msg, sizeof(msg),
             "📊 Reporte periodico\nTemp: %.1f C | Hum: %.1f %%\nMin/Max: %.1f/%.1f C | %.0f/%.0f %%\n"
             "Uptime: %s | RAM libre: %u KB | Wi-Fi: %d dBm",
             tFilt, hFilt, tMin, tMax, hMin, hMax, up,
             (unsigned)(ESP.getFreeHeap() / 1024), WiFi.RSSI());
  } else {
    snprintf(msg, sizeof(msg), "📊 Reporte periodico\nSin lecturas validas.\nUptime: %s", up);
  }
  encolar(msg);
}

// Severidad de un valor frente a umbrales, con histéresis.
// alta=true: peor cuanto mayor es v.  alta=false: peor cuanto menor es v.
Sev evalUmbral(float v, float adv, float crit, float hist, bool alta, Sev prev) {
  float x = alta ? v : -v;
  float a = alta ? adv : -adv;
  float c = alta ? crit : -crit;
  if (x >= c) return SEV_CRITICA;
  if (prev == SEV_CRITICA && x > c - hist) return SEV_CRITICA;
  if (x >= a) return SEV_ADVERTENCIA;
  if (prev >= SEV_ADVERTENCIA && x > a - hist) return SEV_ADVERTENCIA;
  return SEV_NINGUNA;
}

int8_t alarmaDominante() {
  int8_t dom = -1;
  Sev mejor = SEV_NINGUNA;
  for (uint8_t a = 0; a < AL_TOTAL; a++) {
    if (sev[a] > mejor) { mejor = sev[a]; dom = a; }
  }
  return dom;
}

Sev severidadGlobal() {
  int8_t d = alarmaDominante();
  return d < 0 ? SEV_NINGUNA : sev[d];
}

void aplicarAlarma(Alarma a, Sev cand, uint32_t now) {
  Sev ant = sev[a];
  bool cambio = false;
  if (cand > ant) {
    // Subir de severidad exige confirmación (salvo falla del sensor, ya filtrada aparte)
    if (a == AL_SENSOR_FALLA || ++confirmaciones[a] >= CONFIRMACIONES_ALARMA) {
      sev[a] = cand; confirmaciones[a] = 0; cambio = true;
    }
  } else {
    confirmaciones[a] = 0;
    if (cand != ant) { sev[a] = cand; cambio = true; }
  }
  if (cambio) {
    ultimoAviso[a] = now;
    if (sev[a] > ant) silencioActivo = false;      // alarma nueva o más grave: se cancela el silencio
    Serial.printf("[ALARMA] %s: %s -> %s\n", NOMBRE_ALARMA[a], NOMBRE_SEV[ant], NOMBRE_SEV[sev[a]]);
    notificarAlarma(a, ant, sev[a], "");
  }
}

// Variación en °C/min y %/min usando el buffer de 1 minuto
bool calcularTasas(float& dT, float& dH) {
  if (bufN < 10) return false;
  const Muestra& a = bufGet(0);
  const Muestra& b = bufGet(bufN - 1);
  float dtMin = (float)(b.ms - a.ms) / 60000.0f;
  if (dtMin < 0.4f) return false;
  dT = (b.t - a.t) / dtMin;
  dH = (b.h - a.h) / dtMin;
  return true;
}

// Se llama solo cuando entra una muestra válida nueva
void evaluarAlarmasValores(uint32_t now) {
  aplicarAlarma(AL_TEMP_ALTA, evalUmbral(tFilt, T_ALTA_ADV, T_ALTA_CRIT, HIST_T, true,  sev[AL_TEMP_ALTA]), now);
  aplicarAlarma(AL_TEMP_BAJA, evalUmbral(tFilt, T_BAJA_ADV, T_BAJA_CRIT, HIST_T, false, sev[AL_TEMP_BAJA]), now);
  aplicarAlarma(AL_HUM_ALTA,  evalUmbral(hFilt, H_ALTA_ADV, H_ALTA_CRIT, HIST_H, true,  sev[AL_HUM_ALTA]),  now);
  aplicarAlarma(AL_HUM_BAJA,  evalUmbral(hFilt, H_BAJA_ADV, H_BAJA_CRIT, HIST_H, false, sev[AL_HUM_BAJA]),  now);

  Sev c = SEV_NINGUNA;
  float dT, dH;
  if (calcularTasas(dT, dH)) {
    float r = fmaxf(fabsf(dT) / LIM_RATE_T, fabsf(dH) / LIM_RATE_H);   // 1.0 = límite de advertencia
    c = evalUmbral(r, 1.0f, 2.0f, 0.3f, true, sev[AL_CAMBIO_BRUSCO]);
  }
  aplicarAlarma(AL_CAMBIO_BRUSCO, c, now);
}

// Se llama en cada vuelta del loop
void evaluarAlarmasSensor(uint32_t now) {
  aplicarAlarma(AL_SENSOR_FALLA, estadoSensor == SENS_FALLA ? SEV_CRITICA : SEV_NINGUNA, now);
  bool congelado = (estadoSensor == SENS_OK) && (now - igualesDesde > CONGELADO_MS);
  aplicarAlarma(AL_SENSOR_CONGELADO, congelado ? SEV_ADVERTENCIA : SEV_NINGUNA, now);
}

void recordatorios(uint32_t now) {
  for (uint8_t a = 0; a < AL_TOTAL; a++) {
    if (sev[a] == SEV_NINGUNA) continue;
    uint32_t periodo = (sev[a] == SEV_CRITICA) ? REC_CRIT_MS : REC_ADV_MS;
    if (now - ultimoAviso[a] >= periodo) {
      ultimoAviso[a] = now;
      notificarAlarma((Alarma)a, sev[a], sev[a], " (recordatorio)");
    }
  }
}



void imprimirEstado() {
  int8_t d = alarmaDominante();
  Serial.printf("[DATO] T=%.1f (f %.1f) H=%.1f (f %.1f) | sensor=%s | alarma=%s | lect=%lu ms | "
                "heap=%u KB (min %u) | loopMax=%lu us | wifi=%s | ok/fallo/desc=%lu/%lu/%lu\n",
                ultT, tFilt, ultH, hFilt,
                estadoSensor == SENS_OK ? "OK" : (estadoSensor == SENS_FALLA ? "FALLA" : "INICIANDO"),
                d < 0 ? "NINGUNA" : NOMBRE_ALARMA[d],
                (unsigned long)(latLecturaUs / 1000),
                (unsigned)(ESP.getFreeHeap() / 1024), (unsigned)(ESP.getMinFreeHeap() / 1024),
                (unsigned long)maxLoopUs, wifiOk ? "OK" : "sin conexion",
                (unsigned long)(totalLecturas - totalFallos), (unsigned long)totalFallos,
                (unsigned long)totalDescartadas);
  maxLoopUs = 0;
}

void registrarFallo(uint32_t now, const char* motivo) {
  totalFallos++;
  if (fallosSeguidos < 255) fallosSeguidos++;
  aciertosSeguidos = 0;
  Serial.printf("[LECTURA] fallo %u/%u: %s\n", fallosSeguidos, FALLOS_PARA_ERROR, motivo);

  if (fallosSeguidos >= FALLOS_PARA_ERROR && estadoSensor != SENS_FALLA) {
    estadoSensor = SENS_FALLA;
    ultimoReinicioSensor = now;
    Serial.println("[SENSOR] declarado en FALLA");
  }
  if (estadoSensor == SENS_FALLA && now - ultimoReinicioSensor >= REINTENTO_SENSOR_MS) {
    dht.begin();
    ultimoReinicioSensor = now;
    Serial.println("[SENSOR] reinicializando el DHT22");
  }
}

void registrarAcierto(uint32_t now, float t, float h) {
  fallosSeguidos = 0;
  if (estadoSensor == SENS_FALLA) {
    if (++aciertosSeguidos < ACIERTOS_PARA_RECUPERAR) {
      Serial.printf("[SENSOR] recuperandose %u/%u\n", aciertosSeguidos, ACIERTOS_PARA_RECUPERAR);
      return;
    }
    bufLimpiar();                 // los datos viejos ya no sirven para calcular tasas
    igualesDesde = now;
    Serial.println("[SENSOR] recuperado");
  }
  estadoSensor = SENS_OK;
  aciertosSeguidos = 0;

  if (!hayDatos) { tMin = tMax = t; hMin = hMax = h; igualesDesde = now; }
  if (t != ultT || h != ultH) igualesDesde = now;
  tMin = fminf(tMin, t); tMax = fmaxf(tMax, t);
  hMin = fminf(hMin, h); hMax = fmaxf(hMax, h);
  ultT = t; ultH = h; hayDatos = true;

  bufAgregar(t, h, now);
  if (bufN >= 3) {
    tFilt = mediana3(bufGet(bufN - 1).t, bufGet(bufN - 2).t, bufGet(bufN - 3).t);
    hFilt = mediana3(bufGet(bufN - 1).h, bufGet(bufN - 2).h, bufGet(bufN - 3).h);
  } else {
    tFilt = t; hFilt = h;
  }

  evaluarAlarmasValores(now);
  imprimirEstado();
}

void procesarLectura(uint32_t now) {
  uint32_t t0 = micros();
  float h = dht.readHumidity();
  float t = dht.readTemperature();
  latLecturaUs = micros() - t0;
  totalLecturas++;

  // Modo simulación (para pruebas)
  if (simFallo)       { t = NAN; h = NAN; }
  else if (simActiva) { t = simT; h = simH; }

  // Capa 1: lectura inválida
  if (isnan(t) || isnan(h)) { registrarFallo(now, "sin respuesta (NaN)"); return; }
  // Capa 2: fuera del rango físico del DHT22
  if (t < -40 || t > 80 || h < 0 || h > 100) { registrarFallo(now, "valor fuera de rango"); return; }

  // Capa 3: salto imposible entre dos muestras (si se repite, se acepta como cambio real)
  if (bufN > 0) {
    const Muestra& u = bufGet(bufN - 1);
    if (fabsf(t - u.t) > MAX_SALTO_T || fabsf(h - u.h) > MAX_SALTO_H) {
      if (++saltosSeguidos < MAX_SALTOS_SEGUIDOS) {
        totalDescartadas++;
        Serial.printf("[LECTURA] salto sospechoso descartado (%u/%u): T=%.1f H=%.1f\n",
                      saltosSeguidos, MAX_SALTOS_SEGUIDOS - 1, t, h);
        return;
      }
    }
  }
  saltosSeguidos = 0;

  // Capa 4 (sensor congelado) se evalúa en evaluarAlarmasSensor()
  registrarAcierto(now, t, h);
}



void actualizarBuzzer(uint32_t now) {
  int8_t dom = alarmaDominante();
  Sev s = dom >= 0 ? sev[dom] : SEV_NINGUNA;

  if (silencioActivo && now - silencioT0 >= SILENCIO_MS) {
    silencioActivo = false;
    Serial.println("[BUZZER] fin del silencio");
  }
  if (dom < 0 || silencioActivo) {
    buzzerSet(false); bzSonando = false; bzAlarma = -1;
    return;
  }

  if (dom != bzAlarma || s != bzSev) {           // alarma nueva o cambió de severidad: reiniciar patrón
    bzAlarma = dom; bzSev = s;
    bzSonando = true; bzIdx = 0; bzT0 = now;
  }
  if (!bzSonando) {
    if (s == SEV_ADVERTENCIA && now >= bzProxAdv) { bzSonando = true; bzIdx = 0; bzT0 = now; }
    else { buzzerSet(false); return; }
  }

  const Patron& p = PATRONES[dom];
  if (now - bzT0 >= p.d[bzIdx]) {
    bzIdx++; bzT0 = now;
    if (bzIdx >= p.n) {
      bzIdx = 0;
      if (s == SEV_ADVERTENCIA) {                // la advertencia suena una vez y espera
        bzSonando = false;
        bzProxAdv = now + ADV_REPETIR_MS;
        buzzerSet(false);
        return;
      }
    }
  }
  buzzerSet(bzIdx % 2 == 0);
}

void actualizarLeds(uint32_t now) {
  Sev g = severidadGlobal();
  bool v = false, r = false;
  if (!hayDatos && estadoSensor != SENS_FALLA) v = (now / 500) % 2 == 0;     // iniciando: verde parpadea
  else if (g == SEV_NINGUNA)                   v = true;                      // todo normal
  else if (g == SEV_ADVERTENCIA)               r = (now / 500) % 2 == 0;      // rojo lento
  else                                         r = (now / 125) % 2 == 0;      // rojo rápido
  led(PIN_LED_VERDE, v);
  led(PIN_LED_ROJO, r);
}

void leerBoton(uint32_t now) {
  bool presionado = digitalRead(PIN_BOTON) == LOW;
  if (presionado && !botonPrev && now - botonT > 250) {
    botonT = now;
    if (silencioActivo) {
      silencioActivo = false;
      Serial.println("[BOTON] silencio cancelado");
    } else if (alarmaDominante() >= 0) {
      silencioActivo = true; silencioT0 = now;
      Serial.println("[BOTON] alarma sonora silenciada 10 min");
    }
  }
  botonPrev = presionado;
}



void ejecutarComando(const char* c, uint32_t now) {
  float t, h;
  if (strcmp(c, "sim off") == 0)        { simActiva = false; simFallo = false; Serial.println("[SIM] desactivada"); }
  else if (strcmp(c, "sim fallo") == 0) { simFallo = true; Serial.println("[SIM] sensor desconectado simulado"); }
  else if (sscanf(c, "sim %f %f", &t, &h) == 2) {
    simActiva = true; simFallo = false; simT = t; simH = h;
    Serial.printf("[SIM] valores simulados T=%.1f H=%.1f\n", t, h);
  }
  else if (strcmp(c, "reporte") == 0)   { enviarReporte(now); }
  else Serial.println("[CMD] desconocido. Usa: sim <t> <h> | sim fallo | sim off | reporte");
}

void leerSerial(uint32_t now) {
  while (Serial.available()) {
    char ch = Serial.read();
    if (ch == '\n' || ch == '\r') {
      if (lineaN > 0) { lineaSerial[lineaN] = 0; ejecutarComando(lineaSerial, now); lineaN = 0; }
    } else if (lineaN < sizeof(lineaSerial) - 1) {
      lineaSerial[lineaN++] = ch;
    }
  }
}


void setup() {
  buzzerSet(false);
  pinMode(PIN_BUZZER, OUTPUT);
  if (PIN_LED_VERDE >= 0) pinMode(PIN_LED_VERDE, OUTPUT);
  if (PIN_LED_ROJO  >= 0) pinMode(PIN_LED_ROJO, OUTPUT);
  pinMode(PIN_BOTON, INPUT_PULLUP);

  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== Monitor de vivero | ESP32 + DHT22 ===");
  Serial.printf("Motivo de reinicio: %s\n", motivoReinicio());

  // Watchdog: si el loop se cuelga 15 s, la placa se reinicia sola
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  esp_task_wdt_config_t wcfg;
  wcfg.timeout_ms = 15000; wcfg.idle_core_mask = 0; wcfg.trigger_panic = true;
  if (esp_task_wdt_reconfigure(&wcfg) != ESP_OK) esp_task_wdt_init(&wcfg);
#else
  esp_task_wdt_init(15, true);
#endif
  esp_task_wdt_add(NULL);

  // Autotest de salidas
  led(PIN_LED_VERDE, true); led(PIN_LED_ROJO, true);
  buzzerSet(true); delay(120); buzzerSet(false);
  delay(300);
  led(PIN_LED_VERDE, false); led(PIN_LED_ROJO, false);

  for (uint8_t i = 0; i < AL_TOTAL; i++) { sev[i] = SEV_NINGUNA; confirmaciones[i] = 0; ultimoAviso[i] = 0; }

  dht.begin();

  colaNotif = xQueueCreate(8, sizeof(Notif));
  xTaskCreatePinnedToCore(tareaRed, "red", 12288, NULL, 1, NULL, 0);

  char msg[120];
  snprintf(msg, sizeof(msg), "🟢 Sistema iniciado\nMotivo de reinicio: %s", motivoReinicio());
  encolar(msg);

  ultimaLectura = millis();
  ultimoReporte = millis();
}

void loop() {
  uint32_t ini = micros();
  uint32_t now = millis();
  esp_task_wdt_reset();

  leerSerial(now);
  leerBoton(now);

  if (now - ultimaLectura >= INTERVALO_LECTURA_MS) {
    ultimaLectura = now;
    procesarLectura(now);
  }

  evaluarAlarmasSensor(now);
  recordatorios(now);
  actualizarBuzzer(now);
  actualizarLeds(now);

  if (now - ultimoReporte >= REPORTE_MS) { ultimoReporte = now; enviarReporte(now); }

  uint32_t dur = micros() - ini;
  if (dur > maxLoopUs) maxLoopUs = dur;
  delay(5);      // cede tiempo al sistema; el loop sigue siendo mucho más rápido que cualquier patrón
}
