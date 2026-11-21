//
// The line to the app: SoftwareSerial, and one thing more, for the jump game (GameDisplay.h). While
// readForMs is not 0, no byte is read from the line once that many ms have gone by since
// readSinceMs, whatever is still arriving: a read that is waiting then ends at once, as if its
// timeout had run out. (It is here and not in a reader of the game's own, because deserializeJson()
// is built once for every kind of thing it reads from, and a second kind costs 1.6 KB of flash: the
// game reads its frames through the chooser's reader, which reads from this port.)
//
// (In a file of its own because the build writes a declaration for every function it finds in the
// sketch, a class's too.)
//

#ifndef LED_MARQUEE_APPSERIAL_H
#define LED_MARQUEE_APPSERIAL_H

#include "SoftwareSerial/SoftwareSerial.h"

class AppSerial : public SoftwareSerial {
public:
    AppSerial(uint8_t rx, uint8_t tx) : SoftwareSerial(rx, tx) {}

    unsigned long readSinceMs = 0;
    unsigned long readForMs = 0;

    virtual int read() {
        if (readForMs && millis() - readSinceMs >= readForMs) {
            _timeout = 0; // (Stream's reads go on asking until their timeout is over)
            return -1;
        }
        return SoftwareSerial::read();
    }
};

#endif //LED_MARQUEE_APPSERIAL_H
