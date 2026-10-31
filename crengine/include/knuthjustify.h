/** \file knuthjustify.h
    \brief Small, testable helpers for Knuth-Plass paragraph justification.
*/

#ifndef __KNUTH_JUSTIFY_H_INCLUDED__
#define __KNUTH_JUSTIFY_H_INCLUDED__

// Return the cumulative line-wide tracking offset at a glyph gap in the same
// 26.6 coordinate system used by HarfBuzz and FreeType. Keeping this as one
// accumulator for the complete line avoids restarting integer-pixel tracking
// at every separately drawn word.
inline int knuthDistributedTrackingX64(int total_width, int gap,
                                       int gap_count) {
    if ( gap_count <= 0 || gap <= 0 || total_width == 0 )
        return 0;
    if ( gap >= gap_count )
        return total_width * 64;
    return (int)((long long)total_width * 64 * gap / gap_count);
}

#endif
