/*

    Talk to the 6 IR LEDs that are using for communication with adjacent tiles


    THEORY OF OPERATION
    ===================
    
    All communication is 7 bits wide. This leaves us with an 8th bit to use as a canary bit to save needing any counters.
    
    Bytes are transmitted least significant bit first.
    
    MORE TO COME HERE NEED PICTURES. 


*/


#include "blinks.h"
#include "hardware.h"


#include <avr/interrupt.h>
#include <avr/pgmspace.h>
#include <util/delay.h>         // Must come after F_CPU definition


#include "ir_comms.h"
#include "utils.h"

// A bit cycle is 1ms. That cycle is broken into 256 time slots.
// Every bit starts with a sync pulse
// The sync pulse is followed by the data pulse
// The time between the sync pulse and data pulse determines the bit value

// THESE MUST BE DISTINCTIVE!
// We detect a bit by measuring this time between pulses, so the time between 
// a trailing 0 bit pulse and the next sync pulse must be long enough that it does not
// look like another 1 bit.             
#define IR_1_BIT_TIME  50
#define IR_0_BIT_TIME 100


//TODO: Optimize these to be exact minimum for the distance in the real physical object    

#define IR_CHARGE_TIME_US 5        // How long to charge the LED though the pull-up

// Currently chosen empirically to work with some tile cases Jon made 7/28/17
 #define IR_PULSE_TIME_US 10



static inline void chargeLEDs( uint8_t bitmask ) {
    
     PCMSK1 &= ~bitmask;                                 // stop Triggering interrupts on these pins because they are going to change when we charge them
    
    // charge up receiver cathode pins while keeping other pins intact
           
    // This will enable the pull-ups on the LEDs we want to change without impacting other pins
    // The other pins will stay whatever they were.
    
    // NOTE: We are doing something tricky here. Writing a 1 to a PIN bit actually toggles the PORT bit. 
    // This saves about 10 instructions to manually load, or, and write back the bits to the PORT. 
    
    
    /*
        19.2.2. Toggling the Pin
        Writing a '1' to PINxn toggles the value of PORTxn, independent on the value of DDRxn. 
    */
    
    IR_CATHODE_PIN =  bitmask;
    
    // Empirically this is how long it takes to charge 
    // and avoid false positive triggers in 1ms window 12" from halogen desk lamp
    
    _delay_us( IR_CHARGE_TIME_US );
    
    
    // Only takes a tiny bit of time to charge up the cathode, even though the pull-up so no extra delay needed here...
    

    PCMSK1 |= bitmask;                  // Re-enable pin change on the pins we just charged up
                                        // Note that we must do this while we know the pins are still high
                                        // or there might be a *tiny* race condition if the pin changed in the cycle right after
                                        // we finished charging but before we enabled interrupts. This would latch
                                        // forever.
                                                                                    
    // Stop charging LED cathode pins (toggle the triggered bits back to what they were)
    
    IR_CATHODE_PIN = bitmask;     
                                                          
}    

// Send a pulse on all LEDs that have a 1 in bitmask
// bit 0= D1, bit 1= D2...
// This clobbers whatever charge was on the selected LEDs, so only call after you have checked it.

// TODO: Queue TX so they only happen after a successful RX or idle time. Unnecessary since TX time so short?


void ir_tx_pulse( uint8_t bitmask ) {
    
    // TODO: Check for input before sending and abort if found...

    // Remember that the normal state for IR LED pins is...
    // ANODE always driven. Typically DDR driven and PORT low when we are waiting to RX pulses.
    // CATHODE is input, so DDR not driven and PORT low. 
    
    uint8_t cathode_ddr_save = IR_CATHODE_DDR;
    
    PCMSK1 &= ~bitmask;                                 // stop Triggering interrupts on these cathode pins because they are going to change when we pulse
    
    IR_CATHODE_DDR |= bitmask ;   // Drive Cathode too (now driving low)
    
    // Right now both cathode and anode are driven and both are low - so LED off

    // Anode pins are driven output and low normally, so this will
    // make them be driven high output 
     
    IR_ANODE_PIN  = bitmask;    // Blink!       (Remember, a write to PIN actually toggles PORT)
    
    // Right now anode driven and high, so LED on!
      
    // TODO: Is this the right TX pulse with? Currently ~6us total width
    // Making too long wastes (a little?) battery and time
    // Making too short might not be enough light to trigger the RX on the other side
    // when TX voltage is low and RX voltage is high?
    // Also replace with a #define and _delay_us() so works when clock changes?

    _delay_us( IR_PULSE_TIME_US );
                
    IR_ANODE_PIN  = bitmask;    // Un-Blink! Sets anodes back to low (still output)      (Remember, a write to PIN actually toggles PORT)

    // Right now both cathode and anode are driven and both are low - so LED off

    IR_CATHODE_DDR = cathode_ddr_save;   // Cathode back to input too (now driving low) 
      
    // charge up receiver cathode pin while keeping other pins intact
           
    // This will enable the pull-ups on the LEDs we want to change without impacting other pins
    // The other pins will stay whatever they were.
    
    // NOTE: We are doing something tricky here. Writing a 1 to a PIN bit actually toggles the PORT bit. 
    // This saves about 10 instructions to manually load, or, and write back the bits to the PORT. 
        
    /*
    19.2.2. Toggling the Pin
    Writing a '1' to PINxn toggles the value of PORTxn, independent on the value of DDRxn. 
    */
    
    // TODO: Only recharge pins that we high when we started
    
    // TODO: These need to be asm because it sticks a load here.
               
    IR_CATHODE_PIN =  bitmask;
    
    _delay_us( IR_CHARGE_TIME_US ); 
        
    
    PCMSK1 |= bitmask;                  // Re-enable pin change on the pins we just charged up
                                        // Note that we must do this while we know the pins are still high
                                        // or there might be a *tiny* race condition if the pin changed in the cycle right after
                                        // we finished charging but before we enabled interrupts. This would latch until the next 
                                        // recharge timeout.
    
    
    // Stop charging LED cathode pins (toggle the triggered bits back o what they were)
    
    IR_CATHODE_PIN =  bitmask;
    
             
     
}

// For testing. 


// from http://www.microchip.com/forums/m587239.aspx

uint8_t oddParity(uint8_t p)
     {
      p = p ^ (p >> 4 | p << 4);
      p = p ^ (p >> 2);
      p = p ^ (p >> 1);
      return p & 1;
     }

static uint8_t tx_count=0;      // count up 0-127 (will use the extra bit for parity)

static uint8_t tx_value=0;

// Bits are 3x spread to account for clock drift. 
// We always start at 0 which is the start pulse
// Then we send MSB bit at slot 2,8, and every 3rd slot until LSB 8th bit , followed by stop bit 
// Stop bit just makes sure we do not see a single pulse as a 0x00

static uint8_t send_slot=0;        // Count up to 255 just to put some idle between bytes   

// Call this once per slot and it will slowly and repeatedly send out the requested bit

void send_test_byte( uint8_t mask) {
    
    //tx_value=65;
    
    if (send_slot==0) {
        
            // Start 
            ir_tx_pulse( mask );        
            
    } else {
        
        uint8_t bitslot = (send_slot-1) / 3;
        
        if (bitslot<8) {                // Only send actual bits during the 1st 8 slots after 
        
            if ( ( (send_slot-1) - (bitslot*3) ) == 2 ) {       // Only send actual bit in the middle slot
            
                if ( tx_value & ( 1 << (7-bitslot) ) ) {            // Only send pulse if bit is 1
                    
                    ir_tx_pulse( mask );
                    
                }
                
            }
            
        }            
        
    }                                                                 
        
    send_slot++;
    
    if (send_slot == ( 3*8 ) + 20 ) {       // The 20 slots give a little breathing room between bytes for resync
        send_slot=0;
        
        tx_count++;                         // Cycle though values
        if (tx_count==128) tx_count=0;      // Only count up to 127 since we use the bottom bit for parity
    }
                
    tx_value = ( tx_count << 1 ) + oddParity( tx_count ) ;     // send 7 bit counter + 1 bit parity. 
            
}     




// Recharge the specified LEDs 
// Suppress pin change interrupt on them

// This gets called anytime one of the IR LED cathodes has a level change drops. This typically happens because some light 
// hit it and discharged the capacitance, so the pin goes from high to low. We initialize each pin at high, and we charge it
// back to high everything it drops low, so we should in practice only ever see high to low transitions here.

// Note that there is also a background task that periodically recharges all the LEDs frequently enough that 
// that they should only ever go low from a very bright source - like an IR led pointing right down their barrel. 


// Last received byte from corresponding IRLED
// High bit always set, so yuo can set to zero and see if a new one arrived by checking high bit
// TODO: Buffer? Async notice?

volatile uint8_t irled_RX_value[IRLED_COUNT];  

volatile uint8_t irled_rx_error;        // There was an invalid pulse pattern on the indicated face
volatile uint8_t irled_rx_overflow;     // The value[] buffer was not empty when a new byte was recieved


struct tx_state_struct {
    uint8_t bitwindow;          // A sliding window of last 8 samples received. Most recent sample in LSB.
    uint8_t bytebuffer;         // Bits get shifted into here to build bytes. Every byte starts with a 1 in the high bit.
                                // As soon as a byte is decoded, we copy it into values[] to share it with the foreground.
                                // When bytebuffer is 0, then we are waiting for an idle to set it to 0x01 so we we start decoding

};    

/*

    TIMING:
    
    Just straight static uint8_t arrays for bitwindow and bytebuffer takes 312us per pass.
    Array of structs each with the two uints inside takes 220us per pass.
    Array of structs with a pointer that steps though them each iteration take 132us per pass. Wow gcc, you really make me do this?

*/


// TODO: Save a JMP and RET by inlining this?  

// Called every 512us, but must not take more than 256us or it will clobber other background ISRs
// WARNING: IF you modify this code, you MUST make sure it ALWAYS completes in less than ~230us or else things will get unpredicible. 

static struct tx_state_struct tx_state[IRLED_COUNT];


void ir_rx_isr(void)
{	

    //DEBUGA_1();
    
    /*
    static uint8_t bitwindow[IRLED_COUNT];      /
    static uint8_t bytebuffer[IRLED_COUNT];     

    */
    
    //DEBUGB_1();
    uint8_t ir_LED_PIN_sample = IR_CATHODE_PIN;      // Quickly sample the PIN (we will work the bits later)
    
    // charge up receiver cathode pin while keeping other pins intact
           
    // This will enable the pull-ups on the LEDs we want to change without impacting other pins
    // The other pins will stay whatever they were.
    
    // NOTE: We are doing something tricky here. Writing a 1 to a PIN bit actually toggles the PORT bit. 
    // This saves about 10 instructions to manually load, or, and write back the bits to the PORT. 
        
    /*
        19.2.2. Toggling the Pin
        Writing a '1' to PINxn toggles the value of PORTxn, independent on the value of DDRxn. 
    */
    
    // TODO: These need to be asm because it sticks a load here.
    
    chargeLEDs( IR_BITS ); 
               
    IR_CATHODE_PIN =  IR_BITS;
    
    _delay_us( IR_CHARGE_TIME_US ); 
    
    // (Only takes a tiny bit of time to charge up the cathode, even through the pull-up so no extra delay needed here...)
    
    // Stop charging LED cathode pins (toggle the triggered bits back o what they were)
    
    IR_CATHODE_PIN =  IR_BITS;        
    
    uint8_t currentSample = ~ir_LED_PIN_sample;           // recent sample (NOT because a pulse discharges LED)
                      
            
    // work out way though decoding the samples 
    
    // TODO: invert or use a bitslide to make more efficient
    
    // Here we unpack the sample that has 1 bit per face into the moving windows for each face
    

    //DEBUGA_1();        

    static struct tx_state_struct *current_tx_state = tx_state; 
    
        
    for(uint8_t led=0; led<IRLED_COUNT; led++) {
        
        /*
        uint8_t currentBitwindow = tx_state[led].bitwindow;                  // Wow, the compiler really needs us to make a local copy here for else big and slow code.
        uint8_t currentBytebuffer = tx_state[led].bytebuffer;                // Yet this one does not seem to matter
        */

        uint8_t currentBitwindow = current_tx_state->bitwindow;                  // Wow, the compiler really needs us to make a local copy here for else big and slow code.
        uint8_t currentBytebuffer = current_tx_state->bytebuffer;                // Yet this one does not seem to matter

        
        currentBitwindow <<= 1;           // Make room for new sample
        
        currentBitwindow |= (currentSample & 0b00000001);      // Grab received sample for this face
        
        currentSample >>=1;                                 // Shift down the sample so low bit is new face for next pass
        
        // OK, so the bit window now has the current sample and the past 7 samples in it.
        // Now we will check if these sames make a valid bit pattern
        // This is complicated because there is clock drift between the TX and RX sides so a pulse
        // can land in an adjacent position to the expected one. 
        
        // We only care about the bottom 6 positions of the window for decoding bits. The two higher ones will be the left over from a perviously decoded bit
        // One bit is a leading clock pulse, the presence or absence of a pulse for date, and then the trailing clock pulse.
        // (The final data bit in a byte has an extra trailing clock pulse)
        
        // We stipulate that the leading clock will be window position #5 and then look at all the other lower window positions to
        // to see if they could make a 
        
        // First check if we just saw an idle period so we should start decoding...
        
        if (currentBitwindow == 0b00000000 ) {
            
            currentBytebuffer = 0x01;        // This clears the MSB of the buffer, and resets us to the first bit, so we are ready to receive
            
        } else {   
            
            if (currentBytebuffer !=0 ) {      // Are we currently decoding? (If not, ignore everything except the idle reset above)
                                
                if ( currentBitwindow & 0b00100000 ) {            // Did we get a clock pulse? If so, try to decode a bit!
                
                
                    if (      ( currentBitwindow & 0b00100001 ) == 0b00100001 ) {      // C00D0C - TX clock slower
                    
                        currentBytebuffer <<=1;                                 // make room for new bit in byte buffer
                        currentBytebuffer |= ( currentBitwindow>> 2 & 0x01);     // Extract and save data bit 
                            
                        currentBitwindow = 0b00000001;                          // Save the tailing clock to be leading clock next time                        
                    
                    } else if ( ( currentBitwindow & 0b00110111 ) == 0b00100010 ) {      // C0D0C0 - TX clock matches
                    
                        currentBytebuffer <<=1;                                 // make room for new bit in byte buffer
                        currentBytebuffer |= ( currentBitwindow >> 3 & 0x01);     // Extract and save data bit 
                            
                        currentBitwindow = 0b00000010;                          // Save the tailing clock to be leading clock next time                    
                        

                        /*                        
                        if (currentBytebuffer & 0x01) {
                            DEBUGA_PULSE(50); 
                        } else {
                            DEBUGB_PULSE(50);
                        } 
                        */


                    } else if ( (currentBitwindow & 0b00100110 ) == 0b00100100 ) {      // CD0C0X - TX clock faster (X=dont' care because this is the following data bit)
                    
                        currentBytebuffer <<=1;                                 // make room for new bit in byte buffer
                        currentBytebuffer |= ( currentBitwindow >> 4 & 0x01);     // Extract and save data bit 
                    
                        currentBitwindow = currentBitwindow & 0b00000101;         // Save the tailing clock and data for next time    
                        
                    
                    } else {
                    
                        // Pulses do not match any possible sent patterns, so must be noise error
                                            
                        // If we get here then there was a noisy pulse pattern, so all bets are off
                        // We should start looking for a start bit again (which is preceded by idle)
                            
                        irled_rx_error |= _BV( led );       // Tell foreground
                            
                       currentBytebuffer = 0x00;     // We will not start decoding again until an idle
                        
                    }
                    
                    
                    if (currentBytebuffer & 0x80) {       // Have we decoded a full byte?!?
                        
                        // Note that to get here, we had to have successfully decoded 7 consecutive bits with valid patterns,
                        // which adds some filtering.
                        
                        if (irled_RX_value[led] == 0 )  {   // Is the incoming buffer free?
                            
                            irled_RX_value[led] = currentBytebuffer;  // Send to foreground!
                            
                        } else {
                            
                            irled_rx_overflow |= _BV( led );       // Tell foreground they are too damn slow!

                        }
                        
                        currentBytebuffer = 0x00;         // Start hunting for an idle to receive next byte!                                                                                
                        
                        //DEBUGB_PULSE(30);
                        
                                                                                                  
                    }                                                       
                
                }                                       
                                                        
            }                                
            
        }      
        
        
        /*
        tx_state[led].bitwindow = currentBitwindow;  
        tx_state[led].bytebuffer = currentBytebuffer;    
        */
        
        current_tx_state->bitwindow = currentBitwindow;  
        current_tx_state->bytebuffer = currentBytebuffer;    
                       
    }                      

    //DEBUGA_0();


    return;
    
}          


// We care about the time between pulses. 
// This ISR records a timestamp when a pulse is received and compuetes if it was the second in a chain. 


// This holds the timer value of the last time we got triggered
// The overflow ISR will...
// If the topbit==1, then the last pulse was more than 1ms ago so we set both to 0 to start waiting again
// If the topbit==0 && set==1, then topbit=1 and set=0 to wait for the second pulse
// if both 0, then we are just waiting and there was no recent pulse

// If the topbit==1 && set==1, then the last pulse was more than 1ms ago so we set both to 0 to start waiting again

static uint8_t timestampExpiredBits;       // has the timestamp expired?
static uint8_t timestampTopBits;           // has the timestamp overflowed once?
static uint8_t timestamp[IRLED_COUNT];     // Most recent stamp
        


       
// Call this when the timer overflows

static void rollTimestamps(void) {
            
    uint8_t ledBitwalker = _BV( IRLED_COUNT-1 );           // We will walk down though the 6 leds
        
    do {
        
        // TODO: Can this be done with a series of linear boolean operations without branching?
                                            
        if ( timestampTopBits & ledBitwalker ) {         // Have we already passed over it once?

            // This timer has already been passed over once, so consider it expired 


            if ( (ledBitwalker == 0x01 ) && (timestampExpiredBits & ledBitwalker)==0 ) {
                DEBUGB_PULSE(2);
            
            }            
            
            // We might do this repeatedly, but benignly 
            
            timestampExpiredBits |= ledBitwalker;
            
                                        
            // TODO: Reset byte collection
            
        } else {  // !( timestampTopBits & bitmask )
            
           
            // Set the topbit
                
            timestampTopBits |= ledBitwalker;      // TODO: XOR sets because we know it is clear here
            
        }            
            
        ledBitwalker >>=1;
        
    } while (ledBitwalker);    
    
}    



// We pass in a pattern of raw pulses rather than the value. This pushes work into the forground
// and gives the ISR eactly wat it wants to eat pre-chewed. 

typedef uint8_t tx_pattern_t;

// Outgoing data
// TODO: This should only really be volatile in the foreground, not in the ISR
// How does that work in C?
// OR does this really need to be volatile? Foreground only tests and sets if clear. Hmm...
// High bit and low bit always must be set (start and stop bit) 

// TODO: Maybe use a union to make clear where access is volatile and not?

volatile tx_pattern_t ir_tx_data[IRLED_COUNT];

// Local buffer for transmit in progress. 
// This has to be separate to make sure we only start using new values at the beginning of a bit 

// Must have leading 1 start bit in top to bit


// For now, buffer has pattern of actual time slices to send.
// Always a 1011 in top to sync and generate start bit
// Receiver looks for "01" in top two bits to recognize data

// 10111100 = 111 (trailing 0's not sent) 
// 10110101 = 100

// Don't judge me. This mess is to have minimum work in ISR keep interrupts off as short as possible. 

// send next round of bits on any transmits in progress

static void sendnextbits(void) {
    
    //DEBUGA_1();
            
    // On the overflow we send the clock pulse. 
    // This happens for both 0 and 1 bits. 

    uint8_t irbits=0;       // Which IR LEDs have to flash right now? 
    
    // Iterate though the LEDs...
    
    uint8_t bitmask = _BV( IRLED_COUNT -1 );
    
    tx_pattern_t *ledptr = ((tx_pattern_t *) ir_tx_data) +IRLED_COUNT;        // Un-volatile it here since we are in an ISR, we know 
                                                                              // it can't change out from under us.
        
    do {
        
        // TODO: Don't TX if there is an RX in progress
        
        tx_pattern_t pattern = *(--ledptr);            // Load indirect post decrement is 2 cycle single instruction
        
        if (pattern) {             // Go idle if data is all 0's. 
            
            
            if ( pattern & 0x80 ) {        // Send a pulse now?
                
                irbits |= bitmask;      // TODO: XOR here
                
            }                
            
            *ledptr = (pattern <<= 1);   // Shift up to next position
            
        }            
            
        bitmask >>=1;
        
    } while (bitmask);
    
        
    // Ok, here irbits has a 1 for each IR LED that should pulse
        
    ir_tx_pulse( irbits );

    //DEBUGA_0();
   
}   

// Convert a 2 bit value into an 8 bit pulse pattern for assignment to ir_tx-data for transmission
// Value must be 0-3

// TODO: Invert these so higher numbers take longer

tx_pattern_t to_tx_pattern( uint8_t value ) {
    
    static const PROGMEM uint8_t pattern_map[] = {
        
        // Note 1,2, & 3 will finish when there are only trailing 0's 
        
        0b10110101,         // 00
        0b10110110,         // 01
        0b10111010,         // 10
        0b10111100,         // 11   
        
    };
    
    return pgm_read_byte(&( pattern_map[value] ));       
    
}    


// This is broken out into its own function because it is called from two places- the OVR ISR
// and the IR ISR. 

// We have an obscure race condition if the timer happens to overflow right after we enter the IR ISR
// but before we have read the counter value. In this case we get the new (low) value, but the old timestamps 
// have not been rolled yet. So we have to check for an overflow in the IR ISR and manually roll if it happened.
// We clear the OVR bit to show that we did everything the OVR ISR would have done already and stop it from running
// a 2nd redundant time.

static void onOverflow(void) {
    sendnextbits();
    rollTimestamps();    
}    

// Fires every 512us, out of phase with COMPA

ISR(TIMER1_OVF_vect) {
    
    onOverflow();
    
    // TOV1 is cleared automatically when we entered this ISR
            
}    

// COMPA happens in the middle of the timer cycle. This gives us 2 possible bit slots per
// cycle. 

// Fires every 512us, out of phase with TOV

ISR(TIMER1_COMPA_vect) {
    sendnextbits();
    
}    

// We use COMPB as an overflow flag
// This is better than a real overflow since we have a huge ammount of 
// runway after the overflow to keep counting in case interrupts are off at the moment the overflow
// happens. 

ISR(TIMER1_COMPB_vect) {
    

}    



// This gets called anytime one of the IR LED cathodes has a level change drops. This typically happens because some light 
// hit it and discharged the capacitance, so the pin goes from high to low. We initialize each pin at high, and we charge it
// back to high everything it drops low, so we should in practice only ever see high to low transitions here.

// Note that there is also a background task that periodically recharges all the LEDs frequently enough that 
// that they should only ever go low from a very bright source - like an IR led pointing right down their barrel. 

ISR(IR_ISR)
{	


    //DEBUGA_1();                
    
    // ===Time critcal section start===
    
    // Find out which IR LED(s) went low to trigger this interrupt
            
    uint8_t ir_LED_triggered_bits = (~IR_CATHODE_PIN) & IR_BITS;      // A 1 means that LED triggered
        
    // If a pulse comes in after we sample but before we finish charging and enabling pin change, then we will miss it
    // so best to keep this short and straight
    
    // Note that protocol should make sure that real data pulses shuld have a header pulse that 
    // gets this receiver in sync so we only are recharging in the idle time after a pulse. 
    // real data pulses should come less than 1ms after the header pulse, and should always be less than 1ms apart. 
    
    uint8_t now = TCNT1L;                // Capture the current timer value

    // Did the counter *just* overflow in the moments after this ISR fired?
    // If so, then the normal OVR ISR did not run yet, so we need to do it manually. 
    
    if (TIFR1 & _BV(TOV1) ) {
        
        now= TCNT1L;        // Reload the counter just in case it overflowed in the tiny moment between when we last loaded it and when 
                            // we checked the TOV
                            
        onOverflow();       // Do all the stuff we do on an overflow (send bits, roll over timestamps)
        
        TIFR1 |= _BV(TOV1); // Manually clear the flag so the normal overflow ISR doesn't run again.
        
                            // "TOV is automatically cleared when the Timer/Counter 1 Overflow Interrupt Vector is executed.
                            // Alternatively, TOV can be cleared by writing a logic one to its bit location."
        
    }        
    
                                            
    
    // Recharge the ones that have fired

    chargeLEDs( ir_LED_triggered_bits ); 

    // TODO: Some LEDs seem to fire right after IR0 is charged when connected to programmer?

    // ===Time critcal section end===
    
    // only debug on IR0
    
    /*
    if ( ir_LED_triggered_bits & _BV(0) ) {        // IR1
            DEBUGB_1();                
    } 
    
    */   
        
    // This is more efficient than a loop on led_count since AVR only has one pointer register
    // and bit operations are fast 
        
    uint8_t ledBitwalk = _BV( IRLED_COUNT-1 );           // We will walk down though the 6 leds
    
    uint8_t *timestampptr = timestamp+IRLED_COUNT;
    
    do {
           
        if ( ir_LED_triggered_bits & ledBitwalk )  {         // Did we just get a pulse on this LED?
            
            uint16_t delay;     // TODO: Make this more efficient.
                
            uint8_t bit=0;
                                                           
            if ( (timestampExpiredBits & ledBitwalk) ) {            // Do we have a previous sample?
                
                delay = 512;                // Really was greater than 512, but that is the hghest we can know with 2 cycles of 256
                
            } else {                            
                
                // If we get here, then we know that we got 2 samples in a row and they were within 1ms of each other
                                    
                uint8_t timestamp = *timestampptr;                                    
                                                                                                                     
                if (timestampTopBits & ledBitwalk ) {      // did we cross a timer roll boundary since last sample?
                                            
                    delay = (256- timestamp) + now;         // TODO: Simplify with 8-bit math
                    
                    //DEBUGB_PULSE(20);                            

                        
                    // TODO: since we count the ticks before the roll, plus the ticks after with 8 bit overflow
                                            
                        
                } else {        // (now > timestamp)
                    
                    // timestamp was in the same cycle numerically as now, so simple subtraction
                        
                    delay = now - timestamp;
                        
                }                                                
                
                // Delay between last two pulses now calculated
                
                // 1 bit is 128 counts, 0 bit is 256 counts
                
                // The bounds must account for...
                // 1. clock drift between units (small effect)
                // 2. delay on the TX side because of blocked interrupts (makes prev pulse longer, next pulse shorter)
                // 3. delay on the RX side because of blocked interrupts (make prev pulse longer)
                
                if ( ledBitwalk == 0x01 ) {
                    
                    if ( (delay < 500) && (delay > 10 ) ) {     // 1 bit bounds
                        
                        if (delay > 200 ) {
                    
                            DEBUGA_PULSE(20);
                            
                        } else {
                            
                            DEBUGB_PULSE(20);                            
                            
                        }                            
                                                    
                
                    } else if ( ( delay > 80 ) && ( delay < 200 ) ) {     // 1 bit bounds
                    
                        //DEBUGA_PULSE(20);
                    
                    }  else if ( ( delay > 220 ) && ( delay < 350 ) ) { // 0 bit bounds

                        //DEBUGA_PULSE(5);
                    
                    } else if ( delay > 300 ) {            // Noise
                    
                        //DEBUGA_PULSE(50);
                    
                        // Reset byte
                    }                    
                }                                  
                
            } 
            
            // Save current sample for next pass
                                        
            *timestampptr = now;

            timestampExpiredBits  &= ~ledBitwalk;    
            timestampTopBits      &= ~ledBitwalk;                                       
            
        }  //  if ( ir_LED_triggered_bits & bitmask ) 
        
        timestampptr--;
        ledBitwalk >>=1;
        
    } while (ledBitwalk);

   //DEBUGA_0();   
    
    // If any LED pin changed while we were in this ISR, then we will get called again
    // as soon as we return and interrupts are enabled again.      
        
}




// IR comms uses the 16 bit timer1 
// We only want 8 bits so will set the TOP at 256.
// 
// We run this timer at /8 prescaller so at 4Mhz will hit (our simulated) TOP of 256 every ~0.5ms
// TODO: Coordinate with PIXEL timers so they do not step on each other. 


static void init_ir_timer(void) {
    
   // Fast PWM, 8-bit TOP=0x00FF Set TOV=TOP
   // Set via WGM10 in TCCR1A and WGM12 bit in TCCR1B
      
  TCCR1A = _BV(WGM10);       
                                       
   OCR1AL = 128;         // Define spacing for a 1/2 cycle 
   
   /*
                                    
   OCR1B  = 256;         // Define spacing for a 0 bit. This must be 2 bytes wide because the way we simulate the overflow is to 
                         // clear the high byte. If we matched at only 255, we might clear the high byte and then have it roll
                         // on the next tick. 
   */
            
   TIMSK1 = _BV(OCIE1A) | _BV(TOIE1); // Enable interrupts on matching A & Overflow

   TCCR1B = _BV( WGM12 ) | _BV( CS11 );         // clkI/O/8 (From prescaler), start counting!

             
}          

void ir_init(void) {
    
    DEBUG_INIT();     // TODO: Get rid of all debug stuff
      
    
    IR_ANODE_DDR = IR_BITS ;    // Set all ANODES to drive (and leave forever)
                                // The PORT will be 0, so these will be driven low
                                // until we actively send a pulse
                            
    // Leave cathodes DDR in input mode. When we write to PORT, then we will be enabling pull-up which is enough to charge the 
    // LEDs and saves having to switch DDR every charge. 
  
    // Pin change interrupt setup
    IR_MASK = IR_PCINT;             // Enable pin in Pin Change Mask Register for all 6 cathode pins. Any change after this will set the pending interrupt flag.
    SBI( PCICR , IR_PCI );          // Enable the pin group

    // Initial charge up of cathodes    
    //chargeLEDs( IR_BITS );    
    
    init_ir_timer();
      
}
