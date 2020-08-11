//
// Created by afik_cohen on 8/10/20.
//

#ifndef LED_MARQUEE_PIXELBITBANGING_H
#define LED_MARQUEE_PIXELBITBANGING_H


// These values depend on which pins your 8 strings are connected to and what board you are using
// More info on how to find these at http://www.arduino.cc/en/Reference/PortManipulation

// PORTD controls Digital Pins 0-7 on the Uno

// You'll need to look up the port/bit combination for other boards.

// Note that you could also include the DigitalWriteFast header file to not need to to this lookup.

#define PIXEL_PORT  PORTD  // Port of the pin the pixels are connected to
#define PIXEL_DDR   DDRD   // Port of the pin the pixels are connected to


static const uint onBits = 0b11111110;   // Bit pattern to write to port to turn on all pins connected to LED strips.
// If you do not want to use all 8 pins, you can mask off the ones you don't want
// Note that these will still get 0 written to them when we send pixels
// TODO: If we have time, we could even add a variable that will and/or into the bits before writing to the port to support any combination of bits/values

// These are the timing constraints taken mostly from
// imperically measuring the output from the Adafruit library strandtest program

// Note that some of these defined values are for refernce only - the actual timing is determinted by the hard code.

#define T1H  814    // Width of a 1 bit in ns - 13 cycles
#define T1L  438    // Width of a 1 bit in ns -  7 cycles

#define T0H  312    // Width of a 0 bit in ns -  5 cycles
#define T0L  936    // Width of a 0 bit in ns - 15 cycles

// Phase #1 - Always 1  - 5 cycles
// Phase #2 - Data part - 8 cycles
// Phase #3 - Always 0  - 7 cycles

#define RES 500000   // Width of the low gap between bits to cause a frame to latch

// Here are some convenience defines for using nanoseconds specs to generate actual CPU delays

#define NS_PER_SEC (1000000000L)          // Note that this has to be SIGNED since we want to be able to check for negative values of derivatives

#define CYCLES_PER_SEC (F_CPU)

#define NS_PER_CYCLE ( NS_PER_SEC / CYCLES_PER_SEC )

#define NS_TO_CYCLES(n) ( (n) / NS_PER_CYCLE )


// Sends a full 8 bits down all the pins, representing a single color of 1 pixel
// We walk though the 8 bits in colorbyte one at a time. If the bit is 1 then we send the 8 bits of row out. Otherwise we send 0.
// We send onBits at the first phase of the signal generation. We could just send 0xff, but that mught enable pull-ups on pins that we are not using.

/// Unforntunately we have to drop to ASM for this so we can interleave the computaions durring the delays, otherwise things get too slow.

// OnBits is the mask of which bits are connected to strips. We pass it on so that we
// do not turn on unused pins becuase this would enable the pullup. Also, hopefully passing this
// will cause the compiler to allocate a Register for it and avoid a reload every pass.
static inline void sendBitx8(const uint row, const uint colorbyte, const uint onBits) {

    asm volatile (


    "L_%=: \n\r"

    "out %[port], %[onBits] \n\t"                 // (1 cycles) - send either T0H or the first part of T1H. Onbits is a mask of which bits have strings attached.

    // Next determine if we are going to be sending 1s or 0s based on the current bit in the color....

    "mov r0, %[bitwalker] \n\t"                   // (1 cycles)
    "and r0, %[colorbyte] \n\t"                   // (1 cycles)  - is the current bit in the color byte set?
    "breq OFF_%= \n\t"                            // (1 cycles) - bit in color is 0, then send full zero row (takes 2 cycles if branch taken, count the extra 1 on the target line)

    // If we get here, then we want to send a 1 for every row that has an ON dot...
    "nop \n\t  "                                  // (1 cycles)
    "out %[port], %[row]   \n\t"                  // (1 cycles) - set the output bits to [row] This is phase for T0H-T1H.
    // ==========
    // (5 cycles) - T0H (Phase #1)


    "nop \n\t nop \n\t "                          // (2 cycles)
    "nop \n\t nop \n\t "                          // (2 cycles)
    "nop \n\t nop \n\t "                          // (2 cycles)
    "nop \n\t "                                   // (1 cycles)

    "out %[port], __zero_reg__ \n\t"              // (1 cycles) - set the output bits to 0x00 based on the bit in colorbyte. This is phase for T0H-T1H
    // ==========
    // (8 cycles) - Phase #2

    "ror %[bitwalker] \n\t"                      // (1 cycles) - get ready for next pass. On last pass, the bit will end up in C flag

    "brcs DONE_%= \n\t"                          // (1 cycles) Exit if carry bit is set as a result of us walking all 8 bits. We assume that the process around us will tak long enough to cover the phase 3 delay

    "nop \n\t \n\t "                             // (1 cycles) - When added to the 5 cycles in S:, we gte the 7 cycles of T1L

    "jmp L_%= \n\t"                              // (3 cycles)
    // (1 cycles) - The OUT on the next pass of the loop
    // ==========
    // (7 cycles) - T1L


    "OFF_%=: \n\r"                                // (1 cycles)    Note that we land here becuase of breq, which takes takes 2 cycles

    "out %[port], __zero_reg__ \n\t"              // (1 cycles) - set the output bits to 0x00 based on the bit in colorbyte. This is phase for T0H-T1H
    // ==========
    // (5 cycles) - T0H

    "ror %[bitwalker] \n\t"                      // (1 cycles) - get ready for next pass. On last pass, the bit will end up in C flag

    "brcs DONE_%= \n\t"                          // (1 cycles) Exit if carry bit is set as a result of us walking all 8 bits. We assume that the process around us will tak long enough to cover the phase 3 delay

    "nop \n\t nop \n\t "                          // (2 cycles)
    "nop \n\t nop \n\t "                          // (2 cycles)
    "nop \n\t nop \n\t "                          // (2 cycles)
    "nop \n\t nop \n\t "                          // (2 cycles)
    "nop \n\t "                                   // (1 cycles)

    "jmp L_%= \n\t"                               // (3 cycles)
    // (1 cycles) - The OUT on the next pass of the loop
    // ==========
    //(15 cycles) - T0L


    "DONE_%=: \n\t"

    // Don't need an explicit delay here since the overhead that follows will always be long enough

    ::
    [port]    "I"(_SFR_IO_ADDR(PIXEL_PORT)),
    [row]   "d"(row),
    [onBits]   "d"(onBits),
    [colorbyte]   "d"(colorbyte),     // Phase 2 of the signal where the actual data bits show up.
    [bitwalker] "r"(
            0x80)                      // Alocate a register to hold a bit that we will walk down though the color byte

    );

    // Note that the inter-bit gap can be as long as you want as long as it doesn't exceed the reset timeout (which is A long time)

}

#endif //LED_MARQUEE_PIXELBITBANGING_H
