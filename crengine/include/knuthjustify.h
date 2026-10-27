/** \file knuthjustify.h
    \brief Knuth-Plass paragraph justification: settings, and the helpers
    font drawing shares with the formatter.

    The formatter's side (line break planning and line justification) is
    in knuthparagraph.h.
*/

#ifndef __KNUTH_JUSTIFY_H_INCLUDED__
#define __KNUTH_JUSTIFY_H_INCLUDED__

#include "lvtypes.h"

class CRPropAccessor;

// Line breaking strategy: 0=greedy, 1=Knuth-Plass paragraph optimization
#define PROP_FORMAT_LINE_BREAKING_MODE               "crengine.style.line.breaking.mode"
// Knuth-Plass justification controls (used when line breaking mode is 1)
#define PROP_FORMAT_JUSTIFY_SPACE_SHRINK_PERCENT     "crengine.style.justify.space.shrink.percent"
#define PROP_FORMAT_JUSTIFY_SPACE_STRETCH_PERCENT    "crengine.style.justify.space.stretch.percent"
#define PROP_FORMAT_JUSTIFY_TRACKING_SHRINK_PERCENT  "crengine.style.justify.tracking.shrink.percent"
#define PROP_FORMAT_JUSTIFY_TRACKING_STRETCH_PERCENT "crengine.style.justify.tracking.stretch.percent"
#define PROP_FORMAT_JUSTIFY_PRETOLERANCE              "crengine.style.justify.pretolerance"
#define PROP_FORMAT_JUSTIFY_TOLERANCE                 "crengine.style.justify.tolerance"
#define PROP_FORMAT_JUSTIFY_HYPHEN_PENALTY            "crengine.style.justify.hyphen.penalty"
#define PROP_FORMAT_JUSTIFY_EX_HYPHEN_PENALTY         "crengine.style.justify.explicit.hyphen.penalty"
#define PROP_FORMAT_JUSTIFY_LINE_PENALTY              "crengine.style.justify.line.penalty"
#define PROP_FORMAT_JUSTIFY_ADJ_DEMERITS              "crengine.style.justify.adjacent.demerits"
#define PROP_FORMAT_JUSTIFY_DOUBLE_HYPHEN_DEMERITS    "crengine.style.justify.double.hyphen.demerits"
#define PROP_FORMAT_JUSTIFY_FINAL_HYPHEN_DEMERITS     "crengine.style.justify.final.hyphen.demerits"
#define PROP_FORMAT_JUSTIFY_EMERGENCY_STRETCH_PERCENT "crengine.style.justify.emergency.stretch.percent"
#define PROP_FORMAT_JUSTIFY_LAST_LINE_MIN_PERCENT     "crengine.style.justify.last.line.min.percent"
#define PROP_FORMAT_JUSTIFY_TRACKING_DELTA_MAX_BP     "crengine.style.justify.tracking.delta.max.bp"
// Ragged (left-aligned, right-aligned, centered) paragraphs: how far short
// of the full width a line may fall, in percent of the line width, before
// it counts as loose (badness 100). 0 leaves them to greedy line breaking.
#define PROP_FORMAT_RAGGED_WIDTH_PERCENT              "crengine.style.ragged.width.percent"
// Penalty for an automatic hyphen ending a line of a ragged paragraph (its
// line costs are on a much smaller scale than justified lines': about 100
// takes a hyphen only when it saves a fairly short line). Explicit hyphens
// get none.
#define PROP_FORMAT_RAGGED_HYPHEN_PENALTY             "crengine.style.ragged.hyphen.penalty"

/// Line breaking settings, one per property above (their defaults and
/// allowed ranges are in knuthjustify.cpp). Greedy mode ignores all but
/// line_breaking_mode.
struct KnuthJustifyConfig {
    int line_breaking_mode; // 0: greedy, 1: Knuth-Plass optimization of suitable paragraphs
    int space_shrink_percent;
    int space_stretch_percent;
    int tracking_shrink_percent;
    int tracking_stretch_percent;
    int pretolerance;
    int tolerance;
    int hyphen_penalty;
    int explicit_hyphen_penalty;
    int line_penalty;
    int adjacent_demerits;
    int double_hyphen_demerits;
    int final_hyphen_demerits;
    int emergency_stretch_percent;
    int last_line_min_percent;
    int tracking_delta_max_bp;
    int ragged_width_percent;
    int ragged_hyphen_penalty;

    /// all defaults
    KnuthJustifyConfig();
    /// from props (missing ones get their defaults)
    static KnuthJustifyConfig fromProps( CRPropAccessor * props );
    bool operator==( const KnuthJustifyConfig & other ) const;
    bool operator!=( const KnuthJustifyConfig & other ) const { return !(*this == other); }
    bool isGreedy() const { return line_breaking_mode == 0; }
    /// add to a document style hash: Knuth-Plass changes line breaks and
    /// pagination, but in greedy mode the hash is left as it was without
    /// these settings, so existing caches stay valid
    lUInt32 addToHash( lUInt32 hash ) const;
};

/// clamp each line breaking setting in props to its range, adding missing ones
void knuthJustifyPropsUpdateDefaults( CRPropAccessor * props );
/// true if propName is one of the line breaking settings
bool isKnuthJustifyProp( const char * propName );

/// drop cached hyphenation points (hyphenation settings may have changed)
void knuthClearHyphenationCache();

// Line-wide distributed tracking (microtracking spread over all glyph gaps
// of a line, see LFNT_HINT_DISTRIBUTED_TRACKING). Words are drawn
// separately, so each word gets the line total (as DrawTextString()'s
// target_w) and its place among the line's gaps (packed as target_h).
inline int knuthPackTracking( int start_gap, int gap_count ) {
    return ((lUInt32)start_gap << 16) | (lUInt16)gap_count;
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

/// The tracking pen of one word while it is drawn: how far the line's
/// tracking has moved the pen, in whole pixels (as laid out) and in 26.6.
struct KnuthTrackingPen {
    int width;    // the line's total tracking
    int points;   // the line's glyph gaps
    int done;     // gaps passed
    int done_px;
    int done_x64;
    KnuthTrackingPen( int total_width, int packed ) : width(total_width) {
        points = (lUInt32)packed & 0xFFFF;
        done = (lUInt32)packed >> 16;
        done_px = points > 0 ? width * done / points : 0;
        done_x64 = knuthDistributedTrackingX64(width, done, points);
    }
    bool active() const { return points > 0; }
    /// 26.6 offset of the word's start from its whole pixel layout position
    int startOffsetX64() const { return done_x64 - done_px * 64; }
    /// pass the next n gaps: false if the pen does not move, else the
    /// move in whole pixels and in 26.6
    bool advance( int n, int & delta_px, int & delta_x64 ) {
        if ( n <= 0 || done >= points )
            return false;
        done += n;
        if ( done > points )
            done = points;
        int px = width * done / points;
        int x64 = knuthDistributedTrackingX64(width, done, points);
        delta_px = px - done_px;
        delta_x64 = x64 - done_x64;
        done_px = px;
        done_x64 = x64;
        return true;
    }
};

#endif
