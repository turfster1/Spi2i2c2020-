/*
  Important NOTES:
    1. If using Arduino IDE, version 1.5.0 or higher is REQUIRED!
*/

/*
  I2cCharDisplay.h

  Written by: Gary Muhonen  gary@dcity.org

  Versions
    1.0.0 - 3/19/2016
      Original Release.
    1.0.1 - 4/6/2016
      Modified the cursorMove() function to start at (1,1) instead of (0,0).
      Modified the cursorMove() function to work with OLED modules correctly.
      Modified the home() function to work with the newly modified cursorMove().
      Modified the oledBegin() function to work with the Newhaven OLED modules.
    1.0.2 - 4/17/2017
      Added a second class constructor (with additional parameter i2cPort),
        so that the user could specify which i2c port to use (0 or 1)
        Port 0 is the main i2c port using pins (SDA and SCL).
        Port 1 is the aux i2c port using pins (SDA1 and SCL1, e.g. on an Arduino DUE board).
        This adds support for the Arduino DUE board (using either of it's i2c ports).
    1.0.3 - 8/27/2018 Transfer to GM, and some minor changes
        Added these OLED "fade display" functions (not very useful for some types of OLED displays)
          void fadeOff();           // turns off the fade feature of the OLED
          void fadeOnce(uint8_t);   // fade out the display to off (fade time 0-16) - (on some display types, it doesn't work very well. It takes the display to half brightness and then turns off display)
          void fadeBlink(uint8_t);  // blinks the fade feature of the OLED (fade time 0-16) - (on some display types, it doesn't work very well. It takes the display to half brightness and then turns off display)


  Short Description:

      These files provide a software library and demo program for the Arduino
      and Particle microcontroller boards.

      The library files provide useful functions to make it easy
      to communicate with OLED and LCD character
      display modules that use the I2C communication protocol. The demo
      program shows the usage of the functions in the library.

      The library will work with **LCD** and **OLED** character displays
      (e.g. 16x2, 20x2, 20x4, etc.). The LCD displays must use the the
      HD44780 controller chip and have a I2C PCA8574 i/o expander chip
      on a backpack board (which gives the display I2C capability).
      OLED display modules must have the US2066 controller chip
      (which has I2C built in). Backback boards are available and
      details are in the link below.


  https://www.dcity.org/portfolio/i2c-display-library/
  This link has details including:
      * software library installation for use with Arduino, Particle and Raspberry Pi boards
      * list of functions available in these libraries
      * a demo program (which shows the usage of most library functions)
      * info on OLED and LCD character displays that work with this software
      * hardware design for a backpack board for LCDs and OLEDs, available on github
      * info on backpack “bare” pc boards available from OSH Park.

  License Information:  https://www.dcity.org/license-information/
*/



// include files... some boards require different include files
#ifdef ARDUINO_ARCH_AVR        // if using an arduino
#include "Arduino.h"
#include "../SMWire/SMWire.h"
#elif ARDUINO_ARCH_SAM        // if using an arduino DUE
#include "Arduino.h"
#include "../SMWire/SMWire.h"
#elif PARTICLE                 // if using a core, photon, or electron (by particle.io)
#include "Particle.h"
#elif defined(__MK20DX128__) || (__MK20DX256__) || (__MK20DX256__) || (__MK62FX512__) || (__MK66FX1M0__) // if using a teensy 3.0, 3.1, 3.2, 3.5, 3.6
#include "Arduino.h"
#include "../SMWire/SMWire.h"
#else                          // if using something else then this may work
#include "Arduino.h"
#include "../SMWire/SMWire.h"
#endif


// _displayType options
#define LCD_TYPE                     0 // if the display is an LCD using the PCA8574 outputting to the HD44780 lcd controller chip
#define OLED_TYPE                    1 // if the display is a OLED using the US2066 oled controller chip

// **********************
// oled specific constants
// **********************

#define OLED_COMMANDMODE             0x80       // command value to set up command mode
#define OLED_DATAMODE                0x40       // command value to set up data mode
#define OLED_SETBRIGHTNESSCOMMAND    0x81       // command address for setting the oled brightness
#define OLED_SETFADECOMMAND          0x23       // command address for setting the fade out command

// bits for setting the fade command
#define OLED_FADEOFF              0X00       // command value for setting fade mode to off
#define OLED_FADEON               0X20       // command value for setting fade mode to on
#define OLED_FADEBLINK            0X30       // command value for setting fade mode to blink

// lcd specific constants

// bits on the PCA8574 chip for controlling the lcd
#define LCD_BACKLIGHTON     8 // backlight on bit
#define LCD_BACKLIGHTOFF    0 // backlight off
#define LCD_ENABLEON        4 // Enable bit on
#define LCD_ENABLEOFF       0 // Enable bit off
#define LCD_READ            2 // Read bit
#define LCD_WRITE           0 // Write bit
#define LCD_DATA            1 // Register Select bit for Data
#define LCD_COMMAND         0 // Register Select bit for Command

// lcd and oled constants

// lcd commands
#define LCD_CLEARDISPLAYCOMMAND      0x01
#define LCD_RETURNHOMECOMMAND        0x02
#define LCD_ENTRYMODECOMMAND         0x04
#define LCD_DISPLAYCONTROLCOMMAND    0x08
#define LCD_SHIFTCOMMAND             0x10
#define LCD_FUNCTIONSETCOMMAND       0x20
#define LCD_SETCGRAMADDRCOMMAND      0x40
#define LCD_SETDDRAMADDRCOMMAND      0x80

// bits for _lcdEntryModeCommand
#define LCD_DISPLAYLEFTTORIGHT       0x02
#define LCD_DISPLAYRIGHTTOLEFT       0X00
#define LCD_DISPLAYSHIFTON           0x01
#define LCD_DISPLAYSHIFTOFF          0x00

// bits for _lcdDisplayControlCommand
#define LCD_DISPLAYON                0x04
#define LCD_DISPLAYOFF               0x00
#define LCD_CURSORON                 0x02
#define LCD_CURSOROFF                0x00
#define LCD_CURSORBLINKON            0x01
#define LCD_CURSORBLINKOFF           0x00

// bits for _lcdFunctionSetCommand
#define LCD_8BITMODE                 0x10
#define LCD_4BITMODE                 0x00
#define LCD_2LINES                   0x08
#define LCD_1LINES                   0x00
#define LCD_5x10DOTS                 0x04
#define LCD_5x8DOTS                  0x00

// bits for shifting the display and the cursor
#define LCD_DISPLAYSHIFT             0x08
#define LCD_CURSORSHIFT              0x00
#define LCD_SHIFTRIGHT               0x04
#define LCD_SHIFTLEFT                0x00


class I2cCharDisplay : public Print {       // parent class is Print, so that we can use the print functions
public:

  I2cCharDisplay(uint8_t displayType, uint8_t i2cAddress, uint8_t rows); // creates a display object when using the main i2c port (SDA and SCL pins)
  I2cCharDisplay(uint8_t displayType, uint8_t i2cAddress, uint8_t rows, uint8_t i2cPort); // creates a display object where you can specify the i2c port to use (0 or 1) (port 1 is for the aux i2c port using SDA1 and SCL1 pins, e.g on an Arduino DUE board)
  void begin();                                                      // required to inialize the display. run this first!
  void clear();                                                      // clear the display and home the cursor to 1,1
  void home();                                                       // move the cursor to home position (1,1)
  void cursorMove(uint8_t row, uint8_t col);                         // move cursor to position row,col (positions start at 1)
  void displayOff();                                                 // turns off the entire display
  void displayOn();                                                  // turns on the entire display
  void cursorBlinkOff();                                             // turns off the blinking block cursor
  void cursorBlinkOn();                                              // turns on the blinking block cursor
  void cursorOff();                                                  // turns off the underline cursor
  void cursorOn();                                                   // turns on an underline cursor
  void displayShiftLeft();                                           // shifts all rows of the display one character to the left (shifts cursor too)
  void displayShiftRight();                                          // shifts all rows of the display one character to the right (shifts cursor too)
  void cursorShiftLeft(void);                                        // shifts the cursor one character to the left
  void cursorShiftRight(void);                                       // shifts the cursor one character to the right
  void displayLeftToRight();                                         // characters are displayed left to right (DEFAULT MODE)
  void displayRightToLeft();                                         // characters are displayed left to right
  void displayShiftOn();                                             // cursor is held constant and previous characters are shifted when new ones come in
  void displayShiftOff();                                            // cursor moves after each character is received by the display (DEFAULT MODE)
  void createCharacter(uint8_t, uint8_t[]);                          // used to create custom dot matrix characters (8 are available)
  virtual size_t write(uint8_t);                                     // allows the print command to work (in Arduino or Particle)

// functions specific to lcd displays

  void backlightOn();
  void backlightOff();

// functions specific to oled displays

  void setBrightness(uint8_t);
  void fadeOff();                                                    // turns off the fade feature of the OLED
  void fadeOnce(uint8_t);                                            // fade out the display to off (fade time 0-15) - (on some display types, it doesn't work very well. It takes the display to half brightness and then turns off display)
  void fadeBlink(uint8_t);                                           // blinks the fade feature of the OLED (fade time 0-15) - (on some display types, it doesn't work very well. It takes the display to half brightness and then turns off display)



/*
 * #if defined(ARDUINO) && ARDUINO >= 100
 * virtual size_t write(uint8_t);
 * #else
 * virtual void write(uint8_t);
 * #endif
 */


private:
  void i2cWrite1(uint8_t data);   // write one byte to i2c bus, either i2cPort 0 or 1
  void i2cWrite2(uint8_t data1, uint8_t data2);  // write 2 bytes to the i2c bus, either i2cPort 0 or 1
  void lcdBegin();               // used to initialize the lcd display
  void oledBegin();              // used to initialize the oled display
  void sendCommand(uint8_t);     // send a command to the display
  void sendData(uint8_t);        // send data to the display
  void sendLcdCommand(uint8_t);  // send a command to the lcd display
  void sendLcdData(uint8_t);     // send data to the lcd display
  void sendOledCommand(uint8_t); // send a command to the oled display
  void sendOledData(uint8_t);    // send data to the oled display

  // private variables
  // keep track of current state of these lcd commands
  uint8_t _displayType;           // keep track of the type of display we are using (e.g. lcd, oled, etc.)

  uint8_t _lcdEntryModeCommand;
  uint8_t _lcdDisplayControlCommand;
  uint8_t _lcdFunctionSetCommand;

  uint8_t _i2cAddress;
  uint8_t _i2cPort;                 // 0 or 1, depending on which i2c port on the due is being used
  uint8_t _rows;                   // number of rows in the display (starting at 1)
  uint8_t _lcdBacklightControl;    // 0 if backlight is off, 0x08 is on
};
