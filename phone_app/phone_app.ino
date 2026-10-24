#define SPI_DRIVER_SELECT 2
#include <Arduino.h>
#include <MCUFRIEND_kbv.h>
#include <TouchScreen.h>
#include <SdFat.h>
#include <EEPROM.h>
#include <string.h>
#include <stdlib.h>

// Mega 2560 + MAR2808/MCUFRIEND 2.8 inch shield phone UI.
// Calls use SIM800L differential audio pins. SD music uses D46 PWM through a
// low-pass/coupling network into PAM8403. Keep the two speaker outputs separate
// unless a suitable switch or dedicated audio interface is installed.

MCUFRIEND_kbv tft;

const int XP = 8, YP = A3, XM = A2, YM = 9;
TouchScreen ts(XP, YP, XM, YM, 300);
const int16_t TS_LEFT = 867, TS_RIGHT = 167, TS_TOP = 177, TS_BOTTOM = 953;
const int MIN_PRESSURE = 80, MAX_PRESSURE = 1000;

// The shield microSD slot is wired to Uno-style D10-D13. Mega needs software SPI.
const uint8_t SD_CS_PIN = 10, SD_MISO_PIN = 12, SD_MOSI_PIN = 11, SD_SCK_PIN = 13;
SoftSpiDriver<SD_MISO_PIN, SD_MOSI_PIN, SD_SCK_PIN> softSpi;
SdFs sd;
#define SD_CONFIG SdSpiConfig(SD_CS_PIN, SHARED_SPI, SD_SCK_MHZ(0), &softSpi)

const uint8_t SOUND_SENSOR_PIN = A8; // optional KY-038 sound-level meter only
const uint8_t AUDIO_PWM_PIN = 46;    // Mega OC5A
const uint8_t MUSIC_GAIN_PERCENT = 40; // conservative starting level for the listed 8 ohm / 0.5 W speaker

const uint16_t C_PAPER = 0xFFBB, C_CARD = 0xF7B8, C_NAVY = 0x10A2;
const uint16_t C_WHITE = 0xFFFF, C_INK = 0x2124, C_MUTED = 0x6B4D;
const uint16_t C_GREEN = 0x2E66, C_RED = 0xD985, C_BLUE = 0x34DF;
const uint16_t C_YELLOW = 0xFEC0, C_ORANGE = 0xFC20;

enum UiScreen : uint8_t { HOME, DIALER, CONTACTS, CONTACT_ENTRY, CONTACT_ACTION, MEDIA_MENU,
                          MESSAGES, SAVED_MESSAGES, SMS_COMPOSE, SMS_DETAIL, DELETE_CONFIRM, NETWORK_STATUS,
                          INCOMING, IN_CALL, PHOTO, MUSIC, CALL_HISTORY, TOUCH_CALIBRATION };
UiScreen screen = HOME;
UiScreen composeReturnScreen = CONTACT_ACTION;
UiScreen statusReturnScreen = HOME;

char dialNumber[22] = "";
char callNumber[22] = "";
char callStatus[24] = "Ready";
char modemLine[128];
uint8_t modemLineLength = 0;
bool modemResponding = false, modemConfigured = false;
bool simReady = false, networkRegistered = false;
int8_t registrationStatus = -1;
int16_t signalRssi = 99, signalBer = 99;
char simState[18] = "Not checked";
char operatorName[21] = "Unknown";
bool callConnected = false, touchLatched = false;
bool sdMounted = false;
uint32_t callConnectedAt = 0, lastCallClock = 0, lastCallQuery = 0, lastModemResponse = 0;
uint32_t lastNetworkQuery = 0, lastAtPing = 0, lastSoundSample = 0;
uint32_t lastNetworkScreenDraw = 0;
bool networkStatusDirty = false;
int soundBaseline = 512, soundPeak = 0;

const uint16_t TOUCH_CAL_MAGIC = 0xC41A;
struct TouchCalData { uint16_t magic; int16_t left, middle, right; };
int16_t touchRawLeft = TS_LEFT, touchRawMiddle = (TS_LEFT + TS_RIGHT) / 2, touchRawRight = TS_RIGHT;
int16_t lastRawTouchX = 0;
bool touchCalibrated = false;
uint8_t touchCalStage = 0;
int16_t touchCalSamples[3] = {0, 0, 0};
char touchCalMessage[32] = "Tap the highlighted target";

const uint8_t MAX_CALLS = 8;
struct CallRecord { char direction; char number[21]; uint32_t duration; char result[9]; };
CallRecord callRecords[MAX_CALLS];
uint8_t callRecordCount = 0;
char activeCallDirection = 0;
bool callWasConnected = false, callLogged = true;

struct Contact { char name[13]; char number[21]; };
struct SmsMessage { char sender[21]; char body[81]; int16_t storageIndex; char storage[3]; };
struct SavedDraft { char sender[21]; char body[41]; };
enum DeleteTarget : uint8_t { DELETE_NONE, DELETE_CONTACT, DELETE_INBOX_SMS, DELETE_DRAFT };
const uint8_t MAX_CONTACTS = 8, MAX_SMS = 5, CONTACTS_PER_PAGE = 5;
Contact contacts[MAX_CONTACTS];
SmsMessage smsMessages[MAX_SMS];
SavedDraft savedMessages[MAX_SMS];
uint8_t contactCount = 0, contactPage = 0, selectedContact = 0;
uint8_t smsCount = 0, selectedSms = 0;
uint8_t savedMessageCount = 0, selectedSavedMessage = 0;
DeleteTarget deleteTarget = DELETE_NONE;
bool contactForSms = false, contactNumberField = false, contactNumericLayout = false;
bool smsNumericLayout = false;
char contactNameInput[13] = "";
char contactNumberInput[21] = "";
char contactStatus[24] = "";
char smsRecipient[22] = "";
char smsComposeText[41] = "";
char smsStatus[24] = "";
char smsPendingSender[21] = "";
char smsPendingStorage[3] = "SM", smsListStorage[3] = "SM", smsDeleteStorage[3] = "SM";
int16_t smsPendingStorageIndex = -1, smsDeleteIndex = -1;
bool smsAwaitBody = false, smsAwaitPrompt = false, smsSending = false, smsInboxLoading = false;
bool smsListQueued = false, smsReadQueued = false, smsDeletePreparing = false, smsDeleteCommandPending = false;
uint32_t smsActionStarted = 0;

FsFile audioFile;
bool audioFileOpen = false, audioEof = false;
volatile uint8_t audioRing[256];
volatile uint8_t audioHead = 0, audioTail = 0;
volatile bool audioRunning = false;

void drawScreen();
void drawNetworkStatus();
void stopMusic();
void sendAT(const char *command);
void recordCall(const char *result);
void finishActiveCall(const char *result = NULL);

void loadTouchCalibration() {
  TouchCalData saved;
  EEPROM.get(0, saved);
  if (saved.magic == TOUCH_CAL_MAGIC && saved.left > saved.middle &&
      saved.middle > saved.right && saved.left <= 1023 && saved.right >= 0 &&
      saved.left - saved.middle > 20 && saved.middle - saved.right > 20) {
    touchRawLeft = saved.left;
    touchRawMiddle = saved.middle;
    touchRawRight = saved.right;
    touchCalibrated = true;
    Serial.println(F("[TOUCH] loaded 3-point calibration"));
  }
}

void saveTouchCalibration() {
  TouchCalData saved = {TOUCH_CAL_MAGIC, touchRawLeft, touchRawMiddle, touchRawRight};
  EEPROM.put(0, saved);
  touchCalibrated = true;
}

int16_t mapTouchHorizontal(int16_t raw) {
  if (!touchCalibrated) return constrain(map(raw, TS_LEFT, TS_RIGHT, 0, tft.width()), 0, tft.width() - 1);
  if (raw >= touchRawLeft)
    return constrain(map(raw, touchRawLeft + 45, touchRawLeft, 0, 15), 0, 15);
  if (raw <= touchRawRight)
    return constrain(map(raw, touchRawRight, touchRawRight - 45, 304, tft.width() - 1), 304, tft.width() - 1);
  if (raw >= touchRawMiddle) return constrain(map(raw, touchRawLeft, touchRawMiddle, 15, 160), 0, tft.width() - 1);
  return constrain(map(raw, touchRawMiddle, touchRawRight, 160, 304), 0, tft.width() - 1);
}

int16_t sampleRawTouchHorizontal() {
  int32_t total = 0;
  uint8_t count = 0;
  for (uint8_t i = 0; i < 8; ++i) {
    TSPoint p = ts.getPoint();
    pinMode(XM, OUTPUT); pinMode(YP, OUTPUT);
    if (p.z >= MIN_PRESSURE && p.z <= MAX_PRESSURE) { total += p.y; ++count; }
    delay(3);
  }
  return count ? (int16_t)(total / count) : lastRawTouchX;
}

void beginTouchCalibration() {
  touchCalStage = 0;
  touchCalMessage[0] = '\0';
  screen = TOUCH_CALIBRATION;
  drawScreen();
}

void trimField(char *s) {
  while (*s == ' ' || *s == '\t') memmove(s, s + 1, strlen(s));
  size_t n = strlen(s);
  while (n && (s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = '\0';
}

void addContactLine(char *line) {
  trimField(line);
  if (!line[0] || line[0] == '#' || contactCount >= MAX_CONTACTS) return;
  char *comma = strchr(line, ',');
  if (!comma) return;
  *comma++ = '\0';
  trimField(line); trimField(comma);
  if (!line[0] || !comma[0] || !strcmp(line, "Name")) return;
  strncpy(contacts[contactCount].name, line, sizeof(contacts[contactCount].name) - 1);
  contacts[contactCount].name[sizeof(contacts[contactCount].name) - 1] = '\0';
  strncpy(contacts[contactCount].number, comma, sizeof(contacts[contactCount].number) - 1);
  contacts[contactCount].number[sizeof(contacts[contactCount].number) - 1] = '\0';
  ++contactCount;
}

void loadContacts() {
  contactCount = 0;
  if (!sdMounted) return;
  FsFile f = sd.open("/CONTACTS.CSV", O_RDONLY);
  if (!f) { Serial.println(F("[CONTACTS] CONTACTS.CSV not found")); return; }
  char line[48];
  uint8_t length = 0;
  while (f.available()) {
    int c = f.read();
    if (c == '\r') continue;
    if (c == '\n' || length >= sizeof(line) - 1) {
      line[length] = '\0'; addContactLine(line); length = 0;
    } else line[length++] = (char)c;
  }
  if (length) { line[length] = '\0'; addContactLine(line); }
  f.close();
  Serial.print(F("[CONTACTS] loaded ")); Serial.println(contactCount);
}

void keepCallRecord(const CallRecord &record) {
  if (callRecordCount >= MAX_CALLS) {
    for (uint8_t i = 1; i < MAX_CALLS; ++i) callRecords[i - 1] = callRecords[i];
    callRecordCount = MAX_CALLS - 1;
  }
  callRecords[callRecordCount++] = record;
}

void parseCallHistoryLine(char *line) {
  char *comma1 = strchr(line, ',');
  char *comma2 = comma1 ? strchr(comma1 + 1, ',') : NULL;
  char *comma3 = comma2 ? strchr(comma2 + 1, ',') : NULL;
  if (!comma1 || !comma2 || !comma3) return;
  *comma1 = *comma2 = *comma3 = '\0';
  CallRecord record = {};
  record.direction = line[0] == 'I' ? 'I' : 'O';
  strncpy(record.number, comma1 + 1, sizeof(record.number) - 1);
  record.duration = strtoul(comma2 + 1, NULL, 10);
  strncpy(record.result, comma3 + 1, sizeof(record.result) - 1);
  keepCallRecord(record);
}

void loadCallHistory() {
  callRecordCount = 0;
  if (!sdMounted) return;
  FsFile f = sd.open("/CALLS.CSV", O_RDONLY);
  if (!f) { Serial.println(F("[CALLS] no history file yet")); return; }
  char line[64];
  uint8_t length = 0;
  while (f.available()) {
    int c = f.read();
    if (c == '\r') continue;
    if (c == '\n' || length >= sizeof(line) - 1) {
      line[length] = '\0';
      parseCallHistoryLine(line);
      length = 0;
    } else line[length++] = (char)c;
  }
  if (length) {
    line[length] = '\0';
    parseCallHistoryLine(line);
  }
  f.close();
  Serial.print(F("[CALLS] loaded ")); Serial.println(callRecordCount);
}

void recordCall(const char *result) {
  if (!activeCallDirection || callLogged) return;
  CallRecord record = {};
  record.direction = activeCallDirection;
  strncpy(record.number, callNumber[0] ? callNumber : "Unknown", sizeof(record.number) - 1);
  record.duration = callWasConnected ? (millis() - callConnectedAt) / 1000UL : 0;
  strncpy(record.result, result, sizeof(record.result) - 1);
  keepCallRecord(record);
  callLogged = true;
  if (sdMounted) {
    FsFile f = sd.open("/CALLS.CSV", O_WRONLY | O_CREAT | O_APPEND);
    if (f) {
      f.write(record.direction);
      f.write(','); f.print(record.number);
      f.write(','); f.print(record.duration);
      f.write(','); f.println(record.result);
      bool saved = f.sync();
      f.close();
      Serial.println(saved ? F("[CALLS] saved to SD") : F("[CALLS] SD sync failed"));
    } else Serial.println(F("[CALLS] could not open CALLS.CSV"));
  } else Serial.println(F("[CALLS] not saved: SD is not mounted"));
  Serial.print(F("[CALLS] ")); Serial.print(record.direction == 'I' ? F("IN ") : F("OUT "));
  Serial.print(record.number); Serial.print(' '); Serial.println(record.result);
}

void finishActiveCall(const char *result) {
  if (!activeCallDirection || callLogged) return;
  if (!result) {
    if (callWasConnected) result = activeCallDirection == 'I' ? "IN" : "OUT";
    else result = activeCallDirection == 'I' ? "MISSED" : "FAILED";
  }
  recordCall(result);
  strncpy(callStatus, result, sizeof(callStatus) - 1);
  callStatus[sizeof(callStatus) - 1] = '\0';
  activeCallDirection = 0;
}

void storeSavedMessage(const char *recipient, const char *body) {
  if (savedMessageCount >= MAX_SMS) {
    for (uint8_t i = 1; i < MAX_SMS; ++i) savedMessages[i - 1] = savedMessages[i];
    savedMessageCount = MAX_SMS - 1;
  }
  uint8_t i = savedMessageCount++;
  strncpy(savedMessages[i].sender, recipient, sizeof(savedMessages[i].sender) - 1);
  savedMessages[i].sender[sizeof(savedMessages[i].sender) - 1] = '\0';
  strncpy(savedMessages[i].body, body, sizeof(savedMessages[i].body) - 1);
  savedMessages[i].body[sizeof(savedMessages[i].body) - 1] = '\0';
}

void loadSavedMessages() {
  savedMessageCount = 0;
  if (!sdMounted) return;
  FsFile f = sd.open("/DRAFTS.TXT", O_RDONLY);
  if (!f) return;
  char line[112];
  uint8_t length = 0;
  while (f.available()) {
    int c = f.read();
    if (c == '\r') continue;
    if (c == '\n' || length >= sizeof(line) - 1) {
      line[length] = '\0';
      char *separator = strchr(line, '|');
      if (separator) {
        *separator++ = '\0';
        if (line[0] && separator[0]) storeSavedMessage(line, separator);
      }
      length = 0;
    } else line[length++] = (char)c;
  }
  if (length) {
    line[length] = '\0';
    char *separator = strchr(line, '|');
    if (separator) {
      *separator++ = '\0';
      if (line[0] && separator[0]) storeSavedMessage(line, separator);
    }
  }
  f.close();
  Serial.print(F("[SMS] saved drafts loaded ")); Serial.println(savedMessageCount);
}

bool replaceSdFile(const char *tempPath, const char *livePath, const char *backupPath) {
  if (sd.exists(backupPath)) sd.remove(backupPath);
  bool hadOriginal = sd.exists(livePath);
  if (hadOriginal && !sd.rename(livePath, backupPath)) return false;
  if (!sd.rename(tempPath, livePath)) {
    if (hadOriginal) sd.rename(backupPath, livePath);
    return false;
  }
  if (hadOriginal) sd.remove(backupPath);
  return true;
}

void recoverSdFile(const char *livePath, const char *backupPath, const char *tempPath) {
  if (!sd.exists(livePath) && sd.exists(backupPath)) sd.rename(backupPath, livePath);
  if (sd.exists(livePath) && sd.exists(backupPath)) sd.remove(backupPath);
  if (sd.exists(tempPath)) sd.remove(tempPath);
}

void recoverSavedData() {
  recoverSdFile("/CONTACTS.CSV", "/CBAK.TXT", "/CTEMP.TXT");
  recoverSdFile("/DRAFTS.TXT", "/DBAK.TXT", "/DTEMP.TXT");
}

bool writeContactsFile(uint8_t skipIndex) {
  FsFile f = sd.open("/CTEMP.TXT", O_WRONLY | O_CREAT | O_TRUNC);
  if (!f) return false;
  f.println(F("# Contacts saved by Little Phone"));
  for (uint8_t i = 0; i < contactCount; ++i) {
    if (i == skipIndex) continue;
    f.print(contacts[i].name); f.print(','); f.println(contacts[i].number);
  }
  bool written = f.sync();
  f.close();
  if (!written) { sd.remove("/CTEMP.TXT"); return false; }
  return replaceSdFile("/CTEMP.TXT", "/CONTACTS.CSV", "/CBAK.TXT");
}

bool appendContactToSd(const char *name, const char *number) {
  FsFile f = sd.open("/CONTACTS.CSV", O_WRONLY | O_CREAT | O_APPEND);
  if (!f) return false;
  f.print(name); f.write(','); f.println(number);
  bool written = f.sync();
  f.close();
  return written;
}

void writeDraftLine(FsFile &f, const char *recipient, const char *body) {
  f.print(recipient); f.print('|');
  for (uint8_t i = 0; body[i]; ++i) {
    char c = body[i];
    if (c == '|' || c == '\r' || c == '\n') c = ' ';
    f.print(c);
  }
  f.println();
}

bool writeDraftFile(int8_t skipIndex, bool appendCurrent) {
  FsFile f = sd.open("/DTEMP.TXT", O_WRONLY | O_CREAT | O_TRUNC);
  if (!f) return false;
  uint8_t first = appendCurrent && savedMessageCount >= MAX_SMS ? 1 : 0;
  for (uint8_t i = first; i < savedMessageCount; ++i) {
    if ((int8_t)i == skipIndex) continue;
    writeDraftLine(f, savedMessages[i].sender, savedMessages[i].body);
  }
  if (appendCurrent) writeDraftLine(f, smsRecipient, smsComposeText);
  bool written = f.sync();
  f.close();
  if (!written) { sd.remove("/DTEMP.TXT"); return false; }
  return replaceSdFile("/DTEMP.TXT", "/DRAFTS.TXT", "/DBAK.TXT");
}

void askDelete(DeleteTarget target) {
  deleteTarget = target;
  screen = DELETE_CONFIRM;
  drawScreen();
}

void deleteContactFromSd() {
  if (!sdMounted || selectedContact >= contactCount || !writeContactsFile(selectedContact)) {
    strncpy(contactStatus, "Could not update SD card", sizeof(contactStatus) - 1);
    contactStatus[sizeof(contactStatus) - 1] = '\0';
    screen = CONTACT_ACTION; drawScreen(); return;
  }
  for (uint8_t i = selectedContact + 1; i < contactCount; ++i) contacts[i - 1] = contacts[i];
  --contactCount;
  if (!contactCount) contactPage = 0;
  else if (contactPage * CONTACTS_PER_PAGE >= contactCount) contactPage = (contactCount - 1) / CONTACTS_PER_PAGE;
  contactStatus[0] = '\0';
  screen = CONTACTS; drawScreen();
  Serial.println(F("[CONTACTS] contact deleted"));
}

void deleteDraftFromSd() {
  if (!sdMounted || selectedSavedMessage >= savedMessageCount ||
      !writeDraftFile((int8_t)selectedSavedMessage, false)) {
    strncpy(smsStatus, "Could not update SD card", sizeof(smsStatus) - 1);
    smsStatus[sizeof(smsStatus) - 1] = '\0';
    screen = SAVED_MESSAGES; drawScreen(); return;
  }
  for (uint8_t i = selectedSavedMessage + 1; i < savedMessageCount; ++i) savedMessages[i - 1] = savedMessages[i];
  --savedMessageCount;
  smsStatus[0] = '\0';
  screen = SAVED_MESSAGES; drawScreen();
  Serial.println(F("[SMS] saved draft deleted"));
}

void deleteInboxSms() {
  if (selectedSms >= smsCount || smsMessages[selectedSms].storageIndex < 0 || !modemResponding) {
    strncpy(smsStatus, "Cannot delete this SIM message", sizeof(smsStatus) - 1);
    smsStatus[sizeof(smsStatus) - 1] = '\0';
    screen = SMS_DETAIL; drawScreen(); return;
  }
  smsDeleteIndex = smsMessages[selectedSms].storageIndex;
  strncpy(smsDeleteStorage, smsMessages[selectedSms].storage, sizeof(smsDeleteStorage) - 1);
  smsDeleteStorage[sizeof(smsDeleteStorage) - 1] = '\0';
  smsDeletePreparing = true;
  smsDeleteCommandPending = false;
  smsActionStarted = millis();
  strncpy(smsStatus, "Deleting SIM message...", sizeof(smsStatus) - 1);
  smsStatus[sizeof(smsStatus) - 1] = '\0';
  screen = MESSAGES; drawScreen();
  char command[40];
  snprintf(command, sizeof(command), "AT+CPMS=\"%s\",\"%s\",\"%s\"",
           smsDeleteStorage, smsDeleteStorage, smsDeleteStorage);
  sendAT(command);
}

void removeInboxSmsFromList() {
  if (selectedSms < smsCount) {
    for (uint8_t i = selectedSms + 1; i < smsCount; ++i) smsMessages[i - 1] = smsMessages[i];
    --smsCount;
  }
  smsDeletePreparing = smsDeleteCommandPending = false;
  smsDeleteIndex = -1;
  strncpy(smsStatus, "Message deleted", sizeof(smsStatus) - 1);
  smsStatus[sizeof(smsStatus) - 1] = '\0';
  screen = MESSAGES; drawScreen();
  Serial.println(F("[SMS] SIM inbox message deleted"));
}

void beginNewContact() {
  contactNameInput[0] = '\0';
  contactNumberInput[0] = '\0';
  contactNumberField = false;
  contactNumericLayout = false;
  contactStatus[0] = '\0';
  screen = CONTACT_ENTRY;
  drawScreen();
}

void saveNewContact() {
  trimField(contactNameInput);
  trimField(contactNumberInput);
  size_t nameLength = strlen(contactNameInput);
  size_t numberLength = strlen(contactNumberInput);
  bool validNumber = numberLength >= 5 && numberLength <= 20;
  for (size_t i = 0; i < numberLength; ++i) {
    char c = contactNumberInput[i];
    if (c == '+' && i == 0) continue;
    if (c < '0' || c > '9') validNumber = false;
  }
  if (!nameLength || !validNumber) {
    strncpy(contactStatus, "Enter name and phone number", sizeof(contactStatus) - 1);
    contactStatus[sizeof(contactStatus) - 1] = '\0';
    drawScreen();
    return;
  }
  if (!sdMounted) {
    strncpy(contactStatus, "SD card is not ready", sizeof(contactStatus) - 1);
    contactStatus[sizeof(contactStatus) - 1] = '\0';
    drawScreen();
    return;
  }
  if (contactCount >= MAX_CONTACTS) {
    strncpy(contactStatus, "Contact list is full", sizeof(contactStatus) - 1);
    contactStatus[sizeof(contactStatus) - 1] = '\0';
    drawScreen();
    return;
  }
  for (uint8_t i = 0; i < contactCount; ++i) {
    if (!strcmp(contacts[i].number, contactNumberInput)) {
      strncpy(contactStatus, "Number already saved", sizeof(contactStatus) - 1);
      contactStatus[sizeof(contactStatus) - 1] = '\0';
      drawScreen();
      return;
    }
  }
  if (!appendContactToSd(contactNameInput, contactNumberInput)) {
    strncpy(contactStatus, "SD write failed", sizeof(contactStatus) - 1);
    contactStatus[sizeof(contactStatus) - 1] = '\0';
    drawScreen();
    Serial.println(F("[CONTACTS] append to CONTACTS.CSV failed"));
    return;
  }
  uint8_t newIndex = contactCount;
  strncpy(contacts[newIndex].name, contactNameInput, sizeof(contacts[newIndex].name) - 1);
  contacts[newIndex].name[sizeof(contacts[newIndex].name) - 1] = '\0';
  strncpy(contacts[newIndex].number, contactNumberInput, sizeof(contacts[newIndex].number) - 1);
  contacts[newIndex].number[sizeof(contacts[newIndex].number) - 1] = '\0';
  ++contactCount;
  selectedContact = newIndex;
  contactPage = selectedContact / CONTACTS_PER_PAGE;
  strncpy(contactStatus, "Saved to SD", sizeof(contactStatus) - 1);
  contactStatus[sizeof(contactStatus) - 1] = '\0';
  screen = CONTACT_ACTION;
  drawScreen();
  Serial.print(F("[CONTACTS] saved ")); Serial.println(contactNameInput);
}

void storeSms(const char *sender, const char *body, int16_t storageIndex, const char *storage) {
  if (smsCount >= MAX_SMS) {
    for (uint8_t i = 1; i < MAX_SMS; ++i) smsMessages[i - 1] = smsMessages[i];
    smsCount = MAX_SMS - 1;
  }
  uint8_t i = smsCount++;
  strncpy(smsMessages[i].sender, sender, sizeof(smsMessages[i].sender) - 1);
  smsMessages[i].sender[sizeof(smsMessages[i].sender) - 1] = '\0';
  strncpy(smsMessages[i].body, body, sizeof(smsMessages[i].body) - 1);
  smsMessages[i].body[sizeof(smsMessages[i].body) - 1] = '\0';
  smsMessages[i].storageIndex = storageIndex;
  strncpy(smsMessages[i].storage, storage, sizeof(smsMessages[i].storage) - 1);
  smsMessages[i].storage[sizeof(smsMessages[i].storage) - 1] = '\0';
  if (screen == MESSAGES) drawMessages();
  Serial.print(F("[SMS] from ")); Serial.print(sender); Serial.print(F(": ")); Serial.println(body);
}

bool parseSmsHeader(const char *line, char *sender, size_t senderSize) {
  const char *q1 = strchr(line, '"');
  if (!q1) return false;
  const char *q2 = strchr(q1 + 1, '"');
  if (!q2) return false;
  const char *q3 = strchr(q2 + 1, '"');
  if (!q3) return false;
  const char *q4 = strchr(q3 + 1, '"');
  if (!q4) return false;
  size_t n = (size_t)(q4 - q3 - 1);
  if (n >= senderSize) n = senderSize - 1;
  memcpy(sender, q3 + 1, n); sender[n] = '\0';
  return true;
}

void parseSmsStorage(const char *line, char *storage, size_t storageSize) {
  const char *q1 = strchr(line, '"');
  const char *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
  if (!q1 || !q2) return;
  size_t n = (size_t)(q2 - q1 - 1);
  if (n >= storageSize) n = storageSize - 1;
  memcpy(storage, q1 + 1, n);
  storage[n] = '\0';
}

void refreshSmsInbox() {
  if (!modemResponding) { strncpy(smsStatus, "Modem is not ready", sizeof(smsStatus) - 1); drawScreen(); return; }
  smsCount = 0;
  smsAwaitBody = false;
  smsPendingStorageIndex = -1;
  smsInboxLoading = true;
  smsListQueued = true;
  smsActionStarted = millis();
  strncpy(smsStatus, "Loading inbox...", sizeof(smsStatus) - 1);
  smsStatus[sizeof(smsStatus) - 1] = '\0';
  screen = MESSAGES; drawScreen();
  sendAT("AT+CPMS?");
}

void beginSmsToContact(uint8_t contactIndex) {
  if (contactIndex >= contactCount) return;
  strncpy(smsRecipient, contacts[contactIndex].number, sizeof(smsRecipient) - 1);
  smsRecipient[sizeof(smsRecipient) - 1] = '\0';
  smsComposeText[0] = '\0';
  smsStatus[0] = '\0';
  smsNumericLayout = false;
  composeReturnScreen = CONTACT_ACTION;
  screen = SMS_COMPOSE; drawScreen();
}

void saveDraftToSd() {
  if (!smsRecipient[0] || !smsComposeText[0]) {
    strncpy(smsStatus, "Type a message before saving", sizeof(smsStatus) - 1);
    smsStatus[sizeof(smsStatus) - 1] = '\0';
    drawScreen();
    return;
  }
  if (!sdMounted) {
    strncpy(smsStatus, "SD card is not ready", sizeof(smsStatus) - 1);
    smsStatus[sizeof(smsStatus) - 1] = '\0';
    drawScreen();
    return;
  }
  if (!writeDraftFile(-1, true)) {
    strncpy(smsStatus, "SD write failed", sizeof(smsStatus) - 1);
    smsStatus[sizeof(smsStatus) - 1] = '\0';
    drawScreen();
    return;
  }
  storeSavedMessage(smsRecipient, smsComposeText);
  strncpy(smsStatus, "Draft saved on SD", sizeof(smsStatus) - 1);
  smsStatus[sizeof(smsStatus) - 1] = '\0';
  drawScreen();
  Serial.println(F("[SMS] draft saved on SD"));
}

void sendAT(const char *command) {
  Serial1.println(command);
  Serial.print(F("> "));
  Serial.println(command);
}

void requestNetworkCheck() {
  lastNetworkQuery = millis();
  registrationStatus = -1;
  networkRegistered = false;
  simReady = false;
  signalRssi = 99;
  signalBer = 99;
  strncpy(simState, "Checking", sizeof(simState) - 1);
  simState[sizeof(simState) - 1] = '\0';
  strncpy(operatorName, "Checking", sizeof(operatorName) - 1);
  operatorName[sizeof(operatorName) - 1] = '\0';
  sendAT("AT");
  sendAT("AT+CPIN?");
  sendAT("AT+CREG?");
  sendAT("AT+CSQ");
  sendAT("AT+COPS?");
  if (screen == NETWORK_STATUS) drawNetworkStatus();
}

void centerText(const char *s, int16_t y, uint8_t size, uint16_t fg, uint16_t bg = C_PAPER) {
  int16_t x = (tft.width() - (int16_t)strlen(s) * 6 * size) / 2;
  if (x < 1) x = 1;
  tft.setTextSize(size);
  tft.setTextColor(fg, bg);
  tft.setCursor(x, y);
  tft.print(s);
}

void button(int16_t x, int16_t y, int16_t w, int16_t h, const char *label,
            uint16_t fill, uint8_t size = 2) {
  tft.fillRoundRect(x, y, w, h, 9, fill);
  tft.drawRoundRect(x, y, w, h, 9, C_NAVY);
  int16_t tw = (int16_t)strlen(label) * 6 * size, th = 8 * size;
  tft.setTextSize(size);
  tft.setTextColor(C_WHITE, fill);
  tft.setCursor(x + (w - tw) / 2, y + (h - th) / 2 + 1);
  tft.print(label);
}

void drawStar(int16_t cx, int16_t cy, uint16_t color) {
  tft.fillCircle(cx, cy, 3, color);
  tft.drawLine(cx - 6, cy, cx + 6, cy, color);
  tft.drawLine(cx, cy - 6, cx, cy + 6, color);
  tft.drawLine(cx - 4, cy - 4, cx + 4, cy + 4, color);
  tft.drawLine(cx - 4, cy + 4, cx + 4, cy - 4, color);
}

void drawHeader(const char *title) {
  tft.fillRect(0, 0, 320, 30, C_NAVY);
  tft.setTextSize(2);
  tft.setTextColor(C_PAPER, C_NAVY);
  tft.setCursor(8, 6);
  tft.print(title);
  const char *state = !modemResponding ? "MODEM?" : !simReady ? "SIM?" :
                      networkRegistered ? "GSM OK" : "SEARCH";
  uint16_t badge = networkRegistered ? C_GREEN : C_ORANGE;
  tft.fillRoundRect(233, 4, 80, 22, 6, badge);
  tft.setTextSize(1);
  tft.setTextColor(C_WHITE, badge);
  tft.setCursor(239, 11);
  tft.print(state);
}

void redrawCurrentHeader() {
  switch (screen) {
    case HOME: drawHeader("LITTLE PHONE"); break;
    case DIALER: drawHeader("DIAL A FRIEND"); break;
    case CONTACTS: drawHeader(contactForSms ? "PICK SMS CONTACT" : "CONTACTS"); break;
    case CONTACT_ENTRY: drawHeader("NEW CONTACT"); break;
    case CONTACT_ACTION: drawHeader("FRIEND CARD"); break;
    case MEDIA_MENU: drawHeader("MEDIA BOX"); break;
    case MESSAGES: drawHeader("MESSAGES"); break;
    case SAVED_MESSAGES: drawHeader("SAVED MESSAGES"); break;
    case SMS_COMPOSE: drawHeader("WRITE A MESSAGE"); break;
    case SMS_DETAIL: drawHeader("MESSAGE"); break;
    case DELETE_CONFIRM: drawHeader("DELETE ITEM?"); break;
    case NETWORK_STATUS: drawHeader("NETWORK STATUS"); break;
    case INCOMING: drawHeader("PHONE IS RINGING!"); break;
    case IN_CALL: drawHeader(callConnected ? "TALKING" : "CALLING"); break;
    case PHOTO: drawHeader("PHOTO ALBUM"); break;
    case MUSIC: drawHeader("MUSIC BOX"); break;
    case CALL_HISTORY: drawHeader("CALL HISTORY"); break;
    case TOUCH_CALIBRATION: drawHeader("TOUCH CALIBRATION"); break;
  }
}

void drawHome() {
  tft.fillScreen(C_PAPER);
  drawHeader("LITTLE PHONE");
  drawStar(18, 47, C_ORANGE);
  centerText("A little phone for big adventures", 40, 1, C_MUTED);
  button(16, 55, 136, 40, "CALL", C_GREEN, 2);
  button(168, 55, 136, 40, "CONTACTS", C_BLUE, 1);
  button(16, 100, 136, 40, "MESSAGES", C_BLUE, 1);
  button(168, 100, 136, 40, "MEDIA", C_RED, 2);
  button(92, 145, 136, 34, "CALL HISTORY", C_ORANGE, 1);
  tft.setTextSize(1);
  tft.setTextColor(C_INK, C_PAPER);
  tft.setCursor(18, 190);
  tft.print(F("Last: "));
  tft.print(callNumber[0] ? callNumber : "none yet");
  tft.setCursor(18, 208);
  tft.print(callStatus);
  tft.setCursor(244, 215);
  tft.print(F("SND "));
  tft.drawRect(268, 213, 36, 8, C_NAVY);
  int bar = constrain(map(soundPeak, 0, 240, 0, 32), 0, 32);
  if (bar) tft.fillRect(270, 215, bar, 4, C_GREEN);
}

void drawContacts() {
  tft.fillScreen(C_PAPER);
  drawHeader(contactForSms ? "PICK SMS CONTACT" : "CONTACTS");
  if (!contactCount) {
    centerText("No contacts on SD", 88, 2, C_MUTED);
    centerText("Add names to CONTACTS.CSV", 118, 1, C_MUTED);
  } else {
    uint8_t first = contactPage * CONTACTS_PER_PAGE;
    for (uint8_t row = 0; row < CONTACTS_PER_PAGE; ++row) {
      uint8_t i = first + row;
      if (i >= contactCount) break;
      int16_t y = 37 + row * 30;
      tft.fillRoundRect(8, y, 304, 26, 5, C_CARD);
      tft.drawRoundRect(8, y, 304, 26, 5, C_NAVY);
      tft.setTextSize(1);
      tft.setTextColor(C_INK, C_CARD);
      tft.setCursor(15, y + 9); tft.print(contacts[i].name);
      tft.setCursor(112, y + 9); tft.print(contacts[i].number);
    }
  }
  button(8, 197, 72, 30, "ADD", C_GREEN, 1);
  button(88, 197, 92, 30, "BACK", C_NAVY, 1);
  button(188, 197, 58, 30, "PREV", C_BLUE, 1);
  button(252, 197, 60, 30, "NEXT", C_BLUE, 1);
}

void drawContactEntry() {
  tft.fillScreen(C_PAPER);
  drawHeader("NEW CONTACT");
  tft.setTextSize(1);
  tft.setTextColor(C_INK, C_PAPER);
  tft.setCursor(8, 46); tft.print(F("Name"));
  tft.setCursor(8, 72); tft.print(F("Phone"));
  uint16_t nameFill = contactNumberField ? C_CARD : C_BLUE;
  uint16_t numberFill = contactNumberField ? C_BLUE : C_CARD;
  tft.fillRoundRect(46, 35, 266, 23, 4, nameFill);
  tft.drawRoundRect(46, 35, 266, 23, 4, C_NAVY);
  tft.setTextColor(contactNumberField ? C_INK : C_WHITE, nameFill);
  tft.setCursor(52, 43); tft.print(contactNameInput);
  tft.fillRoundRect(46, 61, 266, 23, 4, numberFill);
  tft.drawRoundRect(46, 61, 266, 23, 4, C_NAVY);
  tft.setTextColor(contactNumberField ? C_WHITE : C_INK, numberFill);
  tft.setCursor(52, 69); tft.print(contactNumberInput);
  if (contactStatus[0]) {
    tft.setTextColor(C_RED, C_PAPER); tft.setCursor(8, 86); tft.print(contactStatus);
  }
  const char *rows[] = {contactNumericLayout ? "1234567890" : "QWERTYUIOP",
                        contactNumericLayout ? "@#$%&*-+/" : "ASDFGHJKL",
                        contactNumericLayout ? "()!?.,:" : "ZXCVBNM"};
  const uint8_t rowLengths[] = {10, 9, 7};
  const int16_t rowY[] = {91, 120, 149};
  for (uint8_t r = 0; r < 3; ++r) {
    int16_t offset = (320 - rowLengths[r] * 29) / 2;
    for (uint8_t c = 0; c < rowLengths[r]; ++c) {
      int16_t keyX = offset + c * 29;
      tft.fillRoundRect(keyX, rowY[r], 26, 26, 4, C_CARD);
      tft.drawRoundRect(keyX, rowY[r], 26, 26, 4, C_NAVY);
      tft.setTextSize(1); tft.setTextColor(C_INK, C_CARD);
      tft.setCursor(keyX + 10, rowY[r] + 9);
      tft.write(rows[r][c]);
    }
  }
  button(4, 181, 48, 43, "SPACE", C_BLUE, 1);
  button(56, 181, 44, 43, contactNumericLayout ? "ABC" : "123", C_ORANGE, 1);
  button(104, 181, 44, 43, "DEL", C_ORANGE, 1);
  button(152, 181, 74, 43, "SAVE", C_GREEN, 1);
  button(230, 181, 84, 43, "BACK", C_NAVY, 1);
}

void drawContactAction() {
  tft.fillScreen(C_PAPER);
  drawHeader("FRIEND CARD");
  if (selectedContact >= contactCount) { centerText("No contact selected", 90, 2, C_MUTED); return; }
  drawStar(160, 58, C_ORANGE);
  centerText(contacts[selectedContact].name, 81, 2, C_INK);
  centerText(contacts[selectedContact].number, 112, 2, C_MUTED);
  if (contactStatus[0]) centerText(contactStatus, 143, 1, C_GREEN);
  button(5, 165, 72, 46, "CALL", C_GREEN, 1);
  button(82, 165, 72, 46, "TEXT", C_BLUE, 1);
  button(159, 165, 72, 46, "DELETE", C_RED, 1);
  button(236, 165, 79, 46, "BACK", C_NAVY, 1);
}

void drawMediaMenu() {
  tft.fillScreen(C_PAPER);
  drawHeader("MEDIA BOX");
  drawStar(160, 64, C_ORANGE);
  button(18, 94, 136, 58, "PHOTOS", C_BLUE, 2);
  button(166, 94, 136, 58, "MUSIC", C_RED, 2);
  button(104, 176, 112, 38, "BACK", C_NAVY, 1);
}

void drawMessages() {
  tft.fillScreen(C_PAPER);
  drawHeader("MESSAGES");
  tft.setTextSize(1); tft.setTextColor(C_MUTED, C_PAPER);
  tft.setCursor(9, 36); tft.print(smsStatus[0] ? smsStatus : "SIM inbox (latest 5)");
  for (uint8_t row = 0; row < MAX_SMS; ++row) {
    int16_t y = 50 + row * 28;
    uint8_t i = (smsCount > row) ? (uint8_t)(smsCount - 1 - row) : MAX_SMS;
    tft.fillRoundRect(8, y, 304, 25, 5, C_CARD);
    tft.drawRoundRect(8, y, 304, 25, 5, C_NAVY);
    if (i < smsCount) {
      tft.setTextColor(C_INK, C_CARD);
      tft.setCursor(14, y + 4); tft.print(smsMessages[i].sender);
      tft.setTextColor(C_MUTED, C_CARD);
      tft.setCursor(14, y + 14);
      for (uint8_t c = 0; c < 32 && smsMessages[i].body[c]; ++c) tft.print(smsMessages[i].body[c]);
    }
  }
  button(8, 197, 68, 30, "NEW", C_GREEN, 1);
  button(82, 197, 72, 30, "SAVED", C_ORANGE, 1);
  button(160, 197, 82, 30, "REFRESH", C_BLUE, 1);
  button(250, 197, 62, 30, "BACK", C_NAVY, 1);
}

void drawSavedMessages() {
  tft.fillScreen(C_PAPER);
  drawHeader("SAVED MESSAGES");
  tft.setTextSize(1); tft.setTextColor(C_MUTED, C_PAPER);
  tft.setCursor(9, 36); tft.print(smsStatus[0] ? smsStatus : "Tap message to edit; DEL removes it");
  if (!savedMessageCount) centerText("No saved drafts yet", 96, 2, C_MUTED);
  for (uint8_t row = 0; row < MAX_SMS; ++row) {
    int16_t y = 50 + row * 28;
    uint8_t i = (savedMessageCount > row) ? (uint8_t)(savedMessageCount - 1 - row) : MAX_SMS;
    tft.fillRoundRect(8, y, 304, 25, 5, C_CARD);
    tft.drawRoundRect(8, y, 304, 25, 5, C_NAVY);
    if (i < savedMessageCount) {
      tft.setTextColor(C_INK, C_CARD); tft.setCursor(14, y + 4); tft.print(savedMessages[i].sender);
      tft.setTextColor(C_MUTED, C_CARD); tft.setCursor(14, y + 14);
      for (uint8_t c = 0; c < 27 && savedMessages[i].body[c]; ++c) tft.print(savedMessages[i].body[c]);
      button(270, y + 2, 38, 21, "DEL", C_RED, 1);
    }
  }
  button(104, 197, 112, 30, "BACK", C_NAVY, 1);
}

void drawCallHistory() {
  tft.fillScreen(C_PAPER);
  drawHeader("CALL HISTORY");
  for (uint8_t row = 0; row < MAX_CALLS; ++row) {
    int16_t y = 35 + row * 20;
    tft.fillRoundRect(7, y, 306, 18, 4, C_CARD);
    tft.drawRoundRect(7, y, 306, 18, 4, C_NAVY);
    if (row < callRecordCount) {
      uint8_t i = callRecordCount - 1 - row;
      uint32_t seconds = callRecords[i].duration;
      char timeText[8];
      snprintf(timeText, sizeof(timeText), "%02lu:%02lu",
               (unsigned long)(seconds / 60), (unsigned long)(seconds % 60));
      tft.setTextSize(1);
      tft.setTextColor(callRecords[i].direction == 'I' ? C_BLUE : C_GREEN, C_CARD);
      tft.setCursor(12, y + 5);
      tft.print(callRecords[i].direction == 'I' ? F("IN ") : F("OUT "));
      tft.print(callRecords[i].result);
      tft.setTextColor(C_INK, C_CARD);
      tft.setCursor(101, y + 5); tft.print(callRecords[i].number);
      tft.setTextColor(C_MUTED, C_CARD);
      tft.setCursor(263, y + 5); tft.print(timeText);
    }
  }
  if (!callRecordCount) centerText("No calls saved yet", 105, 2, C_MUTED);
  button(104, 201, 112, 30, "BACK", C_NAVY, 1);
}

void drawTouchCalibration() {
  tft.fillScreen(C_PAPER);
  drawHeader("TOUCH CALIBRATION");
  const int16_t targetX[3] = {15, 160, 304};
  const int16_t targetY = 120;
  for (uint8_t i = 0; i < 3; ++i)
    drawStar(targetX[i], targetY, i == touchCalStage ? C_RED : C_MUTED);
  char prompt[32];
  snprintf(prompt, sizeof(prompt), "Tap target %u of 3", touchCalStage + 1);
  centerText(prompt, 54, 2, C_INK);
  centerText("Touch the center of each star", 178, 1, C_MUTED);
  centerText("Hold briefly, then release", 193, 1, C_MUTED);
  if (touchCalMessage[0]) centerText(touchCalMessage, 211, 1, C_BLUE);
  button(242, 203, 70, 28, "CANCEL", C_NAVY, 1);
}

const char *registrationText() {
  switch (registrationStatus) {
    case 0: return "Not registered";
    case 1: return "Registered (home)";
    case 2: return "Searching";
    case 3: return "Registration denied";
    case 4: return "Status unknown";
    case 5: return "Registered (roaming)";
    default: return "Not checked";
  }
}

void drawNetworkStatus() {
  networkStatusDirty = false;
  lastNetworkScreenDraw = millis();
  tft.fillScreen(C_PAPER);
  drawHeader("NETWORK STATUS");
  tft.setTextSize(1);
  tft.setTextColor(C_INK, C_PAPER);
  tft.setCursor(12, 42); tft.print(F("Modem: ")); tft.print(modemResponding ? "Responding" : "No response");
  tft.setCursor(12, 64); tft.print(F("SIM: ")); tft.print(simState);
  tft.setCursor(12, 86); tft.print(F("Network: ")); tft.print(registrationText());
  tft.setCursor(12, 108); tft.print(F("Signal: "));
  if (signalRssi == 99 || signalRssi < 0 || signalRssi > 31) tft.print(F("Unknown (CSQ 99)"));
  else { tft.print(F("CSQ ")); tft.print(signalRssi); tft.print(F(" (~")); tft.print(-113 + 2 * signalRssi); tft.print(F(" dBm)")); }
  tft.setCursor(12, 130); tft.print(F("Operator: ")); tft.print(operatorName);
  uint32_t age = lastNetworkQuery ? (millis() - lastNetworkQuery) / 1000UL : 0;
  tft.setTextColor(C_MUTED, C_PAPER);
  tft.setCursor(12, 153); tft.print(F("Auto updates every 5 sec; CHECK now"));
  tft.setCursor(12, 168); tft.print(F("Last request: ")); tft.print(age); tft.print(F(" sec ago"));
  if (signalRssi != 99 && signalRssi >= 0 && signalRssi <= 31) {
    uint8_t bars = (uint8_t)constrain(map(signalRssi, 0, 31, 1, 5), 1, 5);
    for (uint8_t i = 0; i < 5; ++i) {
      int16_t h = 4 + i * 3;
      uint16_t color = i < bars ? C_GREEN : C_CARD;
      tft.fillRect(276 + i * 7, 126 - h, 5, h, color);
    }
  }
  button(8, 197, 94, 30, "CHECK", C_GREEN, 1);
  button(111, 197, 98, 30, "TOUCH CAL", C_ORANGE, 1);
  button(218, 197, 94, 30, "BACK", C_NAVY, 1);
}

void drawSmsCompose() {
  tft.fillScreen(C_PAPER);
  drawHeader("WRITE A MESSAGE");
  tft.setTextSize(1); tft.setTextColor(C_INK, C_PAPER);
  tft.setCursor(8, 34); tft.print(F("To: ")); tft.print(smsRecipient);
  tft.fillRoundRect(7, 46, 306, 30, 5, C_CARD);
  tft.drawRoundRect(7, 46, 306, 30, 5, C_NAVY);
  tft.setTextColor(C_INK, C_CARD); tft.setCursor(12, 57);
  size_t len = strlen(smsComposeText);
  size_t start = len > 48 ? len - 48 : 0;
  tft.print(smsComposeText + start);
  if (smsStatus[0]) { tft.setTextColor(C_MUTED, C_PAPER); tft.setCursor(8, 78); tft.print(smsStatus); }
  const char *rows[] = {smsNumericLayout ? "1234567890" : "QWERTYUIOP",
                        smsNumericLayout ? "@#$%&*-+/" : "ASDFGHJKL",
                        smsNumericLayout ? "()!?.,:" : "ZXCVBNM"};
  const uint8_t rowLengths[] = {10, 9, 7};
  const int16_t rowY[] = {86, 117, 148};
  for (uint8_t r = 0; r < 3; ++r) {
    int16_t offset = (320 - rowLengths[r] * 29) / 2;
    for (uint8_t c = 0; c < rowLengths[r]; ++c) {
      char key[2] = {rows[r][c], '\0'};
      int16_t keyX = offset + c * 29;
      tft.fillRoundRect(keyX, rowY[r], 26, 26, 4, C_CARD);
      tft.drawRoundRect(keyX, rowY[r], 26, 26, 4, C_NAVY);
      tft.setTextSize(1);
      tft.setTextColor(C_INK, C_CARD);
      tft.setCursor(keyX + 10, rowY[r] + 9);
      tft.print(key);
    }
  }
  button(3, 181, 48, 43, "SPACE", C_BLUE, 1);
  button(55, 181, 42, 43, smsNumericLayout ? "ABC" : "123", C_ORANGE, 1);
  button(101, 181, 42, 43, "DEL", C_ORANGE, 1);
  button(147, 181, 50, 43, "SAVE", C_BLUE, 1);
  button(201, 181, 50, 43, "SEND", C_GREEN, 1);
  button(255, 181, 62, 43, "BACK", C_NAVY, 1);
}

void drawSmsDetail() {
  tft.fillScreen(C_PAPER);
  drawHeader("MESSAGE");
  if (selectedSms < smsCount) {
    tft.setTextSize(1); tft.setTextColor(C_MUTED, C_PAPER);
    tft.setCursor(10, 43); tft.print(F("From: ")); tft.print(smsMessages[selectedSms].sender);
    tft.setTextColor(C_INK, C_PAPER); tft.setTextSize(2); tft.setCursor(10, 70);
    tft.setTextWrap(true); tft.print(smsMessages[selectedSms].body); tft.setTextWrap(false);
  }
  button(8, 184, 94, 42, "REPLY", C_BLUE, 1);
  button(113, 184, 94, 42, "DELETE", C_RED, 1);
  button(218, 184, 94, 42, "BACK", C_NAVY, 1);
}

void drawDeleteConfirm() {
  tft.fillScreen(C_PAPER);
  drawHeader("DELETE ITEM?");
  const char *message = deleteTarget == DELETE_CONTACT ? "Remove this contact?" :
                        deleteTarget == DELETE_INBOX_SMS ? "Remove this SIM message?" :
                        "Remove this saved message?";
  drawStar(160, 70, C_ORANGE);
  centerText(message, 106, 2, C_INK);
  centerText("This cannot be undone", 134, 1, C_MUTED);
  button(28, 174, 120, 44, "DELETE", C_RED, 1);
  button(172, 174, 120, 44, "KEEP", C_NAVY, 2);
}

void drawDialer() {
  tft.fillScreen(C_PAPER);
  drawHeader("DIAL A FRIEND");
  tft.fillRoundRect(16, 36, 191, 25, 6, C_CARD);
  tft.drawRoundRect(16, 36, 191, 25, 6, C_NAVY);
  tft.setTextSize(2);
  tft.setTextColor(C_INK, C_CARD);
  tft.setCursor(23, 41);
  tft.print(dialNumber);
  const char keys[12] = {'1','2','3','4','5','6','7','8','9','*','0','#'};
  const int16_t kx[3] = {18, 84, 150}, ky[4] = {68, 106, 144, 182};
  for (uint8_t i = 0; i < 12; ++i) {
    char label[2] = {keys[i], '\0'};
    button(kx[i % 3], ky[i / 3], 54, 32, label, C_CARD, 2);
    tft.setTextColor(C_INK, C_CARD);
  }
  button(225, 71, 82, 45, "CALL", C_GREEN, 2);
  button(225, 127, 82, 37, "DELETE", C_BLUE, 1);
  button(225, 175, 82, 37, "BACK", C_NAVY, 1);
}

void drawIncoming() {
  tft.fillScreen(C_PAPER);
  drawHeader("PHONE IS RINGING!");
  drawStar(160, 58, C_ORANGE);
  centerText("CALL FROM", 78, 2, C_MUTED);
  centerText(callNumber[0] ? callNumber : "Unknown", 115, 2, C_INK);
  button(22, 166, 126, 50, "ANSWER", C_GREEN, 2);
  button(172, 166, 126, 50, "REJECT", C_RED, 2);
}

void drawCallClock() {
  tft.fillRect(0, 147, 320, 27, C_PAPER);
  if (!callConnected) return;
  uint32_t s = (millis() - callConnectedAt) / 1000UL;
  char stamp[16];
  snprintf(stamp, sizeof(stamp), "%02lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
  centerText(stamp, 151, 2, C_ORANGE);
}

void drawCall() {
  tft.fillScreen(C_PAPER);
  drawHeader(callConnected ? "TALKING" : "CALLING");
  drawStar(160, 53, C_ORANGE);
  centerText(callNumber[0] ? callNumber : "Unknown", 78, 2, C_INK);
  centerText(callConnected ? "Connected" : callStatus, 116, 2,
              callConnected ? C_GREEN : C_MUTED);
  drawCallClock();
  button(70, 184, 180, 43, "END CALL", C_RED, 2);
}

void drawPhotoPage(const char *note) {
  tft.fillScreen(C_PAPER);
  drawHeader("PHOTO ALBUM");
  tft.setTextSize(1);
  tft.setTextColor(C_MUTED, C_PAPER);
  tft.setCursor(8, 77);
  tft.print(note);
  button(104, 204, 112, 30, "BACK", C_NAVY, 1);
}

void drawMusicPage(const char *status) {
  tft.fillScreen(C_PAPER);
  drawHeader("MUSIC BOX");
  drawStar(160, 69, C_ORANGE);
  centerText("SONG.RAW", 91, 2, C_INK);
  centerText(status, 123, 1, C_MUTED);
  button(18, 160, 132, 48, "PLAY", C_GREEN, 2);
  button(170, 160, 132, 48, "STOP", C_RED, 2);
  button(104, 211, 112, 26, "BACK", C_NAVY, 1);
}

void drawScreen() {
  switch (screen) {
    case HOME: drawHome(); break;
    case DIALER: drawDialer(); break;
    case CONTACTS: drawContacts(); break;
    case CONTACT_ENTRY: drawContactEntry(); break;
    case CONTACT_ACTION: drawContactAction(); break;
    case MEDIA_MENU: drawMediaMenu(); break;
    case MESSAGES: drawMessages(); break;
    case SAVED_MESSAGES: drawSavedMessages(); break;
    case SMS_COMPOSE: drawSmsCompose(); break;
    case SMS_DETAIL: drawSmsDetail(); break;
    case DELETE_CONFIRM: drawDeleteConfirm(); break;
    case NETWORK_STATUS: drawNetworkStatus(); break;
    case INCOMING: drawIncoming(); break;
    case IN_CALL: drawCall(); break;
    case PHOTO: drawPhotoPage(sdMounted ? "Put PHOTO.BMP on the card" : "SD card is not ready"); break;
    case MUSIC: drawMusicPage(audioRunning ? "Playing 8 kHz mono" : "Ready for 8 kHz mono audio"); break;
    case CALL_HISTORY: drawCallHistory(); break;
    case TOUCH_CALIBRATION: drawTouchCalibration(); break;
  }
}

void stopMusic() {
  noInterrupts();
  audioRunning = false;
  audioHead = audioTail = 0;
  interrupts();
  if (audioFileOpen) {
    audioFile.close();
    audioFileOpen = false;
  }
  audioEof = false;
  if (screen == MUSIC) drawScreen();
}

bool startMusic() {
  if (!sdMounted) {
    drawMusicPage("SD card is not ready");
    return false;
  }
  if (audioFileOpen) stopMusic();
  audioFile = sd.open("/SONG.RAW", O_RDONLY);
  if (!audioFile) {
    drawMusicPage("Missing SONG.RAW on SD");
    return false;
  }
  audioFileOpen = true;
  audioEof = false;
  audioHead = audioTail = 0;
  delay(35);
  fillAudioRing();
  noInterrupts();
  audioRunning = true;
  interrupts();
  drawMusicPage("Playing 8 kHz mono");
  return true;
}

void fillAudioRing() {
  if (!audioFileOpen || audioEof) return;
  uint8_t head = audioHead;
  uint8_t tail = audioTail;
  while ((uint8_t)(head - tail) < 190) {
    int sample = audioFile.read();
    if (sample < 0) {
      audioEof = true;
      break;
    }
    audioRing[head] = (uint8_t)sample;
    ++head;
  }
  audioHead = head;
}

ISR(TIMER4_COMPA_vect) {
  if (audioRunning) {
    uint8_t tail = audioTail;
    if (tail != audioHead) {
      int16_t centered = (int16_t)audioRing[tail] - 128;
      OCR5A = (uint8_t)(128 + centered * MUSIC_GAIN_PERCENT / 100);
      audioTail = (uint8_t)(tail + 1);
    } else {
      OCR5A = 128;
    }
  } else {
    OCR5A = 128;
  }
}

uint16_t readLE16(FsFile &f) {
  uint8_t b[2];
  if (f.read(b, 2) != 2) return 0;
  return (uint16_t)b[0] | ((uint16_t)b[1] << 8);
}

uint32_t readLE32(FsFile &f) {
  uint8_t b[4];
  if (f.read(b, 4) != 4) return 0;
  return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
         ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

bool showPhotoBmp() {
  FsFile f = sd.open("/PHOTO.BMP", O_RDONLY);
  if (!f) return false;
  uint16_t signature = readLE16(f);
  (void)readLE32(f);
  (void)readLE32(f);
  uint32_t imageOffset = readLE32(f);
  uint32_t dibSize = readLE32(f);
  int32_t width = (int32_t)readLE32(f);
  int32_t height = (int32_t)readLE32(f);
  uint16_t planes = readLE16(f), depth = readLE16(f);
  uint32_t compression = readLE32(f);
  if (signature != 0x4D42 || dibSize < 40 || planes != 1 || depth != 24 || compression != 0 ||
      width < 1 || width > 320 || height == 0 || height > 178 || height < -178) {
    f.close();
    return false;
  }
  bool bottomUp = height > 0;
  uint16_t h = (uint16_t)(bottomUp ? height : -height);
  uint16_t w = (uint16_t)width;
  uint32_t rowBytes = ((uint32_t)w * 3 + 3) & ~3UL;
  uint16_t row[320];
  uint8_t bgr[3];
  tft.fillScreen(C_PAPER);
  drawHeader("PHOTO ALBUM");
  int16_t startX = (320 - w) / 2, startY = 31 + (178 - h) / 2;
  for (uint16_t srcRow = 0; srcRow < h; ++srcRow) {
    uint16_t dstRow = bottomUp ? (h - 1 - srcRow) : srcRow;
    uint32_t pos = imageOffset + (uint32_t)srcRow * rowBytes;
    if (!f.seek(pos)) { f.close(); return false; }
    for (uint16_t x = 0; x < w; ++x) {
      if (f.read(bgr, 3) != 3) { f.close(); return false; }
      row[x] = tft.color565(bgr[2], bgr[1], bgr[0]);
    }
    tft.setAddrWindow(startX, startY + dstRow, startX + w - 1, startY + dstRow);
    tft.pushColors(row, w, true);
  }
  f.close();
  button(104, 211, 112, 26, "BACK", C_NAVY, 1);
  return true;
}

void hangUp() {
  if (screen == INCOMING && !callWasConnected) finishActiveCall("REJECT");
  else if (callWasConnected) finishActiveCall(NULL);
  else finishActiveCall("CANCEL");
  sendAT("ATH");
  callConnected = false;
  screen = HOME;
  drawScreen();
}

void startCall() {
  if (!dialNumber[0] || !networkRegistered) {
    strncpy(callStatus, networkRegistered ? "Enter a number" : "No GSM network", sizeof(callStatus) - 1);
    callStatus[sizeof(callStatus) - 1] = '\0';
    screen = HOME;
    drawScreen();
    return;
  }
  if (audioRunning) stopMusic();
  strncpy(callNumber, dialNumber, sizeof(callNumber) - 1);
  callNumber[sizeof(callNumber) - 1] = '\0';
  activeCallDirection = 'O';
  callWasConnected = false;
  callLogged = false;
  char command[32];
  snprintf(command, sizeof(command), "ATD%s;", callNumber);
  callConnected = false;
  strncpy(callStatus, "Calling...", sizeof(callStatus) - 1);
  callStatus[sizeof(callStatus) - 1] = '\0';
  screen = IN_CALL;
  drawScreen();
  sendAT(command);
}

void answerCall() {
  callConnected = false;
  strncpy(callStatus, "Connecting...", sizeof(callStatus) - 1);
  callStatus[sizeof(callStatus) - 1] = '\0';
  screen = IN_CALL;
  drawScreen();
  sendAT("ATA");
}

void addDigit(char digit) {
  size_t n = strlen(dialNumber);
  if (n >= sizeof(dialNumber) - 2) return;
  dialNumber[n] = digit;
  dialNumber[n + 1] = '\0';
  drawScreen();
}

void sendSms() {
  if (!smsRecipient[0] || !smsComposeText[0]) {
    strncpy(smsStatus, "Choose a contact and type text", sizeof(smsStatus) - 1);
    smsStatus[sizeof(smsStatus) - 1] = '\0'; drawScreen(); return;
  }
  if (!modemResponding || !simReady || !networkRegistered) {
    strncpy(smsStatus, "SIM/network is not ready", sizeof(smsStatus) - 1);
    smsStatus[sizeof(smsStatus) - 1] = '\0'; drawScreen(); return;
  }
  if (smsAwaitPrompt || smsSending) return;
  char command[40];
  snprintf(command, sizeof(command), "AT+CMGS=\"%s\"", smsRecipient);
  Serial1.print(command); Serial1.write('\r');
  Serial.print(F("> ")); Serial.println(command);
  smsAwaitPrompt = true;
  smsSending = false;
  smsActionStarted = millis();
  strncpy(smsStatus, "Waiting to send...", sizeof(smsStatus) - 1);
  smsStatus[sizeof(smsStatus) - 1] = '\0'; drawScreen();
}

void onTouch(int16_t x, int16_t y) {
  if (screen != NETWORK_STATUS && screen != CONTACT_ENTRY && screen != SMS_COMPOSE && screen != DELETE_CONFIRM &&
      screen != INCOMING && screen != IN_CALL && screen != TOUCH_CALIBRATION && y < 30 && x >= 233) {
    statusReturnScreen = screen;
    screen = NETWORK_STATUS;
    requestNetworkCheck();
    return;
  }
  if (screen == NETWORK_STATUS) {
    if (y >= 197 && x >= 218) { screen = statusReturnScreen; drawScreen(); }
    else if (y >= 197 && x >= 111 && x < 209) beginTouchCalibration();
    else if (y >= 197 && x < 102) requestNetworkCheck();
    return;
  }
  if (screen == TOUCH_CALIBRATION) {
    if (y >= 203 && x >= 242) { screen = statusReturnScreen; drawScreen(); return; }
    const int16_t targetX[3] = {15, 160, 304};
    const int16_t targetY = 120;
    if (abs(x - targetX[touchCalStage]) > 65 || abs(y - targetY) > 60) return;
    touchCalSamples[touchCalStage] = sampleRawTouchHorizontal();
    Serial.print(F("[TOUCH] target ")); Serial.print(touchCalStage + 1);
    Serial.print(F(" raw x=")); Serial.println(touchCalSamples[touchCalStage]);
    ++touchCalStage;
    if (touchCalStage < 3) { drawScreen(); return; }
    if (!(touchCalSamples[0] > touchCalSamples[1] &&
          touchCalSamples[1] > touchCalSamples[2] &&
          touchCalSamples[0] - touchCalSamples[1] > 20 &&
          touchCalSamples[1] - touchCalSamples[2] > 20)) {
      touchCalStage = 0;
      strncpy(touchCalMessage, "Retry: tap all 3 stars", sizeof(touchCalMessage) - 1);
      touchCalMessage[sizeof(touchCalMessage) - 1] = '\0';
      drawScreen();
      return;
    }
    touchRawLeft = touchCalSamples[0];
    touchRawMiddle = touchCalSamples[1];
    touchRawRight = touchCalSamples[2];
    saveTouchCalibration();
    Serial.print(F("[TOUCH] saved left/middle/right="));
    Serial.print(touchRawLeft); Serial.print('/');
    Serial.print(touchRawMiddle); Serial.print('/'); Serial.println(touchRawRight);
    screen = statusReturnScreen;
    drawScreen();
    return;
  }
  if (screen == HOME) {
    if (y >= 55 && y <= 95 && x < 160) {
      dialNumber[0] = '\0'; screen = DIALER; drawScreen();
    } else if (y >= 55 && y <= 95 && x >= 160) {
      contactForSms = false; contactPage = 0; screen = CONTACTS; drawScreen();
    } else if (y >= 100 && y <= 140 && x < 160) {
      screen = MESSAGES; refreshSmsInbox();
    } else if (y >= 100 && y <= 140 && x >= 160) {
      screen = MEDIA_MENU; drawScreen();
    } else if (y >= 145 && y <= 179) {
      screen = CALL_HISTORY; drawScreen();
    }
    return;
  }
  if (screen == CALL_HISTORY) {
    if (y >= 201) { screen = HOME; drawScreen(); }
    return;
  }
  if (screen == DIALER) {
    const int16_t kx[3] = {18, 84, 150}, ky[4] = {68, 106, 144, 182};
    const char keys[12] = {'1','2','3','4','5','6','7','8','9','*','0','#'};
    for (uint8_t i = 0; i < 12; ++i) {
      int16_t bx = kx[i % 3], by = ky[i / 3];
      if (x >= bx && x < bx + 54 && y >= by && y < by + 32) { addDigit(keys[i]); return; }
    }
    if (x >= 225 && x <= 307 && y >= 71 && y <= 116) startCall();
    else if (x >= 225 && x <= 307 && y >= 127 && y <= 164) {
      size_t n = strlen(dialNumber); if (n) dialNumber[n - 1] = '\0'; drawScreen();
    } else if (x >= 225 && x <= 307 && y >= 175 && y <= 212) { screen = HOME; drawScreen(); }
    return;
  }
  if (screen == CONTACTS) {
    if (y >= 37 && y < 187 && contactCount) {
      uint8_t row = (uint8_t)((y - 37) / 30);
      uint8_t i = contactPage * CONTACTS_PER_PAGE + row;
      if (row < CONTACTS_PER_PAGE && i < contactCount) {
        selectedContact = i; contactStatus[0] = '\0'; screen = CONTACT_ACTION; drawScreen();
      }
    } else if (y >= 197 && x < 80) { beginNewContact(); }
    else if (y >= 197 && x >= 188 && x < 248 && contactPage > 0) { --contactPage; drawScreen(); }
    else if (y >= 197 && x >= 252 && (contactPage + 1) * CONTACTS_PER_PAGE < contactCount) { ++contactPage; drawScreen(); }
    else if (y >= 197 && x >= 88 && x < 180) {
      screen = contactForSms ? MESSAGES : HOME; drawScreen();
    }
    return;
  }
  if (screen == CONTACT_ENTRY) {
    if (y >= 35 && y <= 58) { contactNumberField = false; contactNumericLayout = false; drawScreen(); return; }
    if (y >= 61 && y <= 84) { contactNumberField = true; contactNumericLayout = true; drawScreen(); return; }
    const char *rows[] = {contactNumericLayout ? "1234567890" : "QWERTYUIOP",
                          contactNumericLayout ? "@#$%&*-+/" : "ASDFGHJKL",
                          contactNumericLayout ? "()!?.,:" : "ZXCVBNM"};
    const uint8_t lengths[] = {10, 9, 7};
    const int16_t ys[] = {91, 120, 149};
    for (uint8_t r = 0; r < 3; ++r) {
      int16_t offset = (320 - lengths[r] * 29) / 2;
      if (y >= ys[r] && y < ys[r] + 26) {
        for (uint8_t c = 0; c < lengths[r]; ++c) {
          if (x >= offset + c * 29 && x < offset + c * 29 + 26) {
            char *target = contactNumberField ? contactNumberInput : contactNameInput;
            size_t limit = contactNumberField ? sizeof(contactNumberInput) - 1 : sizeof(contactNameInput) - 1;
            size_t n = strlen(target);
            if (n < limit) { target[n] = rows[r][c]; target[n + 1] = '\0'; }
            contactStatus[0] = '\0'; drawScreen(); return;
          }
        }
      }
    }
    if (y >= 181 && y <= 224) {
      if (x < 53) {
        char *target = contactNumberField ? contactNumberInput : contactNameInput;
        size_t limit = contactNumberField ? sizeof(contactNumberInput) - 1 : sizeof(contactNameInput) - 1;
        size_t n = strlen(target);
        if (n < limit) { target[n] = ' '; target[n + 1] = '\0'; }
        drawScreen();
      } else if (x < 101) { contactNumericLayout = !contactNumericLayout; drawScreen(); }
      else if (x < 149) {
        char *target = contactNumberField ? contactNumberInput : contactNameInput;
        size_t n = strlen(target); if (n) target[n - 1] = '\0';
        drawScreen();
      } else if (x < 228) saveNewContact();
      else { screen = CONTACTS; drawScreen(); }
    }
    return;
  }
  if (screen == CONTACT_ACTION) {
    if (selectedContact < contactCount && y >= 165 && y <= 211) {
      if (x < 78) {
        strncpy(dialNumber, contacts[selectedContact].number, sizeof(dialNumber) - 1);
        dialNumber[sizeof(dialNumber) - 1] = '\0'; startCall();
      } else if (x >= 82 && x < 155) beginSmsToContact(selectedContact);
      else if (x >= 159 && x < 232) askDelete(DELETE_CONTACT);
      else if (x >= 236) { screen = CONTACTS; drawScreen(); }
    }
    return;
  }
  if (screen == MEDIA_MENU) {
    if (y >= 94 && y <= 152 && x < 160) {
      screen = PHOTO;
      if (!sdMounted) drawPhotoPage("SD card is not ready");
      else if (!showPhotoBmp()) drawPhotoPage("Use 24-bit PHOTO.BMP (max 320 x 178)");
    } else if (y >= 94 && y <= 152 && x >= 160) { screen = MUSIC; drawScreen(); }
    else if (y >= 176 && y <= 214) { screen = HOME; drawScreen(); }
    return;
  }
  if (screen == MESSAGES) {
    if (y >= 50 && y < 190) {
      uint8_t row = (uint8_t)((y - 50) / 28);
      if (row < smsCount) { selectedSms = smsCount - 1 - row; screen = SMS_DETAIL; drawScreen(); }
    } else if (y >= 197 && x < 76) {
      contactForSms = true; contactPage = 0; screen = CONTACTS; drawScreen();
    } else if (y >= 197 && x < 154) { screen = SAVED_MESSAGES; drawScreen(); }
    else if (y >= 197 && x < 246) refreshSmsInbox();
    else if (y >= 197) { screen = HOME; drawScreen(); }
    return;
  }
  if (screen == SAVED_MESSAGES) {
    if (y >= 50 && y < 190) {
      uint8_t row = (uint8_t)((y - 50) / 28);
      if (row < savedMessageCount) {
        uint8_t i = savedMessageCount - 1 - row;
        selectedSavedMessage = i;
        if (x >= 268) { askDelete(DELETE_DRAFT); return; }
        strncpy(smsRecipient, savedMessages[i].sender, sizeof(smsRecipient) - 1);
        smsRecipient[sizeof(smsRecipient) - 1] = '\0';
        strncpy(smsComposeText, savedMessages[i].body, sizeof(smsComposeText) - 1);
        smsComposeText[sizeof(smsComposeText) - 1] = '\0';
        smsStatus[0] = '\0'; smsNumericLayout = false;
        composeReturnScreen = SAVED_MESSAGES; screen = SMS_COMPOSE; drawScreen();
      }
    } else if (y >= 197) { screen = MESSAGES; drawScreen(); }
    return;
  }
  if (screen == SMS_COMPOSE) {
    const char *rows[] = {smsNumericLayout ? "1234567890" : "QWERTYUIOP",
                          smsNumericLayout ? "@#$%&*-+/" : "ASDFGHJKL",
                          smsNumericLayout ? "()!?.,:" : "ZXCVBNM"};
    const uint8_t lengths[] = {10, 9, 7};
    const int16_t ys[] = {86, 117, 148};
    for (uint8_t r = 0; r < 3; ++r) {
      int16_t offset = (320 - lengths[r] * 29) / 2;
      if (y >= ys[r] && y < ys[r] + 26) {
        for (uint8_t c = 0; c < lengths[r]; ++c) {
          if (x >= offset + c * 29 && x < offset + c * 29 + 26) {
            size_t n = strlen(smsComposeText);
            if (n < sizeof(smsComposeText) - 1) { smsComposeText[n] = rows[r][c]; smsComposeText[n + 1] = '\0'; }
            smsStatus[0] = '\0'; drawScreen(); return;
          }
        }
      }
    }
    if (y >= 181 && y <= 224) {
      if (x < 53) {
        size_t n = strlen(smsComposeText);
        if (n < sizeof(smsComposeText) - 1) { smsComposeText[n] = ' '; smsComposeText[n + 1] = '\0'; }
        drawScreen();
      } else if (x < 99) {
        smsNumericLayout = !smsNumericLayout; drawScreen();
      } else if (x < 145) {
        size_t n = strlen(smsComposeText); if (n) smsComposeText[n - 1] = '\0'; drawScreen();
      } else if (x < 199) saveDraftToSd();
      else if (x < 253) sendSms();
      else { screen = composeReturnScreen; drawScreen(); }
    }
    return;
  }
  if (screen == SMS_DETAIL) {
    if (y >= 184 && y <= 226 && x < 105 && selectedSms < smsCount) {
      strncpy(smsRecipient, smsMessages[selectedSms].sender, sizeof(smsRecipient) - 1);
      smsRecipient[sizeof(smsRecipient) - 1] = '\0';
      smsComposeText[0] = '\0'; smsStatus[0] = '\0';
      smsNumericLayout = false;
      composeReturnScreen = SMS_DETAIL; screen = SMS_COMPOSE; drawScreen();
    } else if (y >= 184 && x >= 108 && x < 212 && selectedSms < smsCount) askDelete(DELETE_INBOX_SMS);
    else if (y >= 184 && x >= 215) { screen = MESSAGES; drawScreen(); }
    return;
  }
  if (screen == DELETE_CONFIRM) {
    if (y >= 174 && y <= 218 && x < 156) {
      DeleteTarget target = deleteTarget;
      deleteTarget = DELETE_NONE;
      if (target == DELETE_CONTACT) deleteContactFromSd();
      else if (target == DELETE_DRAFT) deleteDraftFromSd();
      else if (target == DELETE_INBOX_SMS) deleteInboxSms();
    } else if (y >= 174 && y <= 218 && x >= 164) {
      DeleteTarget target = deleteTarget;
      deleteTarget = DELETE_NONE;
      screen = target == DELETE_CONTACT ? CONTACT_ACTION :
               target == DELETE_DRAFT ? SAVED_MESSAGES : SMS_DETAIL;
      drawScreen();
    }
    return;
  }
  if (screen == INCOMING) {
    if (x >= 22 && x <= 148 && y >= 166 && y <= 216) answerCall();
    else if (x >= 172 && x <= 298 && y >= 166 && y <= 216) hangUp();
    return;
  }
  if (screen == IN_CALL && x >= 70 && x <= 250 && y >= 184 && y <= 228) { hangUp(); return; }
  if (screen == PHOTO && y >= 205) { screen = HOME; drawScreen(); return; }
  if (screen == MUSIC) {
    if (y >= 160 && y <= 208 && x >= 18 && x <= 150) startMusic();
    else if (y >= 160 && y <= 208 && x >= 170 && x <= 302) stopMusic();
    else if (y >= 211) { stopMusic(); screen = HOME; drawScreen(); }
  }
}

void configureModem() {
  if (modemConfigured) return;
  modemConfigured = true;
  sendAT("ATE0");
  sendAT("AT+CLIP=1");
  sendAT("AT+CREG=1");
  sendAT("AT+CPIN?");
  sendAT("AT+CREG?");
  sendAT("AT+CHFA=0");
  sendAT("AT+CLVL=100");
  sendAT("AT+CRSL=100");
  sendAT("AT+CMIC=0,8");
  sendAT("AT+CMGF=1");
  sendAT("AT+CSCS=\"GSM\"");
  sendAT("AT+CNMI=2,1,0,0,0");
}

void processModemLine(char *line) {
  while (*line == ' ' || *line == '\t') ++line;
  size_t n = strlen(line);
  while (n && (line[n - 1] == ' ' || line[n - 1] == '\t')) line[--n] = '\0';
  if (n) lastModemResponse = millis();
  if (!n) return;
  if (smsAwaitBody) {
    smsAwaitBody = false;
    storeSms(smsPendingSender, line, smsPendingStorageIndex, smsPendingStorage);
    smsPendingStorageIndex = -1;
    if (screen == MESSAGES) { smsStatus[0] = '\0'; drawMessages(); }
    return;
  }
  Serial.print(F("< ")); Serial.println(line);
  if (!strncmp(line, "+CPMS:", 6) && smsListQueued) {
    parseSmsStorage(line, smsListStorage, sizeof(smsListStorage));
    return;
  }
  if (!strncmp(line, "+CMGL:", 6)) {
    smsPendingStorageIndex = atoi(line + 6);
    strncpy(smsPendingStorage, smsListStorage, sizeof(smsPendingStorage) - 1);
    smsPendingStorage[sizeof(smsPendingStorage) - 1] = '\0';
    if (parseSmsHeader(line, smsPendingSender, sizeof(smsPendingSender))) smsAwaitBody = true;
    return;
  }
  if (!strncmp(line, "+CMGR:", 6)) {
    if (parseSmsHeader(line, smsPendingSender, sizeof(smsPendingSender))) smsAwaitBody = true;
    return;
  }
  if (!strncmp(line, "+CMTI:", 6)) {
    char *comma = strrchr(line, ',');
    if (comma) {
      parseSmsStorage(line, smsPendingStorage, sizeof(smsPendingStorage));
      smsPendingStorageIndex = atoi(comma + 1);
      smsReadQueued = true;
      smsActionStarted = millis();
      char command[40];
      snprintf(command, sizeof(command), "AT+CPMS=\"%s\",\"%s\",\"%s\"",
               smsPendingStorage, smsPendingStorage, smsPendingStorage);
      sendAT(command);
    }
    return;
  }
  if (!strncmp(line, "+CMGS:", 6)) {
    smsSending = false;
    strncpy(smsStatus, "SMS sent", sizeof(smsStatus) - 1);
    smsStatus[sizeof(smsStatus) - 1] = '\0';
    if (screen == SMS_COMPOSE) drawScreen();
    return;
  }
  if (!strcmp(line, "ERROR") && (smsAwaitPrompt || smsSending)) {
    smsAwaitPrompt = smsSending = false;
    strncpy(smsStatus, "SMS failed", sizeof(smsStatus) - 1);
    smsStatus[sizeof(smsStatus) - 1] = '\0';
    if (screen == SMS_COMPOSE) drawScreen();
    return;
  }
  if ((!strcmp(line, "ERROR") || !strncmp(line, "+CMS ERROR", 10)) &&
      (smsDeletePreparing || smsDeleteCommandPending)) {
    smsDeletePreparing = smsDeleteCommandPending = false;
    smsDeleteIndex = -1;
    strncpy(smsStatus, "Delete failed", sizeof(smsStatus) - 1);
    smsStatus[sizeof(smsStatus) - 1] = '\0';
    if (screen == MESSAGES) drawScreen();
    return;
  }
  if ((!strcmp(line, "ERROR") || !strncmp(line, "+CMS ERROR", 10)) && smsReadQueued) {
    smsReadQueued = false;
    smsPendingStorageIndex = -1;
    strncpy(smsStatus, "Could not read new SMS", sizeof(smsStatus) - 1);
    smsStatus[sizeof(smsStatus) - 1] = '\0';
    if (screen == MESSAGES) drawScreen();
    return;
  }
  if ((!strcmp(line, "ERROR") || !strncmp(line, "+CMS ERROR", 10)) && smsInboxLoading) {
    smsListQueued = false;
    smsInboxLoading = false;
    strncpy(smsStatus, "Inbox read failed", sizeof(smsStatus) - 1);
    smsStatus[sizeof(smsStatus) - 1] = '\0';
    if (screen == MESSAGES) drawScreen();
    return;
  }
  if (!strncmp(line, "+CMS ERROR", 10) && (smsAwaitPrompt || smsSending)) {
    smsAwaitPrompt = smsSending = false;
    strncpy(smsStatus, "SMS failed", sizeof(smsStatus) - 1);
    smsStatus[sizeof(smsStatus) - 1] = '\0';
    if (screen == SMS_COMPOSE) drawScreen();
    return;
  }
  if (!strcmp(line, "OK") || !strcmp(line, "RDY")) {
    modemResponding = true;
    configureModem();
  }
  if (!strcmp(line, "OK")) {
    if (smsDeletePreparing) {
      smsDeletePreparing = false;
      smsDeleteCommandPending = true;
      char command[24];
      snprintf(command, sizeof(command), "AT+CMGD=%d", smsDeleteIndex);
      sendAT(command);
      return;
    }
    if (smsDeleteCommandPending) { removeInboxSmsFromList(); return; }
    if (smsReadQueued) {
      smsReadQueued = false;
      char command[24];
      snprintf(command, sizeof(command), "AT+CMGR=%d", smsPendingStorageIndex);
      sendAT(command);
      return;
    }
    if (smsListQueued) {
      smsListQueued = false;
      sendAT("AT+CMGL=\"ALL\"");
      return;
    }
  }
  if (screen == NETWORK_STATUS && (!strcmp(line, "OK") || !strcmp(line, "RDY"))) networkStatusDirty = true;
  if (!strcmp(line, "OK") && smsInboxLoading) {
    smsInboxLoading = false;
    strncpy(smsStatus, smsCount ? "Inbox loaded" : "No messages", sizeof(smsStatus) - 1);
    smsStatus[sizeof(smsStatus) - 1] = '\0';
    if (screen == MESSAGES) drawScreen();
  }
  if (strstr(line, "+CPIN: READY")) {
    simReady = true; strncpy(simState, "READY", sizeof(simState) - 1);
  } else if (strstr(line, "+CPIN: SIM PIN")) {
    simReady = false; strncpy(simState, "PIN required", sizeof(simState) - 1);
  } else if (strstr(line, "+CPIN: NOT INSERTED")) {
    simReady = false; strncpy(simState, "Not inserted", sizeof(simState) - 1);
  } else if (strstr(line, "+CPIN:")) {
    simReady = false; strncpy(simState, "SIM not ready", sizeof(simState) - 1);
  }
  simState[sizeof(simState) - 1] = '\0';
  if (strstr(line, "+CPIN:")) {
    if (screen == NETWORK_STATUS) networkStatusDirty = true; else redrawCurrentHeader();
  }
  if (!strncmp(line, "+CREG:", 6)) {
    int mode = -1, status = -1;
    if (sscanf(line, "+CREG: %d,%d", &mode, &status) != 2) sscanf(line, "+CREG: %d", &status);
    registrationStatus = (int8_t)status;
    networkRegistered = status == 1 || status == 5;
    if (screen == NETWORK_STATUS) networkStatusDirty = true; else redrawCurrentHeader();
  }
  if (!strncmp(line, "+CSQ:", 5)) {
    int rssi = 99, ber = 99;
    if (sscanf(line, "+CSQ: %d,%d", &rssi, &ber) == 2) {
      signalRssi = (int16_t)rssi; signalBer = (int16_t)ber;
      if (screen == NETWORK_STATUS) networkStatusDirty = true;
    }
  }
  if (!strncmp(line, "+COPS:", 6)) {
    const char *q1 = strchr(line, '"');
    const char *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
    if (q1 && q2) {
      size_t length = (size_t)(q2 - q1 - 1);
      if (length >= sizeof(operatorName)) length = sizeof(operatorName) - 1;
      memcpy(operatorName, q1 + 1, length); operatorName[length] = '\0';
    } else {
      int mode = -1;
      if (sscanf(line, "+COPS: %d", &mode) == 1 && mode == 0)
        strncpy(operatorName, "Auto selection", sizeof(operatorName) - 1);
      else if (sscanf(line, "+COPS: %d", &mode) == 1 && mode == 1)
        strncpy(operatorName, "Manual selection", sizeof(operatorName) - 1);
      else
        strncpy(operatorName, "No operator data", sizeof(operatorName) - 1);
      operatorName[sizeof(operatorName) - 1] = '\0';
    }
    if (screen == NETWORK_STATUS) networkStatusDirty = true;
  }
  if (!strcmp(line, "RING") && screen != IN_CALL && screen != INCOMING) {
    if (audioRunning) stopMusic();
    callConnected = false;
    callWasConnected = false;
    callLogged = false;
    activeCallDirection = 'I';
    strncpy(callNumber, "Unknown", sizeof(callNumber) - 1);
    callNumber[sizeof(callNumber) - 1] = '\0';
    strncpy(callStatus, "Incoming call", sizeof(callStatus) - 1);
    callStatus[sizeof(callStatus) - 1] = '\0';
    screen = INCOMING;
    drawScreen();
  }
  if (!strncmp(line, "+CLIP:", 6)) {
    char *q1 = strchr(line, '"');
    if (q1) {
      char *q2 = strchr(q1 + 1, '"');
      if (q2) {
        size_t len = (size_t)(q2 - q1 - 1);
        if (len >= sizeof(callNumber)) len = sizeof(callNumber) - 1;
        memcpy(callNumber, q1 + 1, len); callNumber[len] = '\0';
        if (screen == INCOMING) drawScreen();
      }
    }
  }
  if (!strncmp(line, "+CLCC:", 6)) {
    int id, direction, state, mode, multiparty;
    if (sscanf(line, "+CLCC: %d,%d,%d,%d,%d", &id, &direction, &state, &mode, &multiparty) == 5) {
      if (state == 0 && !callConnected) {
        callConnected = true; callWasConnected = true; callConnectedAt = millis();
        lastCallClock = millis();
        strncpy(callStatus, "Connected", sizeof(callStatus) - 1);
        callStatus[sizeof(callStatus) - 1] = '\0'; screen = IN_CALL; drawScreen();
      } else if (screen == IN_CALL && !callConnected && state == 3) {
        strncpy(callStatus, "Ringing...", sizeof(callStatus) - 1);
        callStatus[sizeof(callStatus) - 1] = '\0'; drawScreen();
      } else if (state == 4 && screen != INCOMING) {
        strncpy(callNumber, "Unknown", sizeof(callNumber) - 1);
        callNumber[sizeof(callNumber) - 1] = '\0'; screen = INCOMING; drawScreen();
      }
    }
  }
  if (!strcmp(line, "CONNECT") || strstr(line, "+COLP:")) {
    if (!callConnected) { callConnectedAt = millis(); lastCallClock = millis(); }
    callConnected = true; callWasConnected = true;
    strncpy(callStatus, "Connected", sizeof(callStatus) - 1);
    callStatus[sizeof(callStatus) - 1] = '\0'; screen = IN_CALL; drawScreen();
  }
  if (strstr(line, "NO CARRIER") || !strcmp(line, "BUSY") || !strcmp(line, "NO ANSWER")) {
    bool wasInCallScreen = screen == IN_CALL || screen == INCOMING;
    if (activeCallDirection) finishActiveCall(NULL);
    callConnected = false;
    // Capture the network's release cause immediately; Serial Monitor prints +CEER.
    sendAT("AT+CEER");
    if (wasInCallScreen) { screen = HOME; drawScreen(); }
  }
  if ((!strcmp(line, "ERROR") || strstr(line, "+CME ERROR")) &&
      (screen == IN_CALL || screen == INCOMING)) {
    if (activeCallDirection) finishActiveCall(callWasConnected ? NULL : "FAILED");
    callConnected = false; screen = HOME; drawScreen();
    sendAT("AT+CEER");
  }
}

void readModem() {
  while (Serial1.available()) {
    char c = (char)Serial1.read();
    if (smsAwaitPrompt && c == '>') {
      Serial1.print(smsComposeText);
      Serial1.write(26); // Ctrl+Z sends the text-mode SMS.
      Serial.print(F("[SMS] submitting text: "));
      Serial.println(smsComposeText);
      smsAwaitPrompt = false;
      smsSending = true;
      smsActionStarted = millis();
      strncpy(smsStatus, "Sending...", sizeof(smsStatus) - 1);
      smsStatus[sizeof(smsStatus) - 1] = '\0';
      if (screen == SMS_COMPOSE) drawScreen();
      continue;
    }
    if (c == '\r') continue;
    if (c == '\n') {
      modemLine[modemLineLength] = '\0';
      processModemLine(modemLine);
      modemLineLength = 0;
    } else if (modemLineLength < sizeof(modemLine) - 1) modemLine[modemLineLength++] = c;
    else modemLineLength = 0;
  }
}

bool readTouch(int16_t &x, int16_t &y) {
  TSPoint p = ts.getPoint();
  pinMode(XM, OUTPUT); pinMode(YP, OUTPUT);
  if (p.z < MIN_PRESSURE || p.z > MAX_PRESSURE) return false;
  lastRawTouchX = p.y;
  x = mapTouchHorizontal(p.y);
  y = constrain(map(p.x, TS_TOP, TS_BOTTOM, 0, tft.height()), 0, tft.height() - 1);
  return true;
}

void sampleSoundSensor() {
  if (millis() - lastSoundSample < 100) return;
  lastSoundSample = millis();
  int value = analogRead(SOUND_SENSOR_PIN);
  int delta = abs(value - soundBaseline);
  soundBaseline = (soundBaseline * 7 + value) / 8;
  soundPeak = max(delta, soundPeak * 3 / 4);
  if (screen == HOME) {
    tft.fillRect(270, 215, 32, 4, C_PAPER);
    int bar = constrain(map(soundPeak, 0, 240, 0, 32), 0, 32);
    if (bar) tft.fillRect(270, 215, bar, 4, C_GREEN);
  }
}

void setupAudioTimers() {
  pinMode(AUDIO_PWM_PIN, OUTPUT);
  // Timer 5: 8-bit fast PWM on OC5A / D46, 62.5 kHz carrier.
  TCCR5A = _BV(COM5A1) | _BV(WGM50);
  TCCR5B = _BV(WGM52) | _BV(CS50);
  OCR5A = 128;
  // Timer 4: 8 kHz sample interrupt (16 MHz / 8 / (249 + 1)).
  TCCR4A = 0;
  TCCR4B = _BV(WGM42) | _BV(CS41);
  OCR4A = 249;
  TIMSK4 |= _BV(OCIE4A);
}

void setup() {
  Serial.begin(115200);
  Serial1.begin(9600); // Mega RX1=D19, TX1=D18; UART speed may need matching to the breakout.
  pinMode(SOUND_SENSOR_PIN, INPUT);
  setupAudioTimers();
  loadTouchCalibration();

  tft.reset();
  uint16_t id = tft.readID();
  if (id == 0xD3D3 || id == 0xFFFF || id == 0x0000) id = 0x9341;
  tft.begin(id);
  tft.setRotation(1);
  drawScreen();

  for (uint8_t attempt = 0; attempt < 3 && !sdMounted; ++attempt) {
    sdMounted = sd.begin(SD_CONFIG);
    if (!sdMounted) delay(250);
  }
  if (!sdMounted) {
    Serial.println(F("[SD] mount failed"));
    sd.initErrorPrint(&Serial);
  }
  else {
    Serial.println(F("[SD] card ready"));
    recoverSavedData();
  }
  loadContacts();
  loadSavedMessages();
  loadCallHistory();
  lastAtPing = millis() - 1400;
}

void loop() {
  readModem();
  int16_t x, y;
  bool pressed = readTouch(x, y);
  if (!pressed) touchLatched = false;
  else if (!touchLatched) { touchLatched = true; onTouch(x, y); }

  uint32_t now = millis();
  if (screen == NETWORK_STATUS && now - lastNetworkScreenDraw >= 2000UL) drawNetworkStatus();
  if (!modemResponding && now - lastAtPing >= 1500) { lastAtPing = now; sendAT("AT"); }
  if (modemResponding && lastModemResponse && now - lastModemResponse > 15000UL &&
      !smsAwaitPrompt && !smsSending && !smsInboxLoading && !smsDeletePreparing &&
      !smsDeleteCommandPending && !smsReadQueued && screen != IN_CALL && screen != INCOMING) {
    modemResponding = false;
    networkRegistered = false;
    registrationStatus = -1;
    signalRssi = 99;
    strncpy(simState, "Not checked", sizeof(simState) - 1);
    if (screen == NETWORK_STATUS) networkStatusDirty = true; else redrawCurrentHeader();
  }
  if (screen == IN_CALL && callConnected && now - lastCallClock >= 1000) {
    lastCallClock = now; drawCallClock();
  }
  if ((screen == IN_CALL || screen == INCOMING) && now - lastCallQuery >= 1800) {
    lastCallQuery = now; sendAT("AT+CLCC");
  }
  uint32_t networkPollInterval = screen == NETWORK_STATUS ? 5000UL : 10000UL;
  if (modemResponding && now - lastNetworkQuery >= networkPollInterval && screen != IN_CALL &&
      screen != INCOMING && screen != MESSAGES && screen != SMS_COMPOSE &&
      !smsInboxLoading && !smsAwaitPrompt && !smsSending && !smsDeletePreparing &&
      !smsDeleteCommandPending && !smsReadQueued && !smsListQueued) {
    lastNetworkQuery = now;
    sendAT("AT+CPIN?"); sendAT("AT+CREG?"); sendAT("AT+CSQ");
  }
  if ((smsAwaitPrompt || smsSending) && now - smsActionStarted > (smsAwaitPrompt ? 15000UL : 60000UL)) {
    smsAwaitPrompt = smsSending = false;
    strncpy(smsStatus, "SMS timed out", sizeof(smsStatus) - 1);
    smsStatus[sizeof(smsStatus) - 1] = '\0';
    if (screen == SMS_COMPOSE) drawScreen();
  }
  if ((smsDeletePreparing || smsDeleteCommandPending) && now - smsActionStarted > 15000UL) {
    smsDeletePreparing = smsDeleteCommandPending = false;
    smsDeleteIndex = -1;
    strncpy(smsStatus, "Delete timed out", sizeof(smsStatus) - 1);
    smsStatus[sizeof(smsStatus) - 1] = '\0';
    if (screen == MESSAGES) drawScreen();
  }
  if (smsReadQueued && now - smsActionStarted > 15000UL) {
    smsReadQueued = false;
    smsPendingStorageIndex = -1;
    strncpy(smsStatus, "SMS read timed out", sizeof(smsStatus) - 1);
    smsStatus[sizeof(smsStatus) - 1] = '\0';
    if (screen == MESSAGES) drawScreen();
  }
  if (smsInboxLoading && now - smsActionStarted > 20000UL) {
    smsInboxLoading = smsListQueued = false;
    strncpy(smsStatus, "Inbox timed out", sizeof(smsStatus) - 1);
    smsStatus[sizeof(smsStatus) - 1] = '\0';
    if (screen == MESSAGES) drawScreen();
  }
  if (audioRunning) {
    fillAudioRing();
    if (audioEof && audioHead == audioTail) stopMusic();
  }
  sampleSoundSensor();
}
