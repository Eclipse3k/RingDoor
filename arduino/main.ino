#include <Adafruit_Fingerprint.h>
#include <Wire.h>
#include <Servo.h>
#include <SoftwareSerial.h>
#include <SPI.h>
#include <MFRC522.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#define UID_SIZE 4
#define OLED_LINE_BUFFER_SIZE 25
#define SERVO_PIN 4
#define FINGER_RX_PIN 2
#define FINGER_TX_PIN 3
#define NFC_SS_PIN 10
#define NFC_RST_PIN 9
#define PIR_PIN 6
#define BUZZER_PIN 7
#define ESP_RX_PIN 8
#define ESP_TX_PIN 5
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 32
#define OLED_RESET -1
#define SCREEN_ADDRESS 0x3C
#define ICON_WIDTH 16
#define ICON_HEIGHT 16
#define TEXT_X_OFFSET (ICON_WIDTH + 4)
#define LINE_HEIGHT 8
#define MAX_AUTHORIZED 10
#define MAX_FAILED_ATTEMPTS 3
#define MOTION_DEBOUNCE_MS 5000
#define NFC_AUTH_TIMEOUT 5000
#define HUELLA_TIMEOUT 10000

// --- OBJETOS GLOBALES ---
Servo servo;
SoftwareSerial fingerSerial(FINGER_RX_PIN, FINGER_TX_PIN);
SoftwareSerial espSerial(ESP_RX_PIN, ESP_TX_PIN);
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&fingerSerial);
MFRC522 mfrc522(NFC_SS_PIN, NFC_RST_PIN);
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

static bool puertaAbierta = false;
static byte authorizedCards[MAX_AUTHORIZED][UID_SIZE];
static byte numAuthorizedCards = 0;
static uint8_t consecutiveFailedAttempts = 0;
static bool pirState = LOW;
static bool lastPirState = LOW;
static unsigned long lastMotionTime = 0;
static bool buzzerOn = false;
static unsigned long buzzerStopTime = 0;
static char oledLine1[OLED_LINE_BUFFER_SIZE];
static char oledLine2[OLED_LINE_BUFFER_SIZE];
static bool registrandoHuella = false;

void displayState(const char *line1, const char *line2 = nullptr, uint16_t delay_ms = 0);
void displayState(const __FlashStringHelper *line1, const __FlashStringHelper *line2 = nullptr, uint16_t delay_ms = 0);
void displayState(const __FlashStringHelper *line1, const char *line2, uint16_t delay_ms = 0);
void displayState(const char *line1, const __FlashStringHelper *line2, uint16_t delay_ms = 0);
void abrirCerradura();
void cerrarCerradura();
void registrarHuellas(int numHuellas);
bool capturarImagen(int id, const char* msgBuf, int numImagen);
bool crearYAlmacenarModelo(int id, int idx, int numHuellas);
bool isAuthorized(byte *uid, byte uidSize);
void addAuthorized(byte *uid, byte uidSize);
void handleSerialCommands();
void handleNFCAuthorization(byte *uid, byte uidSize);
void activateBuzzer(unsigned long duration = 0);
void deactivateBuzzer();
void checkPIR();
void updateNfcUidList(String uidListStr);

void setup() {
    Serial.begin(115200);
    espSerial.begin(9600);
    Serial.println(F("Inicializando..."));
    Wire.begin();
    pinMode(PIR_PIN, INPUT);
    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, LOW);

    // Inicializar OLED
    if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
        Serial.println(F("Error OLED"));
        while (true);
    }
    display.clearDisplay();
    display.display();

    // Inicializar sensor de huellas
    fingerSerial.begin(57600);
    
    if (finger.verifyPassword()) {
        uint8_t p = finger.getTemplateCount();
        if (p == FINGERPRINT_OK) {
            snprintf(oledLine2, OLED_LINE_BUFFER_SIZE, "%d Huellas", finger.templateCount);
        }
    } else {
        Serial.println(F("Error huella"));
        while (true) delay(10);
    }    
    
    // Inicializar NFC
    SPI.begin();
    pinMode(NFC_RST_PIN, OUTPUT);
    digitalWrite(NFC_RST_PIN, LOW);
    delay(50);
    digitalWrite(NFC_RST_PIN, HIGH);
    delay(100);
    mfrc522.PCD_Init();
    mfrc522.PCD_SetAntennaGain(0x07<<4);
    
    // Mostrar mensaje inicial
    displayState(F("Esperando"), F("interaccion..."));
}

// --- LOOP PRINCIPAL ---
void loop() {
    handleSerialCommands();
    checkPIR();

    if (buzzerOn && buzzerStopTime > 0 && millis() >= buzzerStopTime) {
        deactivateBuzzer();
    }

    // Detección de tarjetas NFC
    bool cardDetected = false;
    for (byte retry = 0; retry < 3 && !cardDetected; retry++) {
        if ((mfrc522.PICC_IsNewCardPresent() || mfrc522.PICC_IsNewCardPresent()) &&
            mfrc522.PICC_ReadCardSerial()) {
            
            cardDetected = true;
            consecutiveFailedAttempts = 0;
            
            if (isAuthorized(mfrc522.uid.uidByte, mfrc522.uid.size)) {
                displayState(F("Tarjeta NFC"), F("autorizada"), 2000);
                registrarHuellas(2);
            } else {
                Serial.println(F("Tarjeta NO autorizada. Esperando autorización serial... ❌"));
                Serial.println(F("Por favor, introduzca 'n' o 'N' en el monitor serial para autorizar la tarjeta."));
                displayState(F("Tarjeta NFC"), F("no autorizada"), 2000);
                handleNFCAuthorization(mfrc522.uid.uidByte, mfrc522.uid.size);
            }

            mfrc522.PICC_HaltA();
            mfrc522.PCD_StopCrypto1();
            delay(500);
            break;
        }
        
        if (!cardDetected && retry > 0) 
            delay(50);
    }
    
    // Detección de huellas
    if (!puertaAbierta && !registrandoHuella) {
        uint8_t p = finger.getImage();
        if (p == FINGERPRINT_OK) {
            p = finger.image2Tz();
            if (p == FINGERPRINT_OK) {
                p = finger.fingerFastSearch();
                if (p == FINGERPRINT_OK) {
                    consecutiveFailedAttempts = 0;
                    displayState(F("Huella OK"), F(""), 1500);
                    
                    // Enviar notificación al ESP32 de acceso autorizado con ID de huella
                    char mensaje[30];
                    snprintf(mensaje, sizeof(mensaje), "ACCESS_GUARANTEED:%d", finger.fingerID);
                    espSerial.println(mensaje);
                    
                    abrirCerradura();
                    puertaAbierta = true;
                }
                else if (p == FINGERPRINT_NOTFOUND) {
                    consecutiveFailedAttempts++;
                    char bufferIntento[20];
                    snprintf(bufferIntento, sizeof(bufferIntento), "intento %d/3", consecutiveFailedAttempts);
                    displayState(F("Huella Invalida"), bufferIntento, 2000);
                   
                    if (consecutiveFailedAttempts >= MAX_FAILED_ATTEMPTS) {
                        displayState(F("Intruso"), F("detectado"), 3000);
                        espSerial.println("PHOTO_REQUEST_FP_FAIL");
                        activateBuzzer(5000);
                        consecutiveFailedAttempts = 0;
                    }
                }
            }
        }
    }

    // Gestión de puerta abierta
    if (puertaAbierta) {
        delay(3000);
        cerrarCerradura();
        puertaAbierta = false;
    }
    
    // Mostrar mensaje de espera si no hay actividad
    static unsigned long lastDisplayRefresh = 0;
    if (!registrandoHuella && !puertaAbierta && !cardDetected && 
        consecutiveFailedAttempts == 0 && millis() - lastDisplayRefresh > 30000) {
        displayState(F("Esperando"), F("interaccion..."));
        lastDisplayRefresh = millis();
    }
    
    delay(50);
}

// --- Manejador de Comandos Seriales ---
void handleSerialCommands() {
    if (espSerial.available() <= 0) return;
    
    String command = espSerial.readStringUntil('\n');
    command.trim();
    
    if (command.equals("BUZZER_ON")) {
        activateBuzzer(10000);
    } 
    else if (command.equals("BUZZER_OFF")) {
        deactivateBuzzer();
    }
    else if (command.startsWith("NFC_UPDATE:")) {
        updateNfcUidList(command.substring(11));
    }
}

// Actualización de UIDs NFC
void updateNfcUidList(String uidListStr) {
    numAuthorizedCards = 0;
    byte receivedUID[UID_SIZE];
    int startPos = 0, endPos;
    
    while (startPos < uidListStr.length() && numAuthorizedCards < MAX_AUTHORIZED) {
        endPos = uidListStr.indexOf(';', startPos);
        if (endPos < 0) endPos = uidListStr.length();
        
        if (endPos > startPos) {
            String uidStr = uidListStr.substring(startPos, endPos);
            memset(receivedUID, 0, UID_SIZE);
            
            int byteIndex = 0, prevIndex = 0, colonIndex;
            
            while (byteIndex < UID_SIZE && 
                  (colonIndex = uidStr.indexOf(':', prevIndex)) >= 0) {
                receivedUID[byteIndex++] = strtoul(uidStr.substring(prevIndex, colonIndex).c_str(), NULL, 16);
                prevIndex = colonIndex + 1;
            }
            
            if (prevIndex < uidStr.length() && byteIndex < UID_SIZE)
                receivedUID[byteIndex++] = strtoul(uidStr.substring(prevIndex).c_str(), NULL, 16);
            
            if (byteIndex == UID_SIZE) {
                memcpy(authorizedCards[numAuthorizedCards], receivedUID, UID_SIZE);
                numAuthorizedCards++;
            }
        }
        startPos = endPos + 1;
    }
    
    displayState(F("Lista NFC"), F("actualizada"), 2000);
}

// Autorización NFC por Serial
void handleNFCAuthorization(byte *uid, byte uidSize) {
    Serial.println(F("Iniciando autorización NFC por Serial... ⏳"));
    displayState(F("Esperando"), F("autorizacion"));
    
    for (unsigned long startTime = millis(); millis() - startTime < NFC_AUTH_TIMEOUT;) {
        if (Serial.available() > 0) {
            char input = Serial.read();
            Serial.print(F("Entrada Serial Recibida: "));
            Serial.println(input);
            
            if (input == 'n' || input == 'N') {
                addAuthorized(uid, uidSize);
                Serial.println(F("Tarjeta NFC autorizada mediante serial. ✅"));
                displayState(F("Tarjeta NFC"), F("autorizada"), 2000);
                return;
            }
        }
        delay(50);
    }
    Serial.println(F("Tiempo de espera para autorización serial agotado. ❌"));
    Serial.println(F("Tarjeta NFC no autorizada. ❌"));
    displayState(F("Tarjeta NFC"), F("no autorizada"), 2000);
}

// Verificar movimiento PIR
void checkPIR() {
    pirState = digitalRead(PIR_PIN);
    if (pirState == HIGH && lastPirState == LOW) {
        unsigned long now = millis();
        if (now - lastMotionTime > MOTION_DEBOUNCE_MS) {
            espSerial.println("MOTION_DETECTED");
            Serial.println(F("Movimiento detectado. Enviando alerta a ESP32. 📸"));
            lastMotionTime = now;
        }
    }
    lastPirState = pirState;
}

// Control del Buzzer
void activateBuzzer(unsigned long duration) {
    digitalWrite(BUZZER_PIN, HIGH);
    buzzerOn = true;
    buzzerStopTime = (duration > 0) ? (millis() + duration) : 0;
}

void deactivateBuzzer() {
    digitalWrite(BUZZER_PIN, LOW);
    buzzerOn = false;
    buzzerStopTime = 0;
}

// Control del servo para cerradura
void abrirCerradura() {
    servo.attach(SERVO_PIN, 1000, 2000);
    servo.write(100);
    delay(1200);
    servo.write(90);
}

void cerrarCerradura() {
    servo.write(80);
    delay(500);
    servo.write(90);
    servo.detach();
    displayState(F("Esperando"), F("interaccion..."));
}

// Validación de tarjetas NFC
bool isAuthorized(byte *uid, byte uidSize) {
    if (uidSize != UID_SIZE) return false;
    
    for (byte i = 0; i < numAuthorizedCards; i++) {
        if (memcmp(uid, authorizedCards[i], UID_SIZE) == 0) return true;
    }
    
    return false;
}

void addAuthorized(byte *uid, byte uidSize) {
    if (uidSize != UID_SIZE || numAuthorizedCards >= MAX_AUTHORIZED) return;
    
    // Verificar si ya existe
    for (byte i = 0; i < numAuthorizedCards; i++) {
        if (memcmp(uid, authorizedCards[i], UID_SIZE) == 0) return;
    }
    
    // Añadir tarjeta
    memcpy(authorizedCards[numAuthorizedCards], uid, UID_SIZE);
    numAuthorizedCards++;
    
    // Enviar UID al ESP32
    espSerial.print("NFC_UID_AUTH:");
    for (byte i = 0; i < uidSize; i++) {
        if (i > 0) espSerial.print(":");
        if (uid[i] < 0x10) espSerial.print("0");
        espSerial.print(uid[i], HEX);
    }
    espSerial.println();
}

// Funciones de visualización OLED
void displayState(const char *line1, const char *line2, uint16_t delay_ms) {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    
    int16_t x1, y1;
    uint16_t w, h;

    if (line1 && *line1) {
        display.getTextBounds(line1, 0, 0, &x1, &y1, &w, &h);
        display.setCursor((SCREEN_WIDTH - w) / 2, line2 ? 8 : 12);
        display.println(line1);
    }

    if (line2 && *line2) {
        display.getTextBounds(line2, 0, 0, &x1, &y1, &w, &h);
        display.setCursor((SCREEN_WIDTH - w) / 2, 18);
        display.println(line2);
    }

    display.display();
    if (delay_ms) delay(delay_ms);
}

// Sobrecarga para manejar F() strings
void displayState(const __FlashStringHelper *line1, const __FlashStringHelper *line2, uint16_t delay_ms) {
    strcpy_P(oledLine1, (const char *)line1);
    strcpy_P(oledLine2, (const char *)line2);
    displayState(oledLine1, oledLine2, delay_ms);
}

// Sobrecarga para manejar F() como primer parámetro y char* como segundo
void displayState(const __FlashStringHelper *line1, const char *line2, uint16_t delay_ms) {
    strcpy_P(oledLine1, (const char *)line1);
    displayState(oledLine1, line2, delay_ms);
}

// Sobrecarga para manejar char* como primer parámetro y F() como segundo
void displayState(const char *line1, const __FlashStringHelper *line2, uint16_t delay_ms) {
    strcpy_P(oledLine2, (const char *)line2);
    displayState(line1, oledLine2, delay_ms);
}

// Registro de huellas dactilares
void registrarHuellas(int numHuellas) {
    registrandoHuella = true;
    
    // Comprobar capacidad
    if (finger.getTemplateCount() != FINGERPRINT_OK) {
        displayState(F("Error"), F("sensor"), 2000);
        registrandoHuella = false;
        return;
    }
    
    uint16_t currentCount = finger.templateCount;

    if (currentCount + numHuellas > finger.capacity) {
        displayState(F("Sensor"), F("lleno"), 2000);
        registrandoHuella = false;
        return;
    }
    
    displayState(F("Iniciando"), F("registro"), 1500);

    // Procesar cada huella
    for (int i = 0; i < numHuellas; i++) {
        int id = currentCount + 1 + i;
        char msgBuf[16];
        snprintf(msgBuf, sizeof(msgBuf), "Huella %d/%d", i+1, numHuellas);
        
        // Captura primera imagen
        if (!capturarImagen(id, msgBuf, 1)) {
            if (i == 0 && numHuellas == 2) {
                displayState(F("Pasando a"), F("segunda huella"), 2000);
                continue;
            }
            registrandoHuella = false;
            return;
        }
        
        // Esperar que retire el dedo
        displayState(msgBuf, F("Retire dedo"));
        delay(1000);
        while (finger.getImage() != FINGERPRINT_NOFINGER) delay(10);
        
        // Captura segunda imagen
        if (!capturarImagen(id, msgBuf, 2)) {
            registrandoHuella = false;
            return;
        }
        
        // Crear y almacenar modelo
        if (!crearYAlmacenarModelo(id, i, numHuellas)) {
            registrandoHuella = false;
            return;
        }
        
        // Enviar notificación de registro al ESP32-CAM
        espSerial.print("REGISTER:");
        espSerial.print(id);
        espSerial.print(";");
        
        // Añadir el UID de la tarjeta NFC en formato hexadecimal
        for (byte j = 0; j < mfrc522.uid.size; j++) {
            if (mfrc522.uid.uidByte[j] < 0x10) espSerial.print("0");
            espSerial.print(mfrc522.uid.uidByte[j], HEX);
        }
        espSerial.println();
    }
    
    registrandoHuella = false;
    displayState(F("Registro"), F("completado"), 3000);
    displayState(F("Esperando"), F("interaccion..."));
}

bool capturarImagen(int id, const char* msgBuf, int numImagen) {
    uint8_t p;
    
    // Mostrar mensaje en pantalla
    displayState(msgBuf, numImagen == 1 ? F("Coloque dedo") : F("Coloque nuevo"));

    // Loop con tiempo límite
    for (unsigned long inicio = millis(); millis() - inicio < HUELLA_TIMEOUT;) {
        p = finger.getImage();
        
        if (p == FINGERPRINT_OK) {
            displayState(msgBuf, F("Imagen tomada"), 1500);
            
            // Convertir imagen directamente
            if ((p = finger.image2Tz(numImagen)) != FINGERPRINT_OK) {
                displayState(F("Error"), F("conversion"), 2000);
                return false;
            }
            
            return true;
        }
        else if (p != FINGERPRINT_NOFINGER) {
            delay(200);
        }
        delay(50);
    }
    
    // Si llegamos aquí es timeout
    displayState(F("Tiempo agotado"), F("Intente nuevo"), 3000);
    return false;
}

bool crearYAlmacenarModelo(int id, int idx, int numHuellas) {
    uint8_t p;
    
    // Crear modelo
    if ((p = finger.createModel()) != FINGERPRINT_OK) {
        displayState(
            p == FINGERPRINT_ENROLLMISMATCH ? F("Huellas dist.") : F("Error"), 
            p == FINGERPRINT_ENROLLMISMATCH ? F("Reintente") : F("modelo"), 
            2000);
        return false;
    }
    
    // Verificar duplicados
    if ((p = finger.fingerSearch()) == FINGERPRINT_OK) {
        displayState(F("Huella ya"), F("existente"), 2000);
        return false;
    }
    
    // Guardar modelo
    if ((p = finger.storeModel(id)) != FINGERPRINT_OK) {
        displayState(F("Error"), F("al guardar"), 2000);
        return false;
    }
    
    char msgBuf[12];
    snprintf(msgBuf, sizeof(msgBuf), "Huella %d", idx+1);
    displayState(msgBuf, F("guardada"), 1500);
    
    return true;
}