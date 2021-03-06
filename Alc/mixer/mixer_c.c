#include "config.h"

#include <assert.h>

#include "alMain.h"
#include "alu.h"
#include "alSource.h"
#include "alAuxEffectSlot.h"
#include "defs.h"


static inline ALfloat do_point(const InterpState* UNUSED(state), const ALfloat *restrict vals, ALsizei UNUSED(frac))
{ return vals[0]; }
static inline ALfloat do_lerp(const InterpState* UNUSED(state), const ALfloat *restrict vals, ALsizei frac)
{ return lerp(vals[0], vals[1], frac * (1.0f/FRACTIONONE)); }
static inline ALfloat do_cubic(const InterpState* UNUSED(state), const ALfloat *restrict vals, ALsizei frac)
{ return cubic(vals[0], vals[1], vals[2], vals[3], frac * (1.0f/FRACTIONONE)); }
static inline ALfloat do_bsinc(const InterpState *state, const ALfloat *restrict vals, ALsizei frac)
{
    const ALfloat *fil, *scd, *phd, *spd;
    ALsizei j_f, pi;
    ALfloat pf, r;

    ASSUME(state->bsinc.m > 0);

    // Calculate the phase index and factor.
#define FRAC_PHASE_BITDIFF (FRACTIONBITS-BSINC_PHASE_BITS)
    pi = frac >> FRAC_PHASE_BITDIFF;
    pf = (frac & ((1<<FRAC_PHASE_BITDIFF)-1)) * (1.0f/(1<<FRAC_PHASE_BITDIFF));
#undef FRAC_PHASE_BITDIFF

    fil = ASSUME_ALIGNED(state->bsinc.filter + state->bsinc.m*pi*4, 16);
    scd = ASSUME_ALIGNED(fil + state->bsinc.m, 16);
    phd = ASSUME_ALIGNED(scd + state->bsinc.m, 16);
    spd = ASSUME_ALIGNED(phd + state->bsinc.m, 16);

    // Apply the scale and phase interpolated filter.
    r = 0.0f;
    for(j_f = 0;j_f < state->bsinc.m;j_f++)
        r += (fil[j_f] + state->bsinc.sf*scd[j_f] + pf*(phd[j_f] + state->bsinc.sf*spd[j_f])) * vals[j_f];
    return r;
}

const ALfloat *Resample_copy_C(const InterpState* UNUSED(state),
  const ALfloat *restrict src, ALsizei UNUSED(frac), ALint UNUSED(increment),
  ALfloat *restrict dst, ALsizei numsamples)
{
#if defined(HAVE_SSE) || defined(HAVE_NEON)
    /* Avoid copying the source data if it's aligned like the destination. */
    if((((intptr_t)src)&15) == (((intptr_t)dst)&15))
        return src;
#endif
    memcpy(dst, src, numsamples*sizeof(ALfloat));
    return dst;
}

#define DECL_TEMPLATE(Tag, Sampler, O)                                        \
const ALfloat *Resample_##Tag##_C(const InterpState *state,                   \
  const ALfloat *restrict src, ALsizei frac, ALint increment,                 \
  ALfloat *restrict dst, ALsizei numsamples)                                  \
{                                                                             \
    const InterpState istate = *state;                                        \
    ALsizei i;                                                                \
                                                                              \
    ASSUME(numsamples > 0);                                                   \
                                                                              \
    src -= O;                                                                 \
    for(i = 0;i < numsamples;i++)                                             \
    {                                                                         \
        dst[i] = Sampler(&istate, src, frac);                                 \
                                                                              \
        frac += increment;                                                    \
        src  += frac>>FRACTIONBITS;                                           \
        frac &= FRACTIONMASK;                                                 \
    }                                                                         \
    return dst;                                                               \
}

DECL_TEMPLATE(point, do_point, 0)
DECL_TEMPLATE(lerp, do_lerp, 0)
DECL_TEMPLATE(cubic, do_cubic, 1)
DECL_TEMPLATE(bsinc, do_bsinc, istate.bsinc.l)

#undef DECL_TEMPLATE


static inline void ApplyCoeffs(ALsizei Offset, ALfloat (*restrict Values)[2],
                               const ALsizei IrSize,
                               const ALfloat (*restrict Coeffs)[2],
                               ALfloat left, ALfloat right)
{
    ALsizei c;
    for(c = 0;c < IrSize;c++)
    {
        const ALsizei off = (Offset+c)&HRIR_MASK;
        Values[off][0] += Coeffs[c][0] * left;
        Values[off][1] += Coeffs[c][1] * right;
    }
}

#define MixHrtf MixHrtf_C
#define MixHrtfBlend MixHrtfBlend_C
#define MixDirectHrtf MixDirectHrtf_C
#include "hrtf_inc.c"


void Mix_C(const ALfloat *data, ALsizei OutChans, ALfloat (*restrict OutBuffer)[BUFFERSIZE],
           ALfloat *CurrentGains, const ALfloat *TargetGains, ALsizei Counter, ALsizei OutPos,
           ALsizei BufferSize)
{
    const ALfloat delta = (Counter > 0) ? 1.0f/(ALfloat)Counter : 0.0f;
    ALsizei c;

    ASSUME(OutChans > 0);
    ASSUME(BufferSize > 0);

    for(c = 0;c < OutChans;c++)
    {
        ALsizei pos = 0;
        ALfloat gain = CurrentGains[c];
        const ALfloat diff = TargetGains[c] - gain;

        if(fabsf(diff) > FLT_EPSILON)
        {
            ALsizei minsize = mini(BufferSize, Counter);
            const ALfloat step = diff * delta;
            ALfloat step_count = 0.0f;
            for(;pos < minsize;pos++)
            {
                OutBuffer[c][OutPos+pos] += data[pos] * (gain + step*step_count);
                step_count += 1.0f;
            }
            if(pos == Counter)
                gain = TargetGains[c];
            else
                gain += step*step_count;
            CurrentGains[c] = gain;
        }

        if(!(fabsf(gain) > GAIN_SILENCE_THRESHOLD))
            continue;
        for(;pos < BufferSize;pos++)
            OutBuffer[c][OutPos+pos] += data[pos]*gain;
    }
}

/* Basically the inverse of the above. Rather than one input going to multiple
 * outputs (each with its own gain), it's multiple inputs (each with its own
 * gain) going to one output. This applies one row (vs one column) of a matrix
 * transform. And as the matrices are more or less static once set up, no
 * stepping is necessary.
 */
void MixRow_C(ALfloat *OutBuffer, const ALfloat *Gains, const ALfloat (*restrict data)[BUFFERSIZE], ALsizei InChans, ALsizei InPos, ALsizei BufferSize)
{
    ALsizei c, i;

    ASSUME(InChans > 0);
    ASSUME(BufferSize > 0);

    for(c = 0;c < InChans;c++)
    {
        const ALfloat gain = Gains[c];
        if(!(fabsf(gain) > GAIN_SILENCE_THRESHOLD))
            continue;

        for(i = 0;i < BufferSize;i++)
            OutBuffer[i] += data[c][InPos+i] * gain;
    }
}
