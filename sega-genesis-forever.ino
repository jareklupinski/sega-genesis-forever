#include <SPI.h>
#include <SdFat.h>
#include <SPIFlash.h>
#include <Wire.h>
#include <Fatlib/FatFile.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#define SPI_SPEED SD_SCK_MHZ(50)
#define NOP __asm__ __volatile__ ("nop\n\t")
/*
   struct bmp_file_header_t {
  uint16_t signature;
  uint32_t file_size;
  uint16_t reserved[2];
  uint32_t image_offset;
  };

  struct bmp_image_header_t {
  uint32_t header_size;
  uint32_t image_width;
  uint32_t image_height;
  uint16_t color_planes;
  uint16_t bits_per_pixel;
  uint32_t compression_method;
  uint32_t image_size;
  uint32_t horizontal_resolution;
  uint32_t vertical_resolution;
  uint32_t colors_in_palette;
  uint32_t important_colors;
  };

  bmp_file_header_t fileHeader;
  bmpImage.read(&fileHeader, sizeof(fileHeader));

  bmp_image_header_t imageHeader;
  bmpImage.read(&imageHeader, sizeof(imageHeader));

   bmpImage.seek(fileHeader.image_offset);


*/



//#define DEBUG

//EEPROM COMMANDS
#define MANID        0x90
#define PAGEPROG     0x02
#define READDATA     0x03
#define READSTAT1    0x05
#define WRITEENABLE  0x06
#define GLOBALUNLOCK 0x98
#define SUSPEND      0x75
#define RESUME       0x7A
#define JEDECID      0x9f
#define RELEASE      0xAB
#define BLOCK64ERASE 0xD8
//PORTA
#define CARD_CS ( 1 << 11 ) // 0
#define MISO    ( 1 << 12 ) // 0
#define SN_WE   ( 1 << 13 ) // 1
#define YM_IC   ( 1 << 14 ) // 1
#define YM_A0   ( 1 << 15 ) // 0
#define YM_A1   ( 1 << 16 ) // 0
#define YM_RD   ( 1 << 17 ) // 1
#define YM_WR   ( 1 << 18 ) // 1
#define SN_CS   ( 1 << 19 ) // 1
#define YM_CS   ( 1 << 20 ) // 1
#define MEM_CS  ( 1 << 21 ) // 1
#define SDA     ( 1 << 22 ) // 1
#define SCL     ( 1 << 23 ) // 1
#define SN_RDY  ( 1 << 27 ) // 1
#define YM_IRQ  ( 1 << 28 ) // 1
//PORTB
#define BACKBUTTON        ( 1 << 2 ) // 1
#define BACKARDUINO       19
#define PLAYBUTTON        ( 1 << 3 ) // 1
#define PLAYARDUINO       25
#define HOLD              ( 1 << 8 ) // 0
#define NEXTBUTTON        ( 1 << 9 ) // 1
#define NEXTARDUINO       16
#define MOSI              ( 1 << 10 ) // 0
#define SCK               ( 1 << 11 ) // 0
#define MEM_WP            ( 1 << 22 ) // 1
#define MIDI_IN           ( 1 << 23 ) // 1
//DEBUG
#ifdef DEBUG
#define DEBUG_PRINT(...)   Serial.print(__VA_ARGS__)
#define DEBUG_PRINTLN(...) Serial.println(__VA_ARGS__)
#else
#define DEBUG_PRINT(...)
#define DEBUG_PRINTLN(...)
#endif

SdFat SD;
FatFile vgmDir;
FatFile vgmFile;
StdioStream vgmStream;
Adafruit_SSD1306 display;
volatile uint32_t pause;
volatile uint8_t backButtonFlag;
volatile uint8_t playButtonFlag;
volatile uint8_t nextButtonFlag;
#define RAMBUFFERSIZE 19000
uint8_t pcmBuffer[ RAMBUFFERSIZE ];
uint8_t ramBufferFlag;

char fileNameBuf[ 256 ];
uint8_t nextSongFlag = 0;
uint32_t numFiles;
uint32_t vgmStreamPosition = 0;
uint32_t pcmPosition = 0;
uint32_t pcmPositionOffset = 0;
uint32_t ident = 0;
uint32_t eof = 0;
uint32_t vers = 0;
uint32_t snclk = 0;
uint32_t ymclk = 0;
uint32_t gd3Offset = 0;
uint32_t totalSamples = 0;
uint32_t loopOffset = 0;
uint32_t loopSamples = 0;
uint32_t rateHz = 0;
uint32_t snFeedback = 0;
uint32_t snShiftReg = 0;
uint32_t snFlags = 0;
uint32_t ym2612Clock = 0;
uint32_t ym2151Clock = 0;
uint32_t vgmDataOffset = 0;

void TC3_Handler(void) {
  TcCount16* TC = (TcCount16*) TC3;
  if ( TC->INTFLAG.bit.MC0 == 1 ) {
    if ( pause ) pause--;
    TC->INTFLAG.bit.MC0 = 1;
  }
}

void backButtonTrigger() {
  backButtonFlag = 1;
}

void playButtonTrigger() {
  playButtonFlag = 1;
}
void nextButtonTrigger() {
  nextButtonFlag = 1;
}

void writeEnable() {
  PORT->Group[PORTA].OUTCLR.reg = MEM_CS;
  SPI.transfer( WRITEENABLE );
  PORT->Group[PORTA].OUTSET.reg = MEM_CS;
}

void waitForFlashReady() {
  PORT->Group[PORTA].OUTCLR.reg = MEM_CS;
  SPI.transfer( READSTAT1 );
  while ( SPI.transfer( 0x00 ) & 0x01 ) {
    DEBUG_PRINT( "." );
  }
  PORT->Group[PORTA].OUTSET.reg = MEM_CS;
}

void dumpPcmToFlash( uint32_t bytes ) {
  uint8_t buf[ 256 ];
  uint32_t i = 0;
  uint32_t y = 0;
  while ( i < bytes ) {
    writeEnable();
    waitForFlashReady();
    DEBUG_PRINTLN( "Erasing 64K Block Starting At " + String( i, HEX ));
    PORT->Group[PORTA].OUTCLR.reg = MEM_CS;
    SPI.transfer( BLOCK64ERASE );
    SPI.transfer( i >> 16 );
    SPI.transfer( i >> 8 );
    SPI.transfer( 0 );
    PORT->Group[PORTA].OUTSET.reg = MEM_CS;
    waitForFlashReady();
    for ( int page = 0; page < 256; page++ ) {
      for ( int j = 0; j < 256; j++ ) {
        if ( y < ( bytes ) ) buf[ j ] = vgmStream.getc();
        else buf[ j ] = 0xFF;
        y++;
      }
      writeEnable();
      waitForFlashReady();
      PORT->Group[PORTA].OUTCLR.reg = MEM_CS;
      SPI.transfer( PAGEPROG );
      SPI.transfer( i >> 16 );
      SPI.transfer( i >> 8 );
      SPI.transfer( i );
      for ( int k = 0; k < 256; k++ ) {
        SPI.transfer( buf[ k ] );
        i++;
      }
      PORT->Group[PORTA].OUTSET.reg = MEM_CS;
      waitForFlashReady();
      DEBUG_PRINTLN( "Wrote to flash page " + String( i, HEX ) );
    }
  }
  DEBUG_PRINTLN( "Verifying:" );
  //for ( uint32_t k = 0; k < bytes; k++ ) { DEBUG_PRINT( readFromFlash( k ), HEX ); } PORT->Group[PORTB].OUTSET.reg = HOLD;
  PORT->Group[PORTA].OUTSET.reg = MEM_CS;
  DEBUG_PRINTLN( "" );
}
byte readFromFlash( uint32_t addr ) {
  byte data;
  static uint32_t lastAddr = 0xFFFFFFF0;
  if ( ramBufferFlag ) {
    data = pcmBuffer[ addr ];
  }
  else {
    PORT->Group[PORTB].OUTSET.reg = HOLD;
    PORT->Group[PORTA].OUTCLR.reg = MEM_CS;
    if ( addr != ( lastAddr + 1 )) {
      DEBUG_PRINT( "Last Address was " + String( lastAddr, HEX ) + " Changing Address to " + String( addr, HEX ) + " | ");
      PORT->Group[PORTA].OUTSET.reg = MEM_CS;
      PORT->Group[PORTA].OUTCLR.reg = MEM_CS;
      SPI.transfer( READDATA );
      SPI.transfer( addr >> 16 );
      SPI.transfer( addr >> 8 );
      SPI.transfer( addr );
    }
    data = SPI.transfer( 0 );
    PORT->Group[PORTB].OUTCLR.reg = HOLD;
    lastAddr = addr;
  }
  return data;
}

void playSNdata( uint8_t data ) {
  DEBUG_PRINTLN( "SN 0x" + String( data, HEX ));
  PORT->Group[PORTA].OUTCLR.reg = SN_CS;
  PORT->Group[PORTA].OUTSET.reg = data << 3;
  PORT->Group[PORTA].OUTCLR.reg = ( ~( data ) & 0xFF ) << 3;
  PORT->Group[PORTA].OUTCLR.reg = SN_WE;
  while ( !(PORT->Group[PORTA].IN.reg & SN_RDY ));
  PORT->Group[PORTA].OUTSET.reg = SN_WE;
  PORT->Group[PORTA].OUTSET.reg = SN_CS;
}

void playYMdata( uint8_t port, uint8_t addr, uint8_t data ) {
  DEBUG_PRINTLN( "YM" + String( port ) + " 0x" + String( addr, HEX ) + " 0x" + String( data, HEX ) );
  PORT->Group[PORTA].OUTCLR.reg = YM_CS;
  PORT->Group[PORTA].OUTCLR.reg = YM_A0;
  if ( port ) PORT->Group[PORTA].OUTSET.reg = YM_A1;
  else PORT->Group[PORTA].OUTCLR.reg = YM_A1;
  PORT->Group[PORTA].OUTSET.reg = ( addr << 3 );
  PORT->Group[PORTA].OUTCLR.reg = ( ~( addr ) & 0xFF ) << 3;
  PORT->Group[PORTA].OUTCLR.reg = YM_WR;
  for ( int i = 0; i < 9; i++ ) NOP;
  PORT->Group[PORTA].OUTSET.reg = YM_WR;
  PORT->Group[PORTA].OUTSET.reg = YM_A0;
  PORT->Group[PORTA].OUTSET.reg = ( data << 3 );
  PORT->Group[PORTA].OUTCLR.reg = ( ~( data ) & 0xFF ) << 3;
  PORT->Group[PORTA].OUTCLR.reg = YM_WR;
  for ( int i = 0; i < 9; i++ ) NOP;
  PORT->Group[PORTA].OUTSET.reg = YM_WR;
  PORT->Group[PORTA].OUTSET.reg = YM_CS;
}

void updateDisplay() {
  display.clearDisplay();
  display.setCursor(0, 0);
  display.println("Now Playing");
  display.println("");
  display.println(String( fileNameBuf ));
  display.println(String( pcmPosition ));
  display.display();
}

typedef void ( *jumpTable )( void );
void nogo( void ) {}
void playSN() {
  uint8_t data = vgmStream.getc();
  playSNdata( data );
}
void playYM( uint8_t port ) {
  uint8_t addr = vgmStream.getc();
  uint8_t data = vgmStream.getc();
  playYMdata( port, addr, data );
}
void playYM1() {
  playYM( 1 );
}
void playYM0() {
  playYM( 0 );
}
void waitNSamples( uint32_t n ) {
  if (n) n--;
  pause = n;
  DEBUG_PRINTLN( "DL " + String( n ));
}
void waitSamples() {
  uint32_t n = vgmStream.getc();
  n += uint16_t( vgmStream.getc() ) << 8;
  waitNSamples( n );
}
void endOfFile() {
  if ( loopOffset ) {
    vgmStream.fseek( loopOffset + 0x1C, SEEK_SET );
    loopOffset = 0;
  }
  else nextSongFlag = 1;
  DEBUG_PRINTLN( "EOL 0x66" );
}
void pcmStream() {
  uint8_t addr = vgmStream.getc(); //0x66
  uint8_t data = vgmStream.getc(); //Data Block Type
  uint32_t s;
  if ( data < 0x40 ) {
    s += uint32_t( vgmStream.getc() ) << ( 8 * 0 );
    s += uint32_t( vgmStream.getc() ) << ( 8 * 1 );
    s += uint32_t( vgmStream.getc() ) << ( 8 * 2 );
    s += uint32_t( vgmStream.getc() ) << ( 8 * 3 );
    Serial.println( "Data Block Type " + String( data, HEX ) + " Space Needed: " + s + " Set Command Pointer to " + String( s + vgmStream.ftell(), HEX ) );
    pcmPositionOffset = vgmStream.ftell();
    if ( s < RAMBUFFERSIZE ) {
      Serial.println( "Dumping to RAM" );
      for ( uint32_t i = 0; i < s; i++ ) {
        pcmBuffer[ i ] = vgmStream.getc();
      }
      ramBufferFlag = 1;
    }
    else {
      dumpPcmToFlash( s );
      ramBufferFlag = 0;
    }
  }
  else {
    Serial.println( "Error: Unsupported PCM Type " + String( data, HEX ) );
    nextSongFlag = 1;
  }
}
void playSample( uint32_t n ) {
  //if (n) n--;
  pause = n;
  uint8_t addr = 0x2A;
  uint8_t data = readFromFlash( pcmPosition );
  DEBUG_PRINT( "YM PCM 0x" + String( pcmPosition, HEX ) + " -> 0x" + String( pcmPosition + pcmPositionOffset, HEX) + " 0x" + String( data, HEX ) + " DL " + String( n ) + " | ");
  pcmPosition++;
  playYMdata( 0, addr, data );
}
void play0Sample() {
  playSample( 0 );
}
void play1Sample() {
  playSample( 1 );
}
void play2Sample() {
  playSample( 2 );
}
void play3Sample() {
  playSample( 3 );
}
void play4Sample() {
  playSample( 4 );
}
void play5Sample() {
  playSample( 5 );
}
void play6Sample() {
  playSample( 6 );
}
void play7Sample() {
  playSample( 7 );
}
void play8Sample() {
  playSample( 8 );
}
void play9Sample() {
  playSample( 9 );
}
void play10Sample() {
  playSample( 10 );
}
void play11Sample() {
  playSample( 11 );
}
void play12Sample() {
  playSample( 12 );
}
void play13Sample() {
  playSample( 13 );
}
void play14Sample() {
  playSample( 14 );
}
void play15Sample() {
  playSample( 15 );
}
void wait1Samples() {
  waitNSamples( 1 );
}
void wait2Samples() {
  waitNSamples( 2 );
}
void wait3Samples() {
  waitNSamples( 3 );
}
void wait4Samples() {
  waitNSamples( 4 );
}
void wait5Samples() {
  waitNSamples( 5 );
}
void wait6Samples() {
  waitNSamples( 6 );
}
void wait7Samples() {
  waitNSamples( 7 );
}
void wait8Samples() {
  waitNSamples( 8 );
}
void wait9Samples() {
  waitNSamples( 9 );
}
void wait10Samples() {
  waitNSamples( 10 );
}
void wait11Samples() {
  waitNSamples( 11 );
}
void wait12Samples() {
  waitNSamples( 12 );
}
void wait13Samples() {
  waitNSamples( 13 );
}
void wait14Samples() {
  waitNSamples( 14 );
}
void wait15Samples() {
  waitNSamples( 15 );
}
void wait16Samples() {
  waitNSamples( 16 );
}
void wait735Samples() {
  waitNSamples( 735 );
}
void wait882Samples() {
  waitNSamples( 882 );
}
void seekPCM() {
  uint32_t s;
  s += uint32_t( vgmStream.getc() ) << ( 8 * 0 );
  s += uint32_t( vgmStream.getc() ) << ( 8 * 1 );
  s += uint32_t( vgmStream.getc() ) << ( 8 * 2 );
  s += uint32_t( vgmStream.getc() ) << ( 8 * 3 );
  pcmPosition = s;
  DEBUG_PRINTLN( "Seeking PCM Pointer to Offset " + String( pcmPosition, HEX ));
}

void badCommand( uint8_t command, uint32_t debugAddress ) {
  Serial.println( "Error: Unknown Command 0x" + String( command, HEX ) + " @ 0X" + String( debugAddress, HEX ));
  nextSongFlag = 1;
}

void silence() {
  playSNdata( 0x9F ); //4 byte silence sn sequence
  playSNdata( 0xBF );
  playSNdata( 0xDF );
  playSNdata( 0xFF );
  playYMdata( 0, 0x22, 0x00 ); // LFO off
  playYMdata( 0, 0x27, 0x00 ); // Note off (channel 0)
  playYMdata( 0, 0x28, 0x01 ); // Note off (channel 1)
  playYMdata( 0, 0x28, 0x02 ); // Note off (channel 2)
  playYMdata( 0, 0x28, 0x04 ); // Note off (channel 3)
  playYMdata( 0, 0x28, 0x05 ); // Note off (channel 4)
  playYMdata( 0, 0x28, 0x06 ); // Note off (channel 5)
  playYMdata( 0, 0x2B, 0x00 ); // DAC off
  playYMdata( 0, 0x30, 0x71 ); //
  playYMdata( 0, 0x34, 0x0D ); //
  playYMdata( 0, 0x38, 0x33 ); //
  playYMdata( 0, 0x3C, 0x01 ); // DT1/MUL
  playYMdata( 0, 0x40, 0x23 ); //
  playYMdata( 0, 0x44, 0x2D ); //
  playYMdata( 0, 0x48, 0x26 ); //
  playYMdata( 0, 0x4C, 0x00 ); // Total level
  playYMdata( 0, 0x50, 0x5F ); //
  playYMdata( 0, 0x54, 0x99 ); //
  playYMdata( 0, 0x58, 0x5F ); //
  playYMdata( 0, 0x5C, 0x94 ); // RS/AR
  playYMdata( 0, 0x60, 0x05 ); //
  playYMdata( 0, 0x64, 0x05 ); //
  playYMdata( 0, 0x68, 0x05 ); //
  playYMdata( 0, 0x6C, 0x07 ); // AM/D1R
  playYMdata( 0, 0x70, 0x02 ); //
  playYMdata( 0, 0x74, 0x02 ); //
  playYMdata( 0, 0x78, 0x02 ); //
  playYMdata( 0, 0x7C, 0x02 ); // D2R
  playYMdata( 0, 0x80, 0x11 ); //
  playYMdata( 0, 0x84, 0x11 ); //
  playYMdata( 0, 0x88, 0x11 ); //
  playYMdata( 0, 0x8C, 0xA6 ); // D1L/RR
  playYMdata( 0, 0x90, 0x00 ); //
  playYMdata( 0, 0x94, 0x00 ); //
  playYMdata( 0, 0x98, 0x00 ); //
  playYMdata( 0, 0x9C, 0x00 ); // Proprietary
  playYMdata( 0, 0xB0, 0x32 ); // Feedback/algorithm
  playYMdata( 0, 0xB4, 0xC0 ); // Both speakers on
  playYMdata( 0, 0x28, 0x00 ); // Key off
}

jumpTable vgmFunctions[ 256 ] = { nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, playSN, playSN, playYM0, playYM1, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, waitSamples, wait735Samples, wait882Samples, nogo, nogo, endOfFile, pcmStream, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, wait1Samples, wait2Samples, wait3Samples, wait4Samples, wait5Samples, wait6Samples, wait7Samples, wait8Samples, wait9Samples, wait10Samples, wait11Samples, wait12Samples, wait13Samples, wait14Samples, wait15Samples, wait16Samples, play0Sample, play1Sample, play2Sample, play3Sample, play4Sample, play5Sample, play6Sample, play7Sample, play8Sample, play9Sample, play10Sample, play11Sample, play12Sample, play13Sample, play14Sample, play15Sample, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, seekPCM, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo, nogo };

void setup() {
  delay( 2000 );
  Serial.begin( 115200 );
  Serial.println( "Serial Online" );
  display.begin( SSD1306_SWITCHCAPVCC, 0x3C );
  display.clearDisplay();
  display.setTextSize( 1 );
  display.setTextColor( WHITE );
  display.setCursor( 0, 0 );
  display.println( "ChipTuneForever" );
  display.println( "Sega Genesis Model" );
  display.display();
  Serial.println( "OLED Online" );
  SPI.begin();
  SPI.beginTransaction(SPISettings( 50000000, MSBFIRST, SPI_MODE0 ));
  REG_GCLK_CLKCTRL = ( uint16_t ) ( GCLK_CLKCTRL_CLKEN | GCLK_CLKCTRL_GEN_GCLK0 | GCLK_CLKCTRL_ID ( GCM_TCC2_TC3 )) ;
  while ( GCLK->STATUS.bit.SYNCBUSY == 1 );
  TcCount16* TC = (TcCount16*) TC3;
  TC->CTRLA.reg &= ~TC_CTRLA_ENABLE;
  TC->CTRLA.reg |= TC_CTRLA_MODE_COUNT16;// Use the 16-bit timer
  while ( TC->STATUS.bit.SYNCBUSY == 1 );
  TC->CTRLA.reg |= TC_CTRLA_WAVEGEN_MFRQ;// Use match mode so that the timer counter resets when the count matches the compare register
  while ( TC->STATUS.bit.SYNCBUSY == 1 );
  TC->CTRLA.reg |= TC_CTRLA_PRESCALER_DIV16; // Set prescaler to 16
  while ( TC->STATUS.bit.SYNCBUSY == 1 );
  int compareValue = ( 48000000 / ( 16 * 44100 )) - 1;
  TC->COUNT.reg = map( TC->COUNT.reg, 0, TC->CC[ 0 ].reg, 0, compareValue ); // Make sure the count is in a proportional position to where it was to prevent any jitter or disconnect when changing the compare value.
  TC->CC[ 0 ].reg = compareValue;
  while ( TC->STATUS.bit.SYNCBUSY == 1 );
  TC->INTENSET.reg = 0; // Enable the compare interrupt
  TC->INTENSET.bit.MC0 = 1;
  NVIC_EnableIRQ( TC3_IRQn );
  TC->CTRLA.reg |= TC_CTRLA_ENABLE;
  while ( TC->STATUS.bit.SYNCBUSY == 1 );
  PORT->Group[PORTA].DIRSET.reg = ( 0xFF << 3 ) | CARD_CS | SN_WE | YM_IC | YM_A0 | YM_A1 | YM_RD | YM_WR | SN_CS | YM_CS | MEM_CS | SDA | SCL;
  PORT->Group[PORTA].DIRCLR.reg = SN_RDY | YM_IRQ | MISO;

  //PORT->Group[PORTB].DIRSET.reg = MOSI | SCK | HOLD | MEM_WP;
  //PORT->Group[PORTB].DIRCLR.reg = BACKBUTTON | PLAYBUTTON | NEXTBUTTON | MIDI_IN;
  PORT->Group[PORTB].DIRSET.reg = MOSI | SCK | HOLD | MEM_WP | MIDI_IN;
  PORT->Group[PORTB].DIRCLR.reg = BACKBUTTON | PLAYBUTTON | NEXTBUTTON;

  PORT->Group[PORTA].OUTSET.reg = YM_IC | YM_RD | YM_WR | YM_CS | MEM_CS | SN_WE | SN_CS | CARD_CS;
  PORT->Group[PORTB].OUTSET.reg = BACKBUTTON | PLAYBUTTON | NEXTBUTTON | MEM_WP | HOLD;
  delay( 10 );
  PORT->Group[PORTA].OUTCLR.reg = YM_IC | MEM_CS;
  delay( 10 );
  PORT->Group[PORTA].OUTSET.reg = YM_IC | MEM_CS;
  delay( 10 );
  silence();
  PORT->Group[PORTA].OUTCLR.reg = MEM_CS;
  SPI.transfer( RELEASE ); //release
  PORT->Group[PORTA].OUTSET.reg = MEM_CS;
  delayMicroseconds( 5 );
  DEBUG_PRINTLN( "Flash Chip Released" );
  PORT->Group[PORTA].OUTCLR.reg = MEM_CS;
  SPI.transfer( JEDECID ); //read jedec
  SPI.transfer( 0x00 ); //manuf
  SPI.transfer( 0x00 ); //id high
  SPI.transfer( 0x00 ); //id low
  PORT->Group[PORTA].OUTSET.reg = MEM_CS;
  PORT->Group[PORTA].OUTCLR.reg = MEM_CS;
  SPI.transfer( WRITEENABLE ); // write enable
  PORT->Group[PORTA].OUTSET.reg = MEM_CS;
  PORT->Group[PORTA].OUTCLR.reg = MEM_CS;
  SPI.transfer( GLOBALUNLOCK ); // write enable
  PORT->Group[PORTA].OUTSET.reg = MEM_CS;
  DEBUG_PRINTLN( "Starting SD Card" );
  SD.begin( 0, SD_SCK_MHZ(50) );

  SD.vwd()->rewind();
  DEBUG_PRINTLN( "Looking for files" );
  while ( vgmFile.openNext( SD.vwd(), O_READ )) {
    vgmFile.close();
    numFiles++;
  }
  if ( numFiles == 0 ) {
    Serial.println( "No Files Found" );
    delay( 500 );
  }

  pinMode( BACKARDUINO, INPUT_PULLUP );
  pinMode( PLAYARDUINO, INPUT_PULLUP );
  pinMode( NEXTARDUINO, INPUT_PULLUP );
  attachInterrupt( digitalPinToInterrupt( BACKARDUINO ), backButtonTrigger, FALLING );
  attachInterrupt( digitalPinToInterrupt( PLAYARDUINO ), playButtonTrigger, FALLING );
  attachInterrupt( digitalPinToInterrupt( NEXTARDUINO ), nextButtonTrigger, FALLING );
  DEBUG_PRINTLN( "Setup Done" );
}

void loop() {
  String fileNameString;

  randomSeed( micros() );
  uint16_t randomFile = random( numFiles - 1 );
  DEBUG_PRINTLN( "Opening File Number: " + String( randomFile ));
  SD.vwd()->rewind();
  vgmFile.openNext( SD.vwd(), O_READ );
  for ( int i = 0; i < randomFile; i++ ) {
    vgmFile.close();
    vgmFile.openNext( SD.vwd(), O_READ );
  }
  fileNameString = vgmFile.getName( fileNameBuf, sizeof( fileNameBuf ));
  vgmFile.close();

  //fileNameString = "Sonic Spinball - Lava Power House (2)"; fileNameString.toCharArray( fileNameBuf, 256 );
  //fileNameString = "Sonic 3 & Knuckles - Angel Island Zone 1"; fileNameString.toCharArray( fileNameBuf, 256 );
  //fileNameString = "Sonic 3 & Knuckles - Carnival Night Zone 1"; fileNameString.toCharArray( fileNameBuf, 256 );

  Serial.println( "Opening VGM File " + String( fileNameBuf ));
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.setCursor(0, 0);
  display.println("Now Playing");
  display.println(String( fileNameBuf ));
  display.display();
  if ( vgmStream.fopen( fileNameBuf , "r" )) {
    ramBufferFlag = 0;
    vgmStreamPosition = 0;
    pcmPosition = 0;
    pcmPositionOffset = 0;
    ident = 0;
    eof = 0;
    vers = 0;
    snclk = 0;
    ymclk = 0;
    gd3Offset = 0;
    totalSamples = 0;
    loopOffset = 0;
    loopSamples = 0;
    rateHz = 0;
    snFeedback = 0;
    snShiftReg = 0;
    snFlags = 0;
    ym2612Clock = 0;
    ym2151Clock = 0;
    vgmDataOffset = 0;
    for ( int i = 0; i < 4; i++ ) ident += uint32_t(vgmStream.getc()) << ( 8 * i );
    for ( int i = 0; i < 4; i++ ) eof += uint32_t(vgmStream.getc()) << ( 8 * i );
    for ( int i = 0; i < 4; i++ ) vers += uint32_t(vgmStream.getc()) << ( 8 * i );
    for ( int i = 0; i < 4; i++ ) snclk += uint32_t(vgmStream.getc()) << ( 8 * i );
    for ( int i = 0; i < 4; i++ ) ymclk += uint32_t(vgmStream.getc()) << ( 8 * i );
    for ( int i = 0; i < 4; i++ ) gd3Offset += uint32_t(vgmStream.getc()) << ( 8 * i );
    for ( int i = 0; i < 4; i++ ) totalSamples += uint32_t(vgmStream.getc()) << ( 8 * i );
    for ( int i = 0; i < 4; i++ ) loopOffset += uint32_t(vgmStream.getc()) << ( 8 * i );
    for ( int i = 0; i < 4; i++ ) loopSamples += uint32_t(vgmStream.getc()) << ( 8 * i );
    for ( int i = 0; i < 4; i++ ) rateHz += uint32_t(vgmStream.getc()) << ( 8 * i );
    for ( int i = 0; i < 4; i++ ) snFeedback += uint32_t(vgmStream.getc()) << ( 8 * i );
    for ( int i = 0; i < 4; i++ ) snShiftReg += uint32_t(vgmStream.getc()) << ( 8 * i );
    for ( int i = 0; i < 4; i++ ) snFlags += uint32_t(vgmStream.getc()) << ( 8 * i );
    for ( int i = 0; i < 4; i++ ) ym2612Clock += uint32_t(vgmStream.getc()) << ( 8 * i );
    for ( int i = 0; i < 4; i++ ) ym2151Clock += uint32_t(vgmStream.getc()) << ( 8 * i );
    for ( int i = 0; i < 4; i++ ) vgmDataOffset += uint32_t(vgmStream.getc()) << ( 8 * i );
    if ( ident != 0x206d6756 ) {
      Serial.println( "Ident Match Failed! " + String( ident, HEX ) );
      return;
    }
    if (( vgmDataOffset == 0x40 ) || ( vgmDataOffset == 0x00 )) vgmStream.fseek( 0x40, SEEK_SET );
    else vgmStream.fseek( vgmDataOffset, SEEK_SET );
    DEBUG_PRINTLN( "--------------VGM File Data Start--------------" );
    nextButtonFlag = 0;
    nextSongFlag = 0;
    while ( ( nextSongFlag == 0 ) && ( nextButtonFlag == 0 )) {
      if ( playButtonFlag ) {
        display.clearDisplay();
        display.setCursor(0, 0);
        display.println("Paused");
        display.println(String( fileNameBuf ));
        display.display();
        delay( 100 );
        playButtonFlag = 0;
        while ( !playButtonFlag ) ;
        display.clearDisplay();
        display.setCursor(0, 0);
        display.println("Now Playing");
        display.println("");
        display.println(String( fileNameBuf ));
        display.display();
        delay( 100 );
        playButtonFlag = 0;
      }
      //int32_t debugAddress = 0; // vgmStream.ftell();
      uint8_t command = vgmStream.getc();
      //DEBUG_PRINT( "Addr:0x" + String( debugAddress, HEX ) + " Cmd:0x" + String( command, HEX ) + " | " );
      vgmFunctions[ command ]();
      while ( pause );
    }
  }
  else Serial.println( "Cannot open " + String( fileNameBuf ) + ": File read error" );
  PORT->Group[PORTA].OUTSET.reg = MEM_CS;
  PORT->Group[PORTA].OUTSET.reg = HOLD;
  vgmStream.fclose();
  PORT->Group[PORTA].OUTCLR.reg = YM_IC | MEM_CS;
  delay( 10 );
  PORT->Group[PORTA].OUTSET.reg = YM_IC | MEM_CS;
  delay( 10 );
  silence();
  Serial.println( "Next" );
}
