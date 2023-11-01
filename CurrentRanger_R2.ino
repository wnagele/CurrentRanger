// *************************************************************************************************************
// CurrentRanger(TM) stock firmware
// https://lowpowerlab.com/CurrentRanger
// CurrentRanger is a high-side precision current meter featuring:
//   - autoranging
//   - uni/bi-directional modes (ie. DC/AC measurements)
//   - ultra low burden voltage
//   - 1mV per nA/uA/mA measurements with DMM/scope
//   - OLED standalone readings
//   - bluetooth data logging option via 3.3v/RX/TX header
//   - full digital control for power/switching
//   - LiPo powered with auto power-off feature (0.6uA quiescent current)
// *************************************************************************************************************
#ifndef CURRENT_RANGER
  #error You need to choose CurrentRanger board as target, see guide for details how to add it to the IDE.
#endif
//***********************************************************************************************************
#include <SAMD_AnalogCorrection.h> //for analogReadCorrection, comes with ArduinoIDE
#include <FlashStorage.h>          //for emulated EEPROM - https://github.com/cmaglie/FlashStorage
#include <Adafruit_FreeTouch.h>    //https://github.com/adafruit/Adafruit_FreeTouch
#include <U8g2lib.h>               //https://github.com/olikraus/u8g2/wiki/u8g2reference fonts:https://github.com/olikraus/u8g2/wiki/fntlistall
//***********************************************************************************************************
#define OFFSET_LED     11
#define LPFPIN         4
#define LPFLED         LED_BUILTIN
#define MA             38
#define UA             2
#define NA             5
#define AUTOFF         PIN_AUTO_OFF
char rangeUnit = 'm';
uint32_t lastRangeChange=0;
//***********************************************************************************************************
#define SENSE_OUTPUT           A3
#define SENSE_GNDISO           A2
#define SENSE_VIN              A5
#define ADCREADINGS            1   //do averaging in hardware rather than software
#define ADC_PRESCALER          ADC_CTRLB_PRESCALER_DIV16
#define ADC_AVGCTRL            ADC_AVGCTRL_SAMPLENUM_256 | ADC_AVGCTRL_ADJRES(0x4ul)
#define ADCFULLRANGE           4095.0
#define VBATREADLOOPS          100  //read vbat every this many OLED_REFRESH_INTERVAL loops
#define LOBAT_THRESHOLD        3.40 //volts
#define DAC_GND_ISO_OFFSET     10
#define DAC_HALF_SUPPLY_OFFSET 512
#define OUTPUT_CALIB_FACTOR    1.00  //calibrate final VOUT value
#define ADC_OVERLOAD           3900  //assuming GNDISO DAC output is very close to 0, this is max value less ground offset (varies from unit to unit, 3900 is a safe value)
//***********************************************************************************************************
#define ADC_CALIBRATE_EN
#define ADC_CALIBRATE_FORCED          //uncomment to set manual offset/gain values below
#define ADC_CALIBRATE_FORCED_OFFSET 15
#define ADC_CALIBRATE_FORCED_GAIN   2056
#define LDO_OUTPUT                  3.311 //volts, change to actual LDO output (measure GND-3V on OLED header)
#define ANALOG_REFERENCE            AR_DEFAULT //AR_EXTERNAL
//***********************************************************************************************************
#define BUZZER     1           // BUZZER pin
#define NOTE_C5  523
#define NOTE_D5  587
#define NOTE_E5  659
#define NOTE_F5  698
#define NOTE_G5  784
#define NOTE_B5  988
#define NOTE_C6  1047
#define TONE_BEEP 4200
//***********************************************************************************************************
#define MODE_MANUAL 0
#define MODE_AUTORANGE 1
#define STARTUP_MODE        MODE_MANUAL //MODE_AUTORANGE
#define SWITCHDELAY_UP      8 //ms
#define SWITCHDELAY_DOWN    8 //ms
#define RANGE_SWITCH_THRESHOLD_HIGH ADC_OVERLOAD //ADC's 12bit value
#define RANGE_SWITCH_THRESHOLD_LOW  0
//***********************************************************************************************************
#include <Wire.h>
//i2c scanner: https://playground.arduino.cc/Main/I2cScanner
#define OLED_ADDRESS  0x3C //i2c address on most small OLEDs
#define OLED_REFRESH_INTERVAL 200 //ms
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);
byte OLED_found=false;
//***********************************************************************************************************
#define TOUCH_N        8
#define TOUCH_U        9
#define TOUCH_M        A4
Adafruit_FreeTouch qt[3] = {
  Adafruit_FreeTouch( TOUCH_N, OVERSAMPLE_8, RESISTOR_20K, FREQ_MODE_HOP ),
  Adafruit_FreeTouch( TOUCH_U, OVERSAMPLE_8, RESISTOR_20K, FREQ_MODE_HOP ),
  Adafruit_FreeTouch( TOUCH_M, OVERSAMPLE_8, RESISTOR_20K, FREQ_MODE_HOP ),
};
#define TOUCH_HIGH_THRESHOLD  600   //range is 0..1023
#define MA_PRESSED      qt[2].measure()>TOUCH_HIGH_THRESHOLD
#define MA_NOT_PRESSED  !(MA_PRESSED)
#define UA_PRESSED      qt[1].measure()>TOUCH_HIGH_THRESHOLD
#define UA_NOT_PRESSED  !(UA_PRESSED)
#define NA_PRESSED      qt[0].measure()>TOUCH_HIGH_THRESHOLD
#define NA_NOT_PRESSED  !(NA_PRESSED)
//***********************************************************************************************************
#define SERIAL_UART_BAUD    230400      //Serial baud for HC-06/bluetooth output
#define BT_EN
#define LOGGER_FORMAT_EXPONENT  //ex: 123E-3 = 123mA
//#define LOGGER_FORMAT_NANOS     //ex: 123456 = 123456nA = 123.456uA
//#define LOGGER_FORMAT_ADC       //raw ADC output
#define BT_REFRESH_INTERVAL 200 //ms
#define AUTOFF_DEFAULT  600     //seconds, turn unit off after 10min of inactivity
//***********************************************************************************************************
int offsetCorrectionValue = 0;
uint16_t gainCorrectionValue = 0;
float ldoValue = 0;
uint32_t AUTOFF_INTERVAL = 0;
uint8_t USB_LOGGING_ENABLED = false;
uint8_t BT_LOGGING_ENABLED = true;
byte calibrationPerformed=false;
#ifdef BT_EN
  byte BT_found=false;
#endif
FlashStorage(eeprom_ADCoffset, int);
FlashStorage(eeprom_ADCgain, uint16_t);
FlashStorage(eeprom_LDO, float);
FlashStorage(eeprom_AUTOFF, uint16_t);
//***********************************************************************************************************
#define AUTOFFBUZZDELAY 500
byte AUTOOFFBUZZ=0;
uint32_t autoOffBuzzInterval=0;
byte autoffWarning=false;
//***********************************************************************************************************

void setup() {
  Serial.begin(1); //USB speed
/*
  //some buzz
  tone(BUZZER, NOTE_C5); delay(100);
  tone(BUZZER, NOTE_E5); delay(100);
  tone(BUZZER, NOTE_G5); delay(100);
  tone(BUZZER, NOTE_C6); delay(200);
  noTone(BUZZER);        delay(50);
  tone(BUZZER, NOTE_G5); delay(100);
  tone(BUZZER, NOTE_C6); delay(400);
  noTone(BUZZER);
*/

  delay(50); //Wire apparently needs this
  Wire.begin();
  Wire.beginTransmission(OLED_ADDRESS);
  byte error = Wire.endTransmission();
  if (error == 0)
  {
    Serial.print("OLED FOUND at 0x"); Serial.println(OLED_ADDRESS);
    u8g2.begin();
    //u8g2.setDisplayRotation(U8G2_R2); //if required (inside/custom mount?)
    u8g2.setBusClock(1000000); //1Mhz i2C clock
    OLED_found = true;
  }
  else Serial.println("NO OLED found...");

  pinMode(A0, OUTPUT); //DAC/GNDISO
  //DAC->CTRLB.bit.EOEN = 0x00; //enable high drive strength - already done in wiring.c

  pinMode(SENSE_OUTPUT, INPUT);
  pinMode(SENSE_GNDISO, INPUT); //GND-ISO
  pinMode(SENSE_VIN, INPUT); //VIN > 1MEG > SENSE_VIN > 2MEG > GND
  pinMode(AUTOFF, INPUT_PULLUP);
  pinMode(OFFSET_LED, OUTPUT);
  pinMode(LPFLED, OUTPUT); //STATUS/LPF-LED
  pinMode(LPFPIN, OUTPUT); //LPF control pin
  pinMode(BUZZER, OUTPUT);
  pinMode(MA,OUTPUT);
  pinMode(UA,OUTPUT);
  pinMode(NA,OUTPUT);

  qt[0].begin(); qt[1].begin(); qt[2].begin(); //touch pads
  analogReadResolution(12);
  analogWriteResolution(10);  //DAC resolution
  analogReference(ANALOG_REFERENCE);

  //DAC->CTRLA.bit.RUNSTDBY = 0x01;delay(1);
  //DAC->CTRLB.bit.REFSEL=0;//pick internal reference, skip SYNCDAC (done by analogWrite)
  analogWrite(A0, DAC_GND_ISO_OFFSET);  // Initialize Dac to OFFSET

  AUTOFF_INTERVAL = eeprom_AUTOFF.read();
  if (AUTOFF_INTERVAL==0) {
    AUTOFF_INTERVAL = AUTOFF_DEFAULT;
    eeprom_AUTOFF.write(AUTOFF_INTERVAL);
  }

#ifdef ADC_CALIBRATE_EN
  #ifndef ADC_CALIBRATE_FORCED
    adcCorrectionCheck();
  #else
    offsetCorrectionValue = eeprom_ADCoffset.read();
    gainCorrectionValue = eeprom_ADCgain.read();
    ldoValue = eeprom_LDO.read();
    if (gainCorrectionValue!=0) //check if anything saved in EEPROM (gain changed via SerialUSB +/-)
      analogReadCorrectionForced(offsetCorrectionValue, gainCorrectionValue);
    else
      //hardcoded:
      analogReadCorrectionForced(ADC_CALIBRATE_FORCED_OFFSET, ADC_CALIBRATE_FORCED_GAIN);
      //(offset, gain) - gain is 12 bit number (1 bit integer + 11bit fractional, see DS p895)
      //               - offset is 12bit 2s complement format (p896)
    if (ldoValue == 0)
    {
      ldoValue = LDO_OUTPUT; //set default forced value
      eeprom_LDO.write(ldoValue);
    }
  #endif

  if (OLED_found /*&& !calibrationPerformed && MA_PRESSED*/)
  {
    u8g2.clearBuffer();
    Serial.println("ADC calib. values:");
    Serial.print("Offset="); Serial.println(offsetCorrectionValue);
    Serial.print("Gain="); Serial.println(gainCorrectionValue);
    Serial.print("LDO="); Serial.println(ldoValue);
    u8g2.setFont(u8g2_font_8x13B_tf);
    u8g2.setCursor(0,24); u8g2.print("CurrentRanger");
    u8g2.setFont(u8g2_font_6x12_tf);
    u8g2.setCursor(0,28); u8g2.print("Offset:");
    u8g2.setCursor(64,28); u8g2.print(offsetCorrectionValue);
    u8g2.setCursor(0,42); u8g2.print("Gain  :");
    u8g2.setCursor(64,42); u8g2.print(gainCorrectionValue);
    u8g2.setCursor(0,56); u8g2.print("LDO   :");
    u8g2.setCursor(64,56); u8g2.print(ldoValue,3);
    u8g2.sendBuffer();
    delay(2000);
  }
#endif

#ifdef BT_EN
  //BT check
  Serial.print("Bluetooth AT check @");Serial.print(SERIAL_UART_BAUD);Serial.print("baud...");
  SerialBT.begin(SERIAL_UART_BAUD);
  SerialBT.print("AT"); //assuming HC-06, no line ending required
  uint32_t timer=millis();
  while(millis()-timer<1000) //about 1s to respond
  {
    if (SerialBT.available()==2 && SerialBT.read()=='O' && SerialBT.read()=='K')
    {
      BT_found=true;
      break;
    }
  }

  Serial.print(BT_found?"OK!":"No response.\r\nChecking for version 3.0...");

  if (!BT_found)
  {
    SerialBT.print("\r\n"); //assuming HC-06 version 3.0 that requires line ending
    uint32_t timer=millis();
    while(millis()-timer<50) //about 50ms to respond
    {
      if (SerialBT.available()==4 && SerialBT.read()=='O' && SerialBT.read()=='K' && SerialBT.read()=='\r' && SerialBT.read() == '\n')
      {
        BT_found=true;
        break;
      }
    }
  
    Serial.println(BT_found?"OK!":"No response.");
  }
#endif

  printSerialMenu();

  //rangeMA(); //done in bootloader
  WDTset();

/*
  // ADC Linearity/Bias Calibration from NVM (should already be done done in core)
  uint32_t bias = (*((uint32_t *) ADC_FUSES_BIASCAL_ADDR) & ADC_FUSES_BIASCAL_Msk) >> ADC_FUSES_BIASCAL_Pos;
  uint32_t linearity = (*((uint32_t *) ADC_FUSES_LINEARITY_0_ADDR) & ADC_FUSES_LINEARITY_0_Msk) >> ADC_FUSES_LINEARITY_0_Pos;
  linearity |= ((*((uint32_t *) ADC_FUSES_LINEARITY_1_ADDR) & ADC_FUSES_LINEARITY_1_Msk) >> ADC_FUSES_LINEARITY_1_Pos) << 5;
  ADC->CALIB.reg = ADC_CALIB_BIAS_CAL(bias) | ADC_CALIB_LINEARITY_CAL(linearity);
*/

  if (STARTUP_MODE == MODE_AUTORANGE)
    toggleAutoranging();
}

uint32_t oledInterval=0, lpfInterval=0, offsetInterval=0, autorangeInterval=0, btInterval=0;
byte LPF=0, OFFSET=0, AUTORANGE=0;
byte readVbatLoop=0;
float vbat=0;
float read1=0,read2=0,readDiff=0;
bool rangeSwitched=false;

#define RANGE_MA rangeUnit=='m'
#define RANGE_UA rangeUnit=='u'
#define RANGE_NA rangeUnit=='n'

void rangeBeep(uint16_t switch_delay=0)
{
  uint16_t freq = NOTE_C5;
  if (RANGE_UA) freq = NOTE_D5;
  if (RANGE_MA) freq = NOTE_E5;
  if (switch_delay==0)
    tone(BUZZER, freq, 20);
  else {
    tone(BUZZER, freq);
    delay(switch_delay);
    noTone(BUZZER);
  }
}

uint32_t timestamp=0;
void loop()
{
  timestamp = millis();
  if (Serial.available()>0)
  {
    char inByte = Serial.read();
    switch (inByte)
    {
      case '+':
        eeprom_ADCgain.write(++gainCorrectionValue);
        analogReadCorrection(offsetCorrectionValue,gainCorrectionValue);
        Serial.print("new gainCorrectionValue = ");
        Serial.println(gainCorrectionValue);
        break;
      case '-':
        eeprom_ADCgain.write(--gainCorrectionValue);
        analogReadCorrection(offsetCorrectionValue,gainCorrectionValue);
        Serial.print("new gainCorrectionValue = ");
        Serial.println(gainCorrectionValue);
        break;
      case '<':
        ldoValue -= 0.001;
        eeprom_LDO.write(ldoValue);
        Serial.print("new LDO_Value = ");
        Serial.println(ldoValue, 3);
        break;
      case '>':
        ldoValue += 0.001;
        eeprom_LDO.write(ldoValue);
        Serial.print("new LDO_Value = ");
        Serial.println(ldoValue, 3);
        break;
      case 'u': //toggle USB logging
        USB_LOGGING_ENABLED =! USB_LOGGING_ENABLED;
        Serial.println(USB_LOGGING_ENABLED ? "USB_LOGGING_ENABLED" : "USB_LOGGING_DISABLED");
        break;
      case 'b': //toggle BT/serial logging
        BT_LOGGING_ENABLED =! BT_LOGGING_ENABLED;
        Serial.println(BT_LOGGING_ENABLED ? "BT_LOGGING_ENABLED" : "BT_LOGGING_DISABLED");
        break;
      case 'a': //toggle autoOff function
        if (AUTOFF_INTERVAL == AUTOFF_DEFAULT)
        {
          Serial.println("AUTO_OFF_DISABLED");
          AUTOFF_INTERVAL = 0xFFFFFF;
        }
        else
        {
          Serial.println("AUTO_OFF_ENABLED");
          AUTOFF_INTERVAL = AUTOFF_DEFAULT;
        }
        eeprom_AUTOFF.write(AUTOFF_INTERVAL);
        break;
      default: break;
    }
  }

  WDTclear(); //keep the dog happy
  handleTouchPads();
  handleAutoOff();

  if (AUTORANGE)
  {
    readVOUT();
    //assumes we only auto-range in DC mode (no bias)
    if (readDiff <= RANGE_SWITCH_THRESHOLD_LOW)
    {
      if      (RANGE_MA) { rangeUA(); rangeSwitched=true; rangeBeep(SWITCHDELAY_DOWN); }
      //else if (RANGE_UA) { rangeNA(); rangeSwitched=true; rangeBeep(SWITCHDELAY_DOWN); }
    }
    else if (readDiff >= RANGE_SWITCH_THRESHOLD_HIGH)
    {
      if      (RANGE_NA) { rangeUA(); rangeSwitched=true; rangeBeep(SWITCHDELAY_UP); }
      else if (RANGE_UA) { rangeMA(); rangeSwitched=true; rangeBeep(SWITCHDELAY_UP); }
    }
    if (rangeSwitched) {
      lastRangeChange=millis();
      rangeSwitched=false;
      return; //!!!
    }
  }

  float VOUT;

if (USB_LOGGING_ENABLED)
{//TODO: refactor
  if (!AUTORANGE) readVOUT();
  VOUT = ((readDiff)/ADCFULLRANGE)*ldoValue*1000*(OFFSET?1:OUTPUT_CALIB_FACTOR);
#if defined LOGGER_FORMAT_ADC
    Serial.println(readDiff,0);
#elif defined LOGGER_FORMAT_EXPONENT
    Serial.print(VOUT); Serial.print("e"); Serial.println(RANGE_NA ? -9 : RANGE_UA ? -6 : -3);
#elif defined LOGGER_FORMAT_NANOS
    Serial.println(VOUT * (RANGE_NA ? 1 : RANGE_UA ? 1000 : 1000000));
#endif
}

#ifdef BT_EN
  if (BT_found && BT_LOGGING_ENABLED && millis() - btInterval > BT_REFRESH_INTERVAL) //refresh rate (ms)
  {
    btInterval = millis();
    if (!AUTORANGE) readVOUT();
    VOUT = ((readDiff)/ADCFULLRANGE)*ldoValue*1000*(OFFSET?1:OUTPUT_CALIB_FACTOR);
#if defined LOGGER_FORMAT_ADC
    SerialBT.println(readDiff,0);
#elif defined LOGGER_FORMAT_EXPONENT
    SerialBT.print(VOUT); SerialBT.print("e"); SerialBT.println(RANGE_NA ? -9 : RANGE_UA ? -6 : -3);
#elif defined LOGGER_FORMAT_NANOS
    SerialBT.println(VOUT * (RANGE_NA ? 1 : RANGE_UA ? 1000 : 1000000));
#endif
  }
#endif

  if (OLED_found && millis() - oledInterval > OLED_REFRESH_INTERVAL) //refresh rate (ms)
  {
    oledInterval = millis();
    readVOUT();
    float VOUT = ((readDiff)/ADCFULLRANGE)*ldoValue*1000*(OFFSET?1:OUTPUT_CALIB_FACTOR);

    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x10_tf);

    //limit how often we read the battery since it's not expected to change a lot
    if (readVbatLoop==0) {
      vbat=adcRead(SENSE_VIN);
      vbat=((vbat/ADCFULLRANGE) * ldoValue) * 1.5; //1.5 given by vbat->A5 resistor ratio
    }
    if (readVbatLoop == 100) readVbatLoop=0;
    else readVbatLoop++;

    if (vbat < LOBAT_THRESHOLD) u8g2.drawStr(88,12,"LoBat!");
    else {
      u8g2.drawStr(106,12,"vBat");
      u8g2.setCursor(80,12); u8g2.print(vbat); //VIN
    }

    if (AUTOOFFBUZZ) //autoffWarning
        u8g2.drawStr(5,26,"* AUTO OFF! *");

    //float VOUTDIFF = (readDiff/ADCFULLRANGE)*ldoValue*1000; //*(OFFSET?1:OUTPUT_CALIB_FACTOR)
    if (AUTORANGE)
    {
      //u8g2.setFontMode(0);
      //u8g2.setDrawColor(0);
      u8g2.drawStr(0,12, "AUTO");
      u8g2.setCursor(0,24);
      u8g2.print(readDiff,0);
    }
    else
    {
      u8g2.setCursor(0,12);
      u8g2.print(readDiff,0);
    }

    u8g2.setFont(u8g2_font_helvB24_te);
    u8g2.setCursor(106,64); u8g2.print('A');
    u8g2.setCursor(rangeUnit=='m'?102:106,38); u8g2.print(rangeUnit=='u'?char('µ'):rangeUnit);
    u8g2.setFont(u8g2_font_logisoso32_tr);
    u8g2.setCursor(0,64); u8g2.print((OFFSET&&abs(VOUT)>=1||!OFFSET&&VOUT>=1)?VOUT:0, abs(VOUT)>=1000?0:1); //diff
    if (!OFFSET && readDiff>ADC_OVERLOAD || OFFSET && abs(readDiff)>ADC_OVERLOAD/2)
    {
      u8g2.setFont(u8g2_font_9x15B_tf);
      u8g2.drawStr(0,28, "OVERLOAD!");
    }
    u8g2.sendBuffer();

    //Serial.print(adcRead(A1));
    //Serial.print(" - ");
    //Serial.println(adcRead(A6));
  }

  //Serial.println(uint32_t(AUTOFF_INTERVAL)*1000);
  //Serial.println(millis()-timestamp);
} //loop()

uint32_t buttonLastChange_range;
uint16_t valM=0, valU=0, valN=0;
void handleTouchPads() {
  if (millis() - buttonLastChange_range < 200) return;
  if (MA_PRESSED || UA_PRESSED || NA_PRESSED) lastRangeChange=millis();

  //range switching
  if (!AUTORANGE)
  {
    if (MA_PRESSED && UA_NOT_PRESSED && NA_NOT_PRESSED && rangeUnit!='m') { rangeMA(); rangeBeep(); }
    if (UA_PRESSED && MA_NOT_PRESSED && NA_NOT_PRESSED && rangeUnit!='u') { rangeUA(); rangeBeep(); }
    if (NA_PRESSED && UA_NOT_PRESSED && MA_NOT_PRESSED && rangeUnit!='n') { rangeNA(); rangeBeep(); }
  }
  
  //LPF activation --- [NA+UA]
  if (UA_PRESSED && NA_PRESSED && MA_NOT_PRESSED && millis()-lpfInterval>1000) { toggleLPF(); Beep(3, false); }

  //offset toggling (GNDISO to half supply) --- [MA+UA]
  if (MA_PRESSED && UA_PRESSED && NA_NOT_PRESSED && millis()-offsetInterval>1000) { toggleOffset(); Beep(3, false); }

  //AUTORANGE toggling
  if (MA_PRESSED && NA_PRESSED && UA_NOT_PRESSED && millis()-autorangeInterval>1000) { toggleAutoranging(); Beep(20, false); delay(50); (20, false); }
}

void rangeMA() {
  rangeUnit='m';
  digitalWrite(MA,HIGH);
  digitalWrite(UA,LOW);
  digitalWrite(NA,LOW);
#ifdef BT_OUTPUT_ADC
  if (BT_found) SerialBT.println("RANGE: MA");
#endif
}

void rangeUA() {
  rangeUnit='u';
  digitalWrite(UA,HIGH);
  digitalWrite(MA,LOW);
  digitalWrite(NA,LOW);
#ifdef BT_OUTPUT_ADC
  if (BT_found) SerialBT.println("RANGE: UA");
#endif
}

void rangeNA() {
  rangeUnit='n';
  digitalWrite(NA,HIGH);
  digitalWrite(MA,LOW);
  digitalWrite(UA,LOW);
#ifdef BT_OUTPUT_ADC
  if (BT_found) SerialBT.println("RANGE: NA");
#endif
}

void handleAutoOff() {
  if (millis() - lastRangeChange > uint32_t(AUTOFF_INTERVAL)*1000-5000)
  {
    autoffWarning = true;

    if (millis()-autoOffBuzzInterval> AUTOFFBUZZDELAY)
    {
      autoOffBuzzInterval = millis();
      AUTOOFFBUZZ=!AUTOOFFBUZZ;

      if (AUTOOFFBUZZ)
        tone(BUZZER, NOTE_B5);
      else
        noTone(BUZZER);
    }

    if (millis() - lastRangeChange > uint32_t(AUTOFF_INTERVAL)*1000)
    {
      pinMode(AUTOFF, OUTPUT);
      digitalWrite(AUTOFF, LOW);
    }
  }
  else if (autoffWarning) { autoffWarning=false; digitalWrite(AUTOFF, HIGH); noTone(BUZZER); }
}

void toggleLPF() {
  LPF=!LPF;
  lpfInterval = millis();
  digitalWrite(LPFPIN, LPF);
  digitalWrite(LPFLED, LPF);
  if (AUTORANGE && !LPF) toggleAutoranging(); //turn off AUTORANGE
}

void toggleOffset() {
  OFFSET=!OFFSET;
  offsetInterval = millis();
  analogWrite(A0, (OFFSET ? DAC_HALF_SUPPLY_OFFSET : DAC_GND_ISO_OFFSET));
  digitalWrite(OFFSET_LED, OFFSET);
  if (AUTORANGE && OFFSET) toggleAutoranging(); //turn off AUTORANGE
}

void toggleAutoranging() {
  autorangeInterval = millis();
  AUTORANGE=!AUTORANGE;
  if (AUTORANGE && OFFSET) toggleOffset(); //turn off OFFSET
  if (AUTORANGE && !LPF) toggleLPF(); //turn on OFFSET
}

void Beep(byte theDelay, boolean twoSounds) {
  //if (theDelay > 20) theDelay = 20;
  tone(BUZZER, TONE_BEEP, theDelay);
  if (twoSounds)
  {
    delay(10);
    tone(BUZZER, 4500, theDelay);
  }
}

#define ADCSYNC while (ADC->STATUS.bit.SYNCBUSY)
int adcRead(byte ADCpin)
{
  ADC->CTRLA.bit.ENABLE = 0;              // disable ADC
  ADCSYNC;

  int CTRLBoriginal = ADC->CTRLB.reg;
  int AVGCTRLoriginal = ADC->AVGCTRL.reg;
  int SAMPCTRLoriginal = ADC->SAMPCTRL.reg;

  ADC->CTRLB.reg &= 0b1111100011111111;          // mask PRESCALER bits
  ADC->CTRLB.reg |= ADC_PRESCALER;   // divide Clock by PRESCALER

  //ADC->AVGCTRL.reg = ADC_AVGCTRL_SAMPLENUM_1 | ADC_AVGCTRL_ADJRES(0x00ul);  // take 1 sample, adjusting result by 0
  //ADC->AVGCTRL.reg = ADC_AVGCTRL_SAMPLENUM_16 | ADC_AVGCTRL_ADJRES(0x4ul); //take 16 samples adjust by 4
  //ADC->AVGCTRL.reg = ADC_AVGCTRL_SAMPLENUM_256 | ADC_AVGCTRL_ADJRES(0x4ul); //take 256 samples adjust by 4
  //ADC->AVGCTRL.reg = ADC_AVGCTRL_SAMPLENUM_512 | ADC_AVGCTRL_ADJRES(0x4ul); //take 512 samples adjust by 4
  //ADC->AVGCTRL.reg = ADC_AVGCTRL_SAMPLENUM_1024 | ADC_AVGCTRL_ADJRES(0x4ul); //take 1024 samples adjust by 4
  ADC->AVGCTRL.reg = ADC_AVGCTRL;

  //sample timing (0 .. 0b111);
  ADC->SAMPCTRL.reg = 0b100; //0x00 fastest

  ADC->CTRLA.bit.ENABLE = 1;  // enable ADC
  ADCSYNC;

  int adc = analogRead(ADCpin); 

  ADC->CTRLB.reg = CTRLBoriginal;
  ADC->AVGCTRL.reg = AVGCTRLoriginal;
  ADC->SAMPCTRL.reg = SAMPCTRLoriginal;

  return adc;
}

void readVOUT() {
  read1=0,read2=0;
  for (byte i=0;i<ADCREADINGS;i++)
  {
    read1+=adcRead(SENSE_GNDISO);
    read2+=adcRead(SENSE_OUTPUT);
    //readDiff+=analogDifferentialRaw(0x1C, 0x04); //DAC-AIN4 (mux_pos,uint8_t mux_neg)
    //readDiff+=analogDifferentialRaw(0x04, 0x03); //AIN4-AIN3
    //readDiff+=analogDifferential(SENSE_OUTPUT,SENSE_GNDISO);
  }

  read1/=ADCREADINGS;
  read2/=ADCREADINGS;
  readDiff=read2-read1;
}

//***********************************************************************************************************
//ADC OFFSET/GAIN CALIBRATION - adapted from SAMD_AnalogCorrection>CorrectADCResponse Example
// calibration runs automatically ONCE after unit is (re)programmed, and stores calib values in EEPROM
// (EEPROM is emulated and gets erased when reflashing SAMD21)
//***********************************************************************************************************
#define ADC_GND_PIN          A6
#define ADC_3V3_PIN          A1
#define ADC_READS_SHIFT      8
#define ADC_READS_COUNT      (1 << ADC_READS_SHIFT)
#define ADC_MIN_GAIN         0x0400
#define ADC_UNITY_GAIN       0x0800
#define ADC_MAX_GAIN         (0x1000 - 1)
#define ADC_RESOLUTION_BITS  12
#define ADC_RANGE            (1 << ADC_RESOLUTION_BITS)
#define ADC_TOP_VALUE        (ADC_RANGE - 1)
#define MAX_TOP_VALUE_READS  10

void adcCorrectionCheck() {
  offsetCorrectionValue = eeprom_ADCoffset.read();
  gainCorrectionValue = eeprom_ADCgain.read();

  if (offsetCorrectionValue==0 && gainCorrectionValue==0)
  {
    if (OLED_found)
    {
      u8g2.clearBuffer();
      u8g2.setFont(u8g2_font_9x15B_tf);
      u8g2.setCursor(0,12); u8g2.print("ADC CALIB...");
      u8g2.sendBuffer();
    }
    delay(1000);
    Serial.println("Starting ADC Calibration...");
    gainCorrectionValue = ADC_UNITY_GAIN;
    calibrateADC();
  }
  else
  {
    analogReadCorrection(offsetCorrectionValue, gainCorrectionValue);
  }
}

void analogReadCorrectionForced(int offset, uint16_t gain) {
  offsetCorrectionValue=offset;
  gainCorrectionValue=gain;
  analogReadCorrection(offset,gain);
}

void calibrateADC() {
  calibrationPerformed=true;
  Serial.println("\r\nCalibrating ADC with factory values");
  analogReadResolution(ADC_RESOLUTION_BITS);
  Serial.println("\r\nReading GND and 3.3V ADC levels");
  Serial.print("   ");
  readGndLevel();
  Serial.print("   ");
  read3V3Level();
  Serial.print("\r\nOffset correction (@gain = ");
  Serial.print(gainCorrectionValue);
  Serial.println(" (unity gain))");

  // Set default correction values and enable correction
  analogReadCorrection(offsetCorrectionValue, gainCorrectionValue);

  for (int offset = 0; offset < (int)(ADC_OFFSETCORR_MASK >> 1); ++offset)
  {
    analogReadCorrection(offset, gainCorrectionValue);
    Serial.print("   Offset = ");
    Serial.print(offset);
    Serial.print(", ");

    if (readGndLevel() == 0)
    {
      offsetCorrectionValue = offset;
      break;
    }
  }

  Serial.println("\r\nGain correction");
  uint8_t topValueReadsCount = 0U;
  uint16_t minGain = 0U, maxGain = 0U;
  analogReadCorrection(offsetCorrectionValue, gainCorrectionValue);
  Serial.print("   Gain = ");
  Serial.print(gainCorrectionValue);
  Serial.print(", ");
  uint16_t highLevelRead = read3V3Level();
  
  if (highLevelRead < ADC_TOP_VALUE)
  {
    for (uint16_t gain = ADC_UNITY_GAIN + 1; gain <= ADC_MAX_GAIN; ++gain)
    {
      analogReadCorrection(offsetCorrectionValue, gain);
      Serial.print("   Gain = ");
      Serial.print(gain);
      Serial.print(", ");
      highLevelRead = read3V3Level();

      if (highLevelRead == ADC_TOP_VALUE)
      {
        if (minGain == 0U) minGain = gain;
        if (++topValueReadsCount >= MAX_TOP_VALUE_READS)
        {
          maxGain = minGain;
          break;
        }
        maxGain = gain;
      }
      if (highLevelRead > ADC_TOP_VALUE) break;
    }
  }
  else if (highLevelRead >= ADC_TOP_VALUE)
  {
    if (highLevelRead == ADC_TOP_VALUE) maxGain = ADC_UNITY_GAIN;
    for (uint16_t gain = ADC_UNITY_GAIN - 1; gain >= ADC_MIN_GAIN; --gain)
    {
      analogReadCorrection(offsetCorrectionValue, gain);
      Serial.print("   Gain = ");
      Serial.print(gain);
      Serial.print(", ");
      highLevelRead = read3V3Level();

      if (highLevelRead == ADC_TOP_VALUE)
      {
        if (maxGain == 0U) maxGain = gain;
        minGain = gain;
      }
      Serial.print("* LOOP : minGain="); Serial.print( minGain ); Serial.print(" maxGain="); Serial.println( maxGain );
      if (highLevelRead < ADC_TOP_VALUE)
      {
        if (minGain == 0U) minGain = maxGain;
        break;
      }
    }
  }

  gainCorrectionValue = (minGain + maxGain) >> 1;
  analogReadCorrection(offsetCorrectionValue, gainCorrectionValue);

  //save values to EEPROM
  eeprom_ADCoffset.write(offsetCorrectionValue);
  eeprom_ADCgain.write(gainCorrectionValue);

  if (OLED_found)
  {
    u8g2.clearBuffer();
    Serial.println("ADC Calib done. Values:");
    Serial.print("Offset="); Serial.println(offsetCorrectionValue);
    Serial.print("Gain="); Serial.println(gainCorrectionValue);
    u8g2.setFont(u8g2_font_9x15B_tf);
    u8g2.setCursor(0,12); u8g2.print("ADC CALIB...");
    u8g2.setCursor(0,28); u8g2.print("DONE:");
    u8g2.setCursor(0,40); u8g2.print("offset:");
    u8g2.setCursor(64,40); u8g2.print(offsetCorrectionValue);
    u8g2.setCursor(0,54); u8g2.print("gain  :");
    u8g2.setCursor(64,54); u8g2.print(gainCorrectionValue);
    u8g2.sendBuffer();
    delay(3000);
  }
}

uint16_t readGndLevel() {
  uint32_t readAccumulator = 0;
  for (int i = 0; i < ADC_READS_COUNT; ++i)
    readAccumulator += analogRead(ADC_GND_PIN);
  uint16_t readValue = readAccumulator >> ADC_READS_SHIFT;
  Serial.print("ADC(GND) = ");
  Serial.println(readValue);
  return readValue;
}

uint16_t read3V3Level()
{
  uint32_t readAccumulator = 0;
  for (int i = 0; i < ADC_READS_COUNT; ++i)
    readAccumulator += analogRead(ADC_3V3_PIN);
  uint16_t readValue = readAccumulator >> ADC_READS_SHIFT;
  if (readValue < (ADC_RANGE >> 1)) readValue += ADC_RANGE;
  Serial.print("ADC(3.3V) = ");
  Serial.println(readValue);
  return readValue;
}

void WDTset() {
  // Generic clock generator 2, divisor = 32 (2^(DIV+1))
  GCLK->GENDIV.reg = GCLK_GENDIV_ID(2) | GCLK_GENDIV_DIV(4);
  // Enable clock generator 2 using low-power 32KHz oscillator. With /32 divisor above, this yields 1024Hz(ish) clock.
  GCLK->GENCTRL.reg = GCLK_GENCTRL_ID(2) | GCLK_GENCTRL_GENEN | GCLK_GENCTRL_SRC_OSCULP32K | GCLK_GENCTRL_DIVSEL;
  while(GCLK->STATUS.bit.SYNCBUSY);
  // WDT clock = clock gen 2
  GCLK->CLKCTRL.reg = GCLK_CLKCTRL_ID_WDT | GCLK_CLKCTRL_CLKEN | GCLK_CLKCTRL_GEN_GCLK2;

  WDT->CTRL.reg = 0; //disable WDT
  while(WDT->STATUS.bit.SYNCBUSY);
  WDT->INTENCLR.bit.EW   = 1;      //disable early warning
  WDT->CONFIG.bit.PER    = 0x7;    //period ~1s
  WDT->CTRL.bit.WEN      = 0;      //disable window mode
  while(WDT->STATUS.bit.SYNCBUSY);
  WDTclear();
  WDT->CTRL.bit.ENABLE = 1;        //enable WDT
  while(WDT->STATUS.bit.SYNCBUSY);
}

void WDTclear(){
  WDT->CLEAR.reg = WDT_CLEAR_CLEAR_KEY;
  while(WDT->STATUS.bit.SYNCBUSY);
}

void printSerialMenu() {
  if (OLED_found /*&& !calibrationPerformed && MA_PRESSED*/)
  {
    Serial.println("\r\nUSB serial commands:");
    Serial.println("a = toggle Auto-Off function");
    Serial.print  ("b = toggle BT/serial logging (");Serial.print(SERIAL_UART_BAUD);Serial.println("baud)");
    Serial.println("u = toggle USB/serial logging");
    Serial.println("< = Calibrate LDO value (-1mV)");
    Serial.println("> = Calibrate LDO value (+1mV)");
    Serial.println("- = Calibrate GAIN value (-1)");
    Serial.println("+ = Calibrate GAIN value (+1)");
    Serial.println();
  }
}
