/* ----------------------------------------------------------------------------
 * "THE BEER-WARE LICENSE" (Revision 42):
 * <info@gerhard-bertelsmann.de> wrote this file. As long as you retain this
 * notice you can do whatever you want with this stuff. If we meet some day,
 * and you think this stuff is worth it, you can buy me a beer in return
 * Gerhard Bertelsmann
 * ----------------------------------------------------------------------------
 */

#include <xc.h>
#include "main.h"
#include "i2c_lcd.h"
#include "lcd.h"

#pragma config FOSC=INTOSC, PLLEN=OFF, MCLRE=ON, WDTE=OFF
#pragma config LVP=ON, CLKOUTEN=OFF

// timer interval in microseconds
#define INTERVAL	50
#define TIMER0_VAL	(256 - (INTERVAL-2))

  /* DCC    58us */
  /* MM1    26us */
  /* MM2    52us */
  /* mfx    50us */

struct serial_buffer tx_fifo, rx_fifo;

volatile unsigned int pulse_high = 25;
volatile unsigned int pulse_low = 75;

void interrupt ISR(void) {
    if (CCP1IF) {
	CCP1IF = 0;
	if (CCP1M0) {
	    CCP1M0 = 0;
	    CCPR1 += pulse_high;
	} else {
	    CCP1M0 = 1;
	    CCPR1 += pulse_low;
	}
    }
}

/* RA4 SDA I2C
 * RA5 SCL I2C
 * RC1 RxD UART
 * RC2 TxD UART
 * // RC3 CCP1
 */

void pps_init(void) {
    PPSLOCK = 0x55;
    PPSLOCK = 0xaa;
    PPSLOCK = 0;		// unlock PPS
    // set USART : RX on RC1 , TX on RA1 / 40001729B.pdf page 141
    RXPPS = 0b10001;		// input  EUSART RX -> RC1
    RC2PPS = 0b10100;		// RC2 output TX/CK
    /* I2C */
    SSPCLKPPS = 0x05;
    RA5PPS = 0b10000;		// RA5 output SCL
    SSPDATPPS = 0x04;
    RA4PPS = 0b10001;		// RA4 output SDA
    // CCP1
    // RC3PPS = 0b01100

    PPSLOCK = 0x55;
    PPSLOCK = 0xaa;
    PPSLOCK = 1;		// lock PPS
}

void system_init(void) {
    // switch off analog
    OSCCON = 0b11110000;	// Configure oscillator
    //1------- use PLL to get 4x8 Mhz (system clock)
    //-1110--- 8 MHz internal oscillator (instruction clock)
    //------00 oscillator selected with INTOSC
    ANSELA = 0;
    ANSELC = 0;
    ADCON0 = 0;
    ADCON1 = 0;
    ADCON2 = 0;
    CM1CON0 = 0;
    CM1CON1 = 0;
    CM2CON0 = 0;
    CM2CON1 = 0;

    /* I2C MSSP 40001729B.pdf page 302 */
    TRISA4 = 1;
    TRISA5 = 1;
    /*
       TRISC2 = 0;
     */
    /* USART */
    TRISC0 = 0;
    TRISC1 = 1;
    /* RA2&RC0 analog input */
    TRISA2 = 1;
    TRISC0 = 1;
    // TRISC5 = 0;  // CCP1
    // setup interrupt events
    //clear all relevant interrupt flags
    PIR1bits.SSP1IF = 0;
    PIR1bits.TMR1IF = 0;
    PIR1bits.CCP1IF = 0;
    //activate interrupt bits
    PIE1bits.CCP1IE = 0;	// disable interrupt on CCP1 will be check by polling as of today
    PIE1bits.SSP1IE = 1;
    INTCONbits.PEIE = 1;
    INTCONbits.GIE = 1;
}

void i2c_init(void) {
    /* Initialise I2C MSSP Master */
    SSP1CON1 = 0b00101000;
    SSP1CON2 = 0x00;
    SSP1CON3 = 0x00;
    /* Master at 100KHz */
    /* clock = FOSC/(4 * (SSPADD + 1)) */
    SSP1ADD = SSP1ADD_VAL;
    /* Slew rate disabled */
    SSP1STAT = 0b11000000;
}

void ad_init(void) {
    /* RA2&RC0 analog */
    ANSELA = 1 << 2;
    ANSELC = 1 << 0;
    /* right justified ; FOSC/64 ;VREF- GND & VREF+ VDD */
    ADCON1 = 0b01100000;
    /* RA2 analog input */
    ADCON0 = AD_POTI;
    /* RC0 (AN4) analog input */
    /* ADCON0 = AD_SENSE; */
}

void uart_init(void) {
    TXSTAbits.TX9 = 1;		// 8-bit transmission
    TXSTAbits.TX9D = 1;		//  one extra stop bit
    TXSTAbits.TXEN = 1;		// transmit enabled
    TXSTAbits.SYNC = 0;		// asynchronous mode
    TXSTAbits.BRGH = 1;		// high speed
    RCSTAbits.SPEN = 1;		// enable serial port (configures RX/DT and TX/CK pins as serial port pins)
    RCSTAbits.RX9 = 0;		// 8-bit reception
    RCSTAbits.CREN = 1;		// enable receiver
    BAUDCON1bits.BRG16 = USE_BRG16;	// 8-bit baud rate generator

    SPBRG = SBRG_VAL;		// calculated by defines

    PIR1bits.RCIF = 0;
}

void timer0_init(void) {
    OPTION_REGbits.TMR0CS = 0;	// FOSC / 4
    OPTION_REGbits.PSA = 0;	// use prescaler
    OPTION_REGbits.PS = 0b010;	// prescaler 1:8
    TMR0 = TIMER0_VAL;
    T0IE = 1;
}

void timer1_init(void) {
    T1CON = 0b00110001;
    //00------ FOSC/4 as counting source
    //--11---- prescaler 1:8 (counting every us)
    //-------1 timer on
    T1GCONbits.TMR1GE = 0;	// timer is not controlled by gate.
    TMR1H = 0;			// reset timer1 high
    TMR1L = 0;			// and low bytes - prescaler automatic reset
    CCP1CON = 0b00001000;	// set up capture and compare
    //----1000   Compare mode: set output on compare match (set CCPxIF)
    // set ccp1 register to the highest value to avoid useless interrupt
    CCPR1H = 0xFF;
    CCPR1L = 0xFF;
    // don't use yet
    // CCP1IE = 1;
}

void timer2_init(void) {
    // default (FOSC/4)
    T2CON = 0b00000100;
    //-----1-- timer on
    //------00 prescaler 1:1 (overflow every 32us)
    TMR2 = 0;			// reset timer2
}

void main(void) {
    uint8_t counter = 0;

    pps_init();
    system_init();
    uart_init();
    i2c_init();
    ad_init();
    //timer1_init();

    /* empty circular buffers */
    tx_fifo.head = 0;
    tx_fifo.tail = 0;
    rx_fifo.head = 0;
    rx_fifo.tail = 0;

    GIE = 0;
    LCD_init(LCD_01_ADDRESS);

    while (1) {
	if (counter == 0) {
	    LCD_putcmd(LCD_01_ADDRESS, LCD_CLEAR, 1);
	    LCD_puts(LCD_01_ADDRESS, "Booster Max=8.0A\0");
	    LCD_goto(LCD_01_ADDRESS, 2, 1);
	    LCD_puts(LCD_01_ADDRESS, "Pwr On      0.0%\0");
	    //LATCbits.LATC0 = 1;
	    //LATCbits.LATC0 ^= 1;
	    putchar_wait(0x55);
	}
	__delay_ms(4);

	counter++;
    }
}
