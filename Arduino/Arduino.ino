// -----------------------------------------------------------------------------
// Programmer for Ben Eater's Breadboard Computer
// Copyright (C) 2017 David Hansel
// Modified (C) 2024 Thomas Proffen
//
// Simplified code to only include the 4Bit Den Eater version.
// Added more instructions to output.
// -----------------------------------------------------------------------------

#include <EEPROM.h>

#define MAX_BREAKPOINTS 10
#define PIN_BUS0  2 // first pin connected to bus (plus following 7)
#define PIN_RO   10 // RO signal
#define PIN_RI   11 // RI signal
#define PIN_MI   12 // MI signal
#define PIN_CLK  A0 // clock (analog used as digital in)
#define PIN_BTN  A1 // LOAD/SAVE buttons (analog in)
#define PIN_SW   A2 // DIP switches 1-4 (analog in)
#define PIN_EE   A3
#define ASM_MAX_LABELS            16
#define ASM_MAX_LABEL_FORWARD_REF 16


// Monitor operating using 4-bit opcode/4 bit address mode 
static bool mode_4bit = true;

#define CLOCK()           digitalRead(PIN_CLK)
#define SET_RO_HIGH()     digitalWrite(PIN_RO, HIGH)
#define SET_RO_LOW()      digitalWrite(PIN_RO, LOW)
#define SET_RI_HIGH()     digitalWrite(PIN_RI, HIGH)
#define SET_RI_LOW()      digitalWrite(PIN_RI, LOW)
#define SET_MI_HIGH()     digitalWrite(PIN_MI, HIGH)
#define SET_MI_LOW()      digitalWrite(PIN_MI, LOW)
#define SET_EE_HIGH()     digitalWrite(PIN_EE, HIGH)
#define SET_EE_LOW()      digitalWrite(PIN_EE, LOW)
#define SET_CS_INPUT()    { pinMode(PIN_MI, INPUT);  pinMode(PIN_RO, INPUT);  pinMode(PIN_RI, INPUT);  }
#define SET_CS_OUTPUT()   { pinMode(PIN_MI, OUTPUT); pinMode(PIN_RO, OUTPUT); pinMode(PIN_RI, OUTPUT); }
#define SET_BUS_INPUT()   { for(int i=PIN_BUS0; i<PIN_BUS0+8; i++) pinMode(i, INPUT);  }
#define SET_BUS_OUTPUT()  { for(int i=PIN_BUS0; i<PIN_BUS0+8; i++) pinMode(i, OUTPUT); }

#define BUFFER_SIZE 256
byte buffer[BUFFER_SIZE];
bool print_serial_output = true;
bool auto_load = false;

// ------------------------------------------------------------------------------------------

struct opcodes_struct
{
  const char *mnemonic;
  byte        opcode;
  byte        num_cycles:3;
  byte        has_arg:1;
};

// opcodes for 4-bit architecture.
// First is the mnemonic, "#" means immediate argument
// Second is the opcode (note: low four bits are ignored)
// Third is number of cycles (in addition to two fetch cycles)
// Fourth is whether this mnemonic requires an argument

struct opcodes_struct opcodes_4bit [] = 
  {
    {"NOP  ", B00000000, 3, false},
    {"LDA  ", B00010000, 3, true},
    {"ADD  ", B00100000, 3, true},
    {"SUB  ", B00110000, 3, true},
    {"STA  ", B01000000, 3, true},
    {"LDI  ", B01010000, 3, true},
    {"JMP  ", B01100000, 3, true},
    {"JC   ", B01110000, 3, true},
    {"JZ   ", B10000000, 3, true},
    {"DSI  ", B10010000, 3, true},
    {"INC  ", B10100000, 3, true},
    {"DEC  ", B10110000, 3, true},
    {"LDB  ", B11000000, 3, true},
    {"DSP  ", B11010000, 3, true},
    {"OUT  ", B11100000, 3, false},
    {"HLT  ", B11110000, 3, false},
    // following are pseudo-opcodes used by assembler/disassembler
    {".OR  ", B11111110, 0, true},  // set start address
    {".BY  ", B11111111, 0, true},  // define one byte of data
    {NULL, 0, 0, false}
  };

#define wait_clock_low()  while(CLOCK())
#define wait_clock_high() while(!CLOCK())

// ------------------------------------------------------------------------------------------
byte read_bus()
{
  byte res = 0;
  for(int i=0; i<8; i++) 
    if( digitalRead(PIN_BUS0+i)==HIGH )
      res |= 1<<i;
  return res;    
}

// ------------------------------------------------------------------------------------------
void write_bus(byte data)
{
  for(int i=PIN_BUS0; i<PIN_BUS0+8; i++)
    { digitalWrite(i, data&1); data /= 2; }
}

// ------------------------------------------------------------------------------------------
inline void wait_clock_info(int status)
{
  unsigned long t = millis() + 2000;
  if( status==LOW )
    {
      while( CLOCK() )
        { if( t>0 && millis()>t ) {Serial.println(F("Waiting for clock LOW...")); t = 0;} }
    }
  else
    {
      while( !CLOCK() )
        { if( t>0 && millis()>t ) {Serial.println(F("Waiting for clock HIGH...")); t = 0;} }
    }
}

// ------------------------------------------------------------------------------------------
void wait_clock_stop()
{
  // measure length of one clock cycle
  wait_clock_low();
  unsigned long t, len = micros();
  wait_clock_high();
  wait_clock_low();
  len = micros() - len;
  
  Serial.println(F("Waiting for clock to stop..."));
  bool wait = true;
  while( wait )
    {
      while( CLOCK() );
      t = micros(); while( wait && !CLOCK() ) if( (micros()-t)>2*len && (micros()-t)>100000 ) wait = false;
    }
}

// ------------------------------------------------------------------------------------------
inline void enable_signal_control()
{
  // set control pins (MI/RI/RO) as outputs and output LOW
  SET_MI_LOW();
  SET_RO_LOW();
  SET_RI_LOW();
  SET_CS_OUTPUT();

  // disable EEPROM
  SET_EE_HIGH();
}

// ------------------------------------------------------------------------------------------
inline void disable_signal_control(bool wait_clock)
{
  // Make sure we don't get delayed by an interrupt
  // and take too long after clock LOW to return control.
  noInterrupts();

  // Wait for a HIGH->LOW transition on the clock
  // to make sure the clock is low at the point where
  // control is returned to the control logic
  // (otherwise the control unit's step counter will
  // already count up on the HIGH->LOW edge and
  // thereby skip the first step of the fetch cycle).
  if( wait_clock ) 
    {
      wait_clock_high();
      wait_clock_low();
    }
  
  // must disable EEPROM before switching
  // I/O pins to INPUT, otherwise we produce a short
  // HIGH signal on the I/O pins
  SET_EE_LOW();

  // set control pins (MI/RI/RO) as inputs
  SET_CS_INPUT();
  
  interrupts();
}

// ------------------------------------------------------------------------------------------
void read_ram(byte *data, byte start, int len)
{
  byte address;

  // wait with info (to notify user if clock is not running)
  wait_clock_info(HIGH);

  // disable interrupts, otherwise timing for fast
  // clock speeds can be messed up
  noInterrupts();

  // make sure the clock is still HIGH (or wait until it is again)
  wait_clock_high();

  for(int i=0; i<len; i++)
    {
      byte address = start+i;
      wait_clock_low();
      SET_RO_LOW();
      SET_BUS_OUTPUT();
      write_bus(address);
      SET_MI_HIGH();
      wait_clock_high();
      wait_clock_low();
      SET_MI_LOW();
      SET_BUS_INPUT();
      SET_RO_HIGH();
      wait_clock_high();
      data[i] = read_bus();
    }

  wait_clock_low();
  SET_RO_LOW();
  interrupts();
}

// ------------------------------------------------------------------------------------------
void write_ram(const byte *data, byte start, int len)
{
  byte address;
  
  // wait with info (to notify user if clock is not running)
  wait_clock_info(HIGH);

  // disable interrupts, otherwise timing for fast
  // clock speeds can be messed up
  noInterrupts();

  SET_BUS_OUTPUT();

  // make sure the clock is still HIGH (or wait until it is again)
  wait_clock_high();

  for(int i=0; i<len; i++)
    {
      address = start+i;
      wait_clock_low();
      SET_RI_LOW();
      write_bus(address);
      SET_MI_HIGH();
      wait_clock_high();
      wait_clock_low();
      SET_MI_LOW();
      write_bus(data[i]);
      SET_RI_HIGH();
      wait_clock_high();
    }
    
  wait_clock_low();
  SET_RI_LOW();
  SET_BUS_INPUT();

  // re-enable interrupts
  interrupts();
}

// ------------------------------------------------------------------------------------------
char *dec2hex(byte v)
{
  static char buf[3];
  sprintf(buf, "%02x", v);
  return buf;
}

// ------------------------------------------------------------------------------------------
void print_data(byte *data, int addr, int len)
{
  for(int i=0; i<len; i++)
    {
      if( (i%16)==0 )
        {
          if( i>0 ) Serial.println();
          Serial.print(dec2hex(addr+i));
          Serial.print(F(": "));
        }
      else if( (i%16)==8 )
        Serial.print(' ');
        
      Serial.print(dec2hex(data[i])); 
      Serial.print(' ');
    }
  Serial.println();
}

// ------------------------------------------------------------------------------------------
bool write_eeprom(byte *buffer, bool internal, unsigned int start, unsigned int size)
{
  bool res = true;

  // last 16 bytes in internal EEPROM are used for configuration settings
  // => (1024-16)/16 = 63 programs for 4-bit address range can be stored
  if( internal && start+size<(1024-16) )
    {
      for(unsigned int i=0; i<size; i++)
        EEPROM.write(start+i, buffer[i]);
    }
  else
    res = false;

  return res;
}

// ------------------------------------------------------------------------------------------
bool read_eeprom(byte *buffer, bool internal, unsigned int start, unsigned int size)
{
  bool res = true;

  // last 16 bytes in internal EEPROM are used for configuration settings
  // => (1024-16)/16 = 63 programs for 4-bit address range can be stored
  if( internal && start+size<(1024-16) )
    {
      for(int i=0; i<size; i++)
        buffer[i] = EEPROM.read(start+i);
    }
  else
    res = false;

  return res;
}

// ------------------------------------------------------------------------------------------
void save_data(bool eeprom_internal, unsigned int eeprom_start, unsigned int ram_start, unsigned int size)
{
  if( print_serial_output )
    {
      Serial.print(F("Saving ")); 
      Serial.print(size);
      Serial.print(F(" bytes starting at address "));
      Serial.println(ram_start);
      Serial.flush();

      if( size>BUFFER_SIZE )
        Serial.println(F("[Error: buffer to small]"));
      else
        {
          read_ram(buffer, ram_start, size);
          if( write_eeprom(buffer, eeprom_internal, eeprom_start, size) )
            {
              Serial.println(F("Saved:")); 
              print_data(buffer, ram_start, size);
            }
          else
            Serial.println(F("[Error: exceeded EEPROM address space"));
        }
    }
  else
    {
      read_ram(buffer, ram_start, size);
      write_eeprom(buffer, eeprom_internal, eeprom_start, size);
    }
}

// ------------------------------------------------------------------------------------------
void load_data(bool eeprom_internal, unsigned int eeprom_start, unsigned int ram_start, unsigned int size)
{
  if( print_serial_output )
    {  
      if( size>BUFFER_SIZE )
        Serial.println(F("[Error: buffer to small]"));
      else if( read_eeprom(buffer, eeprom_internal, eeprom_start, size) )
        {
          Serial.println(F("Loading:"));
          print_data(buffer, ram_start, size);
          Serial.flush();
          write_ram(buffer, 0, size);
          Serial.println(F("done")); 
        }
      else
        Serial.println(F("[Error: exceeded EEPROM address space"));
    }
  else
    {
      read_eeprom(buffer, eeprom_internal, eeprom_start, size);
      write_ram(buffer, 0, size);
    }
}

// ------------------------------------------------------------------------------------------
void load_ram_image(byte num, bool size_4bit)
{
  if( print_serial_output )
    { 
      if( num<16 )
        Serial.print(F("Loading RAM image #"));
      else
        Serial.print(F("Invalid RAM image number: "));
      
      Serial.println(num);
    }

  if( size_4bit )
    load_data(true, num * 16, 0, 16);
  else
    load_data(false, num * 256, 0, 256);
}

// ------------------------------------------------------------------------------------------
void save_ram_image(byte num, bool size_4bit)
{
  if( print_serial_output )
    {
      if( num<16 )
        Serial.print(F("Saving RAM image #"));
      else
        Serial.print(F("Invalid RAM image number: "));
      
      Serial.println(num);
    }

  if( size_4bit )
    save_data(true, num * 16, 0, 16);
  else
    save_data(false, num * 256, 0, 256);
}

// ------------------------------------------------------------------------------------------
int read_analog_2bit(int pin)
{
  int res;
  
  int v = analogRead(pin);

  if     ( v < 400 ) res = 0;
  else if( v < 600 ) res = 1;
  else if( v < 700 ) res = 2;
  else               res = 3;

  return res;
}

// ------------------------------------------------------------------------------------------
int read_analog_4bit(int pin)
{
  int res;

  // average value of 10 reads
  unsigned long v = 0;
  for(int i=0; i<10; i++) v += analogRead(pin);
  v = v/10;

  if     ( v <  45 ) res =  0; //   0
  else if( v < 150 ) res =  1; //  90
  else if( v < 230 ) res =  2; // 177
  else if( v < 310 ) res =  3; // 241
  else if( v < 350 ) res =  4; // 321
  else if( v < 400 ) res =  5; // 366
  else if( v < 430 ) res =  6; // 410
  else if( v < 475 ) res =  7; // 444
  else if( v < 525 ) res =  8; // 510
  else if( v < 550 ) res =  9; // 535
  else if( v < 575 ) res = 10; // 560
  else if( v < 600 ) res = 11; // 580
  else if( v < 615 ) res = 12; // 607
  else if( v < 632 ) res = 13; // 623
  else if( v < 647 ) res = 14; // 640
  else               res = 15; // 654

  return res;
}

// ------------------------------------------------------------------------------------------
int read_dip()
{
  // DIP switches are labeled 1-4 from left to right which makes
  // switch 1 the MSB and switch 4 the LSB
  return read_analog_4bit(PIN_SW);
}

// ------------------------------------------------------------------------------------------
char serial_read_wait()
{
  while( Serial.available()==0 );
  return Serial.read();    
}

// ------------------------------------------------------------------------------------------
char hex_char_to_byte(char c)
{
  if( c>='0' && c<='9' )
    return c-'0';
  else if( c>='a' && c<='f' )
    return 10 + (c - 'a');
  else if( c>='A' && c<='F' )
    return 10 + (c - 'A');
  else
    return -1;
}

// ------------------------------------------------------------------------------------------
char read_hex_char(bool first)
{
  char c, cv;
  while( true )
    {
      c  = serial_read_wait();
      cv = hex_char_to_byte(c);
      if( cv>=0 )
        {
          Serial.print(c);
          return cv;
        }
      else if( !first && (c==' ' || c==13) )
        return -1;
      else if( c==27 )
        return -2;
    }
}

// ------------------------------------------------------------------------------------------
int serial_read_byte()
{
  int res = 0;
  char cv1 = -1, cv2 = -1;

  cv1 = read_hex_char(true);
  if( cv1==-2 )
    res = -1;
  else if( cv1<0 ) 
    Serial.print(F("00"));
  else
    {
      cv2 = read_hex_char(false);
      if( cv2==-2 )
        {
          Serial.write(127);
          res = -1;
        }
      else if( cv2<0 )
        {
          res = cv1;
          Serial.write(127);
          Serial.print(dec2hex(res));
        }
      else
        return cv1 * 16 + cv2;
    }

  return res;
}

// ------------------------------------------------------------------------------------------
void memory_dump(byte s, byte e)
{
  if( e==0 )
    {
      int a = s;
      do
        {
          read_ram(buffer, a, 16);
          print_data(buffer, a, 16);
          a += 16;
        }
      while( (!mode_4bit || a<16) && a<256 && serial_read_wait()==32 );
    }
  else
    {
      int n = ((int) e) - ((int) s) + 1;
      read_ram(buffer, s, n);
      print_data(buffer, s, n);
    }
}

// ------------------------------------------------------------------------------------------
void memory_clear(byte s, byte e, byte v)
{
  int n = ((int) e) - ((int) s) + 1;

  for(int i=0; i<n; i++) buffer[i] = v;
  write_ram(buffer, s, n);
  
  Serial.print(F("Set memory range "));
  Serial.print(dec2hex(s)); Serial.print('-'); Serial.print(dec2hex(e)); 
  Serial.print(F(" to value "));
  Serial.print(dec2hex(v)); 
  Serial.println('.');
}

// ------------------------------------------------------------------------------------------
void memory_update(int a)
{
  int i = 0, e = 0xff;
  if( mode_4bit ) e = 0x0f;

  while( a+i<=e )
    {
      if( (i%16)==0 )
        {
          if( i>0 ) Serial.println();
          Serial.print(dec2hex(a+i));
          Serial.print(F(": "));
        }
      else if( (i%16)==8 )
        Serial.print(' ');

      int d = serial_read_byte();
      if( d<0 ) break;
      Serial.print(' ');
      byte b = (byte) d;
      write_ram(&b, a+i, 1);
      i++;
    }

  Serial.println();
}

// ------------------------------------------------------------------------------------------
void disassemble(int a = 0, int e = -1)
{
  while( a <= e || e < 0 )
    {
      byte b;
      Serial.print(dec2hex(a));
      Serial.print(F(": "));
      read_ram(&b, a++, 1);
      Serial.print(dec2hex(b));
      Serial.print(F("   "));
      Serial.print(opcodes_4bit[b>>4].mnemonic);
      if( opcodes_4bit[b>>4].has_arg ) Serial.print(dec2hex(b&0x0f));
      Serial.println();
      if( e<0 && serial_read_wait()!=32 ) break;
    }
}

// ------------------------------------------------------------------------------------------

struct label_struct {
  char label[3];
  int  loc;
  byte forward_count;
  byte forward[ASM_MAX_LABEL_FORWARD_REF];
};

int num_labels;
struct label_struct labels[ASM_MAX_LABELS];


struct label_struct *find_label(char *lbl, int llen)
{
  struct label_struct *res = NULL;

  static char lbuf[3];
  lbuf[0] = lbl[0];
  lbuf[1] = llen>1 ? lbl[1] : 32;
  lbuf[2] = llen>2 ? lbl[2] : 32;

  for(int i=0; i<num_labels; i++)
    if( memcmp(labels[i].label, lbuf, 3)==0 )
      res = labels+i;

  if( res==NULL && num_labels<ASM_MAX_LABELS )
    {
      res = labels+num_labels;
      memcpy(res->label, lbuf, 3);
      res->forward_count = 0;
      res->loc = -1;
      num_labels++;
    }
      
  return res;
}

// ------------------------------------------------------------------------------------------
bool parse_arg(byte *buffer, int len, int &i, byte &value, bool force_hex = false)
{
  bool res = false;
  int b;
  if( i<len && buffer[i] == '$' || force_hex )
    {
      if( buffer[i] == '$' ) i++;
      if( i<len && (b=hex_char_to_byte(buffer[i]))>=0 )
        {
          i++;
          value = b;
          if( i<len && (b=hex_char_to_byte(buffer[i]))>=0 )
            {
              i++;
              value = (value<<4) + b;
            }
          res = true;
        }
    }
  else if( i<len && isDigit(buffer[i]) )
    {
      value = buffer[i]-48;
      i++;
      if( i<len && isDigit(buffer[i]) )
        {
          value = value * 10 + (buffer[i]-48);
          i++;
          if( i<len && isDigit(buffer[i]) )
            {
              value = value * 10 + (buffer[i]-48);
              i++;
            }
        }
      res = true;
    }

  return res;
}

// ------------------------------------------------------------------------------------------
void assemble(int addr)
{
  struct opcodes_struct *opcodes = opcodes_4bit;

  int a = addr;
  num_labels = 0;

  while( true )
    {
      Serial.print(dec2hex(a));
      Serial.print(F(": "));
      
      char c;
      bool ok = true;
      int len = 0, commentLen = 0;
      while(len<BUFFER_SIZE)
        {
          c = serial_read_wait();

          if( c==26 || c==27 || c==13 )
            break;
          else if( c==8 || c==127 )
            {
              if( commentLen>0 )
                { commentLen--; Serial.write(127); }
              else if( len>0 ) 
                { len--; Serial.write(127); }
            }
          else
            {
              if( commentLen==0 && c==';' )
                {
                  commentLen = 1;
                  Serial.write(c);
                }
              else if( commentLen>0 )
                {
                  commentLen++;

                  // if the receive buffer is starting to fill up then
                  // save time by not echoing back comment characters
                  // (if the receive buffer runs over then actual
                  // code characters get lost)
                  if( Serial.available()<10 ) Serial.write(c);
                }
              else
                {
                  buffer[len++] = c;
                  Serial.write(c);
                }
            }
        }
      Serial.println();

      if( c==26 || c==27 )
        break;
      else
        {
          // add a space at the end of the line so 2-character
          // mnemonics can be parsed correctly
          buffer[len++] = ' ';
          buffer[len]   = 0;
        }

      // if line is empty then start over
      if( len==0 ) continue;

      // byte counter in buffer
      int i = 0;

      // check label
      if( isAlpha(buffer[i]) )
        {
          // read label
          while( i<len && isAlphaNumeric(buffer[i]) ) i++;

          struct label_struct *lbl = find_label((char *) buffer, i);
          if( lbl==NULL )
            {
              Serial.println(F("Too many labels."));
              ok = false;
            }
          else if( lbl->loc >= 0 )
            {
              Serial.println(F("Duplicate label definition."));
              ok = false;
            }
          else
            {
              // set label address in forward references (if any)
              for(int j=0; j<lbl->forward_count; j++)
                {
                  byte b;
                  read_ram(&b, lbl->forward[j], 1);
                  
                  if( mode_4bit )
                    b = (b & 0xF0) | ((a+(b&0x0F)) & 0x0F);
                  else
                    b  = b + a;
                  
                  write_ram(&b, lbl->forward[j], 1);
                }
              lbl->forward_count = 0;

              // set label address
              lbl->loc = a;
            }
        }
      else if( !isSpace(buffer[0]) )
        {
          // labels must start with alphabetic character
          Serial.print(F("Invalid label: "));
          while( i<len && !isSpace(buffer[i]) ) Serial.write(buffer[i++]);
          Serial.println();
          ok = false;
        }

      // read mnemonic
      int opcodeIdx = -1;
      byte arg = 0;
      if( ok )
        {
          // skip spaces before mnemonic
          while( i<len && isSpace(buffer[i]) ) i++;

          if( i < len )
            {
              int mstart = i;
              bool isimmediate = false, noargmatch = false;
              ok = false;
              if( i+3<=len )
                {
                  i = i + 3;

                  // skip spaces after mnemonic
                  while( i<len && isSpace(buffer[i]) ) i++;
                  
                  // determine whether we have an immediate ("#") argument
                  if( i<len && buffer[i]=='#' ) { i++; isimmediate = true; }
                  
                  // find opcode
                  for(int j=0; !ok && opcodes[j].mnemonic!=NULL; j++)
                    if( strncasecmp(opcodes[j].mnemonic, (const char *) (buffer+mstart), 3)==0 &&
                        isimmediate == (opcodes[j].mnemonic[4]=='#') )
                      {
                        if( (!(opcodes[j].has_arg) || (i<len)) )
                          {
                            opcodeIdx = j;
                            ok        = true;
                          }
                        else
                          noargmatch = true;
                      }
                }

              if( !ok )
                {
                  if( noargmatch )
                    Serial.println(F("Expected argument"));
                  else
                    {
                      Serial.print(F("Invalid mnemonic: "));
                      i = mstart;
                      while( i<len && !isSpace(buffer[i]) ) Serial.write(buffer[i++]);
                      if( isimmediate ) Serial.print(F(" #"));
                      Serial.println();
                    }
                }
              else if( opcodes[opcodeIdx].has_arg )
                {
                  // opcode requires argument
                  if( i<len )
                    {
                      int b;

                      // if the argument start with an alphabetic character then it is a label reference
                      if( isAlpha(buffer[i]) )
                        {
                          int lblStart = i;
                          while( i<len && isAlphaNumeric(buffer[i]) ) i++;
                          struct label_struct *lbl = find_label((char *) (buffer+lblStart), i-lblStart);

                          if( lbl==NULL )
                            {
                              Serial.println(F("Too many labels."));
                              ok = false;
                            }
                          else
                            {
                              if( lbl->loc<0 )
                                {
                                  // if label address is not yet defined then remember the forward reference
                                  if( lbl->forward_count < ASM_MAX_LABEL_FORWARD_REF )
                                    lbl->forward[lbl->forward_count++] = mode_4bit ? a : a+1;
                                  else
                                    {
                                      Serial.println(F("Too many forward references to label."));
                                      ok = false;
                                    }
                                }
                              else
                                arg = lbl->loc;
                            }

                          // allow for '+' or '-' after label name to support simple arithmetic
                          if( buffer[i]=='+' || buffer[i]=='-' )
                            {
                              bool issub = (buffer[i]=='-');
                              byte addval;
                              i++;

                              // skip spaces after +/-
                              while( i<len && isSpace(buffer[i]) ) i++;

                              if( i<len )
                                {
                                  if( parse_arg(buffer, len, i, addval) )
                                    {
                                      if( issub ) addval = -addval;
                                      arg += addval;
                                    }
                                  else
                                    {
                                      Serial.print(F("Invalid argument to '+/-' operator: "));
                                      while( i<len && !isSpace(buffer[i]) ) Serial.write(buffer[i++]);
                                      Serial.println();
                                      ok = false;
                                    }
                                }
                              else
                                {
                                  Serial.println(F("Expected argument to '+/-' operator"));
                                  ok = false;
                                }
                            }
                        }
                      else if( !parse_arg(buffer, len, i, arg) )
                        {
                          Serial.print(F("Invalid argument: "));
                          while( i<len && !isSpace(buffer[i]) ) Serial.write(buffer[i++]);
                          Serial.println();
                          ok = false;
                        }
                    }
                  else
                    {
                      Serial.println(F("Expected argument"));
                      ok = false;
                    }
                }
            }
        }

      if( ok )
        {
          // warn if there was any extra characters
          while( i<len && isSpace(buffer[i]) ) i++;
          if( i<len )
            {
              Serial.print(F("Extra ignored: "));
              Serial.println((char *) (buffer+i));
            }
        }

      if( ok && opcodeIdx>=0 )
        {
          byte opcode = opcodes[opcodeIdx].opcode;

          if( opcode==0xff )
            {
              // .BY opcode (byte value)
              write_ram(&arg, a++, 1);
            }
          else if( opcode==0xfe )
            {
              // .OR opcode (set new address)
              //if( a>addr ) { Serial.println('\n'); disassemble(addr, a-1); }

              addr = arg;
              a    = addr;
            }
          else if( mode_4bit )
            {
              opcode |= arg & 0x0f;
              write_ram(&opcode, a++, 1);
            }
          else
            {
              write_ram(&opcode, a++, 1);
              if( opcodes[opcodeIdx].has_arg ) write_ram(&arg, a++, 1);
            }
        }
    }

  if( a>addr )
    {
      bool header = true;
      for(int i=0; i<num_labels; i++)
        if( labels[i].loc<0 )
          {
            if( header ) 
              { Serial.print(F("\nUNDEFINED LABELS: ")); header = false; }
            else
              Serial.print(',');

            for(int j=0; j<3 && labels[i].label[j]!=' '; j++) 
              Serial.write(labels[i].label[j]);
          }
      Serial.println();

      //Serial.println('\n'); disassemble(addr, a-1);
    }
}

// ------------------------------------------------------------------------------------------
byte num_breakpoints, breakpoints[10];

void save_breakpoints()
{
  for(byte i=0; i<num_breakpoints; i++)
    EEPROM.write(1021-i, breakpoints[i]);
  EEPROM.write(1022, num_breakpoints);
}

void load_breakpoints()
{
  byte n = EEPROM.read(1022);
  if( n<=MAX_BREAKPOINTS )
    {
      num_breakpoints = n;
      for(byte i=0; i<num_breakpoints; i++)
        breakpoints[i] = EEPROM.read(1021-i);
    }
}

void show_breakpoints()
{
  Serial.print(F("Breakpoints at:"));
  for(int i=0; i<num_breakpoints; i++)
    {
      Serial.print(' ');
      Serial.print(dec2hex(breakpoints[i]));
    }
  Serial.println();
}

bool check_breakpoint(byte pc)
{
  byte bp;
  for(bp=0; bp<num_breakpoints; bp++)
    if( pc==breakpoints[bp] )
      break;
  
  if( bp<num_breakpoints )
    {
      Serial.print(F("Stopped at breakpoint: "));
      Serial.println(dec2hex(breakpoints[bp]));
      return true;
    }

  return false;
}

// ------------------------------------------------------------------------------------------
byte get_pc()
{
  byte pc;

  // Make sure we don't get delayed by an interrupt
  noInterrupts();

  // Wait for a HIGH->LOW transition on the clock
  // to make sure the clock is low at the point where
  // control is returned to the control logic
  // (otherwise the control unit's step counter will
  // already count up on the HIGH->LOW edge and
  // thereby skip the first step of the fetch cycle).
  wait_clock_high();
  wait_clock_low();
  
  // enable EEPROM before switching  I/O pins to INPUT, 
  // otherwise we produce a short HIGH signal on the I/O pins
  SET_EE_LOW();

  // set control pins (MI/RI/RO) as inputs
  SET_CS_INPUT();

  // program counter is on the bus on the first cycle of the instruction
  wait_clock_high();
  pc = read_bus();

  // set control pins (MI/RI/RO) as outputs and output LOW
  SET_MI_LOW();
  SET_RO_LOW();
  SET_RI_LOW();
  SET_CS_OUTPUT();

  // disable EEPROM (take back control)
  SET_EE_HIGH();

  // re-enable interrupts
  interrupts();

  return pc;
}

void set_pc(byte pc)
{
  byte n, data[2], jmp[2], current_pc = get_pc();
  struct opcodes_struct *opcodes = opcodes_4bit;

  // if we're already at address "pc" then there is nothing to do
  if( current_pc == pc ) return;

  // construct command: JMP pc
  int i;
  for(i=0; opcodes[i].mnemonic!=NULL; i++)
    if( strncasecmp(opcodes[i].mnemonic, "JMP", 3)==0 )
      break;

  if( opcodes[i].mnemonic==NULL )
    {
      Serial.print(F("Error in set_pc: Can't find JMP opcode."));
      return;
    }
  else if( mode_4bit )
    { n = 1; jmp[0] = opcodes[i].opcode | (pc & 0x0f); }
  else
    { n = 2; jmp[0] = opcodes[i].opcode;  jmp[1] = pc; }

  // get memory content at current PC location
  read_ram(data, current_pc, n); 

  // write "JMP pc" command at current PC location
  write_ram(jmp, current_pc, n);

  // execute JMP command (i.e. set PC to new location)
  run(1);

  // restore memory content at previous PC location
  write_ram(data, current_pc, n);
}

// ------------------------------------------------------------------------------------------
bool run(int num_instructions)
{
  bool first = true;
  byte pc, i=0, j, opcode, num_cycles, hlt;
  hlt = B11110000;
  for(int i=0; opcodes_4bit[i].mnemonic!=NULL; i++)
    buffer[opcodes_4bit[i].opcode & 0xF0] = opcodes_4bit[i].num_cycles;

  // make sure counter i (of type byte) can never get greater than
  // num_instructions if requested to run indefinitely
  if( num_instructions<0 ) num_instructions = 32767;

  // Make sure we don't get delayed by an interrupt
  noInterrupts();

  // Wait for a HIGH->LOW transition on the clock
  // to make sure the clock is low at the point where
  // control is returned to the control logic
  // (otherwise the control unit's step counter will
  // already count up on the HIGH->LOW edge and
  // thereby skip the first step of the fetch cycle).
  wait_clock_high();
  wait_clock_low();
  
  // enable EEPROM before switching  I/O pins to INPUT, 
  // otherwise we produce a short HIGH signal on the I/O pins
  SET_EE_LOW();

  // set control pins (MI/RI/RO) as inputs
  SET_CS_INPUT();

  while( ++i <= num_instructions )
    {
      // program counter is on the bus on the first cycle of the instruction
      wait_clock_high();
      pc = read_bus();

      // check breakpoints (unrolled loop to improve performance
      // allowing for 10 breakpoints at 160kHz clock frequency)
      if( first || num_breakpoints<1  ) goto go;
      if( pc==breakpoints[0] ) break;
      if( num_breakpoints<2  ) goto go;
      if( pc==breakpoints[1] ) break;
      if( num_breakpoints<3  ) goto go;
      if( pc==breakpoints[2] ) break;
      if( num_breakpoints<4  ) goto go;
      if( pc==breakpoints[3] ) break;
      if( num_breakpoints<5  ) goto go;
      if( pc==breakpoints[4] ) break;
      if( num_breakpoints<6  ) goto go;
      if( pc==breakpoints[5] ) break;
      if( num_breakpoints<7  ) goto go;
      if( pc==breakpoints[6] ) break;
      if( num_breakpoints<8  ) goto go;
      if( pc==breakpoints[7] ) break;
      if( num_breakpoints<9  ) goto go;
      if( pc==breakpoints[8] ) break;
      if( num_breakpoints<10 ) goto go;
      if( pc==breakpoints[9] ) break;

    go:
      // if RX=0 then something is coming in via the serial connection
      // we want to stop running in that case
      if( (PIND & 0x01)==0 ) break;

      wait_clock_low();

      // opcode is on the bus on the second cycle of the instruction
      wait_clock_high();
      opcode = read_bus();
      if( opcode==hlt ) break;
      num_cycles = buffer[opcode&(mode_4bit ? 0xF0 : 0xFC)];
      wait_clock_low();

      // skip the number of cycles associated with the opcode
      for(j=0; j<num_cycles; j++)
        {
          wait_clock_high();
          wait_clock_low();
        }

      first = false;
    }

  // set control pins (MI/RI/RO) as outputs and output LOW
  SET_MI_LOW();
  SET_RO_LOW();
  SET_RI_LOW();
  SET_CS_OUTPUT();

  // disable EEPROM (take back control)
  SET_EE_HIGH();

  // re-enable interrupts
  interrupts();

  // if we stopped because of serial activity then wait 10ms
  // to allow for the character(s) to be received and then purge 
  // the input queue (so we don't have them show up on the command prompt)
  if( (PIND & 0x01)==0 )
    {
      delay(10);
      while( Serial.available() ) Serial.read();
      return false;
    }
  else if( opcode==hlt )
    {
      Serial.println(F("Stopped at HLT instruction."));
      return false;
    }
  else
    return true;
}

// ------------------------------------------------------------------------------------------
void monitor()
{
  Serial.println(F("\nEntering monitor ORCSGirls edition ...\n"));
  wait_clock_info(LOW);
  wait_clock_info(HIGH);

  load_breakpoints();
  if( num_breakpoints>0 ) {Serial.print('\n'); show_breakpoints();}

  bool go = true;
  while( go )
    {
      Serial.print(F("\n."));

      int len = 0;
      while(len<BUFFER_SIZE)
        {
          char c = serial_read_wait();
          if( c==13 ) 
            break;
          else if( c==8 || c==127 )
            {
              if( len>0 ) { len--; Serial.write(127); }
            }
          else if( c==26 || c==27 )
            {
              len = 0; 
              break;
            }
          else
            {
              buffer[len++] = c;
              Serial.write(c);
            }
        }
      
      int i=0;
      while( i<len && isSpace(buffer[i]) ) i++;
      if( i < len )
        {
          Serial.println();

          char cmd = buffer[i];
          switch( cmd )
            {
            case 'l': 
              {
                byte n = 0;
                i++;
                while( i<len && isSpace(buffer[i]) ) i++;
                if( parse_arg(buffer, len, i, n, true) )
                  load_ram_image(n, mode_4bit); 
                else
                  Serial.println(F("Expected image number argument."));
                break;
              }

            case 's': 
              {
                byte n = 0;
                i++;
                while( i<len && isSpace(buffer[i]) ) i++;
                if( parse_arg(buffer, len, i, n, true) )
                  save_ram_image(n, mode_4bit); 
                else
                  Serial.println(F("Expected image number argument."));
                break;
              }

            case 'a': 
              {
                byte s = 0;
                i++;
                while( i<len && isSpace(buffer[i]) ) i++;
                parse_arg(buffer, len, i, s, true);

                assemble(s); 
                break;
              }

            case 'b':
              {
                byte a = 1;
                i++;
                while( i<len && isSpace(buffer[i]) ) i++;
                if( parse_arg(buffer, len, i, a, true) )
                  {
                    int j;
                    for(j=0; j<num_breakpoints; j++)
                      if( breakpoints[j]==a )
                        break;

                    if( j>=num_breakpoints )
                      {
                        if( num_breakpoints<MAX_BREAKPOINTS )
                          {
                            breakpoints[num_breakpoints++] = a;
                            Serial.print(F("Added breakpoint at: "));
                            Serial.println(dec2hex(a));
                            save_breakpoints();
                          }
                        else
                          Serial.println(F("Too many breakpoints."));
                      }
                    else
                      Serial.println(F("Breakpoint already exists."));
                  }
                else
                  show_breakpoints();
                
                break;
              }
              
            case 'B':
              {
                byte a = 1;
                i++;
                while( i<len && isSpace(buffer[i]) ) i++;
                if( parse_arg(buffer, len, i, a, true) )
                  {
                    int j;
                    for(j=0; j<num_breakpoints; j++)
                      if( breakpoints[j]==a )
                        break;

                    if( j<num_breakpoints )
                      {
                        breakpoints[j] = breakpoints[--num_breakpoints];
                        Serial.print(F("Removed breakpoint at: "));
                        Serial.println(dec2hex(a));
                        save_breakpoints();
                      }
                    else
                      Serial.println(F("Breakpoint not found."));
                  }
                else if( i<len && buffer[i]=='*' )
                  {
                    Serial.println(F("Removing all breakpoints."));
                    num_breakpoints = 0;
                    save_breakpoints();
                  }
                else
                  Serial.println(F("Expected address argument."));
                
                break;
              }
              
            case 'd': 
              {
                byte s = 0, e = 0;
                i++;
                while( i<len && isSpace(buffer[i]) ) i++;
                parse_arg(buffer, len, i, s, true);
                while( i<len && isSpace(buffer[i]) ) i++;

                disassemble(s, parse_arg(buffer, len, i, e, true) ? e : -1); 
                break;
              }

            case 'C':
              {
                byte s = 0, e = 0xff, v = 0;
                if( mode_4bit ) e = 0x0f;
                i++;
                while( i<len && isSpace(buffer[i]) ) i++;
                parse_arg(buffer, len, i, s, true);
                while( i<len && isSpace(buffer[i]) ) i++;
                parse_arg(buffer, len, i, e, true);
                while( i<len && isSpace(buffer[i]) ) i++;
                parse_arg(buffer, len, i, v, true);

                memory_clear(s, e, v);
                break;
              }

            case 'm': 
              {
                byte s = 0, e = 0;
                i++;
                while( i<len && isSpace(buffer[i]) ) i++;
                parse_arg(buffer, len, i, s, true);
                while( i<len && isSpace(buffer[i]) ) i++;
                parse_arg(buffer, len, i, e, true);
                
                memory_dump(s, e);
                break;
              }

            case 'M': 
              {
                byte s = 0;
                i++;
                while( i<len && isSpace(buffer[i]) ) i++;
                parse_arg(buffer, len, i, s, true);

                memory_update(s);
                break;
              }

            case 't':
              {
                byte n = 0;
                i++;
                while( i<len && isSpace(buffer[i]) ) i++;
                bool have_num_steps = parse_arg(buffer, len, i, n);

                byte s = 0;
                while( !have_num_steps || s<n )
                  {
                    // show next command
                    byte pc = get_pc();
                    disassemble(pc, pc);
                    
                    // check for breakpoint hit (but not on first step)
                    if( s>0 && check_breakpoint(pc) ) 
                      break;

                    // if we have no step restriction, wait for user input
                    if( !have_num_steps && serial_read_wait()!=32 ) 
                      break;

                    // take step
                    if( run(1) )
                      s++;
                    else
                      break;
                  }

                break;
              }

            case 'r':
              {
                byte n = 0;

                // run number of steps
                i++;
                while( i<len && isSpace(buffer[i]) ) i++;
                if( parse_arg(buffer, len, i, n) )
                  run(n);
                else
                  run(-1);
                
                // notify user if stopped at a breakpoint
                check_breakpoint(get_pc());

                break;
              }

            case 'R':
              {
                byte n = 0;

                i++;
                while( i<len && isSpace(buffer[i]) ) i++;
                if( parse_arg(buffer, len, i, n, true) )
                  set_pc(n);
                else
                  set_pc(0);

                break;
              }
            case 'x': 
              go = false;
              break;

            case 'h':
              Serial.println(F("\nInstruction set:"));
              Serial.println(F("LDA addr      Load value in memory address addr into A register       b0001"));
              Serial.println(F("ADD addr      Add value in memory address addr to A register          b0010")); 
              Serial.println(F("SUB addr      Subtract value in memory address addr from A register   b0011"));
              Serial.println(F("STA addr      Write value in A register to memory address addr        b0100"));
              Serial.println(F("LDI value     Store value in A register directly                      b0101"));
              Serial.println(F("JMP addr      Jump to address addr                                    b0110"));
              Serial.println(F("JC  addr      Jump to address addr if carry flag is set               b0111"));
              Serial.println(F("JZ  addr      Jump to address addr if zero flag is set                b1000"));
              Serial.println(F("DSI value     Display value on number display                         b1001"));
              Serial.println(F("INC value     Add value to register A                                 b1010"));
              Serial.println(F("DEC value     Subtract value from register A                          b1011"));
              Serial.println(F("LDB addr      Load value in memory address addr into B register       b1100"));
              Serial.println(F("DSP addr      Outputs value at address vvvvv on number display        b1101"));
              Serial.println(F("OUT           Display value in A register on number display           b1110"));
              Serial.println(F("HLT           Halt program                                            b1111"));
              break;

            default:
              Serial.println(F("\nCommands:"));
              Serial.println(F("a [n]         Assemble starting at address n"));
              Serial.println(F("d [n] [m]     Disassemble from address n to address m"));
              Serial.println(F("m [n] [m]     Dump RAM from address n to address m"));
              Serial.println(F("M [n]         Modify RAM contents starting at address n"));
              Serial.println(F("C [n] [m] [v] Write value v to RAM from address n to address m"));
              Serial.println(F("R [a]         Reset program counter [to address a]"));
              Serial.println(F("t [n]         Step n instructions, showing instructions to be executed"));
              Serial.println(F("r [n]         Run n instructions (if n not given then run indefinitely)"));
              Serial.println(F("b [a]         Add breakpoint at address a or show breakpoints"));
              Serial.println(F("B a           Remove breakpoint at address a (if a=* then remove all)"));
              Serial.println(F("s n           Save RAM image to EEPROM file #n"));
              Serial.println(F("l n           Load RAM image from EEPROM file #n"));
              Serial.println(F("h             Show help\n"));
              Serial.println(F("x             Exit monitor\n"));
              break;
            }
        }
    }

  Serial.println(F("Exiting monitor..."));
  wait_clock_stop();
}

// ------------------------------------------------------------------------------------------
void setup() 
{
  enable_signal_control();
  pinMode(PIN_CLK, INPUT);
  pinMode(PIN_EE, OUTPUT);
  Serial.begin(9600);

  if( auto_load ) 
    {
    int prog = read_dip();
    if( prog == 0x0f )
      {
        enable_signal_control();
        print_serial_output = true;
        monitor();
        disable_signal_control(false);
      }
    else if( prog>0 )
      {
        enable_signal_control();

        if( print_serial_output )
          {
            Serial.print(F("Auto-run RAM image #"));
            Serial.println(prog);
          }

        load_ram_image(prog, true);
        disable_signal_control(true);
      }
    else
      {
        disable_signal_control(false); 
      }
    }
  else 
    {
      disable_signal_control(false); 
    }
}

// ------------------------------------------------------------------------------------------
void loop() 
{
  int btn = 0;

  while( Serial.available()>0 ) Serial.read();

  if( print_serial_output )
    Serial.println(F("\nWaiting for LOAD or SAVE button or ESC to enter monitor..."));
  while( (btn=read_analog_2bit(PIN_BTN))==0 && Serial.available()==0 );
  print_serial_output = Serial.available()>0;

  if( Serial.read()==27 )
    {
      enable_signal_control();
      monitor();
      disable_signal_control(false);
    }
  else if( btn>0 )
    {
      enable_signal_control();

      int dip = read_dip();
      if( btn & 1 )
        { 
          save_ram_image(dip, true);
          wait_clock_stop();
        }
      else if ( btn & 2 )
        { 
          load_ram_image(dip, true);
          wait_clock_stop();
        }

      disable_signal_control(false);
    }
}
