/* Generic Viterbi decoder,
 * Copyright Phil Karn, KA9Q,
 * Code has been slightly modified for use with Spiral (www.spiral.net)
 * Karn's original code can be found here: http://www.ka9q.net/code/fec/
 * May be used under the terms of the GNU Lesser General Public License (LGPL)
 * see http://www.gnu.org/copyleft/lgpl.html
 */

#include <stdint.h>
#include <limits.h>
#include <math.h>

#include <memory.h>
// Useful description for which headers include which intrinsics
// https://stackoverflow.com/questions/11228855/header-files-for-x86-simd-intrinsics
#include <immintrin.h>

#include "phil_karn_viterbi_decoder.h"

#ifdef _MSC_VER
#define ALIGNED(x) __declspec(align(x))
#else
#define ALIGNED(x) __attribute__ ((aligned(x)))
#endif

#ifdef _WIN32
#define posix_memalign(p, a, s) (((*(p)) = _aligned_malloc((s), (a))), *(p) ? 0 : errno)
#define posix_free(a) _aligned_free(a)
#else
#define posix_free(a) free(a)
#endif


#define K CONSTRAINT_LENGTH
#define NUMSTATES (1 << (K-1))

/* ADDSHIFT and SUBSHIFT make sure that the thing returned is a byte. */
#if ((K-1) < 8)
#define ADDSHIFT (8 - (K-1))
#define SUBSHIFT 0
#elif ((K-1) > 8)
#define ADDSHIFT 0
#define SUBSHIFT ((K-1) - 8)
#else
#define ADDSHIFT 0
#define SUBSHIFT 0
#endif

// Bytes to align to for intrinsic
#define ALIGN_AMOUNT sizeof(__m256i)

uint8_t* CreateParityTable() {
    const int N = 256;
    uint8_t* table = new uint8_t[N];
    for (int i = 0; i < N; i++) {
        uint8_t parity = 0;
        uint8_t b = static_cast<uint8_t>(i);
        for (int j = 0; j < 8; j++) {
            parity ^= (b & 0b1);
            b = b >> 1;
        }
        table[i] = parity;
    }
    return table;
};

uint8_t* CreateBitCountTable() {
    const int N = 256;
    uint8_t* table = new uint8_t[N];
    for (int i = 0; i < N; i++) {
        uint8_t bitcount = 0;
        uint8_t b = static_cast<uint8_t>(i);
        for (int j = 0; j < 8; j++) {
            bitcount += (b & 0b1);
            b = b >> 1;
        }
        table[i] = bitcount;
    }
    return table;
}

uint8_t* CreateBitReverseTable() {
    const int N = 256;
    uint8_t* table = new uint8_t[N];
    for (int i = 0; i < N; i++) {
        uint8_t b = static_cast<uint8_t>(i);
        b = (b & 0xF0) >> 4 | (b & 0x0F) << 4;
        b = (b & 0xCC) >> 2 | (b & 0x33) << 2;
        b = (b & 0xAA) >> 1 | (b & 0x55) << 1;
        table[i] = b;
    }
    return table;
}

const static uint8_t* ParityTable = CreateParityTable();
const static uint8_t* BitcountTable = CreateBitCountTable();
const static uint8_t* BitReverseTable = CreateBitReverseTable();

// decision_t is a BIT vector
typedef ALIGNED(ALIGN_AMOUNT) union {
    DECISIONTYPE buf[1];
} decision_t;

typedef ALIGNED(ALIGN_AMOUNT) union {
    COMPUTETYPE buf[NUMSTATES];
    __m128i b128[8];
    __m256i b256[4];
} metric_t;

struct vitdec_t {
    ALIGNED(ALIGN_AMOUNT) metric_t metrics1;
    ALIGNED(ALIGN_AMOUNT) metric_t metrics2;

    union ALIGNED(ALIGN_AMOUNT) {
        COMPUTETYPE buf[NUMSTATES/2];
    } BranchTable[CODE_RATE];

    metric_t* old_metrics; 
    metric_t* new_metrics; 
    decision_t *decisions;  

    int maximum_decoded_bits;
    int curr_decoded_bit;

    COMPUTETYPE soft_decision_max_error;
};

static inline uint8_t parityb(const uint8_t x) {
    return ParityTable[x];
}

static inline uint8_t parity(uint32_t x) {
    /* Fold down to one byte */
    x ^= (x >> 16);
    x ^= (x >> 8);
    return parityb(x);
}

inline void renormalize(COMPUTETYPE *x, COMPUTETYPE threshold) {
    if (x[0] > threshold) {
        COMPUTETYPE min = x[0];
        for (int i = 0; i < NUMSTATES; i++) {
            if (min > x[i]) {
                min = x[i];
            }
        }
        for (int i = 0; i < NUMSTATES; i++) {
            x[i] -= min;
        }
    }
}

/* Initialize Viterbi decoder for start of new frame */
void init_viterbi(vitdec_t* vp, int starting_state) {
    // Give initial error to all states
    for (int i = 0; i < NUMSTATES; i++) {
        vp->metrics1.buf[i] = INITIAL_NON_START_ERROR;
    }
    vp->old_metrics = &vp->metrics1;
    vp->new_metrics = &vp->metrics2;

    // Only the starting state has 0 error
    vp->old_metrics->buf[starting_state & (NUMSTATES-1)] = INITIAL_START_ERROR;

    // Reset decision array
    {
        const int N = vp->curr_decoded_bit;
        decision_t* d = vp->decisions;
        memset(d, 0, N*sizeof(decision_t));
    }
    vp->curr_decoded_bit = 0;
}

/* Create a new instance of a Viterbi decoder */
vitdec_t* create_viterbi(
    const uint8_t polys[CODE_RATE], const int len,
    COMPUTETYPE soft_decision_high, COMPUTETYPE soft_decision_low) 
{
    vitdec_t* vp;
    if (posix_memalign((void**)&vp, ALIGN_AMOUNT, sizeof(vitdec_t))) {
        return NULL;
    }

    const int nb_padding_bits = (K-1);
    const int nb_max_input_bits = len+nb_padding_bits;
    if (posix_memalign((void**)&vp->decisions, ALIGN_AMOUNT, nb_max_input_bits*sizeof(decision_t))) {
        posix_free(vp);
        return NULL;
    }

    vp->maximum_decoded_bits = nb_max_input_bits;
    // NOTE: We set this to max so when we "init", the entire vp->decisions array is reset
    vp->curr_decoded_bit = vp->maximum_decoded_bits;
    for (int state = 0; state < NUMSTATES/2; state++) {
        for (int i = 0; i < CODE_RATE; i++) {
            const uint8_t v = parity((state << 1) & polys[i]);
            vp->BranchTable[i].buf[state] = v ? soft_decision_high : soft_decision_low;
        }
    }
    vp->soft_decision_max_error = soft_decision_high - soft_decision_low;
    init_viterbi(vp, 0);
    return vp;
}

void delete_viterbi(vitdec_t* vp) {
    if (vp != NULL) {
        posix_free(vp->decisions);
        posix_free(vp);
    }
}

COMPUTETYPE get_error_viterbi(vitdec_t* vp, const int state) {
    return vp->old_metrics->buf[state % NUMSTATES];
}

void chainback_viterbi(
    vitdec_t* const vp, unsigned char *data, 
    const unsigned int nbits, const unsigned int endstate)
{
    decision_t* d = vp->decisions;

    // ignore the tail bits
    d += (K-1);

    // Zero output buffer so we can OR in the decoded bits
    const int nbytes = nbits/8;
    for (int i = 0; i < nbytes; i++) {
        data[i] = 0x00;
    }

    unsigned int curr_state = endstate % NUMSTATES;

    for (int i = nbits-1; i >= 0; i--) {
        const uint8_t input = (d[i].buf[0] >> curr_state) & 0b1;
        curr_state = (curr_state >> 1) | (input << (K-2));
        // data[i/8] = BitReverseTable[curr_state >> SUBSHIFT];
        data[i/8] |= (input << (7-(i % 8)));
    }
}

void chainback_viterbi(
    vitdec_t* const vp, unsigned char *data, 
    const unsigned int nbits)
{
    int min_err = vp->new_metrics->buf[0];
    unsigned int curr_state = 0;
    for (unsigned int i = 1; i < NUMSTATES; i++) {
        auto err = vp->new_metrics->buf[i];
        if (err < min_err) {
            min_err = err;
            curr_state = i;
        }
    }

    chainback_viterbi(vp, data, nbits, curr_state);
}

/* C-language butterfly */
inline void BFLY(int i, int s, const COMPUTETYPE *syms, vitdec_t *vp, decision_t *d) {
    COMPUTETYPE metric = 0;
    COMPUTETYPE m0, m1, m2, m3;
    int decision0, decision1;

    for (int j = 0; j < CODE_RATE; j++) {
        auto& sym = syms[s*CODE_RATE + j];
        // XOR difference (only works for positive integers)
        // COMPUTETYPE error = (vp->BranchTable[j].buf[i] ^ sym) >> METRICSHIFT;
        // Absolute difference 
        COMPUTETYPE error = vp->BranchTable[j].buf[i] - sym;
        error = (error > 0) ? error : -error;
        metric += error >> METRICSHIFT;
    }
    metric = metric >> PRECISIONSHIFT;

    const COMPUTETYPE max = ((CODE_RATE * (vp->soft_decision_max_error >> METRICSHIFT)) >> PRECISIONSHIFT);

    m0 = vp->old_metrics->buf[i] + metric;
    m1 = vp->old_metrics->buf[i+NUMSTATES/2] + (max-metric);
    m2 = vp->old_metrics->buf[i] + (max-metric);
    m3 = vp->old_metrics->buf[i+NUMSTATES/2] + metric;

    decision0 = (signed int)(m0 - m1) > 0;
    decision1 = (signed int)(m2 - m3) > 0;

    vp->new_metrics->buf[2*i]   = decision0 ? m1 : m0;
    vp->new_metrics->buf[2*i+1] = decision1 ? m3 : m2;

    // We push the decision bits into the decision buffer
    const DECISIONTYPE decisions = decision0 | (decision1 << 1);
    const int nb_decision_bits = 2;
    const int buf_type_bits = DECISIONTYPE_BITSIZE;
    const int curr_bit = nb_decision_bits * i;
    const int curr_buf_index = curr_bit / buf_type_bits;
    const int curr_buf_bit = curr_bit % buf_type_bits;
    d->buf[curr_buf_index] |= (decisions << curr_buf_bit);
}

void update_viterbi_blk_scalar(vitdec_t* vp, const COMPUTETYPE *syms, const int nbits) {
    decision_t* d = &vp->decisions[vp->curr_decoded_bit];

    for (int s = 0; s < nbits; s++) {
        for (int i = 0; i < NUMSTATES/2; i++) {
            BFLY(i, s, syms, vp, d);
        }
        renormalize(vp->new_metrics->buf, RENORMALIZE_THRESHOLD);

        d++;
        vp->curr_decoded_bit++;

        metric_t* tmp = vp->old_metrics;
        vp->old_metrics = vp->new_metrics;
        vp->new_metrics = tmp;
    }
}
