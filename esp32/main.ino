#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <HardwareSerial.h>
#include "esp_camera.h"
#include "esp_timer.h"
#include "img_converters.h"
#include "fb_gfx.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include <vector>
#include <algorithm>
#include <UniversalTelegramBot.h>

// --- Configuración USUARIO (¡IMPORTANTE: Modificar!) ---
#define WIFI_SSID         "WIFI_SSID"
#define WIFI_PASSWORD     "WIFI_PASSWORD"
#define API_BASE_URL      "http://HOSTNAME:PORT"
#define TELEGRAM_BOT_TOKEN "TELEGRAM_BOT_TOKEN"
#define TELEGRAM_CHAT_ID   "TELEGRAM_CHAT_ID"

#define API_CHECKIN_ENDPOINT "api/checkin"
#define API_NFC_UIDS_ENDPOINT "api/cards/uids"
#define API_PING_ENDPOINT     "api/ping"

#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

// --- Configuración Comunicación Arduino ---
#define ARDUINO_RX_PIN 14
#define ARDUINO_TX_PIN 15
#define ARDUINO_BAUD_RATE 9600

#define API_FETCH_INTERVAL_MS (10 * 1000)
#define CHECKIN_INTERVAL_MS   (1 * 60 * 1000)
#define JSON_DOC_SIZE_LARGE 10240
#define JSON_DOC_SIZE_SMALL 1024
WiFiClientSecure clientSecure;
HTTPClient http;
HardwareSerial ArduinoSerial(2);
esp_timer_handle_t apiUpdateTimer;
esp_timer_handle_t checkinTimer;
WiFiClientSecure telegramClientSecure;
UniversalTelegramBot bot(TELEGRAM_BOT_TOKEN, telegramClientSecure);

std::vector<String> whitelistNfcUids;

bool wifiConnected = false;
String deviceId = "";
void connectWiFi();
bool initCamera();
void handleArduinoCommands();
void periodicApiUpdate(void *arg);
void periodicCheckin(void *arg);
bool fetchNfcUids();
bool sendNfcUidToApi(const String& uid);
bool sendMotionDetectedLog();
void sendNfcUpdateToArduino();
bool sendMessageTelegram(const String& message); // Nueva función para enviar solo mensaje
bool sendFingerprintLogToBackend(int fingerprintId); // Nueva función para enviar log de huella
bool sendMotionDetectedLog(); // Nueva función para enviar log de movimiento detectado

// =========================================================================
// ===                           SETUP                                   ===
// =========================================================================
void setup() {
    // Desactivar Brownout Detector (a menudo necesario para ESP32-CAM)
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

    // Iniciar comunicación serial
    Serial.begin(115200);
    delay(100); // Pequeña pausa para estabilizar la comunicación serial
    
    Serial.println(F("=================================================="));
    Serial.println(F("Iniciando ESP32-CAM Sistema de Seguridad..."));
    Serial.println(F("=================================================="));


    // Inicializar PSRAM (si está disponible)
    if (!psramInit()) {
        Serial.println(F("[SETUP] Error: PSRAM no encontrada o fallo al inicializar!"));
        // Considerar detenerse o continuar con funcionalidad limitada
    } else {
        Serial.println(F("[SETUP] PSRAM inicializada correctamente."));
    }

    // Inicializar Serial2 para Arduino
    ArduinoSerial.begin(ARDUINO_BAUD_RATE, SERIAL_8N1, ARDUINO_RX_PIN, ARDUINO_TX_PIN);
    Serial.println(F("[SETUP] Serial2 inicializado para comunicación con Arduino."));

    // Conectar a WiFi
    connectWiFi();
    
    // Obtener MAC para usar como Device ID (movido después de conectar WiFi)
    deviceId = WiFi.macAddress();
    Serial.print(F("[SETUP] Device ID (MAC): "));
    Serial.println(deviceId);

    // Inicializar Cámara
    if (!initCamera()) {
        Serial.println(F("[SETUP] Error crítico: Fallo al inicializar la cámara. Reiniciando..."));
        delay(5000);
        ESP.restart();
    }    // Configurar cliente HTTPS (ignorar validación de certificado para desarrollo)
    // ¡¡¡IMPORTANTE!!! Para producción, debes manejar los certificados correctamente.
    clientSecure.setInsecure();
    
    // Configurar certificado raíz para Telegram
    telegramClientSecure.setCACert(TELEGRAM_CERTIFICATE_ROOT); // Usar certificado raíz oficial
    Serial.println(F("[SETUP] Clientes HTTPS configurados correctamente."));
    // Obtener whitelists iniciales de la API
    Serial.println(F("[SETUP] Obteniendo whitelists iniciales de la API..."));
    if (wifiConnected) {
        fetchNfcUids();
        // Enviar lista NFC inicial al Arduino
        sendNfcUpdateToArduino();
    } else {
        Serial.println(F("[SETUP] No hay conexión WiFi, no se pueden obtener whitelists iniciales."));
    }

    // Configurar Timers para tareas periódicas
    esp_timer_create_args_t apiTimerArgs = {
        .callback = &periodicApiUpdate,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "apiUpdateTimer"
    };
    esp_timer_create(&apiTimerArgs, &apiUpdateTimer);
    esp_timer_start_periodic(apiUpdateTimer, API_FETCH_INTERVAL_MS * 1000ULL); // Intervalo en microsegundos
    Serial.println(F("[SETUP] Timer para actualización periódica de API configurado."));


    esp_timer_create_args_t checkinTimerArgs = {
            .callback = &periodicCheckin,
            .arg = NULL,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "checkinTimer"
    };
    esp_timer_create(&checkinTimerArgs, &checkinTimer);
    esp_timer_start_periodic(checkinTimer, CHECKIN_INTERVAL_MS * 1000ULL); // Intervalo en microsegundos
    Serial.println(F("[SETUP] Timer para check-in periódico configurado."));


    Serial.println(F("=================================================="));
    Serial.println(F("[SETUP] Setup completado. Sistema listo. ✅"));
    Serial.println(F("=================================================="));

}

// =========================================================================
// ===                           LOOP                                    ===
// =========================================================================
void loop() {
    // Gestionar conexión WiFi si se pierde
    if (WiFi.status() != WL_CONNECTED && wifiConnected) {
        Serial.println(F("[LOOP] Conexión WiFi perdida. Intentando reconectar..."));
        wifiConnected = false;
        connectWiFi(); // Intentar reconectar
    }

    // Leer y procesar comandos del Arduino
    handleArduinoCommands();

    // Pequeña pausa para no sobrecargar
    delay(50);
}

// =========================================================================
// ===                  FUNCIONES AUXILIARES                           ===
// =========================================================================

// --- Conexión WiFi ---
void connectWiFi() {
    Serial.print(F("[WiFi] Conectando a WiFi SSID: "));
    Serial.println(WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    unsigned long startTime = millis();
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
        if (millis() - startTime > 20000) { // Timeout de 20 segundos
            Serial.println(F("\n[WiFi] Error: No se pudo conectar a WiFi (Timeout)."));
            wifiConnected = false;
            return;
        }
    }

    Serial.println(F("\n[WiFi] Conectado a WiFi!"));
    Serial.print(F("[WiFi] Dirección IP: "));
    Serial.println(WiFi.localIP());
    wifiConnected = true;
}

// --- Inicialización Cámara ---
bool initCamera() {
    camera_config_t config;
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer = LEDC_TIMER_0;
    config.pin_d0 = Y2_GPIO_NUM;
    config.pin_d1 = Y3_GPIO_NUM;
    config.pin_d2 = Y4_GPIO_NUM;
    config.pin_d3 = Y5_GPIO_NUM;
    config.pin_d4 = Y6_GPIO_NUM;
    config.pin_d5 = Y7_GPIO_NUM;
    config.pin_d6 = Y8_GPIO_NUM;
    config.pin_d7 = Y9_GPIO_NUM;
    config.pin_xclk = XCLK_GPIO_NUM;
    config.pin_pclk = PCLK_GPIO_NUM;
    config.pin_vsync = VSYNC_GPIO_NUM;
    config.pin_href = HREF_GPIO_NUM;
    config.pin_sccb_sda = SIOD_GPIO_NUM;
    config.pin_sccb_scl = SIOC_GPIO_NUM;
    config.pin_pwdn = PWDN_GPIO_NUM;
    config.pin_reset = RESET_GPIO_NUM;
    config.xclk_freq_hz = 20000000;
    config.pixel_format = PIXFORMAT_JPEG; // Formato JPEG para envío fácil    // Configuración para PSRAM
    if (psramFound()) {
        // Usar la máxima resolución estable soportada por el módulo y Telegram (UXGA o SXGA)
        Serial.println("[CAM] PSRAM encontrada, usando configuración de alta calidad.");
        config.frame_size = FRAMESIZE_UXGA; // 1600x1200 (prueba primero UXGA, si falla prueba SXGA o XGA)
        config.jpeg_quality = 8;            // Mejor calidad (0-63, menor es mejor)
        config.fb_count = 2;
        config.fb_location = CAMERA_FB_IN_PSRAM;
    } else {
        Serial.println("[CAM] PSRAM no encontrada, usando configuración de calidad media.");
        config.frame_size = FRAMESIZE_XGA; // 1024x768
        config.jpeg_quality = 10;
        config.fb_count = 1;
        config.fb_location = CAMERA_FB_IN_DRAM;
    }

    // Inicializar cámara
    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        Serial.printf("[CAM] Error al inicializar la cámara: 0x%x (%s)\n", err, esp_err_to_name(err));
        return false;
    }

    // Configuración avanzada del sensor para máxima calidad y visibilidad
    sensor_t * s = esp_camera_sensor_get();
    if (s) {
        s->set_framesize(s, FRAMESIZE_SXGA);    // 1280x1024 (alta calidad pero más estable que UXGA)
        s->set_quality(s, 10);                  // 10 es muy buena calidad
        s->set_contrast(s, 2);                  // Mejora definición
        s->set_brightness(s, 1);                // Aumenta brillo si está oscuro
        s->set_saturation(s, 2);                // Colores más vivos
        s->set_sharpness(s, 2);                 // Imagen más nítida
        s->set_denoise(s, 1);                   // Suaviza ruido digital
        s->set_special_effect(s, 0);            // 0 = sin efecto

        // Corrección de orientación
        s->set_vflip(s, 0);                     // 1 si la ves de cabeza
        s->set_hmirror(s, 0); 
        Serial.println("[CAM] Sensor configurado para máxima calidad y visibilidad.");
    }

    Serial.println(F("[CAM] Cámara inicializada correctamente."));
    return true;
}

// Funciones Bluetooth eliminadas


// --- Manejador de Comandos del Arduino ---
void handleArduinoCommands() {
    if (ArduinoSerial.available()) {
        String command = ArduinoSerial.readStringUntil('\n');
        command.trim();
        // Filtrar caracteres extraños que a veces llegan por Serial
        String cleanCommand = "";
        for (int i = 0; i < command.length(); i++) {
            if (isPrintable(command.charAt(i))) {
                cleanCommand += command.charAt(i);
            }
        }
        command = cleanCommand; // Usar el comando limpio

        if (command.length() > 0) { // Procesar solo si queda algo después de limpiar
            Serial.print("[ARDUINO] Comando recibido: '");
            Serial.print(command);
            Serial.println("'");

            if (command.startsWith("MOTION")) {
                Serial.println("[ARDUINO] Movimiento detectado por Arduino. Enviando mensaje...");
                // Enviar mensaje de texto por Telegram
                sendMessageTelegram("⚠ Alerta: Movimiento detectado por el sensor.");
                
                // Enviar log al backend
                bool logSuccess = sendMotionDetectedLog();
                if (logSuccess) {
                    Serial.println("[ARDUINO] Log de movimiento enviado correctamente al backend.");
                } else {
                    Serial.println("[ARDUINO] Error al enviar log de movimiento al backend.");
                }
            } else if (command.startsWith("PHOTO")) {
                Serial.println("[ARDUINO] Fallo de huella en Arduino. Tomando foto y enviando alertas...");
                takePhotoAndSendAlerts("Intento de acceso fallido por huella");
            } else if (command.startsWith("ACCESS")) {
                // Extraer el ID después de "ACCESS_GUARANTEED:"
                String idStr = command.substring(18); // Cambiar 17 a 18 para evitar incluir el colon
                Serial.print("[ARDUINO] Acceso garantizado con huella ID: ");
                Serial.println(idStr);
                
                // Convertir el ID de string a entero
                int fingerprintId = idStr.toInt();
                
                if (fingerprintId > 0) {
                    // Enviar log al backend
                    bool success = sendFingerprintLogToBackend(fingerprintId);
                    if (success) {
                        Serial.println("[ARDUINO] Log de acceso enviado correctamente al backend.");
                    } else {
                        Serial.println("[ARDUINO] Error al enviar log de acceso al backend.");
                    }
                } else {
                    Serial.println("[ARDUINO] Error: ID de huella inválido.");
                }
            } else if (command.startsWith("NFC_UID_AUTH:")) {
                String uidWithColons = command.substring(12); // Extraer el UID completo con los dos puntos
                Serial.print("[ARDUINO] Recibido UID con formato: ");
                Serial.println(uidWithColons);
                
                // Procesar el formato con los dos puntos (B9:7E:FC:03) para extraer todos los bytes
                String newUid = "";
                int lastPos = 0;
                int colonPos = uidWithColons.indexOf(':', lastPos);
                
                while (colonPos >= 0 || lastPos < uidWithColons.length()) {
                    // Extraer el byte actual (entre la posición actual y el siguiente ':' o final)
                    String byteStr;
                    if (colonPos >= 0) {
                        byteStr = uidWithColons.substring(lastPos, colonPos);
                        lastPos = colonPos + 1;
                    } else {
                        byteStr = uidWithColons.substring(lastPos);
                        lastPos = uidWithColons.length();
                    }
                    
                    // Añadir el byte al UID final (sin los dos puntos)
                    newUid += byteStr;
                    
                    // Buscar el siguiente separador
                    colonPos = uidWithColons.indexOf(':', lastPos);
                }
                
                newUid.toUpperCase();
                Serial.print("[ARDUINO] Arduino autorizó nueva tarjeta NFC: ");
                Serial.println(newUid);

                bool alreadyExists = false;
                for(const String& uid : whitelistNfcUids) {
                    if (uid.equalsIgnoreCase(newUid)) {
                        alreadyExists = true;
                        break;
                    }
                }
                if (!alreadyExists) {
                    whitelistNfcUids.push_back(newUid);
                    Serial.println("[ARDUINO] Nuevo UID añadido a la lista local en memoria.");
                    sendNfcUidToApi(newUid);
                } else {
                    Serial.println("[ARDUINO] El UID ya existía en la lista local. No se añade ni se envía a la API.");
                }
            } else if (command.startsWith("REGISTER:")) {
                // Formato esperado: "REGISTER:ID;UID"
                int separatorPos = command.indexOf(';');
                if (separatorPos > 0) {
                    String fingerprintId = command.substring(9, separatorPos); // 9 es la longitud de "REGISTER:"
                    String cardUid = command.substring(separatorPos + 1);
                    cardUid.toUpperCase(); // Asegurarse que el UID está en mayúsculas
                    
                    String userName = "User " + fingerprintId;
                    
                    Serial.print("[ARDUINO] Registrando nueva huella con ID: ");
                    Serial.print(fingerprintId);
                    Serial.print(", UID de tarjeta: ");
                    Serial.println(cardUid);
                    
                    // Preparar JSON para enviar al backend
                    String url = String(API_BASE_URL) + "/api/register-fingerprint";
                    String payload;
                    
                    DynamicJsonDocument jsonDoc(JSON_DOC_SIZE_SMALL);
                    jsonDoc["fingerprintId"] = fingerprintId;
                    jsonDoc["cardUid"] = cardUid;
                    jsonDoc["userName"] = userName;
                    
                    serializeJson(jsonDoc, payload);
                    
                    http.begin(url);
                    http.addHeader("Content-Type", "application/json");
                    
                    int httpCode = http.POST(payload);
                    
                    if (httpCode > 0) {
                        Serial.printf("[API REG] Registro completado, código: %d\n", httpCode);
                        if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_CREATED) {
                            Serial.println("[API REG] Usuario registrado con éxito en el backend");
                        } else {
                            Serial.printf("[API REG] Error al registrar usuario: %d\n", httpCode);
                        }
                    } else {
                        Serial.printf("[API REG] Error en la conexión: %s\n", http.errorToString(httpCode).c_str());
                    }
                    
                    http.end();
                }
            } else {
                Serial.println("[ARDUINO] Comando no reconocido.");
            }
        }
    }
}

// --- Tarea Periódica: Actualizar Whitelists desde API ---
void periodicApiUpdate(void *arg) {
    Serial.println("[TIMER API] Ejecutando actualización periódica de API...");
    if (wifiConnected) {
        Serial.println("[TIMER API] Obteniendo UIDs NFC...");
        bool nfcUpdated = fetchNfcUids();
        if (nfcUpdated) {
            Serial.println("[TIMER API] Lista NFC cambió, enviando actualización a Arduino...");
            sendNfcUpdateToArduino();
        } else {
             Serial.println("[TIMER API] Lista NFC sin cambios.");
        }
    } else {
        Serial.println("[TIMER API] No hay WiFi, omitiendo actualización de API.");
    }
     Serial.println("[TIMER API] Actualización periódica de API finalizada.");
}

// --- Tarea Periódica: Check-in con la API ---
void periodicCheckin(void *arg) {
    if (!wifiConnected) {
        return;
    }

    String url = String(API_BASE_URL) + "/" + API_CHECKIN_ENDPOINT;
    String payload;

    DynamicJsonDocument jsonDoc(JSON_DOC_SIZE_SMALL);
    // Usar WiFi.macAddress() directamente en lugar de deviceId 
    // Asegura que siempre se usa la MAC real, no un valor por defecto
    jsonDoc["deviceId"] = WiFi.macAddress();
    jsonDoc["ip"] = WiFi.localIP().toString();
    serializeJson(jsonDoc, payload);

    // Usar http en lugar de clientSecure ya que API_BASE_URL es http://
    http.begin(url);  
    http.addHeader("Content-Type", "application/json");

    Serial.print("[TIMER CHK] Enviando check-in a: ");
    Serial.println(url);
    Serial.print("[TIMER CHK] Payload: ");
    Serial.println(payload);

    int httpCode = http.POST(payload);

    if (httpCode > 0) {
        if (httpCode >= 200 && httpCode < 300) {
            Serial.println("[TIMER CHK] Check-in exitoso.");
        } else {
            Serial.printf("[TIMER CHK] Check-in completado con código HTTP inesperado: %d\n", httpCode);
            String response = http.getString();
            Serial.println("[TIMER CHK] Respuesta Check-in: " + response);
        }
        http.end();
    } else {
        Serial.printf("[TIMER CHK] Error en Check-in, código de error HTTP: %s\n", http.errorToString(httpCode).c_str());
        Serial.println("[TIMER CHK] Intentando verificar conectividad con el servidor...");
        
        // Intentar un ping al endpoint de ping para verificar conectividad
        http.end();
        String pingUrl = String(API_BASE_URL) + "/" + API_PING_ENDPOINT;
        http.begin(pingUrl);
        int pingCode = http.GET();
        
        if (pingCode > 0) {
            Serial.printf("[TIMER CHK] Ping exitoso, código HTTP: %d. El servidor está disponible.\n", pingCode);
        } else {
            Serial.printf("[TIMER CHK] Ping falló, código error: %s. Verifique que el servidor esté ejecutándose en %s\n", 
                         http.errorToString(pingCode).c_str(), API_BASE_URL);
        }
        http.end();
    }
}

// --- Obtener UIDs NFC desde API ---
bool fetchNfcUids() {
    if (!wifiConnected) {
        Serial.println("[API NFC] No hay conexión WiFi.");
        return false;
    }

    bool updated = false;
    String url = String(API_BASE_URL) + "/" + API_NFC_UIDS_ENDPOINT;
    Serial.print("[API NFC] GET a: ");
    Serial.println(url);

    // Usar http normal (no clientSecure) para conexiones HTTP
    http.begin(url);
    
    int httpCode = http.GET();

    if (httpCode == HTTP_CODE_OK) {
        Serial.println("[API NFC] Petición GET exitosa (200 OK). Procesando respuesta...");
        DynamicJsonDocument jsonDoc(JSON_DOC_SIZE_LARGE);

        DeserializationError error = deserializeJson(jsonDoc, http.getStream());
        http.end();

        if (!error) {
            // Verificar si existe el campo "cards" como en el código de referencia
            if (jsonDoc.containsKey("cards") && jsonDoc["cards"].is<JsonArray>()) {
                JsonArray cardsArray = jsonDoc["cards"];
                
                Serial.print("[API NFC] JSON parseado correctamente. Encontrado array 'cards' con ");
                Serial.print(cardsArray.size());
                Serial.println(" elementos.");

                std::vector<String> newWhitelist;
                newWhitelist.reserve(cardsArray.size());

                for (JsonVariant card : cardsArray) {
                    if (card.containsKey("uid")) {
                        String uidStr = card["uid"].as<String>();
                        uidStr.toUpperCase();
                        newWhitelist.push_back(uidStr);
                        Serial.print("[API NFC] Card UID: ");
                        Serial.println(uidStr);
                    }
                }

                bool changed = false;
                if (newWhitelist.size() != whitelistNfcUids.size()) {
                    changed = true;
                } else {
                    std::vector<String> sorted_new = newWhitelist;
                    std::vector<String> sorted_old = whitelistNfcUids;
                    std::sort(sorted_new.begin(), sorted_new.end());
                    std::sort(sorted_old.begin(), sorted_old.end());
                    if (!std::equal(sorted_new.begin(), sorted_new.end(), sorted_old.begin())) {
                        changed = true;
                    }
                }

                if (changed) {
                    whitelistNfcUids = newWhitelist;
                    updated = true;
                    Serial.printf("[API NFC] Lista NFC actualizada desde API. %d UIDs cargados.\n", whitelistNfcUids.size());
                } else {
                    Serial.println("[API NFC] Lista NFC sin cambios desde la API.");
                }
            } 
            // Verificar si existe el formato anterior "uids"
            else if (jsonDoc.containsKey("uids") && jsonDoc["uids"].is<JsonArray>()) {
                JsonArray uids = jsonDoc["uids"].as<JsonArray>();
                Serial.print("[API NFC] JSON parseado correctamente. Encontrado array 'uids' con ");
                Serial.print(uids.size());
                Serial.println(" elementos.");

                std::vector<String> newWhitelist;
                newWhitelist.reserve(uids.size());

                for (JsonVariant uidVar : uids) {
                    if (uidVar.is<const char*>()) {
                        String uidStr = uidVar.as<String>();
                        uidStr.toUpperCase();
                        newWhitelist.push_back(uidStr);
                    } else {
                        Serial.println("[API NFC] Advertencia: Elemento no string encontrado en el array 'uids'.");
                    }
                }

                bool changed = false;
                if (newWhitelist.size() != whitelistNfcUids.size()) {
                    changed = true;
                } else {
                    std::vector<String> sorted_new = newWhitelist;
                    std::vector<String> sorted_old = whitelistNfcUids;
                    std::sort(sorted_new.begin(), sorted_new.end());
                    std::sort(sorted_old.begin(), sorted_old.end());
                    if (!std::equal(sorted_new.begin(), sorted_new.end(), sorted_old.begin())) {
                        changed = true;
                    }
                }

                if (changed) {
                    whitelistNfcUids = newWhitelist;
                    updated = true;
                    Serial.printf("[API NFC] Lista NFC actualizada desde API. %d UIDs cargados.\n", whitelistNfcUids.size());
                } else {
                    Serial.println("[API NFC] Lista NFC sin cambios desde la API.");
                }
            } else {
                Serial.println("[API NFC] Error: Ni 'cards' ni 'uids' encontrados en la respuesta JSON.");
            }
        } else {
            Serial.print(F("[API NFC] Error al parsear JSON NFC: "));
            Serial.println(error.c_str());
        }
    } else {
        Serial.printf("[API NFC] Error al obtener UIDs NFC, código HTTP: %d\n", httpCode);
        if (httpCode < 0) {
            Serial.printf("[API NFC] Error de conexión: %s\n", http.errorToString(httpCode).c_str());
            // Intentar un ping simple al servidor para diagnóstico
            http.end();
            String pingUrl = String(API_BASE_URL) + "/api/ping";
            http.begin(pingUrl);
            int pingCode = http.GET();
            if (pingCode > 0) {
                Serial.printf("[API NFC] Servidor accesible con ping, código: %d\n", pingCode);
            } else {
                Serial.println("[API NFC] Servidor no accesible. Verificar:");
                Serial.println(" - Si el servidor está ejecutándose");
                Serial.println(" - Si la IP y puerto son correctos");
                Serial.println(" - Si hay un firewall bloqueando la conexión");
            }
        }
        String payload = http.getString();
        http.end();
        Serial.println("[API NFC] Respuesta error: " + payload);
    }

    if (http.connected()) {
        http.end();
    }
    return updated;
}

// Función fetchBtMacs eliminada

// --- Enviar Nuevo UID NFC a la API ---
bool sendNfcUidToApi(const String& uid) {
    if (!wifiConnected) {
         Serial.println("[API POST NFC] No hay conexión WiFi.");
         return false;
    }

    // Cambiado para usar el nuevo endpoint específico para ESP32-CAM sin autenticación
    String url = String(API_BASE_URL) + "/api/esp32/cards";
    String payload;

    DynamicJsonDocument jsonDoc(JSON_DOC_SIZE_SMALL);
    jsonDoc["uid"] = uid;
    jsonDoc["name"] = "Card " + uid.substring(0, 6); // Añadimos un nombre por defecto
    serializeJson(jsonDoc, payload);

    // Usar http normal (no clientSecure) para conexiones HTTP
    http.begin(url);
    http.addHeader("Content-Type", "application/json");

    Serial.print("[API POST NFC] Enviando nuevo UID '");
    Serial.print(uid);
    Serial.print("' a API: ");
    Serial.println(url);
    Serial.print("[API POST NFC] Payload: ");
    Serial.println(payload);

    int httpCode = http.POST(payload);
    bool success = false;

    if (httpCode > 0) {
        Serial.printf("[API POST NFC] POST UID completado, código de respuesta: %d\n", httpCode);
        String response = http.getString();
        http.end();
        Serial.println("[API POST NFC] Respuesta API: " + response);
        if (httpCode == HTTP_CODE_CREATED || httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_CONFLICT) {
            success = true;
             Serial.println("[API POST NFC] Petición POST considerada exitosa.");
             
            if (httpCode == HTTP_CODE_CONFLICT) {
                Serial.println("[API POST NFC] Nota: La tarjeta ya existía en el sistema.");
            }
        } else {
             Serial.println("[API POST NFC] Petición POST fallida (código HTTP no esperado).");
        }
    } else {
        Serial.printf("[API POST NFC] Error en POST UID, código de error HTTP: %s\n", http.errorToString(httpCode).c_str());
        Serial.println("[API POST NFC] Intentando verificar si el servidor está accesible...");
        http.end();
        
        // Intentar un ping simple al servidor para diagnóstico
        String pingUrl = String(API_BASE_URL) + "/api/ping";
        http.begin(pingUrl);
        int pingCode = http.GET();
        if (pingCode > 0) {
            Serial.printf("[API POST NFC] Servidor accesible con ping, código: %d\n", pingCode);
        } else {
            Serial.println("[API POST NFC] Servidor no accesible. Verifica:");
            Serial.println(" - Si el servidor está ejecutándose");
            Serial.println(" - Si la IP y puerto son correctos");
            Serial.println(" - Si hay un firewall bloqueando la conexión");
        }
        http.end();
    }

    if (http.connected()) {
        http.end();
    }
    return success;
}

// --- Enviar Lista NFC Actualizada al Arduino ---
void sendNfcUpdateToArduino() {
    String command = "NFC_UPDATE:";

    if (!whitelistNfcUids.empty()) {
        for (size_t i = 0; i < whitelistNfcUids.size(); ++i) {
            // Formatear cada UID con separadores de dos puntos para cada byte
            String uid = whitelistNfcUids[i];
            String formattedUid = "";
            for (size_t j = 0; j < uid.length(); j += 2) {
                formattedUid += uid.substring(j, j+2);
                if (j < uid.length() - 2) {
                    formattedUid += ":";
                }
            }
            command += formattedUid;
            if (i < whitelistNfcUids.size() - 1) {
                command += ";";
            }
        }
    }

    ArduinoSerial.println(command);
    Serial.print("[ARDUINO TX] Enviando lista NFC actualizada: ");
    if (whitelistNfcUids.empty()) {
         Serial.println("(lista vacía)");
    } else {
         Serial.println(command);
    }
}



// --- Enviar solo mensaje de texto por Telegram (sin foto) ---
bool sendMessageTelegram(const String& message) {
    if (!wifiConnected) {
        Serial.println("[TELEGRAM_MSG] No hay conexión WiFi para enviar mensaje.");
        return false;
    }

    Serial.print("[TELEGRAM_MSG] Enviando mensaje: ");
    Serial.println(message);

    const char* myDomain = "api.telegram.org";
    String getUrl = "/bot" + String(TELEGRAM_BOT_TOKEN) + "/sendMessage?chat_id=" + TELEGRAM_CHAT_ID + "&text=" + urlEncode(message);
    String getAll = "";
    String getBody = "";

    if (telegramClientSecure.connect(myDomain, 443)) {
        Serial.println("[TELEGRAM_MSG] Conectado a api.telegram.org");
        
        telegramClientSecure.println("GET " + getUrl + " HTTP/1.1");
        telegramClientSecure.println("Host: " + String(myDomain));
        telegramClientSecure.println("Connection: close");
        telegramClientSecure.println();
        
        int waitTime = 10000;   // 10 segundos de espera
        long startTime = millis();
        boolean state = false;
        
        while ((startTime + waitTime) > millis()) {
            Serial.print(".");
            delay(100);      
            while (telegramClientSecure.available()) {
                char c = telegramClientSecure.read();
                if (state==true) getBody += String(c);
                if (c == '\n') {
                    if (getAll.length()==0) state=true; 
                    getAll = "";
                } else if (c != '\r') {
                    getAll += String(c);
                }
                startTime = millis();
            }
            if (getBody.length()>0) break;
        }
        telegramClientSecure.stop();
        
        Serial.println();
        
        if (getBody.indexOf("\"ok\":true") > 0) {
            Serial.println("[TELEGRAM_MSG] Mensaje enviado correctamente a Telegram ✅");
            return true;
        } else {
            Serial.println("[TELEGRAM_MSG] Error al enviar mensaje a Telegram ❌");
            Serial.println(getBody);
            return false;
        }
    } else {
        Serial.println("[TELEGRAM_MSG] Error: Fallo al conectar con api.telegram.org");
        return false;
    }
}

// Función auxiliar para codificar URL
String urlEncode(const String& text) {
    String encodedText = "";
    char c;
    char code0;
    char code1;
    for (int i = 0; i < text.length(); i++) {
        c = text.charAt(i);
        if (c == ' ') {
            encodedText += '+';
        } else if (isAlphaNumeric(c)) {
            encodedText += c;
        } else {
            code1 = (c & 0xf) + '0';
            if ((c & 0xf) > 9) {
                code1 = (c & 0xf) - 10 + 'A';
            }
            c = (c >> 4) & 0xf;
            code0 = c + '0';
            if (c > 9) {
                code0 = c - 10 + 'A';
            }
            encodedText += '%';
            encodedText += code0;
            encodedText += code1;
        }
    }
    return encodedText;
}

// --- Enviar log de huella dactilar al backend ---
bool sendFingerprintLogToBackend(int fingerprintId) {
    if (!wifiConnected) {
        Serial.println("[API FP LOG] No hay conexión WiFi para enviar log de huella.");
        return false;
    }

    Serial.print("[API FP LOG] Enviando log de acceso con huella ID ");
    Serial.println(fingerprintId);

    String url = String(API_BASE_URL) + "/api/security-logs";
    String payload;

    DynamicJsonDocument jsonDoc(JSON_DOC_SIZE_SMALL);
    jsonDoc["type"] = "access_granted";
    jsonDoc["description"] = "Access granted with fingerprint ID: " + String(fingerprintId);
    jsonDoc["deviceId"] = WiFi.macAddress();
    // No need to send timestamp - server will generate it
    jsonDoc["photoFilename"] = nullptr;
    
    serializeJson(jsonDoc, payload);

    http.begin(url);
    http.addHeader("Content-Type", "application/json");

    int httpCode = http.POST(payload);
    bool success = false;

    if (httpCode > 0) {
        Serial.printf("[API FP LOG] POST completado, código de respuesta: %d\n", httpCode);
        String response = http.getString();
        http.end();
        
        if (httpCode == HTTP_CODE_CREATED || httpCode == HTTP_CODE_OK) {
            success = true;
            Serial.println("[API FP LOG] Log de acceso enviado correctamente.");
        } else {
            Serial.println("[API FP LOG] Error al enviar log de acceso (código HTTP no esperado).");
            Serial.println("[API FP LOG] Respuesta: " + response);
        }
    } else {
        Serial.printf("[API FP LOG] Error en POST, código de error HTTP: %s\n", http.errorToString(httpCode).c_str());
        http.end();
    }

    if (http.connected()) {
        http.end();
    }
    return success;
}

// --- Enviar log de movimiento detectado al backend ---
bool sendMotionDetectedLog() {    if (!wifiConnected) {
        Serial.println("[API MOTION LOG] No hay conexión WiFi para enviar log de movimiento.");
        return false;
    }

    String mac = WiFi.macAddress();
    Serial.print("[API MOTION LOG] Enviando log de movimiento detectado al backend con deviceId (MAC): ");
    Serial.println(mac);

    String url = String(API_BASE_URL) + "/api/security-logs";
    String payload;    DynamicJsonDocument jsonDoc(JSON_DOC_SIZE_SMALL);    jsonDoc["type"] = "motion_detected";
    jsonDoc["description"] = "Movimiento detectado por sensor PIR";
    jsonDoc["deviceId"] = WiFi.macAddress(); // Usar directamente la MAC en lugar de deviceId
    // No need to send timestamp - server will generate it
    
    serializeJson(jsonDoc, payload);

    http.begin(url);
    http.addHeader("Content-Type", "application/json");

    int httpCode = http.POST(payload);
    bool success = false;

    if (httpCode > 0) {
        Serial.printf("[API MOTION LOG] POST completado, código de respuesta: %d\n", httpCode);
        String response = http.getString();
        http.end();
        
        if (httpCode == HTTP_CODE_CREATED || httpCode == HTTP_CODE_OK) {
            success = true;
            Serial.println("[API MOTION LOG] Log de movimiento enviado correctamente.");
        } else {
            Serial.println("[API MOTION LOG] Error al enviar log de movimiento (código HTTP no esperado).");
            Serial.println("[API MOTION LOG] Respuesta: " + response);
        }
    } else {
        Serial.printf("[API MOTION LOG] Error en POST, código de error HTTP: %s\n", http.errorToString(httpCode).c_str());
        http.end();
    }

    if (http.connected()) {
        http.end();
    }
    return success;
}

// --- Tomar foto y enviar a Telegram y backend simultáneamente ---
void takePhotoAndSendAlerts(const String& message) {
    if (!wifiConnected) {
        Serial.println("[ALERT] No hay conexión WiFi para enviar alertas.");
        return;
    }

    // Añadir un retraso antes de tomar la foto
    Serial.println("[ALERT] Esperando 2 segundos antes de tomar la foto...");
    delay(2000);  // Esperar 2 segundos para dar tiempo a que la persona esté en el cuadro
    
    Serial.println("[ALERT] Limpiando buffer de la cámara...");
    
    // Limpiar completamente el buffer de la cámara antes de tomar la foto
    camera_fb_t* fb_flush;
    for (int i = 0; i < 3; i++) {  // Capturar y descartar varios frames para limpiar la caché
        fb_flush = esp_camera_fb_get();
        if (fb_flush) {
            esp_camera_fb_return(fb_flush);
            delay(50);
        }
    }

    Serial.println("[ALERT] Tomando foto nueva para alertas de seguridad...");
    
    camera_fb_t * fb = NULL;
    fb = esp_camera_fb_get();
    if (!fb) {
        Serial.println("[ALERT] Error: Fallo al capturar el frame de la cámara.");
        return;
    }

    Serial.printf("[ALERT] Foto capturada: %u bytes, %ux%u\n", fb->len, fb->width, fb->height);
    
    if (fb->len == 0 || fb->format != PIXFORMAT_JPEG) {
        Serial.println("[ALERT] Error: Formato de imagen incorrecto o tamaño cero.");
        esp_camera_fb_return(fb);
        return;
    }
    
    // 1. Enviar a Telegram
    bool telegramSent = false;
    {
        const char* telegramDomain = "api.telegram.org";
        
        if (telegramClientSecure.connect(telegramDomain, 443)) {
            Serial.println("[ALERT] Enviando foto a Telegram...");
            
            String boundary = "ESP32CAM_" + String(millis());
            String telegramHead = "--" + boundary + "\r\n";
            telegramHead += "Content-Disposition: form-data; name=\"chat_id\"\r\n\r\n";
            telegramHead += TELEGRAM_CHAT_ID;
            telegramHead += "\r\n";
            
            if (message.length() > 0) {
                telegramHead += "--" + boundary + "\r\n";
                telegramHead += "Content-Disposition: form-data; name=\"caption\"\r\n\r\n";
                telegramHead += message;
                telegramHead += "\r\n";
            }
            
            telegramHead += "--" + boundary + "\r\n";
            telegramHead += "Content-Disposition: form-data; name=\"photo\"; filename=\"esp32-cam.jpg\"\r\n";
            telegramHead += "Content-Type: image/jpeg\r\n\r\n";
            
            String telegramTail = "\r\n--" + boundary + "--\r\n";
            
            uint32_t totalLen = fb->len + telegramHead.length() + telegramTail.length();
            
            telegramClientSecure.println("POST /bot" + String(TELEGRAM_BOT_TOKEN) + "/sendPhoto HTTP/1.1");
            telegramClientSecure.println("Host: " + String(telegramDomain));
            telegramClientSecure.println("Content-Length: " + String(totalLen));
            telegramClientSecure.println("Content-Type: multipart/form-data; boundary=" + boundary);
            telegramClientSecure.println("Connection: close");  // Cambiado a close
            telegramClientSecure.println();
            telegramClientSecure.print(telegramHead);
            
            // Enviar imagen en bloques
            uint8_t *fbBuf = fb->buf;
            size_t fbLen = fb->len;
            for (size_t n=0; n<fbLen; n=n+1024) {
                if (n+1024 < fbLen) {
                    telegramClientSecure.write(fbBuf, 1024);
                    fbBuf += 1024;
                } else {
                    size_t remainder = fbLen - n;
                    telegramClientSecure.write(fbBuf, remainder);
                }
            }
            
            telegramClientSecure.print(telegramTail);
            
            // Esperar respuesta
            String response = "";
            long startTime = millis();
            while (millis() - startTime < 10000) {
                if (telegramClientSecure.available()) {
                    char c = telegramClientSecure.read();
                    response += c;
                    if (response.indexOf("\r\n\r\n") != -1 && response.indexOf("\"ok\":true") != -1) {
                        telegramSent = true;
                        break;
                    }
                }
                delay(10);
            }
            
            telegramClientSecure.stop();
            Serial.println(telegramSent ? "[ALERT] Foto enviada correctamente a Telegram" : 
                                        "[ALERT] Error al enviar foto a Telegram");
        } else {
            Serial.println("[ALERT] No se pudo conectar con Telegram");
        }
    }
    
    // Garantizar que la conexión previa se ha cerrado completamente
    telegramClientSecure.stop();
    delay(200); // Aumentar pausa para asegurar que se liberen recursos
    
    // 2. Enviar al backend - Implementación simplificada
    bool backendSent = false;
    {
        String url = String(API_BASE_URL) + "/api/security-logs/upload-photo";
        Serial.println("[ALERT] Enviando foto y log al backend...");
        
        // Crear un nuevo cliente WiFi y pasar como parámetro a HTTPClient
        WiFiClient wifiClient;
        HTTPClient httpClient;
        httpClient.begin(wifiClient, url);
        
        String boundary = "ESP32CAM_" + String(millis());
        httpClient.addHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
        
        // Construir el cuerpo de la solicitud
        String bodyStart = "--" + boundary + "\r\n";
        bodyStart += "Content-Disposition: form-data; name=\"type\"\r\n\r\n";
        bodyStart += "access_denied\r\n";
        
        bodyStart += "--" + boundary + "\r\n";
        bodyStart += "Content-Disposition: form-data; name=\"description\"\r\n\r\n";
        bodyStart += "Intento de acceso no autorizado mediante huella digital\r\n";
        
        bodyStart += "--" + boundary + "\r\n";
        bodyStart += "Content-Disposition: form-data; name=\"deviceId\"\r\n\r\n";
        bodyStart += WiFi.macAddress() + "\r\n";
        
        bodyStart += "--" + boundary + "\r\n";
        bodyStart += "Content-Disposition: form-data; name=\"photo\"; filename=\"fingerprint_fail.jpg\"\r\n";
        bodyStart += "Content-Type: image/jpeg\r\n\r\n";
        
        String bodyEnd = "\r\n--" + boundary + "--\r\n";
          // Construir un buffer de memoria para el cuerpo completo
        uint8_t* buffer = (uint8_t*)malloc(bodyStart.length() + fb->len + bodyEnd.length());
        if (buffer) {
            uint32_t pos = 0;
            
            // Copiar la parte inicial
            memcpy(buffer, bodyStart.c_str(), bodyStart.length());
            pos += bodyStart.length();
            
            // Copiar la imagen
            memcpy(buffer + pos, fb->buf, fb->len);
            pos += fb->len;
            
            // Copiar la parte final
            memcpy(buffer + pos, bodyEnd.c_str(), bodyEnd.length());
            pos += bodyEnd.length();
            
            // Enviar todo de una vez
            int httpCode = httpClient.POST(buffer, pos);
            Serial.printf("[ALERT] Código de respuesta HTTP: %d\n", httpCode);
            
            free(buffer);
            
            if (httpCode == HTTP_CODE_CREATED || httpCode == HTTP_CODE_OK) {
                backendSent = true;
                String response = httpClient.getString();
                Serial.println("[ALERT] Log de seguridad con foto enviado correctamente al backend.");
                Serial.println("[ALERT] Respuesta: " + response);
            } else {
                Serial.printf("[ALERT] Error al enviar log de seguridad: código HTTP %d\n", httpCode);
                String response = httpClient.getString();
                Serial.println("[ALERT] Respuesta error: " + response);
            }
        } else {
            Serial.println("[ALERT] Error: No se pudo asignar memoria para el buffer");
        }
        
        // Cerrar la conexión
        httpClient.end();
    }
    
    // Liberar el frame buffer
    esp_camera_fb_return(fb);
    
    Serial.println("[ALERT] Proceso de alerta completado.");
}