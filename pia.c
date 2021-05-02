/*
    PIA Communicator from RC6502
    https://github.com/tebl/RC6502-Apple-1-Replica
    
    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#include <avr/io.h>
#include <util/delay.h>
#include <avr/interrupt.h>

#include "iox.h"
#include "uart.h"

#define BAUD 115200

#define HANDSHAKE_DDR DDRD
#define HANDSHAKE_PORT PORTD
#define HANDSHAKE_PIN PIND

#define CLOCK_PORT PORTB
#define CLOCK_PIN  PINB
#define CLOCK_DDR  DDRB

#define KBD_READY 2
#define VIDEO_DA 3
#define KBD_STROBE 4
#define VIDEO_RDA 5

#define RESET_OUT 7 // PD7
#define RESET_IN 0  // PB0
#define CLOCK_OUT 1 // PB1
#define CLOCK_UNUSED 6 // PD6 must be input

#define VIDEO_IODIR IODIRA0
#define VIDEO_GPPU GPPUA0
#define VIDEO_GPIO GPIOA0

#define KBD_IODIR IODIRB0
#define KBD_GPIO GPIOB0

#define KBD_STROBE_LO HANDSHAKE_PORT &= ~(1 << KBD_STROBE)
#define KBD_STROBE_HI HANDSHAKE_PORT |= (1 << KBD_STROBE)
#define VIDEO_RDA_LO HANDSHAKE_PORT &= ~(1 << VIDEO_RDA)
#define VIDEO_RDA_HI HANDSHAKE_PORT |= (1 << VIDEO_RDA)

#define RESET_LO HANDSHAKE_PORT &= ~(1 << RESET_OUT)
#define RESET_HI HANDSHAKE_PORT |= (1 << RESET_OUT)
#define GET_RESETSW (CLOCK_PIN & (1 << RESET_IN))

#define GET_KBD_READY (HANDSHAKE_PIN & (1 << KBD_READY))
#define GET_VIDEO_DA (HANDSHAKE_PIN & (1 << VIDEO_DA))

#define KBD_INTERRUPT_ENABLE 1
#define KBD_SEND_TIMEOUT 23

//#define DIVIDER 7  // divide 16 = 1MHz
#define DIVIDER 3  // divide 8 = 2MHz

void clock_init() {
  // setup timer 0
  TCCR1A = 0b01000000;  // toggle OC1A on compare match CTC mode (WGM11=0, WGM10=0)
  TCCR1B = 0b00001001;  // WGN13 = ,0 WGM12=1, no prescaling
  OCR1A = DIVIDER;  //
}

void interrupt_init() {
  // setup pin change interrupt
  PCMSK0 = (1 << PCINT0);
  PCMSK1 = 0;
  PCMSK2 = 0;
  PCICR |= (1 << PCIE0); // enable PCINT0 (PB0)
}

void print_hello() {
  char msg[] = "RC6502 Apple 1 Replica\n\r";
  for (int i = 0; ; i++) {
    if (msg[i] == '\0') {
      return;
    }
    uart_putc(msg[i]);
  }
}

void pia_init(void)
{
    uart_init(BAUD);
    //uart_putc('!');
    print_hello();
    iox_init();

    HANDSHAKE_DDR |= (1 << KBD_STROBE) | (1 << VIDEO_RDA) | (1 << RESET_OUT);
    HANDSHAKE_DDR &= ~((1 << KBD_READY) | (1 << VIDEO_DA) | (1 << CLOCK_UNUSED));

    CLOCK_DDR |= (1 << CLOCK_OUT);
    CLOCK_DDR &= ~(1 << RESET_IN);
    CLOCK_PORT |= (1 << RESET_IN); // PULL-UP PB0
    clock_init();  // setup timer 0

    RESET_HI;    // reset cpu
    _delay_ms(600);
    RESET_LO;

    interrupt_init();
    
    iox_write(0, VIDEO_IODIR, 0xFF);    // video input
    iox_write(0, VIDEO_GPPU, 0x80);     // pullup on bit 7
    iox_write(0, KBD_IODIR, 0x00);      // keyboard output
}

ISR(PCINT0_vect) {
  if (!GET_RESETSW) {
    RESET_HI;
    _delay_ms(600);// perform 6502 reset
  } else {
    RESET_LO;
  }
}


char map_to_ascii(int c) {
  /* Convert ESC key */
  if (c == 203) {
    c = 27;
  }

  /* Ctrl A-Z */
  if (c > 576 && c < 603) {
    c -= 576;
  }

  /* Convert lowercase keys to UPPERCASE */
  if (c > 96 && c < 123) {
    c -= 32;
  }
  
  return c;
}

void pia_send(int c) {
  /* Make sure STROBE signal is off */
  KBD_STROBE_LO;
  c = map_to_ascii(c);

  /* Output the actual keys as long as it's supported */
  if (c < 96) {
    iox_write(0, KBD_GPIO, c | 128);

    KBD_STROBE_HI;
    if (KBD_INTERRUPT_ENABLE) {
      uint8_t timeout;

      /* Wait for KBD_READY (CA2) to go HIGH */
      timeout = KBD_SEND_TIMEOUT;
      while(GET_KBD_READY == 0) {
        if (timeout == 0) break;
        else timeout--;
      }
      KBD_STROBE_LO;

      /* Wait for KBD_READY (CA2) to go LOW */
      timeout = KBD_SEND_TIMEOUT;
      while(GET_KBD_READY != 0) {
        if (timeout == 0) break;
        else timeout--;
      }
    } else {
      KBD_STROBE_LO;
    }
  }
}

void serial_receive() {
  if (uart_test() > 0) {
    int c = uart_getc();
    pia_send(c);
  }
}

char send_ascii(char c) {
  switch (c) {
    case '\r': uart_putc('\n'); /* Replace CR with LF */
    default:
      uart_putc(c);
  }
}

void serial_transmit() {
    VIDEO_RDA_HI;
    _delay_us(1);
  if (GET_VIDEO_DA != 0) {
    char c = iox_read(0, VIDEO_GPIO) & 127;
    VIDEO_RDA_LO;

    send_ascii(c);
  }
}

void pia_exchange(void) {
  serial_receive();
  serial_transmit();
}

int main(void)
{
    pia_init();

    while(1) {
        pia_exchange();
    }
}
