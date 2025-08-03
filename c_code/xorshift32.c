/* xorshift32 is a pseudorandom number generator with a period
   of 2**32-1.  It is likely not a very good generator from a
   statistical point of view, but it avoids multiplies.  See
   wikipedia "xorshift".

   state must be initialized non-zero.
*/

#include <stdint.h>

uint32_t xorshift32(uint32_t *state)
{
        /* Algorithm "xor" from p. 4 of Marsaglia, "Xorshift RNGs" */
        uint32_t x = *state;
        
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        
        return *state = x;
}
