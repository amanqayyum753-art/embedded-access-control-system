// ============================================================
//   EMBEDDED ACCESS CONTROL SYSTEM
//   Author  : AMAN QAYYUM (EE Student, COMSATS University Lahore)
//   Version : 2.0  |  All 4 Phases Complete
//   Hardware: Arduino UNO + 4x4 Keypad + I2C LCD (16x2)
//             + SG90 Servo + Green/Red LED + Active Buzzer
// ============================================================
//
//  WIRING GUIDE
//  ---------------------------------------------------------
//  Keypad  R1-R4 -> D9, D8, D7, D6   (rows)
//          C1-C4 -> D5, D4, D3, D2   (cols)
//  Servo   Signal -> D10
//  Green LED (+)  -> D11  (220 Ohm to GND)
//  Red   LED (+)  -> D12  (220 Ohm to GND)
//  Buzzer  (+)    -> D13  (active buzzer)
//  LCD     SDA    -> A4
//          SCL    -> A5
//          VCC    -> 5V  |  GND -> GND
//  ---------------------------------------------------------
//
//  KEY MAP
//  ---------------------------------------------------------
//  '#'  -> ENTER / CONFIRM
//  '*'  -> BACKSPACE / CANCEL
//  'D'  -> Enter Change-Password Mode
//  ---------------------------------------------------------
//
//  DEFAULT PASSWORDS
//  ---------------------------------------------------------
//  Normal   : 1234
//  Master   : 9999  (always works; cannot be changed)
//  ---------------------------------------------------------
//
//  LIBRARIES NEEDED  (install via Arduino Library Manager)
//  ---------------------------------------------------------
//  * Keypad             by Mark Stanley
//  * LiquidCrystal_I2C  by Frank de Brabander
//  * Servo              (built-in)
//  ---------------------------------------------------------

// --- Library Includes -------------------------------------------
#include <Keypad.h>
#include <LiquidCrystal_I2C.h>
#include <Servo.h>
#include <EEPROM.h>

// --- Pin Definitions --------------------------------------------
#define PIN_SERVO      10
#define PIN_LED_GREEN  11
#define PIN_LED_RED    12
#define PIN_BUZZER     13

// --- Servo Angles -----------------------------------------------
#define SERVO_LOCKED    0
#define SERVO_UNLOCKED  90

// --- Timing Constants (ms) --------------------------------------
#define AUTO_LOCK_DELAY     5000UL
#define LOCKOUT_DURATION   30000UL
#define BEEP_SHORT           80UL
#define BEEP_LONG           600UL
#define LED_BLINK_PERIOD    300UL
#define BLINK_COUNT           6

// --- Security Constants -----------------------------------------
#define MAX_PWD_LEN         8
#define MAX_WRONG_ATTEMPTS  3
#define EEPROM_ADDR_FLAG    0
#define EEPROM_ADDR_PWD     1
#define EEPROM_VALID_FLAG   0xAA
const char MASTER_PASSWORD[] = "9999";

// --- Keypad Setup -----------------------------------------------
const byte ROWS = 4;
const byte COLS = 4;
char keys[ROWS][COLS] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};
byte rowPins[ROWS] = {9, 8, 7, 6};
byte colPins[COLS] = {5, 4, 3, 2};
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

// --- LCD & Servo Objects ----------------------------------------
LiquidCrystal_I2C lcd(0x27, 16, 2);
Servo doorServo;

// --- System State Enum ------------------------------------------
enum SystemState {
  STATE_IDLE,
  STATE_UNLOCKED,
  STATE_LOCKED_OUT,
  STATE_CHANGE_PWD
};

// --- Change-Password Sub-state ----------------------------------
enum ChangePwdStep {
  CPW_VERIFY_OLD,
  CPW_ENTER_NEW,
  CPW_CONFIRM_NEW
};

// --- Global Variables -------------------------------------------
char currentPassword[MAX_PWD_LEN + 1];
char inputBuffer[MAX_PWD_LEN + 1];
byte inputLen        = 0;
byte wrongAttempts   = 0;

SystemState   sysState = STATE_IDLE;
ChangePwdStep cpwStep  = CPW_VERIFY_OLD;

unsigned long unlockStartTime  = 0;
unsigned long lockoutStartTime = 0;
unsigned long lastSecondTick   = 0;

char cpwNewFirst[MAX_PWD_LEN + 1];

// --- Helper: safe string copy -----------------------------------
static inline void safeCopy(char *dst, const char *src) {
  strncpy(dst, src, MAX_PWD_LEN);
  dst[MAX_PWD_LEN] = '\0';
}

// ================================================================
//  BUZZER MODULE
// ================================================================
void beepShort() {
  tone(PIN_BUZZER, 1000);   // 1kHz sound
  delay(BEEP_SHORT);
  noTone(PIN_BUZZER);
}

void beepLong() {
  tone(PIN_BUZZER, 1000);
  delay(BEEP_LONG);
  noTone(PIN_BUZZER);
}

void beepDouble() {
  beepShort();
  delay(120);
  beepShort();
}

void beepTick() {
  tone(PIN_BUZZER, 2000);   // slightly higher pitch tick
  delay(40);
  noTone(PIN_BUZZER);
}

// ================================================================
//  LCD MODULE
// ================================================================
void lcdPrint(const char *line1, const char *line2 = "") {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(line1);
  lcd.setCursor(0, 1);
  lcd.print(line2);
}

void updateLcdInput() {
  lcd.setCursor(0, 1);
  for (byte i = 0; i < inputLen; i++) lcd.print('*');
  for (byte i = inputLen; i < MAX_PWD_LEN; i++) lcd.print(' ');
}

void showAccessGranted() {
  lcdPrint("  Access Granted", "  Door Unlocked ");
}

void showAccessDenied(byte attemptsLeft) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(" Access Denied! ");
  lcd.setCursor(0, 1);
  if (attemptsLeft > 0) {
    lcd.print("Attempts Left:");
    lcd.print(attemptsLeft);
    lcd.print(" ");
  } else {
    lcd.print(" System Locked! ");
  }
}

void showDoorLocked() {
  lcdPrint("  Door  Locked  ", "  Enter  Code:");
}

void showSystemLocked(int secondsLeft) {
  lcd.setCursor(0, 0);
  lcd.print(" System Locked! ");
  lcd.setCursor(0, 1);
  lcd.print("Wait: ");
  if (secondsLeft < 10) lcd.print('0');
  lcd.print(secondsLeft);
  lcd.print("s          ");
}

void showIdlePrompt() {
  lcdPrint("  Access Control", "  Enter Code:   ");
}==============================
void enterChangePwdMode() {
  sysState = STATE_CHANGE_PWD;
  cpwStep  = CPW_VERIFY_OLD;
  inputLen = 0;
  memset(inputBuffer, 0, sizeof(inputBuffer));
  Serial.println(F("[CHANGE-PWD] Entered change-password mode."));
  lcdPrint("Change Password ", "Old Password:   ");
}

// ================================================================
//  SERVO MODULE
// ================================================================
void unlockDoor() {
  int current = doorServo.read();
  for (int pos = current; pos <= SERVO_UNLOCKED; pos++) {
    doorServo.write(pos);
    delay(15);
  }
  Serial.println(F("[SERVO] Door UNLOCKED"));
}

void lockDoor() {
  int current = doorServo.read();
  for (int pos = current; pos >= SERVO_LOCKED; pos--) {
    doorServo.write(pos);
    delay(15);
  }
  Serial.println(F("[SERVO] Door LOCKED"));
}

// ================================================================
//  LED MODULE
// ================================================================
void greenLedOn()  { digitalWrite(PIN_LED_GREEN, HIGH); }
void greenLedOff() { digitalWrite(PIN_LED_GREEN, LOW);  }
void redLedOn()    { digitalWrite(PIN_LED_RED,   HIGH); }
void redLedOff()   { digitalWrite(PIN_LED_RED,   LOW);  }

void blinkRedLed(byte times) {
  for (byte i = 0; i < times; i++) {
    redLedOn();
    delay(LED_BLINK_PERIOD);
    redLedOff();
    delay(LED_BLINK_PERIOD);
  }
}

// ================================================================
//  EEPROM MODULE
// ================================================================
void savePasswordToEEPROM(const char *pwd) {
  EEPROM.write(EEPROM_ADDR_FLAG, EEPROM_VALID_FLAG);
  for (byte i = 0; i <= strlen(pwd); i++) {
    EEPROM.write(EEPROM_ADDR_PWD + i, pwd[i]);
  }
  Serial.println(F("[EEPROM] Password saved."));
}

void loadPasswordFromEEPROM() {
  if (EEPROM.read(EEPROM_ADDR_FLAG) == EEPROM_VALID_FLAG) {
    for (byte i = 0; i < MAX_PWD_LEN; i++) {
      currentPassword[i] = (char)EEPROM.read(EEPROM_ADDR_PWD + i);
      if (currentPassword[i] == '\0') break;
    }
    currentPassword[MAX_PWD_LEN] = '\0';
    Serial.print(F("[EEPROM] Password loaded: "));
    Serial.println(currentPassword);
  } else {
    safeCopy(currentPassword, "1234");
    savePasswordToEEPROM(currentPassword);
    Serial.println(F("[EEPROM] Default password set: 1234"));
  }
}

// ================================================================
//  PASSWORD VERIFICATION
// ================================================================
bool verifyPassword(const char *input) {
  if (strcmp(input, MASTER_PASSWORD) == 0) {
    Serial.println(F("[AUTH] Master password used."));
    return true;
  }
  return (strcmp(input, currentPassword) == 0);
}

// ================================================================
//  UNLOCK / LOCK LOGIC
// ================================================================
void handleUnlock() {
  Serial.println(F("[EVENT] Password CORRECT - Door Unlocking"));
  showAccessGranted();
  greenLedOn();
  beepDouble();
  unlockDoor();
  wrongAttempts   = 0;
  sysState        = STATE_UNLOCKED;
  unlockStartTime = millis();
}

void handleLock() {
  Serial.println(F("[EVENT] Auto-lock triggered - Door Locking"));
  lockDoor();
  greenLedOff();
  sysState = STATE_IDLE;
  showIdlePrompt();
}

// ================================================================
//  WRONG PASSWORD HANDLER
// ================================================================
void handleWrongPassword() {
  wrongAttempts++;
  byte attemptsLeft = MAX_WRONG_ATTEMPTS - wrongAttempts;

  Serial.print(F("[EVENT] Wrong password. Attempts left: "));
  Serial.println(attemptsLeft);

  if (wrongAttempts >= MAX_WRONG_ATTEMPTS) {
    // --- LOCKOUT ---
    Serial.println(F("[LOCKOUT] System locked for 30 seconds."));
    sysState         = STATE_LOCKED_OUT;
    lockoutStartTime = millis();
    lastSecondTick   = millis();
    showAccessDenied(0);
    blinkRedLed(3);
    beepLong();
    delay(300);
    beepLong();
    showSystemLocked(30);
  } else {
    showAccessDenied(attemptsLeft);
    blinkRedLed(BLINK_COUNT);
    beepLong();
    showIdlePrompt();
  }
}

// ================================================================
//  LOCKOUT UPDATE (called from loop)
// ================================================================
void updateLockout() {
  unsigned long elapsed = millis() - lockoutStartTime;
  int secondsLeft = (int)((LOCKOUT_DURATION - elapsed) / 1000UL) + 1;
  if (secondsLeft < 0) secondsLeft = 0;

  if (millis() - lastSecondTick >= 1000UL) {
    lastSecondTick = millis();
    showSystemLocked(secondsLeft);
    beepTick();
  }

  if (elapsed >= LOCKOUT_DURATION) {
    Serial.println(F("[LOCKOUT] Lockout ended. System reset."));
    wrongAttempts = 0;
    sysState      = STATE_IDLE;
    showIdlePrompt();
  }
}

// ================================================================
//  CHANGE PASSWORD MODULE
// ==================================

void handleChangePwdKey(char key) {
  if (key == '*') {
    sysState = STATE_IDLE;
    inputLen = 0;
    Serial.println(F("[CHANGE-PWD] Cancelled."));
    showIdlePrompt();
    return;
  }

  if (key == '#') {
    inputBuffer[inputLen] = '\0';

    switch (cpwStep) {

      case CPW_VERIFY_OLD:
        if (verifyPassword(inputBuffer)) {
          Serial.println(F("[CHANGE-PWD] Old password verified."));
          cpwStep  = CPW_ENTER_NEW;
          inputLen = 0;
          memset(inputBuffer, 0, sizeof(inputBuffer));
          lcdPrint("Change Password ", "New Password:   ");
        } else {
          Serial.println(F("[CHANGE-PWD] Old password wrong."));
          lcdPrint("Wrong Password! ", "Press # to retry");
          beepLong();
          delay(1500);
          inputLen = 0;
          memset(inputBuffer, 0, sizeof(inputBuffer));
          lcdPrint("Change Password ", "Old Password:   ");
        }
        break;

      case CPW_ENTER_NEW:
        if (inputLen < 1) {
          lcdPrint("Too Short!      ", "Min 1 character ");
          delay(1500);
          lcdPrint("Change Password ", "New Password:   ");
          return;
        }
        safeCopy(cpwNewFirst, inputBuffer);
        cpwStep  = CPW_CONFIRM_NEW;
        inputLen = 0;
        memset(inputBuffer, 0, sizeof(inputBuffer));
        Serial.println(F("[CHANGE-PWD] New pwd entered, awaiting confirm."));
        lcdPrint("Change Password ", "Confirm New Pwd:");
        break;

      case CPW_CONFIRM_NEW:
        if (strcmp(inputBuffer, cpwNewFirst) == 0) {
          safeCopy(currentPassword, cpwNewFirst);
          savePasswordToEEPROM(currentPassword);
          Serial.print(F("[CHANGE-PWD] Password updated: "));
          Serial.println(currentPassword);
          lcdPrint("Password Updated", "    Success!    ");
          beepDouble();
          delay(2000);
          sysState = STATE_IDLE;
          inputLen = 0;
          showIdlePrompt();
        } else {
          Serial.println(F("[CHANGE-PWD] Confirm mismatch."));
          lcdPrint("Mismatch! Retry ", "New Password:   ");
          beepLong();
          delay(1500);
          cpwStep  = CPW_ENTER_NEW;
          inputLen = 0;
          memset(inputBuffer, 0, sizeof(inputBuffer));
          lcdPrint("Change Password ", "New Password:   ");
        }
        break;
    }
    return;
  }

  // Regular key - append to buffer
  if (inputLen < MAX_PWD_LEN) {
    inputBuffer[inputLen++] = key;
    beepShort();
    // Show asterisks on LCD
    lcd.setCursor(0, 1);
    for (byte i = 0; i < inputLen; i++) lcd.print('*');
    for (byte i = inputLen; i < 16; i++) lcd.print(' ');
  }
}

// ================================================================
//  IDLE KEY HANDLER
// ================================================================
void handleIdleKey(char key) {

  if (key == 'D') {
    enterChangePwdMode();
    return;
  }

  if (key == '#') {
    inputBuffer[inputLen] = '\0';
    if (inputLen == 0) return;

    if (verifyPassword(inputBuffer)) {
      handleUnlock();
    } else {
      handleWrongPassword();
    }

    inputLen = 0;
    memset(inputBuffer, 0, sizeof(inputBuffer));
    return;
  }

  if (key == '*') {
    if (inputLen > 0) {
      inputLen--;
      inputBuffer[inputLen] = '\0';
      Serial.println(F("[KEY] Backspace"));
      updateLcdInput();
    }
    return;
  }

  if (inputLen < MAX_PWD_LEN) {
    inputBuffer[inputLen++] = key;
    beepShort();
    Serial.print(F("[KEY] "));
    Serial.println(key);
    updateLcdInput();
  }
}

// ================================================================
//  SETUP
// ================================================================
void setup() {
  Serial.begin(9600);
  Serial.println(F("===================================="));
  Serial.println(F("  Embedded Access Control System   "));
  Serial.println(F("  EE Student - COMSATS Lahore      "));
  Serial.println(F("===================================="));

  pinMode(PIN_LED_GREEN, OUTPUT);
  pinMode(PIN_LED_RED,   OUTPUT);
  pinMode(PIN_BUZZER,    OUTPUT);
  digitalWrite(PIN_LED_GREEN, LOW);
  digitalWrite(PIN_LED_RED,   LOW);
  digitalWrite(PIN_BUZZER,    LOW);

  doorServo.attach(PIN_SERVO);
  doorServo.write(SERVO_LOCKED);

  lcd.init();
  lcd.backlight();

  loadPasswordFromEEPROM();

  lcdPrint(" Access Control ", "   System  v2.0 ");
  beepShort();
  delay(1500);
  lcdPrint("  Initializing  ", "  Please wait...");
  delay(1000);

  showIdlePrompt();
  Serial.println(F("[SYSTEM] Ready. Enter password on keypad."));
  Serial.print  (F("[SYSTEM] Active password length: "));
  Serial.println(strlen(currentPassword));
}

// ================================================================
//  LOOP  (non-blocking with millis())
// ================================================================
void loop() {

  if (sysState == STATE_UNLOCKED) {
    if (millis() - unlockStartTime >= AUTO_LOCK_DELAY) {
      handleLock();
    }
    return;
  }

  if (sysState == STATE_LOCKED_OUT) {
    updateLockout();
    return;
  }

  char key = keypad.getKey();
  if (!key) return;

  if (sysState == STATE_CHANGE_PWD) {
    handleChangePwdKey(key);
  } else {
    handleIdleKey(key);
  }
}
// -- END OF FILE --------------------------
