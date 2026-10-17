#define BLYNK_TEMPLATE_NAME "..."
#define BLYNK_AUTH_TOKEN "..."
#define BLYNK_TEMPLATE_ID "..."
const char* URL_SHEET = "..."

#include <WiFi.h>
#include <WiFiClient.h>
#include <BlynkSimpleEsp32.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <DHT.h>
#include <HTTPClient.h>

// --- CREDENZIALI WI-FI ---
char ssid[] = "...";
char pass[] = "...";

// --- CONFIGURAZIONE PIN ---
const int PIN_POMPA = 25;    // D25
const int PIN_TERRENO = 32;  // D32 (Umidità terreno)
const int LED_PIN = 13;   // D13
const int DHTPIN = 4;    // D4 (Sensore DHT22 per Aria)

// --- IMPOSTAZIONI SENSORI ---
#define DHTTYPE DHT22
DHT dht(DHTPIN, DHTTYPE);

#define SCREEN_WIDTH 128     // Larghezza OLED
#define SCREEN_HEIGHT 64     // Altezza OLED
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

enum StatoPompa { IDLE, POMPA_ACCESA, ATTESA_PROPAGAZIONE };
StatoPompa stato = IDLE;
unsigned long timestampStato = 0;

// --- CALIBRAZIONE E SOGLIE ---
const int VALORE_SECCO   = 3332; 
const int VALORE_BAGNATO = 1200;
const int SOGLIA_CRITICA = 30;  
const int SOGLIA_TARGET  = 65;  

const unsigned long TEMPO_POMPA = 2000;       
const unsigned long TEMPO_PROPAGAZIONE = 5000;

// --- VARIABILI FILTRO ---
float umiditaFiltrata = -1.0; 
float tempFiltrata = -1.0;
float umidAriaFiltrata = -1.0;
float umiditaTerrenoPct = 0;
const float ALPHA = 0.15; // il filtro pesa poco il nuovo dato e molto la storia passata perchè misura l'umidità che varia lentamente
                          // è più lento ma più smooth, 
                          // elimina meglio il rumore ma introduce più ritardo nel reagire a cambiamenti, ma questo è accettabile per questo tipo di sistema

BlynkTimer timer;

// --- INVIA DATI IN CLOUD ---
void inviaDatiCloud(float u_terr, float temp, float um_aria) {
  if (WiFi.status() != WL_CONNECTED) return;  // niente Wi-Fi, salta

  HTTPClient http;
  String url = String(URL_SHEET) + "?terreno=" + String(u_terr, 1) +
               "&temp=" + String(temp, 1) + "&umaria=" + String(um_aria, 1);

  http.begin(url);
  int httpCode = http.GET();
  
  if (httpCode > 0) {
    Serial.println("-> Dati inviati al cloud (Google Sheets)");
  } else {
    Serial.println("-> Errore invio dati cloud");
  }
  http.end();
}

void salvaStoricoCloud() {
  inviaDatiCloud(umiditaTerrenoPct, tempFiltrata, umidAriaFiltrata);
}


// --- FILTRO BUTTERWORTH PER IL TERRENO ---
float leggiUmiditaTerreno() {
  long somma = 0;
  for (int i = 0; i < 15; i++) {
    somma += analogRead(PIN_TERRENO);
    delay(5); 
  }
  float letturaGrezza = (float)somma / 15.0;
  
  if (umiditaFiltrata == -1.0) {
    umiditaFiltrata = letturaGrezza;
  } else {
    umiditaFiltrata = (ALPHA * letturaGrezza) + (1.0 - ALPHA) * umiditaFiltrata;
  }
  
  float pct = (float)(umiditaFiltrata - VALORE_SECCO) / (float)(VALORE_BAGNATO - VALORE_SECCO) * 100.0;
  return constrain(pct, 0.0, 100.0);
}


// --- FACCINA ---
void disegnaFaccina(int x, int y, float u_terreno) {
  // Occhi (ridotti proporzionalmente)
  display.fillCircle(x - 5, y - 3, 1, SSD1306_WHITE);
  display.fillCircle(x + 5, y - 3, 1, SSD1306_WHITE);

  if (u_terreno >= SOGLIA_TARGET) {
    // FELICE: bocca a sorriso
    display.drawLine(x - 4, y + 3, x, y + 6, SSD1306_WHITE);
    display.drawLine(x, y + 6, x + 4, y + 3, SSD1306_WHITE);
  } 
  else if (u_terreno >= SOGLIA_CRITICA) {
    // NEUTRA: bocca dritta
    display.drawLine(x - 4, y + 4, x + 4, y + 4, SSD1306_WHITE);
  } 
  else {
    // TRISTE: bocca a smorfia
    display.drawLine(x - 4, y + 6, x, y + 3, SSD1306_WHITE);
    display.drawLine(x, y + 3, x + 4, y + 6, SSD1306_WHITE);
  }

  // Contorno faccia (raggio 8)
  display.drawCircle(x, y, 8, SSD1306_WHITE);
}

// --- FUNZIONE AGGIORNAMENTO SCHERMO OLED ---
void aggiornaOLED(float u_terr, float temp, float um_aria) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  
  display.setCursor(0, 0);
  display.println("--- SMART PLANT ---");
  
  display.setCursor(0, 20);
  display.print("Umid. Terr: "); display.print(u_terr, 1); display.println("%");
  
  display.setCursor(0, 36);
  display.print("Temp. Aria: "); display.print(temp, 1); display.println(" C");
  
  display.setCursor(0, 52);
  display.print("Umid. Aria: "); display.print(um_aria, 1); display.println("%");
  
  disegnaFaccina(112, 40, u_terr);
  
  display.display();
}

void aggiornaStatoPompa(float u_terreno) {
  unsigned long ora = millis();

  switch (stato) {
    case IDLE:
      if (u_terreno < SOGLIA_CRITICA) {
        digitalWrite(PIN_POMPA, HIGH);
        digitalWrite(LED_PIN, HIGH);
        Blynk.virtualWrite(V2, 1);
        timestampStato = ora;
        stato = POMPA_ACCESA;
        Serial.println("-> Stato: POMPA_ACCESA");
      }
      break;

    case POMPA_ACCESA:
      if (ora - timestampStato >= TEMPO_POMPA) {
        digitalWrite(PIN_POMPA, LOW);
        digitalWrite(LED_PIN, LOW);
        Blynk.virtualWrite(V2, 0);
        timestampStato = ora;
        stato = ATTESA_PROPAGAZIONE;
        Serial.println("-> Stato: ATTESA_PROPAGAZIONE");
      }
      break;

    case ATTESA_PROPAGAZIONE:
      if (ora - timestampStato >= TEMPO_PROPAGAZIONE) {
        stato = IDLE;
        Serial.println("-> Stato: IDLE");
      }
      break;
  }
}

float applicaFiltro(float nuovoValore, float &valoreFiltrato) {
  if (valoreFiltrato == -1.0) {
    valoreFiltrato = nuovoValore;
  } else {
    valoreFiltrato = (ALPHA * nuovoValore) + (1.0 - ALPHA) * valoreFiltrato;
  }
  return valoreFiltrato;
}

// --- FUNZIONE CONTROLLO PERIODICA (Ogni 2 secondi) ---
void controllaEInviaDati() {
  // Lettura sensori
  float u_terreno = leggiUmiditaTerreno();
  float t_aria_raw = dht.readTemperature();
  float u_aria_raw = dht.readHumidity(); 

  if (isnan(t_aria_raw) || isnan(u_aria_raw)) {
    Serial.println("Errore di lettura dal sensore DHT!");
    // non aggiorniamo il filtro con un dato invalido, usiamo l'ultimo valore filtrato
  } else {
    applicaFiltro(t_aria_raw, tempFiltrata);
    applicaFiltro(u_aria_raw, umidAriaFiltrata);
  }

  float t_aria = tempFiltrata;
  float u_aria = umidAriaFiltrata;

  // Stampa Seriale di Debug
  Serial.print("Terr: "); Serial.print(u_terreno);
  Serial.print("% | Temp: "); Serial.print(t_aria);
  Serial.print("C | Umid Aria: "); Serial.print(u_aria); Serial.println("%");

  // Invio dati a Blynk (V1, V3, V4)
  Blynk.virtualWrite(V1, u_terreno);
  Blynk.virtualWrite(V3, t_aria);
  Blynk.virtualWrite(V4, u_aria);

  // Aggiorna lo schermo fisico
  aggiornaOLED(u_terreno, t_aria, u_aria);

  // Logica irrigazione a stati
  aggiornaStatoPompa(u_terreno);
  umiditaTerrenoPct=u_terreno;
}

void setup() {
  digitalWrite(PIN_POMPA, LOW); 
  digitalWrite(LED_PIN, LOW);
  pinMode(PIN_POMPA, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  
  
  Serial.begin(9600);
  delay(1000);
  
  // Inizializzazione schermo OLED (Indirizzo I2C standard 0x3C)
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) { 
    Serial.println(F("Errore di allocazione SSD1306 (Schermo non trovato)"));
  } else {
    display.clearDisplay();
    display.display();
  }
  
  // Inizializzazione DHT
  dht.begin();
  
  Serial.println("Connessione a Blynk...");
  Blynk.begin(BLYNK_AUTH_TOKEN, ssid, pass);
  
  // Eseguiamo il controllo ogni 2 secondi (tempo minimo per il DHT22)
  timer.setInterval(2000L, controllaEInviaDati);
  Serial.println("--- CENTRALINA ONLINE ---");
  timer.setInterval(3000L, salvaStoricoCloud);  // ogni 5 minuti (300000 ms)
}

void loop() {
  Blynk.run();
  timer.run();
}