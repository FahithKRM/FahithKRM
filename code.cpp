#include <Keypad.h>
#include <LiquidCrystal_I2C.h>
#include <Servo.h>

// Define pins for door lock system
#define SERVO_PIN 3           // Servo motor pin
#define DOOR_RED_LED 5        // Red LED for incorrect password
#define DOOR_GREEN_LED 6      // Green LED for access granted

// Define pins for smoke detector
#define GAS_SENSOR A0
#define BUZZER 4
#define SMOKE_THRESHOLD 600   // Threshold for smoke detection

// Define pins for automatic light system
#define PIR1_PIN A3           // PIR1 for entry (outside)
#define PIR2_PIN A2           // PIR2 for exit/inside
#define LIGHT_PIN A1          // Output for room light
#define SEQUENCE_TIMEOUT 15000UL  // Increased max time between PIR triggers for a valid sequence (ms) to allow password entry
#define PIR_TIMEOUT 20000UL      // Increased reset individual PIR trigger after no activity (ms)
#define DEBOUNCE 2000UL          // Min time between occupancy count changes (ms)

Servo myservo;                // Servo object
LiquidCrystal_I2C lcd(0x27, 16, 2); // I2C LCD object

// Keypad configuration
const byte rows = 4, cols = 4;
char keys[rows][cols] = {
  {'1', '2', '3', 'A'},
  {'4', '5', '6', 'B'},
  {'7', '8', '9', 'C'},
  {'*', '0', '#', 'D'}
};
byte rowPins[rows] = {7, 8, 9, 10}; // Row pins
byte colPins[cols] = {11, 12, 13, 2}; // Column pins
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, rows, cols);

// Passwords
char masterPassword[] = "1234";  // Master password for changing user password
char savedPassword[5] = "0000";  // Default user password
char enteredPassword[5];         // Buffer for entered password
int currentPosition = 0;         // Tracks entered digits
int failedAttempts = 0;          // Tracks failed attempts

// Flags for modes
bool isSettingPassword = false;
bool isCheckingMaster = false;

// Door unlock variables (non-blocking)
bool doorUnlocked = false;
unsigned long unlockStartTime = 0;
int unlockSecondsLeft = 10;

// Light control and occupancy variables
bool lightState = false;
int occupancy = 0;

// PIR directional counting variables
bool pir1Triggered = false;
bool pir2Triggered = false;
unsigned long pir1Time = 0;
unsigned long pir2Time = 0;
unsigned long lastCountTime = 0;
bool prevPIR1 = false;
bool prevPIR2 = false;
bool exitSequenceStarted = false; // Flag to track if exit was initiated (PIR2 while locked)

void setup() {
  Serial.begin(9600);  // Start Serial Monitor
  lcd.init();
  lcd.backlight();
  pinMode(DOOR_RED_LED, OUTPUT);
  pinMode(DOOR_GREEN_LED, OUTPUT);
  myservo.attach(SERVO_PIN);
  myservo.write(0);  // Locked position

  // Smoke pins
  pinMode(GAS_SENSOR, INPUT);
  pinMode(BUZZER, OUTPUT);

  // Light and sensor pins
  pinMode(PIR1_PIN, INPUT);
  pinMode(PIR2_PIN, INPUT);
  pinMode(LIGHT_PIN, OUTPUT);
  digitalWrite(LIGHT_PIN, LOW);  // Ensure light is off at startup

  // Initialize previous states
  prevPIR1 = digitalRead(PIR1_PIN);
  prevPIR2 = digitalRead(PIR2_PIN);

  // Initial display
  lcd.print("Smart Room Ready");
  delay(2000);
  updateLcdMainScreen();
}

void loop() {
  // Handle smoke detection
  int gas_value = analogRead(GAS_SENSOR);
  if (gas_value > SMOKE_THRESHOLD) {
    tone(BUZZER, 1000, 500);
  } else {
    noTone(BUZZER);
  }

  // Handle non-blocking door unlock and countdown
  if (doorUnlocked) {
    unsigned long currentTime = millis();
    int remaining = (10000 - (currentTime - unlockStartTime)) / 1000;
    if (remaining <= 0) {
      lockDoor();
      exitSequenceStarted = false; // Reset on lock if not completed
    } else if (remaining != unlockSecondsLeft) {
      unlockSecondsLeft = remaining;
      lcd.setCursor(0, 1);
      lcd.print("Time: ");
      lcd.print(unlockSecondsLeft);
      lcd.print(" sec      ");
    }
  }

  // Handle keypad
  char key = keypad.getKey();
  if (key) {
    if (doorUnlocked && key == 'D') {
      lockDoor();
      exitSequenceStarted = false; // Reset on manual lock
    } else if (isSettingPassword) {
      handlePasswordSetup(key);
    } else if (isCheckingMaster) {
      handleMasterPasswordCheck(key);
    } else {
      handlePasswordEntry(key);
    }
  }

  // Handle occupancy counting with PIR sequence (edge detection)
  unsigned long currentTime = millis();
  int currPIR1 = digitalRead(PIR1_PIN);
  int currPIR2 = digitalRead(PIR2_PIN);

  // Rising edge detection for PIR1 (only if door unlocked)
  if (currPIR1 == HIGH && prevPIR1 == LOW && doorUnlocked && !pir1Triggered) {
    pir1Triggered = true;
    pir1Time = currentTime;
    Serial.println("PIR1 triggered (potential entry or exit)");
  }

  // Rising edge detection for PIR2 (always)
  if (currPIR2 == HIGH && prevPIR2 == LOW && !pir2Triggered) {
    pir2Triggered = true;
    pir2Time = currentTime;
    if (!doorUnlocked) {
      exitSequenceStarted = true;
      Serial.println("PIR2 triggered (exit initiated while locked)");
    } else {
      Serial.println("PIR2 triggered (potential entry)");
    }
  }

  prevPIR1 = currPIR1;
  prevPIR2 = currPIR2;

  // Timeout for individual PIR triggers
  if (pir1Triggered && (currentTime - pir1Time > PIR_TIMEOUT)) {
    pir1Triggered = false;
    pir1Time = 0;
    Serial.println("PIR1 timeout reset");
  }
  if (pir2Triggered && (currentTime - pir2Time > PIR_TIMEOUT)) {
    pir2Triggered = false;
    pir2Time = 0;
    exitSequenceStarted = false;
    Serial.println("PIR2 timeout reset");
  }

  // Check for complete sequence if both triggered and debounce time passed
  if (pir1Triggered && pir2Triggered && (currentTime - lastCountTime > DEBOUNCE)) {
    long timeDiff = (long)(pir2Time - pir1Time);
    if (timeDiff > 0 && labs(timeDiff) <= SEQUENCE_TIMEOUT && !exitSequenceStarted) {  // PIR1 first then PIR2, not exit: Entry
      occupancy++;
      Serial.println("Entry detected: PIR1 before PIR2, no exit flag");
      updateDisplay();
      resetPIRTriggers();
      exitSequenceStarted = false;
      lastCountTime = currentTime;
    } else if (timeDiff < 0 && labs(timeDiff) <= SEQUENCE_TIMEOUT && exitSequenceStarted) {  // PIR2 first then PIR1, with exit flag: Exit
      if (occupancy > 0) {
        occupancy--;
        Serial.println("Exit detected: PIR2 before PIR1, with exit flag");
      }
      updateDisplay();
      resetPIRTriggers();
      exitSequenceStarted = false;
      lastCountTime = currentTime;
    }
  }

  // Automatic light control (based solely on occupancy)
  if (occupancy > 0) {
    if (!lightState) {
      digitalWrite(LIGHT_PIN, HIGH);
      lightState = true;
      Serial.println("Light turned ON");
    }
  } else {
    if (lightState) {
      digitalWrite(LIGHT_PIN, LOW);
      lightState = false;
      Serial.println("Light turned OFF");
    }
  }

  delay(50);  // Reduced delay for responsiveness
}

// Reset PIR trigger flags
void resetPIRTriggers() {
  pir1Triggered = false;
  pir2Triggered = false;
  pir1Time = 0;
  pir2Time = 0;
}

// Update LCD and Serial Monitor with occupancy count
void updateDisplay() {
  if (!doorUnlocked && !isSettingPassword && !isCheckingMaster) {
    lcd.setCursor(0, 1);
    lcd.print("People: ");
    lcd.print(occupancy);
    lcd.print("        ");  // Clear previous digits
  }
  Serial.print("Occupancy updated: ");
  Serial.println(occupancy);
}

// Update main LCD screen (password prompt + count)
void updateLcdMainScreen() {
  lcd.clear();
  lcd.print("Enter Password:");
  updateDisplay();
}

// Function to lock the door
void lockDoor() {
  lcd.clear();
  lcd.print("Locking Door...");
  myservo.write(0);
  delay(1000);
  doorUnlocked = false;
  updateLcdMainScreen();
}

// Updated unlockDoor (non-blocking setup)
void unlockDoor() {
  lcd.clear();
  lcd.print("Access Granted");
  for (int i = 0; i < 5; i++) {
    digitalWrite(DOOR_GREEN_LED, HIGH);
    delay(200);
    digitalWrite(DOOR_GREEN_LED, LOW);
    delay(200);
  }
  delay(1000);

  myservo.write(90); // Unlock
  doorUnlocked = true;
  unlockStartTime = millis();
  unlockSecondsLeft = 10;
  lcd.clear();
  lcd.print("Press D to lock");
  lcd.setCursor(0, 1);
  lcd.print("Time: 10 sec");
  if (exitSequenceStarted) {
    Serial.println("Door unlocked during exit sequence");
  }
}

// Remaining functions unchanged
void handlePasswordSetup(char key) {
  lcd.clear();

  if (key == 'B') {
    if (currentPosition > 0) {
      currentPosition--;
      enteredPassword[currentPosition] = '\0';
    }
  } else if (key >= '0' && key <= '9' && currentPosition < 4) {
    enteredPassword[currentPosition] = key;
    currentPosition++;
  } else if (key == 'C') {
    if (currentPosition == 4) {
      enteredPassword[4] = '\0';
      strncpy(savedPassword, enteredPassword, 5);
      lcd.print("Password Set!");
      delay(2000);
      isSettingPassword = false;
      updateLcdMainScreen();
      currentPosition = 0;
      failedAttempts = 0;
    } else {
      lcd.print("Incomplete Pass");
      delay(2000);
    }
    return;
  }

  lcd.setCursor(0, 0);
  lcd.print("Set New Pass:");
  lcd.setCursor(0, 1);
  for (int i = 0; i < currentPosition; i++) {
    lcd.print(enteredPassword[i]);
  }
}

void handleMasterPasswordCheck(char key) {
  lcd.clear();

  if (key == 'B') {
    if (currentPosition > 0) {
      currentPosition--;
      enteredPassword[currentPosition] = '\0';
    }
  } else if (key >= '0' && key <= '9' && currentPosition < 4) {
    enteredPassword[currentPosition] = key;
    currentPosition++;
  } else if (key == 'C') {
    if (currentPosition == 4) {
      enteredPassword[4] = '\0';
      if (strcmp(enteredPassword, masterPassword) == 0) {
        lcd.print("Master Verified");
        delay(2000);
        isCheckingMaster = false;
        isSettingPassword = true;
        currentPosition = 0;
        lcd.clear();
        lcd.print("Set Password:");
      } else {
        lcd.print("Incorrect Master");
        delay(2000);
        isCheckingMaster = false;
        currentPosition = 0;
        updateLcdMainScreen();
      }
    } else {
      lcd.print("Incomplete Pass");
      delay(2000);
    }
    return;
  }

  lcd.setCursor(0, 0);
  lcd.print("Master Pass:");
  lcd.setCursor(0, 1);
  for (int i = 0; i < currentPosition; i++) {
    lcd.print("*");
  }
}

void handlePasswordEntry(char key) {
  if (key == 'A') {
    lcd.clear();
    lcd.print("Master Pass:");
    currentPosition = 0;
    isCheckingMaster = true;
    return;
  } else if (key == 'B') {
    if (currentPosition > 0) {
      currentPosition--;
      enteredPassword[currentPosition] = '\0';
    }
  } else if (key >= '0' && key <= '9' && currentPosition < 4) {
    enteredPassword[currentPosition] = key;
    currentPosition++;
  } else if (key == 'C') {
    if (currentPosition == 4) {
      enteredPassword[4] = '\0';
      if (strcmp(enteredPassword, savedPassword) == 0) {
        unlockDoor();
        failedAttempts = 0;
      } else {
        incorrectPassword();
      }
      currentPosition = 0;
    } else {
      lcd.clear();
      lcd.print("Incomplete Pass");
      delay(2000);
      updateLcdMainScreen();
    }
    return;
  }

  lcd.clear();
  lcd.print("Enter Password:");
  lcd.setCursor(0, 1);
  for (int i = 0; i < currentPosition; i++) {
    lcd.print("*");
  }
}

void incorrectPassword() {
  failedAttempts++;
  lcd.clear();
  lcd.print("CODE INCORRECT");
  digitalWrite(DOOR_RED_LED, HIGH);

  if (failedAttempts >= 3) {
    lcd.setCursor(0, 1);
    lcd.print("Wait ");
    for (int secondsLeft = 30; secondsLeft > 0; secondsLeft--) {
      lcd.setCursor(5, 1);
      lcd.print(secondsLeft);
      lcd.print(" sec");
      delay(1000);
    }
    failedAttempts = 0;
  } else {
    delay(3000);
  }

  digitalWrite(DOOR_RED_LED, LOW);
  updateLcdMainScreen();
}
