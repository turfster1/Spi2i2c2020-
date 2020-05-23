#include "xeniumspi.h" // modded from ryan's source
#include "xboxsmbus.h"
#include "src/SMWire/SMWire.h"
#include "src/i2c-oled/I2cCharDisplay.h" //modified oled library
#include "config.h"

I2cCharDisplay oled2004(OLED_TYPE, OLEDPRIMARY, 4);  //first and second oled constructor
I2cCharDisplay oledsec(OLED_TYPE, OLEDSECONDARY, 4);

const uint8_t ss_in = 17, miso = 14, mosi = 16, sck = 15, ss_out = A2; //SPI pin numbers, ss_in is the CS for the slave. As Xenium doesn't seem to use CS, ss_out is connected to ss_in on the PCB to manually trigger CS.
const uint8_t i2c_sda = 2, i2c_scl = 3; //i2c pins for SMBus
uint8_t cursorPosCol = 0, cursorPosRow = 0; //Track the position of the cursor
uint8_t wrapping = 0, scrolling = 0; //Xenium spi command toggles for the lcd screen.

//SPI Data
int16_t RxQueue[256]; //Input FIFO buffer for raw SPI data from Xenium
uint8_t QueuePos; //Tracks the current position in the FIFO queue that is being processed
uint8_t QueueRxPos; //Tracks the current position in the FIFO queue of the unprocessed input data (raw realtime SPI data)
uint8_t SPIState; //SPI State machine flag to monitor the SPI bus state can = SPI_ACTIVE, SPI_IDLE, SPI_SYNC, SPI_WAIT
uint32_t SPIIdleTimer; //Tracks how long the SPI bus has been idle for

//I2C Bus
uint32_t SMBusTimer; //Timer used to trigger SMBus reads
uint8_t i2cCheckCount = 0;      //Tracks what check we're up to of the i2c bus busy state
uint8_t I2C_BUSY_CHECKS = 5;  //To ensure we don't interfere with the actual Xbox's SMBus activity, we check the bus for activity for sending.

//SPI Bus Receiver Interrupt Routine
ISR (SPI_STC_vect) {
  RxQueue[QueueRxPos] = SPDR;
  QueueRxPos++; //This is an unsigned 8 bit variable, so will reset back to 0 after 255 automatically
  SPIState = SPI_ACTIVE;
}
int spiTimeLimiter = 10000;
int SaverActive = 200; //dumb value for the screensaver initialiser active=1 deactivated=0
int bitcount = 0 ; // bits counter for the smc chip string
char smcArray[3];  // smc chip data array
String smcString;  // smc value string
int FOCUS = 0;     // focus chip detection status 1=detected 0=not
int CONEXANT = 0;  // same as above
int shiftTimer = 0; // numerical value for the screen scroll

void setup() {
  pinMode(2, INPUT_PULLUP); // pull-up resistor for i2c lines
  pinMode(3, INPUT_PULLUP); //same as above
  pinMode(ss_out, OUTPUT);
  digitalWrite(ss_out, HIGH); //Force SPI CS signal high while we setup
  delay(5);
  memset(RxQueue, -1, 256);

  //I put my logic analyser on the Xenium SPI bus to confirm the bus properties.
  //The master clocks at ~16kHz. SPI Clock is high when inactive, data is valid on the trailing edge (CPOL/CPHA=1. Also known as SPI mode 3)
  SPCR |= _BV(SPE);   //Turn on SPI. We don't set the MSTR bit so it's slave.
  SPCR |= _BV(SPIE);  //Enable to SPI Interrupt Vector
  SPCR |= _BV(CPOL);  //SPI Clock is high when inactive
  SPCR |= _BV(CPHA);  //Data is Valid on Clock Trailing Edge

  Wire.begin(0xDD); //Random address that is different from existing bus devices.
  spiTimeLimiter = 500;
  oled2004.begin();   //main screen boot animation
  oled2004.cursorOff();
  oled2004.displayOn();
  oled2004.setBrightness(255);
  oled2004.cursorMove(1, 1);
  oled2004.print("OpenXenium OLED");
  oled2004.cursorMove(2, 1);
  oled2004.print("   Edition By  ");
  oled2004.cursorMove(3, 1);
  oled2004.print(" Luc Francoeur ");
  oled2004.cursorMove(4, 1);
  oled2004.print("Thx to RYZEE119");
  oled2004.displayShiftRight();
  delay(200);
  oled2004.displayShiftRight();
  delay(200);
  oled2004.displayShiftRight();
  delay(200);
  oled2004.displayShiftRight();
  delay(200);
  oled2004.displayShiftLeft();
  delay(200);
  oled2004.displayShiftLeft();
  delay(200);
  oled2004.displayShiftLeft();
  delay(200);
  oled2004.displayShiftLeft();
  delay(200);
  oled2004.cursorMove(1, 1);
  oledsec.begin();
  oledsec.cursorOff();; // secondary oled boot msg
  oledsec.displayOn();
  oledsec.cursorMove(1, 1);
  oledsec.print("   Waiting   for  ");
  oledsec.cursorMove(2, 1);
  oledsec.print("  Hardware   Data ");
  oledsec.cursorMove(1, 1);

  TWBR = ((F_CPU / 100000) - 16) / 2; //Change I2C frequency closer to OG Xbox SMBus speed. ~100kHz Not compulsory really, but a safe bet

  //Speed up PWM frequency.
  TCCR1B &= 0b11111000;
  TCCR1B |= (1 << CS00); //Change Timer Prescaler for PWM

  oled2004.cursorMove(1, 1); // be sure that bopth screen cursor are in the correct spot before looping
  oledsec.cursorMove(1, 1);

  uint32_t spiReadyTimer = millis();
  bool spiReady = false;

  //Hold until spi bus is ready
  while (!spiReady) {
    if (digitalRead(sck) == 0) {
      spiReadyTimer = millis();
    }
    //SPI Clock high for 100ms
    else if (millis() - spiReadyTimer > 100) {
      spiReady = true;
    }
  }
  digitalWrite(ss_out, LOW); //SPI Slave is ready to receive data

  oled2004.clear(); // cleaning up the primary oled for xeniumOS or xbmc data
}


void loop() {
  // screen scrolling routine
  shiftTimer++;
  if (shiftTimer == 10000) { // lower value mean faster scroll
    if (primaryBurnIn == "enable" ) {
      oled2004.displayShiftLeft();
    }
    if (secondaryBurnIn == "enable" ) {
      oledsec.displayShiftLeft();
    }
    shiftTimer == 0;
  }

  //SPI to Parallel Conversion State Machine
  //One completion of processing command, set the buffer data value to -1
  //to indicate processing has been completed.

  if (QueueRxPos != QueuePos) {
    switch (RxQueue[(uint8_t)QueuePos]) {
      case -1:
        //No action required.
        break;

      case XeniumCursorHome: // bring back the cursor to 1,1
        cursorPosRow = 0;
        cursorPosCol = 0;
        oled2004.home();
        oledsec.home();
        completeCommand(&RxQueue[(uint8_t)QueuePos]);
        break;

      case XeniumHideDisplay: // turn off oled
        oled2004.displayOff();
        oledsec.displayOff();
        completeCommand(&RxQueue[(uint8_t)QueuePos]);
        break;

      case XeniumShowDisplay:  // turn on oled
        oled2004.displayOn();
        oledsec.displayOn();
        completeCommand(&RxQueue[(uint8_t)QueuePos]);
        break;

      case XeniumHideCursor:  // turn off cursor
        oled2004.cursorOff();
        oledsec.cursorOff();
        completeCommand(&RxQueue[(uint8_t)QueuePos]);
        break;

      case XeniumShowUnderLineCursor:
        completeCommand(&RxQueue[(uint8_t)QueuePos]);
        break;
      case XeniumShowBlockCursor:
        completeCommand(&RxQueue[(uint8_t)QueuePos]);
        break;
      case XeniumShowInvertedCursor:
        completeCommand(&RxQueue[(uint8_t)QueuePos]);
        break;

      case XeniumBackspace:
        if (cursorPosCol > 0) {
          cursorPosCol--;
          oled2004.cursorMove((cursorPosRow + 1), (cursorPosCol + 1));
          oled2004.print(" ");
          oled2004.cursorMove((cursorPosRow + 1), (cursorPosCol + 1));
          oledsec.cursorMove((cursorPosRow + 1), (cursorPosCol + 1));
          oledsec.print(" ");
          oledsec.cursorMove((cursorPosRow + 1), (cursorPosCol + 1));
        }
        completeCommand(&RxQueue[(uint8_t)QueuePos]);
        break;

      case XeniumLineFeed: //Move Cursor down one row, but keep column
        if (cursorPosRow < 3) {
          cursorPosRow++;
          oled2004.cursorMove((cursorPosRow + 1), (cursorPosCol + 1));
          oledsec.cursorMove((cursorPosRow + 1), (cursorPosCol + 1));
        }
        completeCommand(&RxQueue[(uint8_t)QueuePos]);
        break;

      case XeniumDeleteInPlace: //Delete the character at the current cursor position
        oled2004.print(" ");
        oled2004.cursorMove((cursorPosRow + 1), (cursorPosCol + 1));
        oledsec.print(" ");
        oledsec.cursorMove((cursorPosRow + 1), (cursorPosCol + 1));
        completeCommand(&RxQueue[(uint8_t)QueuePos]);
        break;

      case XeniumFormFeed: //Formfeed just clears the screen and resets the cursor.
        cursorPosRow = 0;
        cursorPosCol = 0;
        oled2004.clear();
        oled2004.cursorMove((cursorPosRow + 1), (cursorPosCol + 1));
        oledsec.clear();
        oledsec.cursorMove((cursorPosRow + 1), (cursorPosCol + 1));
        completeCommand(&RxQueue[(uint8_t)QueuePos]);
        break;

      case XeniumCarriageReturn: //Carriage returns moves the cursor to the start of the current line
        cursorPosCol = 0;
        oled2004.cursorMove((cursorPosRow + 1), (cursorPosCol + 1));
        oledsec.cursorMove((cursorPosRow + 1), (cursorPosCol + 1));
        completeCommand(&RxQueue[(uint8_t)QueuePos]);
        break;

      case XeniumSetCursorPosition: //Sets the row and column of cursor. The following two bytes are the row and column.
        if (RxQueue[(uint8_t)(QueuePos + 2)] != -1) {
          uint8_t col = RxQueue[(uint8_t)(QueuePos + 1)]; //Column
          uint8_t row = RxQueue[(uint8_t)(QueuePos + 2)]; //Row
          if (col < 20 && row < 4) {
            oled2004.cursorMove((row + 1), (col + 1));
            oledsec.cursorMove((row + 1), (col + 1));
            cursorPosCol = col, cursorPosRow = row;
          }
          completeCommand(&RxQueue[(uint8_t)QueuePos]);
          completeCommand(&RxQueue[(uint8_t)(QueuePos + 1)]);
          completeCommand(&RxQueue[(uint8_t)(QueuePos + 2)]);
        }
        break;

      case XeniumSetBacklight:
        //The following byte after the backlight command is the brightness value
        //Value is 0-100 for the backlight brightness. 0=OFF, 100=ON
        //AVR PWM Output is 0-255. We multiply by 2.55 to match AVR PWM range.
        if (RxQueue[(uint8_t)(QueuePos + 1)] != -1) { //ensure the command is complete
          uint8_t brightness = RxQueue[(uint8_t)(QueuePos + 1)];
          if (brightness >= 0 && brightness <= 100) {
            oled2004.setBrightness( (uint8_t)(brightness * 2.55f)); //0-255 for AVR PWM
            oledsec.setBrightness( (uint8_t)(brightness * 2.55f)); //0-255 for AVR PWM
          }
          completeCommand(&RxQueue[(uint8_t)QueuePos]);
          completeCommand(&RxQueue[(uint8_t)(QueuePos + 1)]);
        }
        break;

      case XeniumSetContrast:  //no contrast control implemented in the library and not needed on oled screens
        if (RxQueue[(uint8_t)(QueuePos + 1)] != -1) { //ensure the command is complete
          uint8_t contrastValue = 100 - RxQueue[(uint8_t)(QueuePos + 1)]; //needs to convert to 100-0 instead of 0-100.
          if (contrastValue >= 0 && contrastValue <= 100) {
            //no contrast control implemented in the library and not needed on oled screens
          }
          completeCommand(&RxQueue[(uint8_t)QueuePos]);
          completeCommand(&RxQueue[(uint8_t)(QueuePos + 1)]);
        }
        break;

      case XeniumReboot:
        cursorPosRow = 0;
        cursorPosCol = 0;
        oled2004.clear();
        oledsec.clear();
        completeCommand(&RxQueue[(uint8_t)QueuePos]);
        break;

      case XeniumCursorMove:
        //The following 2 bytes after the initial command is the direction to move the cursor
        //offset+1 is always 27, offset+2 is 65,66,67,68 for Up,Down,Right,Left
        if (RxQueue[(uint8_t)(QueuePos + 1)] == 27 &&
            RxQueue[(uint8_t)(QueuePos + 2)] != -1) {

          switch (RxQueue[(uint8_t)(QueuePos + 2)]) {
            case 65: //UP
              if (cursorPosRow > 0) cursorPosRow--;
              break;
            case 66: //DOWN
              if (cursorPosRow < 3) cursorPosRow++;
              break;
            case 67: //RIGHT
              if (cursorPosCol < 19) cursorPosCol++;
              break;
            case 68: //LEFT
              if (cursorPosCol > 0) cursorPosCol--;
              break;
            default:
              //Error: Invalid cursor direction
              break;
          }
          oled2004.cursorMove((cursorPosRow + 1), (cursorPosCol + 1));
          oledsec.cursorMove((cursorPosRow + 1), (cursorPosCol + 1));
          completeCommand(&RxQueue[(uint8_t)QueuePos]);
          completeCommand(&RxQueue[(uint8_t)(QueuePos + 1)]);
          completeCommand(&RxQueue[(uint8_t)(QueuePos + 2)]);
        }
        break;

      //Scrolling and wrapping commands are handled here.
      //The flags are toggled, but are not implemented properly yet
      //My testing seems to indicates it's not really needed.
      case XeniumWrapOff:
        wrapping = 0;
        oled2004.displayShiftOff();
        oledsec.displayShiftOff();
        completeCommand(&RxQueue[(uint8_t)QueuePos]);
        break;

      case XeniumWrapOn:
        wrapping = 1;
        oled2004.displayShiftOn();
        oledsec.displayShiftOn();
        completeCommand(&RxQueue[(uint8_t)QueuePos]);
        break;

      case XeniumScrollOff:
        scrolling = 0;
        completeCommand(&RxQueue[(uint8_t)QueuePos]);
        break;

      case XeniumScrollOn:
        scrolling = 1;
        completeCommand(&RxQueue[(uint8_t)QueuePos]);
        break;

      case  32 ... 64: //Just an ASCII character
        if (cursorPosCol < 20) {
          oled2004.cursorMove((cursorPosRow + 1), (cursorPosCol + 1));
          oled2004.write((char)RxQueue[(uint8_t)QueuePos]);
          cursorPosCol++;
        }
        completeCommand(&RxQueue[(uint8_t)QueuePos]);
        break;

      case  65 ... 123: //Just an ASCII character
        if (cursorPosCol < 20) {
          oled2004.cursorMove((cursorPosRow + 1), (cursorPosCol + 1));
          oled2004.write((char)RxQueue[(uint8_t)QueuePos]);
          cursorPosCol++;
          if (SaverActive = 1) {
            oledsec.displayOn();
            oled2004.displayOn();
            SaverActive = 0;
          }
        }
        completeCommand(&RxQueue[(uint8_t)QueuePos]);
        break;

      case  125 ... 255: //Just an ASCII character
        if (cursorPosCol < 20) {
          oled2004.cursorMove((cursorPosRow + 1), (cursorPosCol + 1));
          oled2004.write((char)RxQueue[(uint8_t)QueuePos]);
          cursorPosCol++;
        }
        completeCommand(&RxQueue[(uint8_t)QueuePos]);
        break;

      //Not implemented. Dont seem to be used anyway.
      case XeniumLargeNumber:
        completeCommand(&RxQueue[(uint8_t)QueuePos]);
        break;
      case XeniumDrawBarGraph:
        completeCommand(&RxQueue[(uint8_t)QueuePos]);
        break;
      case XeniumModuleConfig:
        completeCommand(&RxQueue[(uint8_t)QueuePos]);
        break;
      case XeniumCustomCharacter:
        completeCommand(&RxQueue[(uint8_t)QueuePos]);
        break;

      case XeniumSSaver:
        oled2004.clear();
        oled2004.home();
        oled2004.displayOff();
        oledsec.clear();
        oledsec.home();
        oledsec.displayOff();
        SaverActive = 1;
        completeCommand(&RxQueue[(uint8_t)QueuePos]);
        break;

      default:
        completeCommand(&RxQueue[(uint8_t)QueuePos]);
        break;
    }
    if (RxQueue[(uint8_t)QueuePos] == -1) {
      QueuePos = (uint8_t)(QueuePos + 1);
    }
  }

  /* State machine to monitor the SPI Bus idle state */
  //If SPI bus has been idle pulse the CS line to resync the bus.
  //Xenium SPI bus doesnt use a Chip select line to resync the bus so this is a bit hacky, but improved reliability
  if (SPIState == SPI_ACTIVE) {
    SPIState = SPI_IDLE;
    SPIIdleTimer = millis();

  } else if (SPIState == SPI_IDLE && (millis() - SPIIdleTimer) > 100) {
    SPIState = SPI_SYNC;
    digitalWrite(ss_out, HIGH);
    SPIIdleTimer = millis();

  } else if (SPIState == SPI_SYNC && (millis() - SPIIdleTimer) > 25) {
    digitalWrite(ss_out, LOW);
    SPIState = SPI_WAIT;
  }

  /* Read data from Xbox System Management Bus */
  //Check that i2cBus is free, it has been atleast 2 seconds since last call,
  //and the SPI Bus has been idle for >10 seconds (i.e in a game or app that doesnt support LCD)
  if (i2cBusy() == 250 && (millis() - SMBusTimer)   > 2000 && (millis() - SPIIdleTimer) >> spiTimeLimiter)  {
    //oledsec.displayShiftLeft();
    char rxBuffer[20];    //Raw data received from SMBus
    char lineBuffer[20]; //Fomatted data for printing to LCD

    //Read the current fan speed directly from the SMC and print to LCD
    if (readSMBus(SMC_ADDRESS, SMC_FANSPEED, &rxBuffer[0], 1) == 0) {
      if (rxBuffer[0] >= 0 && rxBuffer[0] <= 50) { //Sanity check. Number should be 0-50
        if ((mainScreenType == "us2066" ) && (secScreenType == "NONE" )) {
          snprintf(lineBuffer, sizeof lineBuffer, "FAN:%u%% ", rxBuffer[0] * 2);
          oled2004.cursorMove(3, 1);
          oled2004.print(lineBuffer);
        }
        if (secScreenType == "us2066" ) {
          snprintf(lineBuffer, sizeof lineBuffer, "FAN:%u%% ", rxBuffer[0] * 2);
          oledsec.cursorMove(1, 1);
          oledsec.print(lineBuffer);
        }
      }
    }

    //Read Focus Chip to determine video resolution (for Version 1.4 console only)
    if (readSMBus(FOCUS_ADDRESS, FOCUS_PID, &rxBuffer[0], 2) == 0) {
      uint16_t PID = ((uint16_t)rxBuffer[1]) << 8 | rxBuffer[0];
      if (PID == 0xFE05) {
        readSMBus(FOCUS_ADDRESS, FOCUS_VIDCNTL, &rxBuffer[0], 2);
        uint16_t VID_CNTL0 = ((uint16_t)rxBuffer[1]) << 8 | rxBuffer[0];
        FOCUS == 1;
        if (secScreenType == "us2066" ) {
          oledsec.cursorMove(4, 1);
          oledsec.print("GPU:Focus ");
          oledsec.cursorMove(1, 9);
        }
        if (VID_CNTL0 & FOCUS_VIDCNTL_VSYNC5_6 && VID_CNTL0 & FOCUS_VIDCNTL_INT_PROG) {
          //Must be HDTV, interlaced (1080i)
          if ((mainScreenType == "us2066" ) && (secScreenType == "NONE" )) {
            oled2004.print("  1080i   ");
          }
          if (secScreenType == "us2066" ) {
            oledsec.print("  RES:1080i");
          }
        } else if (VID_CNTL0 & FOCUS_VIDCNTL_VSYNC5_6 && !(VID_CNTL0 & FOCUS_VIDCNTL_INT_PROG)) {
          //Must be HDTV, Progressive 720p
          if ((mainScreenType == "us2066" ) && (secScreenType == "NONE" )) {
            oled2004.print("  720p    ");
          }
          if (secScreenType == "us2066" ) {
            oledsec.print("  RES:720p ");
          }
        } else if (!(VID_CNTL0 & FOCUS_VIDCNTL_VSYNC5_6) && VID_CNTL0 & FOCUS_VIDCNTL_INT_PROG) {
          //Must be SDTV, interlaced 480i
          if ((mainScreenType == "us2066" ) && (secScreenType == "NONE" )) {
            oled2004.print("  480i    ");
          }
          if (secScreenType == "us2066" ) {
            oledsec.print("  RES:480i ");
          }
        } else if (!(VID_CNTL0 & FOCUS_VIDCNTL_VSYNC5_6) && !(VID_CNTL0 & FOCUS_VIDCNTL_INT_PROG)) {
          //Must be SDTV, Progressive 480p
          if ((mainScreenType == "us2066" ) && (secScreenType == "NONE" )) {
            oled2004.print("  480p    ");
          }
          if (secScreenType == "us2066" ) {
            oledsec.print("  RES:480p ");
          }
        } else {
          if ((mainScreenType == "us2066" ) && (secScreenType == "NONE" )) {
            oled2004.print("  0x");
            oled2004.print(VID_CNTL0, HEX); //Not sure what it is. Print the code.
          }
          if (secScreenType == "us2066" ) {
            oledsec.print("  0x");
            oledsec.print(VID_CNTL0, HEX); //Not sure what it is. Print the code.
          }
        }
      }

      //Read Conexant Chip to determine video resolution (for Version 1.0 to 1.3 console only)
    } else if (readSMBus(CONEX_ADDRESS, CONEX_2E, &rxBuffer[0], 1) == 0) {
      CONEXANT == 1;
      if (secScreenType == "us2066" ) {
        oledsec.cursorMove(4, 1);
        oledsec.print("GPU:CXT.  ");
        oledsec.cursorMove(1, 9);
      }
      if ((uint8_t)(rxBuffer[0] & 3) == 3) {
        //Must be 1080i
        if ((mainScreenType == "us2066" ) && (secScreenType == "NONE" )) {
          oled2004.print("  1080i   ");
        }
        if (secScreenType == "us2066" ) {
          oledsec.print("  RES:1080i");
        }
      } else if ((uint8_t)(rxBuffer[0] & 3) == 2) {
        //Must be 720p

        if ((mainScreenType == "us2066" ) && (secScreenType == "NONE" )) {
          oled2004.print("  720p    ");
        }
        if (secScreenType == "us2066" ) {
          oledsec.print("  RES:720p ");
        }
      } else if ((uint8_t)(rxBuffer[0] & 3) == 1 && rxBuffer[0]&CONEX_2E_HDTV_EN) {
        //Must be 480p
        if ((mainScreenType == "us2066" ) && (secScreenType == "NONE" )) {
          oled2004.print("  480p    ");
        }
        if (secScreenType == "us2066" ) {
          oledsec.print("  RES:480p ");
        }
      } else {
        if ((mainScreenType == "us2066" ) && (secScreenType == "NONE" )) {
          oled2004.print("  480i    ");
        }
        if (secScreenType == "us2066" ) {
          oledsec.print("  RES:480i ");
        }
      }
    }



    //Read the CPU and M/B temps
    //Try ADM1032
    if ((readSMBus(ADM1032_ADDRESS, ADM1032_CPU, &rxBuffer[0], 1) == 0 &&
         readSMBus(ADM1032_ADDRESS, ADM1032_MB, &rxBuffer[1], 1) == 0)  ||
        //If fails, its probably a 1.6. Read SMC instead.
        (readSMBus(SMC_ADDRESS, SMC_CPUTEMP, &rxBuffer[0], 1) == 0 &&
         readSMBus(SMC_ADDRESS, SMC_BOARDTEMP, &rxBuffer[1], 1) == 0)) {
      if (rxBuffer[0] < 200 && rxBuffer[1] < 200 && rxBuffer[0] > 0 && rxBuffer[1] > 0) {
#ifdef USE_FAHRENHEIT

        if ((mainScreenType == "us2066" ) && (secScreenType == "NONE" )) {
          snprintf(lineBuffer, sizeof lineBuffer, "CPU:%u%cF M/B:%u%cF", (uint8_t)((float)rxBuffer[0] * 1.8 + 32.0),
                   (char)223, (uint8_t)((float)rxBuffer[1] * 1.8 + 32.0), (char)223);
          oled2004.cursorMove(4, 1);
          oled2004.print(lineBuffer);
        }
        if (secScreenType == "us2066" ) {
          snprintf(lineBuffer, sizeof lineBuffer, "CPU:%u%cF M/B:%u%cF", (uint8_t)((float)rxBuffer[0] * 1.8 + 32.0),
                   (char)223, (uint8_t)((float)rxBuffer[1] * 1.8 + 32.0), (char)223);
          oledsec.cursorMove(2, 1);
          oledsec.print(lineBuffer);
        }
#else

        if ((mainScreenType == "us2066" ) && (secScreenType == "NONE" )) {
          snprintf(lineBuffer, sizeof lineBuffer, "CPU:%u%cC  M/B:%u%cC", rxBuffer[0], (char)223, rxBuffer[1], (char)223);
          oled2004.cursorMove(4, 1);
          oled2004.print(lineBuffer);
        }
        if (secScreenType == "us2066" ) {
          snprintf(lineBuffer, sizeof lineBuffer, "CPU:%u%cC  M/B:%u%cC", rxBuffer[0], (char)223, rxBuffer[1], (char)223);
          oledsec.cursorMove(2, 1);
          oledsec.print(lineBuffer);
        }
#endif
      }

      //Read the current hw ver from smc
      if ((readSMBus(SMC_ADDRESS, SMC_VER, &rxBuffer[0], 1) == 0) & (bitcount == 0)) {
        bitcount++;
        smcArray[0] = rxBuffer[0];
      }

      if ((readSMBus(SMC_ADDRESS, SMC_VER, &rxBuffer[0], 1) == 0) & (bitcount == 1)) {
        bitcount++;
        smcArray[1] = rxBuffer[0];
      }

      if ((readSMBus(SMC_ADDRESS, SMC_VER, &rxBuffer[0], 1) == 0) & (bitcount == 2)) {
        bitcount++;
        smcArray[2] = rxBuffer[0];
        smcArray[3] = '\0';
        smcString = smcArray;
      }

      if (secScreenType == "us2066") {
        if (smcString == "P01" ) {
          oledsec.cursorMove(3, 1);
          oledsec.print("H/W:V1.0");
        }
      }

      if (secScreenType == "us2066") {
        if (smcString == "P05" ) {
          oledsec.cursorMove(3, 1);
          oledsec.print("H/W:V1.1");
        }
      }
      if (secScreenType == "us2066") {
        if (smcString == "DBG")  {
          oledsec.cursorMove(3, 1);
          oledsec.print("H/W:DEBUG");
        }
      }
      if (secScreenType == "us2066") {
        if (smcString == "B11") {
          oledsec.cursorMove(3, 1);
          oledsec.print("H/W:DEBUG");
        }
      }
      if (secScreenType == "us2066") { //"P2L" or "1P1" or "11P" = v1.6
        if ((smcString == "P2L" || "1P1" || "11P") && FOCUS << 1 && CONEXANT << 1) {
          oledsec.cursorMove(3, 1);
          oledsec.print("H/W:V1.6");
          oledsec.cursorMove(4, 1);
          oledsec.print("GPU:Xcal. ");
          oledsec.cursorMove(3, 1);
        }
      }
      if (secScreenType == "us2066") { //"P11" or "1P1" or "11P" v1.2/v1.3 or v1.4.
        if ((smcString == "P11" || "1P1" || "11P") && FOCUS << 1 && CONEXANT >> 0) {
          oledsec.cursorMove(3, 1);
          oledsec.print("HW:V1.2/3");
        }
      }
      if (secScreenType == "us2066") { //"P11" or "1P1" or "11P" v1.2/v1.3 or v1.4.
        if ((smcString == "P11" || "1P1" || "11P") && FOCUS >> 0 && CONEXANT << 1) {
          oledsec.cursorMove(3, 1);
          oledsec.print("H/W:V1.4");
        }
      }
      SMBusTimer = millis();
    }
  } else if (SPIState != SPI_WAIT) {
    SMBusTimer = millis();
  }

}

/*
   Function: Check if the SMBus/I2C bus is busy
   ----------------------------
     returns: 0 if busy is free, non zero if busy or still checking
*/
uint8_t i2cBusy() {
  if (digitalRead(i2c_sda) == 0 || digitalRead(i2c_scl) == 0) { //If either the data or clock line is low, the line must be busy
    i2cCheckCount = I2C_BUSY_CHECKS;
  } else {
    i2cCheckCount--; //Bus isn't busy, decrease check counter so we check multiple times to be sure.
  }
  return i2cCheckCount;
}

/*
   Function: Read the Xbox SMBus
   ----------------------------
     address: The address of the device on the SMBus
     command: The command to send to the device. Normally the register to read from
     rx: A pointer to a receive data buffer
     len: How many bytes to read
     returns: 0 on success, -1 on error.
*/
int8_t readSMBus(uint8_t address, uint8_t command, uint8_t* rx, uint8_t len) {
  Wire.beginTransmission(address);
  Wire.write(command);
  if (Wire.endTransmission(false) == 0) { //Needs to be false. Send I2c repeated start, dont release bus yet
    Wire.requestFrom(address, len);
    for (uint8_t i = 0; i < len; i++) {
      rx[i] = Wire.read();
    }
    return 0;
  } else {
    return -1;
  }
}

void completeCommand(int16_t* c) {
  *c = -1;
}
