/** \file knuthjustify.h
    \brief Small, testable helpers for Knuth-Plass paragraph justification.
*/

#ifndef __KNUTH_JUSTIFY_H_INCLUDED__
#define __KNUTH_JUSTIFY_H_INCLUDED__

inline int knuthOpticalLineAdjustment(int target_width, int natural_width,
                                      int hanging_width) {
    return target_width - (natural_width - hanging_width);
}

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

inline long long knuthTrackingMagnitudeCost(int tracking_basis_points) {
    long long magnitude = tracking_basis_points;
    // Tracking remains available as an emergency contraction mechanism, but
    // a break that fits through word spacing alone should normally win.
    return 4 * magnitude * magnitude;
}

// Choose one integer width for every visible word gap. Normal lines respect
// the configured upper bound; an emergency line may exceed it, because that
// is what emergency stretch means. When bounded negative tracking is
// available, round the gaps upward and use the continuous line pen to absorb
// the small overshoot. Otherwise round downward and leave at most gaps-1
// pixels of optical edge slack.
inline int knuthCommonSpaceTarget(int desired_space_total, int gaps,
                                  int minimum_space, int maximum_space,
                                  bool allow_emergency_stretch,
                                  int tracking_shrink_capacity) {
    if ( gaps <= 0 )
        return 0;
    int target = desired_space_total / gaps;
    if ( target < 1 )
        target = 1;
    bool common_interval = minimum_space <= maximum_space;
    if ( common_interval ) {
        if ( target < minimum_space )
            target = minimum_space;
        if ( !allow_emergency_stretch && target > maximum_space )
            target = maximum_space;
    }

    int upward = target + 1;
    int upward_overshoot = upward * gaps - desired_space_total;
    bool upward_allowed = !common_interval || allow_emergency_stretch ||
                          upward <= maximum_space;
    if ( upward_allowed && upward_overshoot > 0 &&
            upward_overshoot <= tracking_shrink_capacity )
        target = upward;
    return target;
}

#endif
