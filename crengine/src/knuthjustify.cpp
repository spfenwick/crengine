/** \file knuthjustify.cpp
    \brief Knuth-Plass paragraph justification: settings, line break
    planning, and the justification of planned lines.
*/

#include <string.h>
#include <algorithm>
#include <limits>
#include <vector>
#include "../include/knuthparagraph.h"
#include "../include/hyphman.h"
#include "../include/props.h"

// ---------------------------------------------------------------------------
// Settings

namespace {

struct KnuthJustifyProp {
    const char * name;
    int KnuthJustifyConfig::* field;
    int def;
    int min;
    int max;
};

// In style hash order: keep it, so that existing caches stay valid.
const KnuthJustifyProp knuthJustifyProps[] = {
    { PROP_FORMAT_LINE_BREAKING_MODE,               &KnuthJustifyConfig::line_breaking_mode,        0,     0, 1 },
    { PROP_FORMAT_JUSTIFY_SPACE_SHRINK_PERCENT,     &KnuthJustifyConfig::space_shrink_percent,      33,    0, 100 },
    { PROP_FORMAT_JUSTIFY_SPACE_STRETCH_PERCENT,    &KnuthJustifyConfig::space_stretch_percent,     50,    0, 200 },
    { PROP_FORMAT_JUSTIFY_TRACKING_SHRINK_PERCENT,  &KnuthJustifyConfig::tracking_shrink_percent,   1,     0, 20 },
    { PROP_FORMAT_JUSTIFY_TRACKING_STRETCH_PERCENT, &KnuthJustifyConfig::tracking_stretch_percent,  0,     0, 20 },
    { PROP_FORMAT_JUSTIFY_PRETOLERANCE,             &KnuthJustifyConfig::pretolerance,              100,   0, 10000 },
    { PROP_FORMAT_JUSTIFY_TOLERANCE,                &KnuthJustifyConfig::tolerance,                 200,   0, 10000 },
    { PROP_FORMAT_JUSTIFY_HYPHEN_PENALTY,           &KnuthJustifyConfig::hyphen_penalty,            0,     0, 100000 },
    { PROP_FORMAT_JUSTIFY_EX_HYPHEN_PENALTY,        &KnuthJustifyConfig::explicit_hyphen_penalty,   0,     0, 100000 },
    { PROP_FORMAT_JUSTIFY_LINE_PENALTY,             &KnuthJustifyConfig::line_penalty,              10,    0, 10000 },
    { PROP_FORMAT_JUSTIFY_ADJ_DEMERITS,             &KnuthJustifyConfig::adjacent_demerits,         10000, 0, 100000 },
    { PROP_FORMAT_JUSTIFY_DOUBLE_HYPHEN_DEMERITS,   &KnuthJustifyConfig::double_hyphen_demerits,    0,     0, 1000000000 },
    { PROP_FORMAT_JUSTIFY_FINAL_HYPHEN_DEMERITS,    &KnuthJustifyConfig::final_hyphen_demerits,     0,     0, 1000000000 },
    { PROP_FORMAT_JUSTIFY_EMERGENCY_STRETCH_PERCENT, &KnuthJustifyConfig::emergency_stretch_percent, 6,    0, 100 },
    { PROP_FORMAT_JUSTIFY_LAST_LINE_MIN_PERCENT,    &KnuthJustifyConfig::last_line_min_percent,     33,    0, 100 },
    { PROP_FORMAT_JUSTIFY_TRACKING_DELTA_MAX_BP,    &KnuthJustifyConfig::tracking_delta_max_bp,     50,    0, 2000 },
    { PROP_FORMAT_RAGGED_WIDTH_PERCENT,             &KnuthJustifyConfig::ragged_width_percent,      15,    0, 100 },
    { PROP_FORMAT_RAGGED_HYPHEN_PENALTY,            &KnuthJustifyConfig::ragged_hyphen_penalty,     100,   0, 100000 },
};
const int knuthJustifyPropCount = sizeof(knuthJustifyProps) / sizeof(knuthJustifyProps[0]);

// The prop's value in props, or its default, clamped to its range.
int getClampedProp( CRPropAccessor * props, const KnuthJustifyProp & prop ) {
    int p = props->getIntDef(prop.name, prop.def);
    if ( p < prop.min )
        p = prop.min;
    if ( p > prop.max )
        p = prop.max;
    return p;
}

} // namespace

KnuthJustifyConfig::KnuthJustifyConfig() {
    for ( int i=0; i<knuthJustifyPropCount; i++ )
        this->*knuthJustifyProps[i].field = knuthJustifyProps[i].def;
}

KnuthJustifyConfig KnuthJustifyConfig::fromProps( CRPropAccessor * props ) {
    KnuthJustifyConfig config;
    // Clamped here too: propsApply() reads values that
    // knuthJustifyPropsUpdateDefaults() has not seen.
    for ( int i=0; i<knuthJustifyPropCount; i++ ) {
        const KnuthJustifyProp & prop = knuthJustifyProps[i];
        config.*prop.field = getClampedProp(props, prop);
    }
    return config;
}

bool KnuthJustifyConfig::operator==( const KnuthJustifyConfig & other ) const {
    for ( int i=0; i<knuthJustifyPropCount; i++ ) {
        if ( this->*knuthJustifyProps[i].field != other.*knuthJustifyProps[i].field )
            return false;
    }
    return true;
}

lUInt32 KnuthJustifyConfig::addToHash( lUInt32 hash ) const {
    if ( isGreedy() )
        return hash;
    for ( int i=0; i<knuthJustifyPropCount; i++ )
        hash = hash * 31 + this->*knuthJustifyProps[i].field;
    return hash;
}

void knuthJustifyPropsUpdateDefaults( CRPropAccessor * props ) {
    for ( int i=0; i<knuthJustifyPropCount; i++ ) {
        const KnuthJustifyProp & prop = knuthJustifyProps[i];
        props->setInt(prop.name, getClampedProp(props, prop));
    }
}

bool isKnuthJustifyProp( const char * propName ) {
    for ( int i=0; i<knuthJustifyPropCount; i++ ) {
        if ( !strcmp(propName, knuthJustifyProps[i].name) )
            return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Line break search

// Same as lvtextfm.cpp's greedy hyphenation
#define MIN_WORD_LEN_TO_HYPHENATE 4
#define MAX_WORD_SIZE 64
static_assert(MAX_WORD_SIZE <= 64, "a word's hyphenation points are one 64-bit mask");

namespace {

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

// Hyphenation points of recently seen words, for the paragraph planner, which
// hyphenates every word of a paragraph rather than only the one meeting the
// line end. Each word maps to a fixed slot (a newer word replaces an older
// one), so lookups never allocate. Hyphenation settings, dictionaries and
// methods only change between document renders, and
// ldomDocument::render() clears this cache.
class OptimalHyphenationCache {
public:
    // Slots keep a 64-bit key for the word instead of the word itself:
    // 16 bytes per slot, so 1 MiB for all of them. Two different words
    // would need the same 64-bit key to get each other's hyphenation
    // points, which for the tens of thousands of distinct words of a book
    // is about a 1 in 10^10 chance.
    enum { SLOT_COUNT = 65536 };
    struct Slot {
        lUInt64 key;    // 0 when empty
        lUInt64 points; // bit i: a hyphen may follow char i
    };
    // Returns the slot for this word, and its key in key: the slot holds
    // this word's points if slot->key == key, or is to fill with store().
    // len is at most MAX_WORD_SIZE.
    Slot * find(HyphMethod * method, const lChar32 * word, int len, lUInt64 & key) {
        if ( !_slots ) {
            _slots = (Slot *)calloc(SLOT_COUNT, sizeof(Slot));
            if ( !_slots )
                return NULL;
        }
        // 64-bit FNV-1a over the method, the length and the chars.
        const lUInt64 prime = 1099511628211ULL;
        key = (14695981039346656037ULL ^ (lUInt64)(size_t)method) * prime;
        key = (key ^ (lUInt64)len) * prime;
        for ( int i=0; i<len; i++ )
            key = (key ^ word[i]) * prime;
        if ( key == 0 )
            key = 1; // 0 marks an empty slot
        return &_slots[(key ^ (key >> 32)) & (SLOT_COUNT - 1)];
    }
    static void store(Slot * slot, lUInt64 key, lUInt64 points) {
        slot->key = key;
        slot->points = points;
    }
    void clear() {
        free(_slots);
        _slots = NULL;
    }
    OptimalHyphenationCache() : _slots(NULL) { }
    ~OptimalHyphenationCache() { clear(); }
private:
    Slot * _slots;
};
OptimalHyphenationCache optimalHyphenationCache;

struct OptimalBreakCandidate {
    int pos;
    bool hyphenated;
    bool flagged;
    int penalty;
    OptimalBreakCandidate(int p, bool h, bool f, int v)
        : pos(p), hyphenated(h), flagged(f), penalty(v) {}
};


// Per-paragraph data for search(). It depends only on the paragraph
// (text, widths, indents, settings), not on a pass's tolerance or
// stretch options, so it is built once and shared by all passes.
struct OptimalBreakSetup {
    std::vector<OptimalBreakCandidate> candidates;
    int regular_width;
    int first_indent;
    int following_indent;
    bool if_not_first;
    int align;        // LTEXT_ALIGN_*
    bool ragged;      // not justified: lines keep their natural spacing
    int last_align;
    long long pruning_pct;
    // As a line end (index: candidate): where capacity stops, natural
    // width up to the break (trailing space dropped, hyphen added), last
    // visible char, its hanging, and the adjustable word spaces before
    // capacity_end (count and natural width).
    std::vector<int> end_capacity;
    std::vector<int> end_width;
    std::vector<int> end_last_char;
    std::vector<int> end_hanging;
    std::vector<int> end_space_count;
    std::vector<int> end_space_width;
    std::vector<int> end_box_width;     // images and inline boxes before capacity_end
    // As a line start (index: previous candidate + 1, so 0 is the
    // paragraph start): first char, width before it, first visible char,
    // its hanging, the word spaces before it, and the largest non-space
    // width before any position up to it (for overfull pruning).
    std::vector<int> start_pos;
    std::vector<int> start_width_before;
    std::vector<int> start_first_char;
    std::vector<int> start_hanging;
    std::vector<int> start_space_count;
    std::vector<int> start_space_width;
    std::vector<int> start_box_width;
    std::vector<int> start_nonspace_max;
    // Word-space limits within each candidate's segment of text,
    // (previous candidate, this candidate], with and without its last char.
    std::vector<int> segment_minimum;
    std::vector<int> segment_maximum;
    std::vector<int> segment_minimum_excl;
    std::vector<int> segment_maximum_excl;
    std::vector<int> sp_min;
    std::vector<int> sp_max;
    std::vector<long long> prune_threshold;
    // The paragraph's last char, which is always the last candidate.
    int last_pos;
    // The paragraph's settings, and whether tracking is available
    // (HarfBuzz kerning) and punctuation hangs.
    KnuthJustifyConfig config;
    bool harfbuzz;
    bool hanging_punctuation;

    /// Fill all the above for a paragraph. false if it has no candidates.
    bool prepare(const KnuthParagraph & p, const std::vector<bool> & hyphenation);
    /// Find the best breaks for one pass. false when no layout fits
    /// within tolerance (and the stretch the pass allows).
    bool search(std::vector<OptimalLineBreak> & result, int tolerance,
                bool allow_emergency_stretch, bool unlimited_stretch=false) const;
private:
    void clear();
    void finish(int max_start_hanging);
    /// The alignment addLine() gives a line (as for left-to-right text:
    /// the planner never sees right-to-left paragraphs).
    int lineAlign(bool first_line, bool final_line) const {
        if ( final_line && !(if_not_first && first_line) ) {
            if ( last_align )
                return last_align;
            if ( align == LTEXT_ALIGN_WIDTH )
                return LTEXT_ALIGN_LEFT;
        }
        return align;
    }
};

// A line bound that never prunes (see OptimalBreakSetup::prune_threshold).
const long long OPTIMAL_PRUNE_NEVER = std::numeric_limits<long long>::min() / 4;

static int optimalLineFitness(int diff, int capacity) {
    if ( diff == 0 )
        return 1;
    double ratio = capacity > 0 ? (double)diff / (double)capacity : 2.0;
    if ( ratio < -0.5 )
        return 0; // tight
    if ( ratio <= 0.5 )
        return 1; // normal
    if ( ratio <= 1.0 )
        return 2; // loose
    return 3; // very loose
}

static int optimalLineBadness(int diff, int capacity) {
    if ( diff == 0 )
        return 0;
    if ( capacity <= 0 )
        return 10000;
    double ratio = (double)(diff < 0 ? -diff : diff) / (double)capacity;
    int badness = (int)(100.0 * ratio * ratio * ratio + 0.5);
    return badness < 10000 ? badness : 10000;
}

// Completes a setup whose per-candidate vectors are filled: makes the
// overfull bounds monotone and builds the word-space range tables.
// max_start_hanging is the largest start_hanging.
void OptimalBreakSetup::finish(int max_start_hanging) {
    const int candidate_count = (int)candidates.size();
    const long long PRUNE_NEVER = OPTIMAL_PRUNE_NEVER;

    // The overfull bound, made monotone in the line end too: a start is
    // skipped only when the bound rejects it for this end and every later
    // one (a suffix minimum), so the earliest viable start never moves
    // back and one pointer serves all ends. This only skips lines the
    // checks in the search would reject anyway.
    {
        long long running = std::numeric_limits<long long>::max();
        for ( int c=candidate_count-1; c>=0; c-- ) {
            long long value = prune_threshold[c];
            if ( value != PRUNE_NEVER )
                value -= (long long)max_start_hanging * 100;
            running = std::min(running, value);
            prune_threshold[c] = running;
        }
    }

    // The widest run of segments the search can ask about: from just
    // after the earliest start the pruning keeps, to just before the end.
    // (Same pointer walk as in search().)
    int max_span = 1;
    {
        int earliest_previous = -1;
        for ( int current=0; current<candidate_count; current++ ) {
            while ( earliest_previous < current &&
                    (100 - pruning_pct) *
                    (long long)start_nonspace_max[earliest_previous+1]
                        < prune_threshold[current] )
                earliest_previous++;
            max_span = std::max(max_span, current - earliest_previous - 1);
        }
    }

    // Sparse tables over the segment word-space limits: the common
    // interval for any run of segments costs O(1), so only reachable
    // starts are visited instead of every start that might fit. Only
    // levels for runs up to max_span are needed. Entries a level never
    // fills are never read, so the vectors are resized, not filled.
    int sp_levels = 1;
    while ( (1 << sp_levels) <= max_span )
        sp_levels++;
    sp_min.resize(sp_levels * candidate_count);
    sp_max.resize(sp_levels * candidate_count);
    std::copy(segment_minimum.begin(), segment_minimum.end(), sp_min.begin());
    std::copy(segment_maximum.begin(), segment_maximum.end(), sp_max.begin());
    for ( int l=1; l<sp_levels; l++ ) {
        int half = 1 << (l-1);
        int * dmin = &sp_min[l*candidate_count];
        int * dmax = &sp_max[l*candidate_count];
        const int * smin = &sp_min[(l-1)*candidate_count];
        const int * smax = &sp_max[(l-1)*candidate_count];
        for ( int k=0; k + (1<<l) <= candidate_count; k++ ) {
            dmin[k] = std::max(smin[k], smin[k+half]);
            dmax[k] = std::min(smax[k], smax[k+half]);
        }
    }
}

bool OptimalBreakSetup::search(std::vector<OptimalLineBreak> & result,
                               int tolerance, bool allow_emergency_stretch,
                               bool unlimited_stretch) const {
    const int candidate_count = (int)candidates.size();
    const long long INF = std::numeric_limits<long long>::max() / 8;
    size_t state_count = candidates.size() * 4;
    std::vector<long long> cost(state_count, INF);
    std::vector<int> previous_candidate(state_count, -2);
    std::vector<int> previous_fitness(state_count, -1);
    std::vector<int> last_tracking_basis_points(state_count, 0);

    auto foldSegments = [&](int lo, int hi, int & minimum, int & maximum) {
        int level = 31 - __builtin_clz(hi - lo + 1);
        const int * level_min = &sp_min[level * candidate_count];
        const int * level_max = &sp_max[level * candidate_count];
        int other = hi - (1 << level) + 1;
        minimum = std::max(minimum, std::max(level_min[lo], level_min[other]));
        maximum = std::min(maximum, std::min(level_max[lo], level_max[other]));
    };
    // Candidates with at least one reachable state, ascending. Unreachable
    // starts are skipped before any line arithmetic: their pairs never
    // update a cost, so skipping them changes nothing but the time spent.
    std::vector<int> reachable_list;
    reachable_list.reserve(candidate_count);
    size_t reachable_from = 0;
    int earliest_previous = -1;

    for ( int current=0; current<candidate_count; current++ ) {
        const OptimalBreakCandidate & cur = candidates[current];
        const int line_end = cur.pos;
        const int capacity_end = end_capacity[current];
        const int own_min = capacity_end == line_end + 1
                ? segment_minimum[current] : segment_minimum_excl[current];
        const int own_max = capacity_end == line_end + 1
                ? segment_maximum[current] : segment_maximum_excl[current];
        while ( earliest_previous < current &&
                (100 - pruning_pct) *
                (long long)start_nonspace_max[earliest_previous+1]
                    < prune_threshold[current] )
            earliest_previous++;
        while ( reachable_from < reachable_list.size() &&
                reachable_list[reachable_from] < earliest_previous )
            reachable_from++;
        bool any_reachable = false;
        // Ascending order, so ties resolve exactly as before.
        for ( int idx = earliest_previous < 0 ? -1 : (int)reachable_from;
                idx < (int)reachable_list.size(); idx++ ) {
            int previous = idx < 0 ? -1 : reachable_list[idx];
            int line_start = start_pos[previous+1];
            if ( line_start > line_end )
                continue;
            // Word-space limits of every segment on this line.
            int window_lo = own_min;
            int window_hi = own_max;
            if ( previous + 1 <= current - 1 )
                foldSegments(previous + 1, current - 1, window_lo, window_hi);
            int natural_width = end_width[current] -
                    start_width_before[previous+1];
            bool first_line = previous < 0;
            bool final_line = line_end == last_pos;
            // Account for the exact left/right punctuation (or hyphen)
            // overhang that addLine() will apply while rendering.
            int hanging_width = 0;
            if ( hanging_punctuation &&
                    start_first_char[previous+1] <= end_last_char[current] ) {
                // addLine() hangs punctuation only on the edges a line is
                // aligned to (both when justified, none when centered).
                int line_align = lineAlign(first_line, final_line);
                if ( line_align == LTEXT_ALIGN_LEFT || line_align == LTEXT_ALIGN_WIDTH )
                    hanging_width += start_hanging[previous+1];
                if ( line_align == LTEXT_ALIGN_RIGHT || line_align == LTEXT_ALIGN_WIDTH )
                    hanging_width += end_hanging[current];
            }
            natural_width -= hanging_width;

            int target_width = regular_width -
                    (first_line ? first_indent : following_indent);
            if ( target_width <= 0 )
                continue;
            int diff = target_width - natural_width;
            bool ragged_final = final_line &&
                    lineAlign(first_line, final_line) != LTEXT_ALIGN_WIDTH;
            if ( ragged_final && diff < 0 )
                continue; // A left-aligned final line is never condensed.

            int badness;
            int fitness;
            int tracking_basis_points = 0;
            long long line_cost;
            if ( ragged ) {
                // A line of a ragged (left-aligned, right-aligned or
                // centered) paragraph keeps its natural word spacing, so
                // it is never condensed. Its badness comes from how far
                // short of the full width it falls, against the rag width,
                // as with the stretchable right margin of TeX's
                // \raggedright. The final line costs only its length (as
                // for justified paragraphs, below).
                if ( diff < 0 )
                    continue;
                badness = 0;
                fitness = 1;
                if ( !final_line ) {
                    int capacity = target_width *
                            config.ragged_width_percent / 100;
                    if ( allow_emergency_stretch )
                        capacity += target_width *
                                config.emergency_stretch_percent / 100;
                    badness = optimalLineBadness(diff, capacity);
                    if ( badness > tolerance )
                        continue;
                    fitness = optimalLineFitness(diff, capacity);
                }
                line_cost = (long long)(config.line_penalty + badness) *
                        (config.line_penalty + badness);
            }
            else {
                int space_count = end_space_count[current] -
                                  start_space_count[previous+1];
                int natural_space_total = 0;
                int minimum_common_space = 0;
                int maximum_common_space = 0;
                int minimum_space_adjustment = 0;
                int maximum_space_adjustment = 0;
                if ( space_count > 0 ) {
                    natural_space_total = end_space_width[current] -
                                          start_space_width[previous+1];
                    minimum_common_space = window_lo;
                    maximum_common_space = window_hi;
                    if ( minimum_common_space > maximum_common_space ) {
                        // Integer rasterisation can make nominally identical
                        // spaces differ by a pixel (and mixed fonts can differ
                        // more). Uniform visible gaps are the hard invariant:
                        // collapse a disjoint configured interval to the
                        // nearest common natural width and use tracking for
                        // the remaining line-width correction.
                        int normalized_space = (natural_space_total +
                                space_count / 2) / space_count;
                        minimum_common_space = std::max(1, normalized_space);
                        maximum_common_space = minimum_common_space;
                    }
                    minimum_space_adjustment =
                            minimum_common_space * space_count -
                            natural_space_total;
                    maximum_space_adjustment =
                            maximum_common_space * space_count -
                            natural_space_total;
                }
                // Tracking only moves glyphs: images and inline boxes on the
                // line add nothing to what it may take.
                int trackable_width = natural_width - (end_box_width[current] -
                                                       start_box_width[previous+1]);
                int tracking_shrink = harfbuzz
                        ? trackable_width * config.tracking_shrink_percent / 100 : 0;
                int tracking_stretch = harfbuzz
                        ? trackable_width * config.tracking_stretch_percent / 100 : 0;
                int minimum_adjustment = minimum_space_adjustment -
                                         tracking_shrink;
                int maximum_adjustment = maximum_space_adjustment +
                                         tracking_stretch;
                if ( diff < minimum_adjustment )
                    continue;
                if ( diff > maximum_adjustment && !ragged_final &&
                        !allow_emergency_stretch )
                    continue;
                int total_shrink = std::max(0, -minimum_adjustment);
                int total_stretch = std::max(0, maximum_adjustment);
                int capacity = diff < 0 ? total_shrink : total_stretch;
                int emergency_excess = 0;

                if ( ragged_final && diff >= 0 ) {
                    badness = 0;
                }
                else if ( diff > maximum_adjustment ) {
                    int emergency = target_width *
                            config.emergency_stretch_percent / 100;
                    emergency_excess = diff - maximum_adjustment;
                    if ( emergency_excess > emergency && !unlimited_stretch )
                        continue;
                    capacity = std::max(capacity,
                            maximum_adjustment + emergency);
                    badness = optimalLineBadness(diff, capacity);
                }
                else {
                    badness = optimalLineBadness(diff, capacity);
                }
                if ( !ragged_final && badness > tolerance )
                    continue;
                fitness = ragged_final && diff >= 0
                        ? 1 : optimalLineFitness(diff, capacity);
                // Model the renderer's exact ordering: choose one absolute
                // word-space width, including real emergency stretch. When
                // possible, round upward and absorb only the small overshoot
                // with bounded continuous negative microtracking.
                int optical_slack = 0;
                if ( !ragged_final && natural_width > 0 ) {
                    int space_adjustment = 0;
                    int target_space = 0;
                    if ( space_count > 0 ) {
                        int desired_space_total = natural_space_total + diff;
                        // (natural + min/max adjustment) / count is exactly
                        // the common space chosen above.
                        target_space = knuthCommonSpaceTarget(
                                desired_space_total, space_count,
                                minimum_common_space, maximum_common_space,
                                allow_emergency_stretch, tracking_shrink);
                        space_adjustment =
                                target_space * space_count -
                                natural_space_total;
                    }
                    int tracking_adjustment = diff - space_adjustment;
                    if ( tracking_adjustment >= 0 ) {
                        // Never turn word-space integer rounding into
                        // letter spacing.
                        optical_slack = tracking_adjustment;
                        tracking_adjustment = 0;
                    }
                    if ( tracking_adjustment > tracking_stretch ||
                            -tracking_adjustment > tracking_shrink )
                        continue;
                    if ( tracking_adjustment != 0 ) {
                        // (Non-zero only within the tracking limits, so
                        // trackable_width > 0.)
                        long long scaled = (long long)tracking_adjustment * 10000;
                        if ( scaled >= 0 )
                            tracking_basis_points =
                                    (int)((scaled + trackable_width / 2) /
                                          trackable_width);
                        else
                            tracking_basis_points =
                                    (int)((scaled - trackable_width / 2) /
                                          trackable_width);
                    }
                }
                line_cost = (long long)(config.line_penalty +
                        badness) * (config.line_penalty + badness);
                line_cost += (long long)optical_slack * optical_slack;
                line_cost += knuthTrackingMagnitudeCost(
                        tracking_basis_points);
                if ( first_line && diff > 0 ) {
                    // A loose indented first line is especially conspicuous:
                    // it has fewer spaces, and it establishes the paragraph's
                    // texture. Give its spacing four times the normal weight.
                    line_cost *= 4;
                }
                if ( emergency_excess > 0 ) {
                    // Price emergency stretch by how concentrated it is. The
                    // same few pixels are far less conspicuous across six
                    // spaces than when poured into one two-word gap.
                    long long concentration = (long long)20 *
                            emergency_excess / std::max(1, total_stretch);
                    line_cost += concentration * concentration;
                }
            }
            // Knuth-Plass penalties and demerits for flagged (hyphen)
            // breaks. For justified paragraphs, all default to 0, which
            // makes hyphenation a neutral break opportunity (ragged ones
            // have their own hyphen penalty).
            if ( cur.penalty > 0 )
                line_cost += (long long)cur.penalty * cur.penalty;
            if ( previous >= 0 && candidates[previous].flagged ) {
                if ( cur.flagged )
                    line_cost += config.double_hyphen_demerits;
                if ( final_line )
                    line_cost += config.final_hyphen_demerits;
            }
            if ( ragged_final ) {
                // A very short paragraph ending is visually conspicuous.
                // The cost grows with the square of the shortfall, so a
                // nearly long enough ending costs little and a stub costs
                // as much as several loose lines. It only chooses among
                // layouts whose lines are within tolerance. (A flat cost
                // of 5000-10000 was too weak to change most paragraphs,
                // and above about 33% charged almost every ending the
                // same, so the setting had no visible effect.)
                int preferred = target_width *
                        config.last_line_min_percent / 100;
                if ( natural_width < preferred ) {
                    double shortfall = (double)(preferred - natural_width) /
                            preferred;
                    line_cost += (long long)(1000000.0 *
                            shortfall * shortfall);
                }
            }
            int prior_last = previous < 0 ? 1 : 4;
            for ( int prior_fitness=0;
                    prior_fitness<prior_last; prior_fitness++ ) {
                long long prior_cost = previous < 0
                        ? 0 : cost[previous*4 + prior_fitness];
                if ( prior_cost == INF )
                    continue;
                if ( previous >= 0 && !ragged_final &&
                        config.tracking_delta_max_bp > 0 &&
                        std::abs(tracking_basis_points -
                            last_tracking_basis_points[
                                previous*4 + prior_fitness]) >
                            config.tracking_delta_max_bp )
                    continue;
                long long total = prior_cost + line_cost;
                if ( previous >= 0 ) {
                    if ( std::abs(fitness - prior_fitness) > 1 )
                        total += config.adjacent_demerits;
                }
                size_t state = current*4 + fitness;
                if ( total < cost[state] ) {
                    cost[state] = total;
                    any_reachable = true;
                    previous_candidate[state] = previous;
                    previous_fitness[state] = previous < 0 ? -1 : prior_fitness;
                    last_tracking_basis_points[state] =
                            tracking_basis_points;
                }
            }
        }
        if ( any_reachable )
            reachable_list.push_back(current);
    }

    int final_candidate = (int)candidates.size() - 1;
    int final_fitness = -1;
    long long final_cost = INF;
    for ( int fitness=0; fitness<4; fitness++ ) {
        long long value = cost[final_candidate*4 + fitness];
        if ( value < final_cost ) {
            final_cost = value;
            final_fitness = fitness;
        }
    }
    if ( final_fitness < 0 )
        return false;

    result.clear();
    int current = final_candidate;
    int fitness = final_fitness;
    while ( current >= 0 ) {
        size_t state = current*4 + fitness;
        result.push_back(OptimalLineBreak(candidates[current].pos,
                                          candidates[current].hyphenated,
                                          allow_emergency_stretch));
        current = previous_candidate[state];
        fitness = previous_fitness[state];
    }
    std::reverse(result.begin(), result.end());
    return !result.empty() && result.back().pos == last_pos;
}

// ---------------------------------------------------------------------------
// Paragraph preparation

bool isPadChar(const KnuthParagraph & p, int i) {
    return (p.flags[i] & LCHAR_IS_OBJECT) && p.charindex[i] == p.pad_char_index;
}

// An inline image or inline-block box: a fixed-width box, like a pad, but
// one that tracking can't adjust. (Floats never get to the planner.)
bool isBoxChar(const KnuthParagraph & p, int i) {
    return (p.flags[i] & LCHAR_IS_OBJECT) && p.charindex[i] != p.pad_char_index;
}

// Hyphenation points of the word at p.text[wstart, wstart+len), as a bit
// mask (bit i: a hyphen may follow char wstart+i). len <= MAX_WORD_SIZE.
lUInt64 getWordHyphenationPoints(const KnuthParagraph & p, src_text_fragment_t * src,
                                 int wstart, int len) {
    HyphMethod * method = src->lang_cfg->getHyphMethod();
    // With a maxWidth of 0xFFFF, the glyph widths passed below never
    // change the result: the word alone decides, so it can be cached.
    lUInt64 key = 0;
    OptimalHyphenationCache::Slot * slot =
            optimalHyphenationCache.find(method, p.text+wstart, len, key);
    if ( slot && slot->key == key )
        return slot->points;
    lUInt16 widths[MAX_WORD_SIZE];
    lUInt16 flags[MAX_WORD_SIZE];
    memset(flags, 0, sizeof(flags));
    int word_start_width = wstart > 0 ? p.widths[wstart-1] : 0;
    for ( int i=0; i<len; i++ ) {
        int width = p.widths[wstart+i] - word_start_width;
        widths[i] = width < 0xFFFF ? width : 0xFFFF;
    }
    LVFont * font = (LVFont *)src->t.font;
    int hyphen_width = font ? font->getHyphenWidth() : 0;
    method->hyphenate(p.text+wstart, len, widths, (lUInt8 *)flags,
                      hyphen_width, 0xFFFF, 2);
    lUInt64 points = 0;
    for ( int i=0; i<len; i++ ) {
        if ( flags[i] & LCHAR_ALLOW_HYPH_WRAP_AFTER )
            points |= (lUInt64)1 << i;
    }
    if ( slot )
        OptimalHyphenationCache::store(slot, key, points);
    return points;
}

void findParagraphHyphenation(const KnuthParagraph & p, std::vector<bool> & hyphenation) {
    hyphenation.assign(p.length, false);
    int wordpos = p.length - 1;
    while ( wordpos >= 0 ) {
        int wstart, wend;
        bool has_rtl;
        lStr_findWordBounds(p.text, p.length, wordpos, wstart, wend, has_rtl);
        if ( wend <= 0 )
            break;
        int len = wend - wstart;
        if ( len >= MIN_WORD_LEN_TO_HYPHENATE && !has_rtl && wstart < wordpos + 1 ) {
            if ( len > MAX_WORD_SIZE )
                len = MAX_WORD_SIZE;
            src_text_fragment_t * src = p.srcs[wstart];
            if ( src && (src->flags & LTEXT_HYPHENATE) &&
                    !(src->flags & LTEXT_FLAG_NOWRAP) ) {
                lUInt64 points = getWordHyphenationPoints(p, src, wstart, len);
                for ( ; points; points &= points-1 )
                    hyphenation[wstart + __builtin_ctzll(points)] = true;
            }
        }
        if ( wstart <= 0 )
            break;
        wordpos = wstart - 1;
    }
}

// Hanging punctuation, split into the part that depends only on the
// line's first visible char and the part that depends only on its last
// visible char, so the planner can compute each once per candidate
// instead of once per (start, end) pair. Together they reproduce what
// addLine() applies. (getHangingPercent()'s last argument is unused.)
// addLine() only hangs text words, so a line starting or ending with an
// object (pad, image or inline box) gets no hanging. (An object's src
// t.font is not a font.)

int getOptimalLeftHanging(const KnuthParagraph & p, int first_char, int usable_left_overflow) {
    if ( p.flags[first_char] & LCHAR_IS_OBJECT )
        return 0;
    src_text_fragment_t * first_src = p.srcs[first_char];
    LVFont * first_font = first_src ? (LVFont *)first_src->t.font : NULL;
    if ( !first_font || (p.flags[first_char] & LCHAR_LOCKED_SPACING) ||
            first_font->getFontFamily() == css_ff_monospace )
        return 0;
    int lsb = first_font->getLeftSideBearing(p.text[first_char]);
    int left_overflow = lsb < 0 ? -lsb : 0;
    bool check_font;
    int percent = first_src->lang_cfg->getHangingPercent(
            false, false, check_font, p.text,
            first_char, p.length-1-first_char);
    if ( percent && check_font && left_overflow > 0 )
        percent = 0;
    int shift = 0;
    if ( percent ) {
        int char_width = p.widths[first_char] -
                (first_char > 0 ? p.widths[first_char-1] : 0);
        shift = char_width * percent / 100;
        if ( shift == 0 )
            shift = 1;
        if ( char_width > 0.9 * first_font->getSize() &&
                lsb > 0.4 * char_width )
            shift = 0;
    }
    if ( shift - lsb > usable_left_overflow )
        shift = usable_left_overflow + lsb;
    return shift > 0 ? shift : 0;
}

int getOptimalRightHanging(const KnuthParagraph & p, int last_char, bool hyphenated,
                           int usable_right_overflow) {
    if ( p.flags[last_char] & LCHAR_IS_OBJECT )
        return 0;
    src_text_fragment_t * last_src = p.srcs[last_char];
    LVFont * last_font = last_src ? (LVFont *)last_src->t.font : NULL;
    if ( !last_font || last_font->getFontFamily() == css_ff_monospace )
        return 0;
    int rsb = hyphenated ? 0 : last_font->getRightSideBearing(p.text[last_char]);
    int right_overflow = rsb < 0 ? -rsb : 0;
    int shift = 0;
    if ( hyphenated ) {
        int percent = last_src->lang_cfg->getHyphenHangingPercent();
        if ( percent ) {
            shift = last_font->getHyphenWidth() * percent / 100;
            if ( shift == 0 )
                shift = 1;
        }
    }
    else {
        bool check_font;
        int percent = last_src->lang_cfg->getHangingPercent(
                true, false, check_font, p.text,
                last_char, p.length-1-last_char);
        if ( percent && check_font && right_overflow > 0 )
            percent = 0;
        if ( percent ) {
            int char_width = p.widths[last_char] -
                    (last_char > 0 ? p.widths[last_char-1] : 0);
            shift = char_width * percent / 100;
            if ( shift == 0 )
                shift = 1;
            if ( char_width > 0.9 * last_font->getSize() &&
                    rsb > 0.4 * char_width )
                shift = 0;
        }
    }
    if ( shift - rsb > usable_right_overflow )
        shift = usable_right_overflow + rsb;
    return shift > 0 ? shift : 0;
}

// One setup shared by all formatters: a formatter only lives for one
// final block, so per-formatter vectors would be reallocated for every
// paragraph. This keeps their capacity instead (sized by the largest
// paragraph planned so far). It is only in use while a paragraph is
// being planned, which never formats another block.
OptimalBreakSetup & optimalSetup() {
    static OptimalBreakSetup setup;
    return setup;
}

void OptimalBreakSetup::clear() {
    // Vectors are cleared rather than freed, so they keep their capacity
    // from one paragraph to the next.
    candidates.clear();
    end_capacity.clear();
    end_width.clear();
    end_last_char.clear();
    end_hanging.clear();
    end_space_count.clear();
    end_space_width.clear();
    end_box_width.clear();
    start_pos.clear();
    start_width_before.clear();
    start_first_char.clear();
    start_hanging.clear();
    start_space_count.clear();
    start_space_width.clear();
    start_box_width.clear();
    start_nonspace_max.clear();
    segment_minimum.clear();
    segment_maximum.clear();
    segment_minimum_excl.clear();
    segment_maximum_excl.clear();
    prune_threshold.clear();
}

bool OptimalBreakSetup::prepare(const KnuthParagraph & p,
                                const std::vector<bool> & hyphenation) {
    // One forward pass over the paragraph builds the candidates and
    // everything the search needs about them. The search never reads
    // per-char data, so only per-candidate values are kept.
    clear();
    config = *p.config;
    regular_width = p.line_width;
    first_indent = p.first_indent;
    following_indent = p.following_indent;
    if_not_first = p.para_flags & LTEXT_LAST_LINE_IF_NOT_FIRST;
    align = p.para_flags & LTEXT_FLAG_NEWLINE;
    ragged = align != LTEXT_ALIGN_WIDTH;
    last_align = (p.para_flags >> LTEXT_LAST_LINE_ALIGN_SHIFT) & LTEXT_FLAG_NEWLINE;
    last_pos = p.length - 1;
    harfbuzz = p.harfbuzz;
    hanging_punctuation = p.hanging_punctuation;

    // (The formatter's y stays fixed while planning, and so do these.)
    const int usable_left_overflow = p.usable_left_overflow;
    const int usable_right_overflow = p.usable_right_overflow;

    // Overfull pruning. Without it every candidate is paired with every
    // earlier one, which is quadratic in paragraph length.
    //
    // A line is rejected by the search when its width after all shrink
    // is still wider than the target:
    //   natural - spaces - tracking_shrink - hanging > target
    // because word spaces never go below zero, tracking shrink is at most
    // pct% of the natural width, and hanging is at most the largest start
    // hanging plus this end's hanging. So a line is certainly rejected when
    //   (100-pct)*nonspace - pct*spaces > 100*(max_target + hanging)
    // Both sides are monotonic in the line start (a prefix maximum keeps
    // the non-space width monotonic even with negative glyph advances),
    // so once one start is rejected every earlier start is too.
    pruning_pct = p.harfbuzz
            ? std::max(0, config.tracking_shrink_percent) : 0;
    long long max_target = (long long)regular_width -
            std::min(first_indent, following_indent);
    const long long PRUNE_NEVER = OPTIMAL_PRUNE_NEVER;

    // The renderer gives every adjustable word gap one absolute width.
    // Keep the sum of natural widths and the intersection of all allowed
    // absolute-width intervals so the planner models that exact operation,
    // rather than merely applying the same delta to unlike natural spaces.
    const int CAPACITY_INF = std::numeric_limits<int>::max() / 4;
    const int IGNORABLE = LCHAR_IS_SPACE|LCHAR_IS_COLLAPSED_SPACE|LCHAR_IS_TO_IGNORE;
    // addLine() drops these from a line's start (other spaces, such as a
    // run of no-break spaces, start its first word, which then can't hang)
    const int DROPPED_AT_START = LCHAR_IS_COLLAPSED_SPACE|LCHAR_IS_TO_IGNORE;
    const int space_shrink_percent = config.space_shrink_percent;
    const int space_stretch_percent = config.space_stretch_percent;

    // Running state, for the text before the current char unless noted.
    int space_count = 0;       // adjustable word spaces
    int space_width = 0;       // and their natural width
    int box_width = 0;         // images and inline boxes
    int nonspace_max = std::numeric_limits<int>::min(); // up to and including the current char
    int last_visible = -1;
    int segment_min = -CAPACITY_INF; // word-space limits since the previous candidate
    int segment_max = CAPACITY_INF;
    int next_visible = 0;      // first char kept at or after the latest line start
    int max_start_hanging = 0;

    auto addLineStart = [&](int line_start, int spaces_before,
                            int spaces_width_before, int boxes_width_before,
                            int nonspace) {
        start_pos.push_back(line_start);
        // A line starting inside a cluster (hyphenation inside a ligature)
        // counts the whole cluster's width, as the greedy line breaking
        // does: the cluster's first char carries it all, and the chars
        // starting this line have none of their own. (Overfull pruning,
        // which ignores this, stays on the safe side.)
        int width_from = line_start;
        while ( width_from > 0 && width_from < p.length &&
                (p.flags[width_from] & LCHAR_IS_CLUSTER_TAIL) )
            width_from--;
        start_width_before.push_back(width_from > 0 ? p.widths[width_from-1] : 0);
        // Starts increase, and the chars the previous scan skipped are
        // all dropped at any start, so the scan can resume where it stopped.
        if ( next_visible < line_start )
            next_visible = line_start;
        while ( next_visible < p.length && (p.flags[next_visible] & DROPPED_AT_START) )
            next_visible++;
        start_first_char.push_back(next_visible);
        int hanging = 0;
        if ( p.hanging_punctuation && next_visible < p.length ) {
            hanging = getOptimalLeftHanging(p, next_visible, usable_left_overflow);
            max_start_hanging = std::max(max_start_hanging, hanging);
        }
        start_hanging.push_back(hanging);
        start_space_count.push_back(spaces_before);
        start_space_width.push_back(spaces_width_before);
        start_box_width.push_back(boxes_width_before);
        start_nonspace_max.push_back(nonspace);
    };
    addLineStart(0, 0, 0, 0, 0);

    for ( int i=0; i<p.length; i++ ) {
        int width_before = i > 0 ? p.widths[i-1] : 0;
        int char_width = p.widths[i] - width_before;
        nonspace_max = std::max(nonspace_max, width_before - space_width);

        // This char as an adjustable word space.
        int char_space_min = -CAPACITY_INF;
        int char_space_max = CAPACITY_INF;
        int spaces_after = space_count;
        int spaces_width_after = space_width;
        if ( (p.flags[i] & LCHAR_IS_SPACE) &&
                !(p.flags[i] & (LCHAR_LOCKED_SPACING|LCHAR_IS_COLLAPSED_SPACE)) &&
                (i+1 >= p.length || !(p.flags[i+1] & LCHAR_IS_SPACE)) ) {
            int shrink = std::min(char_width > 1 ? char_width - 1 : 0,
                    char_width * space_shrink_percent / 100);
            int stretch = char_width * space_stretch_percent / 100;
            char_space_min = char_width - shrink;
            char_space_max = char_width + stretch;
            spaces_after++;
            spaces_width_after += char_width;
        }
        bool visible = !(p.flags[i] & IGNORABLE);
        int boxes_width_after = box_width + (isBoxChar(p, i) ? char_width : 0);

        // This char as a break candidate.
        bool candidate = false;
        bool hyphenated = false;
        bool flagged = false;
        int penalty = 0;
        if ( i == p.length-1 ) {
            candidate = true;
        }
        else if ( (p.flags[i] & LCHAR_ALLOW_WRAP_AFTER) &&
                !(p.flags[i] & LCHAR_DEPRECATED_WRAP_AFTER) ) {
            candidate = true;
            flagged = p.text[i] == '-' || p.text[i] == UNICODE_HYPHEN;
            penalty = flagged && !ragged ? config.explicit_hyphen_penalty : 0;
        }
        else if ( hyphenation[i] ) {
            candidate = true;
            hyphenated = true;
            flagged = true;
            penalty = ragged ? config.ragged_hyphen_penalty : config.hyphen_penalty;
        }

        if ( candidate ) {
            candidates.push_back(OptimalBreakCandidate(i, hyphenated, flagged, penalty));
            // As a line end. A trailing space is dropped from the width
            // and from the adjustable capacity.
            bool drop_space = !hyphenated && (p.flags[i] & LCHAR_IS_SPACE);
            int width = drop_space ? width_before : p.widths[i];
            if ( hyphenated ) {
                LVFont * font = (LVFont *)p.srcs[i]->t.font;
                if ( font )
                    width += font->getHyphenWidth();
            }
            int last = !drop_space && visible ? i : last_visible;
            int hanging = 0;
            if ( p.hanging_punctuation && last >= 0 )
                hanging = getOptimalRightHanging(p, last, hyphenated, usable_right_overflow);
            end_capacity.push_back(drop_space ? i : i + 1);
            end_width.push_back(width);
            end_last_char.push_back(last);
            end_hanging.push_back(hanging);
            end_space_count.push_back(drop_space ? space_count : spaces_after);
            end_space_width.push_back(drop_space ? space_width : spaces_width_after);
            end_box_width.push_back(boxes_width_after); // (a dropped space is no box)
            segment_minimum_excl.push_back(segment_min);
            segment_maximum_excl.push_back(segment_max);
            segment_minimum.push_back(std::max(segment_min, char_space_min));
            segment_maximum.push_back(std::min(segment_max, char_space_max));
            segment_min = -CAPACITY_INF;
            segment_max = CAPACITY_INF;
            // The overfull bound, still without the largest start
            // hanging, which is only known at the end.
            long long bound = PRUNE_NEVER;
            if ( i > 0 && pruning_pct < 100 ) {
                long long r = std::min(width_before - space_width,
                                       p.widths[i] - spaces_width_after);
                bound = r * (100 - pruning_pct) -
                        (long long)spaces_width_after * pruning_pct -
                        (max_target + hanging) * 100;
            }
            prune_threshold.push_back(bound);
            // The next line starts after this char, or on it when it is
            // a real hyphen that this language repeats on the next line.
            // Starts stay strictly increasing, as pruning requires.
            if ( !hyphenated && knuthDuplicatesHyphenAfter(p, i) )
                addLineStart(i, space_count, space_width, box_width, nonspace_max);
            else
                addLineStart(i + 1, spaces_after, spaces_width_after, boxes_width_after,
                        std::max(nonspace_max, p.widths[i] - spaces_width_after));
        }
        else {
            segment_min = std::max(segment_min, char_space_min);
            segment_max = std::min(segment_max, char_space_max);
        }
        space_count = spaces_after;
        space_width = spaces_width_after;
        box_width = boxes_width_after;
        if ( visible )
            last_visible = i;
    }
    const int candidate_count = (int)candidates.size();
    if ( candidate_count == 0 )
        return false;

    finish(max_start_hanging);
    return true;
}

// ---------------------------------------------------------------------------
// Justification of planned lines

// One justifier per line: the settings and fonts of its paragraph.
struct LineJustifier {
    const formatted_text_fragment_t * pbuffer;
    bool harfbuzz;
    bool emergency_stretch; // see OptimalLineBreak

    // Glyph gaps of this word that distributed tracking may adjust. Only the
    // HarfBuzz draw path implements LFNT_HINT_DISTRIBUTED_TRACKING, and
    // MathML stretched glyphs and scaled CJK glyphs already use the
    // target_w/target_h draw arguments for their own data.
    int getDistributedTrackingPoints(formatted_word_t * word)
    {
        if ( !harfbuzz || word->distinct_glyphs <= 1 ||
                (word->flags & (LTEXT_WORD_IS_CJK|LTEXT_WORD_IS_IMAGE|LTEXT_WORD_IS_INLINE_BOX|LTEXT_WORD_IS_PAD)) )
            return 0;
        src_text_fragment_t * src = &pbuffer->srctext[word->src_text_index];
        if ( src->flags & LTEXT_MATH_TRANSFORM )
            return 0;
        return word->distinct_glyphs - 1;
    }

    int countDistributedTrackingPoints(formatted_line_t * frmline)
    {
        int points = 0;
        for ( int i=0; i<(int)frmline->word_count; i++ )
            points += getDistributedTrackingPoints(&frmline->words[i]);
        return points;
    }

    // Most tracking the line may take, in the same terms as the planner:
    // a percentage of the line's glyph width (images and inline boxes left out).
    int getDistributedTrackingLimit(int trackable_width, bool shrink)
    {
        int percent = shrink ? pbuffer->knuth.tracking_shrink_percent
                             : pbuffer->knuth.tracking_stretch_percent;
        if ( percent <= 0 || trackable_width <= 0 )
            return 0;
        return trackable_width * percent / 100;
    }

    int applyDistributedTracking(formatted_line_t * frmline, int requested_width,
                                 int trackable_width)
    {
        // Never exceed the configured limits: a line the planner did not
        // fully fit is better left slightly short or overflowing than drawn
        // with overlapping or visibly spread-out glyphs. This also keeps
        // Max + = 0 a renderer-level invariant.
        int limit = getDistributedTrackingLimit(trackable_width, requested_width < 0);
        if ( requested_width > limit )
            requested_width = limit;
        else if ( requested_width < -limit )
            requested_width = -limit;
        int tracking_points = countDistributedTrackingPoints(frmline);
        for ( int i=0; i<(int)frmline->word_count; i++ ) {
            formatted_word_t * word = &frmline->words[i];
            word->flags &= ~LTEXT_WORD_HAS_DISTRIBUTED_TRACKING;
            word->distributed_tracking = 0;
            word->distributed_tracking_start = 0;
            word->distributed_tracking_points = 0;
        }
        if ( requested_width == 0 || tracking_points == 0 )
            return 0;

        int points_done = 0;
        int shift_x = 0;
        for ( int i=0; i<(int)frmline->word_count; i++ ) {
            formatted_word_t * word = &frmline->words[i];
            word->x += shift_x;
            int points = getDistributedTrackingPoints(word);
            int start_width = requested_width * points_done / tracking_points;
            int end_width = requested_width * (points_done + points) /
                    tracking_points;
            int added = end_width - start_width;
            if ( points > 0 ) {
                word->flags |= LTEXT_WORD_HAS_DISTRIBUTED_TRACKING;
                // Keep one accumulator for the full visual line. Restarting
                // the division in every word concentrates a small remainder
                // in one short word, making only that word look tracked out.
                word->distributed_tracking = requested_width;
                word->distributed_tracking_start = points_done;
                word->distributed_tracking_points = tracking_points;
            }
            word->width += added;
            word->min_width += added;
            shift_x += added;
            points_done += points;
        }
        frmline->width += requested_width;
        return requested_width;
    }

    int getNaturalWordSpaceWidth(formatted_word_t * word)
    {
        int natural = word->natural_space_width;
        if ( natural <= 0 )
            natural = std::max(1, word->width - word->min_width);
        return natural;
    }

    int applyFixedWordSpacing(formatted_line_t * frmline, int target_space_width)
    {
        if ( target_space_width < 1 )
            target_space_width = 1;
        int shift_x = 0;
        int total = 0;
        for ( int i=0; i<(int)frmline->word_count; i++ ) {
            formatted_word_t * word = &frmline->words[i];
            word->x += shift_x;
            if ( i < (int)frmline->word_count - 1 &&
                    (word->flags & LTEXT_WORD_CAN_ADD_SPACE_AFTER) ) {
                // Equal *adjustments* are not enough: fractional HarfBuzz
                // measurement can round otherwise identical natural spaces
                // to neighbouring integer widths. Set every actual trailing
                // word space to one absolute width instead.
                int delta = target_space_width -
                        getNaturalWordSpaceWidth(word);
                word->width += delta;
                word->min_width += delta;
                shift_x += delta;
                total += delta;
            }
        }
        frmline->width += total;
        return total;
    }

    int applyOptimizedJustification(formatted_line_t * frmline, int extra_width)
    {
        // What tracking may adjust, as the planner counts it: the line
        // without its images and inline boxes.
        int trackable_width = frmline->width;
        for ( int i=0; i<(int)frmline->word_count; i++ ) {
            if ( frmline->words[i].flags & (LTEXT_WORD_IS_IMAGE|LTEXT_WORD_IS_INLINE_BOX) )
                trackable_width -= frmline->words[i].width;
        }
        int spaces = 0;
        int natural_space_total = 0;
        int min_common_space = 1;
        int max_common_space = std::numeric_limits<int>::max();
        for ( int i=0; i<(int)frmline->word_count-1; i++ ) {
            formatted_word_t * word = &frmline->words[i];
            if ( !(word->flags & LTEXT_WORD_CAN_ADD_SPACE_AFTER) )
                continue;
            spaces++;
            int natural = getNaturalWordSpaceWidth(word);
            natural_space_total += natural;
            int shrink = std::min(natural > 1 ? natural - 1 : 0,
                    natural * pbuffer->knuth.space_shrink_percent / 100);
            int stretch = natural *
                    pbuffer->knuth.space_stretch_percent / 100;
            min_common_space = std::max(min_common_space,
                    natural - shrink);
            max_common_space = std::min(max_common_space,
                    natural + stretch);
        }
        if ( spaces > 0 && min_common_space > max_common_space ) {
            // No width suits every space: as the planner does, use the
            // rounded mean natural width and let tracking do the rest.
            min_common_space = std::max(1, (natural_space_total + spaces / 2) / spaces);
            max_common_space = min_common_space;
        }
        if ( spaces == 0 ) {
            // A positive limit of zero is a direction invariant, not merely
            // a planner preference. With no word gaps available, leave the
            // line short instead of silently expanding letters.
            if ( extra_width > 0 &&
                    pbuffer->knuth.tracking_stretch_percent == 0 )
                return 0;
            return applyDistributedTracking(frmline, extra_width, trackable_width);
        }

        // Pick one absolute space width. Emergency stretch must actually be
        // rendered, rather than being converted into a ragged right edge.
        // With bounded contraction available, round upward and absorb only
        // the small integer overshoot with the continuous tracking pen.
        int desired_space_total = natural_space_total + extra_width;
        int tracking_points = countDistributedTrackingPoints(frmline);
        int tracking_shrink_capacity = tracking_points > 0
                ? getDistributedTrackingLimit(trackable_width, true) : 0;
        // Same choice as the planner made for this line: without emergency
        // stretch, spaces stop at their maximum and tracking stretch (if
        // allowed) does the rest.
        int target_space = knuthCommonSpaceTarget(
                desired_space_total, spaces, min_common_space,
                max_common_space, emergency_stretch,
                tracking_shrink_capacity);
        int predicted_applied = target_space * spaces - natural_space_total;
        if ( tracking_points == 0 && predicted_applied > extra_width ) {
            // There is no subpixel sink for an indivisible negative
            // remainder. Bias the common word space downward: a slightly
            // short line is acceptable, crossing the margin is not.
            int lower_by = (predicted_applied - extra_width + spaces - 1) /
                    spaces;
            target_space = std::max(1, target_space - lower_by);
        }
        int applied = applyFixedWordSpacing(frmline, target_space);
        int remaining = extra_width - applied;
        if ( tracking_points == 0 && remaining > 0 ) {
            // No tracking sink (e.g. not HarfBuzz): spread the downward
            // rounding remainder as single pixels over the word gaps, as
            // the legacy justification does, to keep the right edge flush.
            int extra = std::min(remaining, spaces);
            int gap = 0;
            int shift_x = 0;
            for ( int i=0; i<(int)frmline->word_count; i++ ) {
                formatted_word_t * word = &frmline->words[i];
                word->x += shift_x;
                if ( i < (int)frmline->word_count - 1 &&
                        (word->flags & LTEXT_WORD_CAN_ADD_SPACE_AFTER) ) {
                    int add = (gap+1) * extra / spaces - gap * extra / spaces;
                    word->width += add;
                    word->min_width += add;
                    shift_x += add;
                    gap++;
                }
            }
            frmline->width += shift_x;
            applied += shift_x;
            remaining -= shift_x;
        }
        if ( remaining > 0 &&
                pbuffer->knuth.tracking_stretch_percent == 0 )
            return applied;
        if ( remaining != 0 && tracking_points > 0 )
            applied += applyDistributedTracking(frmline, remaining, trackable_width);
        return applied;
    }
};

} // namespace

// ---------------------------------------------------------------------------
// Interface (knuthparagraph.h, knuthjustify.h)

void knuthClearHyphenationCache()
{
    optimalHyphenationCache.clear();
}

bool knuthDuplicatesHyphenAfter( const KnuthParagraph & p, int pos )
{
#if (USE_LIBUNIBREAK==1)
    return pos >= 0 && pos + 1 < p.length - 1 &&
            (p.text[pos] == '-' || p.text[pos] == UNICODE_HYPHEN) &&
            p.srcs[pos]->lang_cfg->duplicateRealHyphenOnNextLine();
#else
    return false;
#endif
}

bool knuthCanBreakParagraph( const KnuthParagraph & p )
{
    for ( int i=0; i<p.length; i++ ) {
        // Inline pads (margin/border/padding of inline elements, such as
        // styled footnote references), inline images and inline-block
        // boxes are fixed-width boxes: measureText() has put their width
        // in widths and set their wrap flags. Floats are positioned while
        // lines are added, which the planner can't foresee.
        if ( isPadChar(p, i) )
            continue;
        bool object = p.flags[i] & LCHAR_IS_OBJECT;
        if ( object && p.charindex[i] == p.float_char_index )
            return false;
        if ( p.text[i] == '\n' ||
                (p.flags[i] & (LCHAR_IS_CJK|LCHAR_IS_FLEXIBLE_WIDTH_CJK)) )
            return false;
        src_text_fragment_t * src = p.srcs[i];
        if ( !src || (src->flags & (LTEXT_FLAG_PREFORMATTED|
                                    LTEXT_FLAG_NOWRAP|LTEXT_LOCKED_SPACING|
                                    LTEXT_IS_FIRST_LINE_CLONE)) )
            return false;
        if ( object )
            continue; // an image or inline box: no glyphs
        if ( src->flags & LTEXT_SRC_IS_OBJECT )
            return false;
        // Ignorable chars are never drawn, so their font coverage
        // does not matter.
        if ( p.harfbuzz &&
                !(p.flags[i] & (LCHAR_IS_SPACE|LCHAR_IS_TO_IGNORE)) ) {
            LVFont * font = (LVFont *)src->t.font;
            if ( !font || !font->hasGlyph(p.text[i]) )
                return false;
        }
    }
    return true;
}

bool knuthBreakParagraph( const KnuthParagraph & p, std::vector<OptimalLineBreak> & breaks )
{
    std::vector<bool> hyphenation;
    findParagraphHyphenation(p, hyphenation);
    // Hyphens are included in the first pass (their cost is set by the
    // hyphen penalties); only emergency stretch is reserved for the
    // fallback pass. Like TeX with tolerance 10000, a last pass accepts
    // lines looser than the emergency stretch allows, so that one hard
    // line does not send the whole paragraph back to greedy breaking
    // (where none of the Knuth settings apply). Shrink limits stay hard.
    OptimalBreakSetup & setup = optimalSetup();
    if ( !setup.prepare(p, hyphenation) )
        return false;
    return setup.search(breaks, p.config->pretolerance, false) ||
            setup.search(breaks, p.config->tolerance, true) ||
            setup.search(breaks, 10000, true, true);
}

void knuthJustifyLine( formatted_line_t * frmline, int extra_width,
                       const formatted_text_fragment_t * pbuffer, bool harfbuzz,
                       bool emergency_stretch )
{
    LineJustifier justifier = { pbuffer, harfbuzz, emergency_stretch };
    justifier.applyOptimizedJustification(frmline, extra_width);
}
