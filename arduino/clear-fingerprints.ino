#include <Adafruit_Fingerprint.h>
#include <SoftwareSerial.h>

#define FINGER_RX_PIN 2
#define FINGER_TX_PIN 3

SoftwareSerial fingerSerial(FINGER_RX_PIN, FINGER_TX_PIN);
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&fingerSerial);

void setup() {  Serial.begin(115200);
  while (!Serial);
  
  Serial.println(F("Iniciando limpiador de huellas dactilares..."));
  
  fingerSerial.begin(57600);
  
  if (!finger.verifyPassword()) {
    Serial.println(F("Error: No se pudo conectar con el sensor de huellas"));
    while (true) delay(1000);
  }
  
  Serial.println(F("Sensor de huellas detectado!"));
    uint8_t p = finger.getTemplateCount();
  if (p == FINGERPRINT_OK) {
    Serial.print(F("Sensor contiene ")); 
    Serial.print(finger.templateCount);
    Serial.println(F(" huellas registradas"));
  } else {
    Serial.println(F("Error al leer el número de huellas"));
    return;
  }
  
  if (finger.templateCount == 0) {
    Serial.println(F("El sensor ya está vacío. No hay huellas que eliminar."));
    return;
  }
  
  Serial.println(F("\nEliminando todas las huellas..."));
  Serial.println(F("Esto tomará unos segundos"));
  
  p = finger.emptyDatabase();
  if (p == FINGERPRINT_OK) {
    Serial.println(F("¡Sensor limpiado correctamente! Todas las huellas han sido eliminadas."));
  } else {    Serial.print(F("Error al eliminar las huellas: "));
    Serial.println(p);
    
    Serial.println(F("Intentando eliminar huellas una por una..."));
    bool success = true;
    
    for (int id = 1; id <= 127; id++) {
      p = finger.deleteModel(id);
      if (p == FINGERPRINT_OK) {
        Serial.print(F("ID #")); Serial.print(id); Serial.println(F(" eliminada."));
      } else if (p == FINGERPRINT_PACKETRECIEVEERR) {
        Serial.print(F("Error de comunicación en ID #")); Serial.println(id);
        success = false;
      } else if (p == FINGERPRINT_BADLOCATION) {
      } else {
        Serial.print(F("Error desconocido en ID #")); Serial.println(id);
        success = false;
      }
      delay(100);
    }
    
    if (success) {
      Serial.println(F("Eliminación individual completada."));
    } else {
      Serial.println(F("Proceso completado con algunos errores."));
    }
  }
    p = finger.getTemplateCount();
  if (p == FINGERPRINT_OK) {
    Serial.print(F("Quedan ")); 
    Serial.print(finger.templateCount);
    Serial.println(F(" huellas en el sensor."));
  }
}

void loop() {
  delay(1000);
}