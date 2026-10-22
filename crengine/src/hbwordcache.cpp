/** \file hbwordcache.cpp
    \brief Word-level cache of HarfBuzz shaping results, for text measuring

    See hbwordcache.h.

    How words are made safe to shape separately:

    A text run is split into units, each a word with its trailing spaces.
    The shaping of a unit alone is the same as in the whole run unless a
    font rule matches glyphs on both sides of the space ending a unit
    (eg. a pair kerning the space with the next word's first letter, or
    a contextual substitution looking past the space). Rules involving
    the space and the glyphs before it only (eg. kerning a letter with the
    space following it) are fine, as all these glyphs are in the unit.

    So, for each font (and script and language), the lookups HarfBuzz will
    apply are collected, and their rules with glyphs after a space glyph
    ("crossing rules") gathered. Each char is then mapped to the set of
    glyphs it may become through substitutions, which tells which positions
    of these rules it could fill. A boundary between two units is
    "sensitive" when a crossing rule could match both the end of the first
    unit and the start of the next: these two units are then shaped (and
    cached) together.

    The rule matching is relaxed (any char of a unit may fill any rule
    position on its side) so that it is conservative: it may see
    sensitive boundaries that are not, never the reverse. Anything not
    understood makes the font use normal shaping.

    CoolReader Engine, (c) Vadim Lopatin, 2000-2006
    This source code is distributed under the terms of
    GNU General Public License.
    See LICENSE file for details.
*/

#include "../include/hbwordcache.h"

#if USE_HARFBUZZ==1

#include <hb-ot.h>
#include <hb-aat.h>
#include <algorithm>
#include <unordered_map>
#include <stdlib.h>
#include <string.h>
#ifdef HB_WORD_CACHE_VERIFY
#include <stdio.h>
#endif

// Same as in lvfntman.cpp
#define HBWC_METRIC_TO_PX(x) (((x)+32) >> 6)

#define HBWC_MAX_RULES 64           // crossing rules tracked per font (more get merged)
#define HBWC_MAX_POSITIONS 8        // rule positions checked on each side of a space (more are ignored)
#define HBWC_MASK_WORDS (HBWC_MAX_RULES / 8)
#define HBWC_LOW_CHARS 0x3000       // chars with their masks index in a direct table
// When most boundaries need to be merged, the cache does not help: stop using it
#define HBWC_MERGES_MIN_BOUNDARIES 4096
#define HBWC_MERGES_MAX_PERCENT 30

#ifdef HB_WORD_CACHE_VERIFY
struct HBWordCacheStats {
    long runs, runsCached, runsRefused, runsFailed, segments, hits, misses, stored, merges, models, unusableModels;
    HBWordCacheStats() { memset(this, 0, sizeof(*this)); }
    ~HBWordCacheStats() {
        fprintf(stderr, "HBWC stats: runs=%ld cached=%ld refused=%ld failed=%ld segments=%ld hits=%ld misses=%ld stored=%ld merged_boundaries=%ld models=%ld unusable_models=%ld\n",
                runs, runsCached, runsRefused, runsFailed, segments, hits, misses, stored, merges, models, unusableModels);
    }
};
static HBWordCacheStats hbwc_stats;
#define HBWC_STAT(x) (hbwc_stats.x++)
#else
#define HBWC_STAT(x)
#endif

namespace {

// ===== OpenType table reading (big-endian, bounds-checked: out of range reads give 0)

class OTData {
    const lUInt8 * _d;
    unsigned int _len;
public:
    OTData() : _d(NULL), _len(0) { }
    void set( hb_blob_t * blob ) {
        unsigned int len = 0;
        _d = blob ? (const lUInt8 *)hb_blob_get_data(blob, &len) : NULL;
        _len = _d ? len : 0;
    }
    bool has( unsigned int off, unsigned int size ) const { return off <= _len && size <= _len - off; }
    unsigned int u16( unsigned int off ) const {
        return has(off, 2) ? ((unsigned int)_d[off] << 8) | _d[off+1] : 0;
    }
    lUInt32 u32( unsigned int off ) const {
        return has(off, 4) ? ((lUInt32)_d[off] << 24) | ((lUInt32)_d[off+1] << 16) | ((lUInt32)_d[off+2] << 8) | _d[off+3] : 0;
    }
};

// Glyphs of a Coverage table, in coverage index order
static void coverageGlyphs( const OTData & t, unsigned int off, std::vector<hb_codepoint_t> & out )
{
    out.clear();
    unsigned int format = t.u16(off);
    unsigned int count = t.u16(off+2);
    if ( format == 1 ) {
        for ( unsigned int i = 0; i < count && t.has(off+4+2*i, 2); i++ )
            out.push_back(t.u16(off+4+2*i));
    }
    else if ( format == 2 ) {
        for ( unsigned int i = 0; i < count && t.has(off+4+6*i, 6); i++ ) {
            unsigned int start = t.u16(off+4+6*i);
            unsigned int end = t.u16(off+4+6*i+2);
            for ( unsigned int g = start; g <= end; g++ )
                out.push_back(g);
        }
    }
}

// Coverage index of a glyph, or -1
static int coverageIndex( const OTData & t, unsigned int off, hb_codepoint_t g )
{
    unsigned int format = t.u16(off);
    int count = (int)t.u16(off+2);
    int lo = 0, hi = count - 1;
    if ( format == 1 ) {
        while ( lo <= hi ) {
            int mid = (lo + hi) / 2;
            unsigned int mg = t.u16(off+4+2*mid);
            if ( mg == g ) return mid;
            if ( mg < g ) lo = mid + 1; else hi = mid - 1;
        }
    }
    else if ( format == 2 ) {
        while ( lo <= hi ) {
            int mid = (lo + hi) / 2;
            unsigned int r = off+4+6*mid;
            unsigned int start = t.u16(r), end = t.u16(r+2);
            if ( g < start ) hi = mid - 1;
            else if ( g > end ) lo = mid + 1;
            else return (int)(t.u16(r+4) + (g - start));
        }
    }
    return -1;
}

// Class of a glyph in a ClassDef table (0 when not listed, or no table)
static unsigned int classOf( const OTData & t, unsigned int off, hb_codepoint_t g )
{
    if ( !off )
        return 0;
    unsigned int format = t.u16(off);
    if ( format == 1 ) {
        unsigned int start = t.u16(off+2), count = t.u16(off+4);
        if ( g >= start && g < start + count )
            return t.u16(off+6+2*(g-start));
    }
    else if ( format == 2 ) {
        int lo = 0, hi = (int)t.u16(off+2) - 1;
        while ( lo <= hi ) {
            int mid = (lo + hi) / 2;
            unsigned int r = off+4+6*mid;
            unsigned int start = t.u16(r), end = t.u16(r+2);
            if ( g < start ) hi = mid - 1;
            else if ( g > end ) lo = mid + 1;
            else return t.u16(r+4);
        }
    }
    return 0;
}

static unsigned int valueRecordSize( unsigned int valueFormat )
{
    unsigned int n = 0;
    for ( unsigned int b = valueFormat & 0xFF; b; b >>= 1 )
        n += b & 1;
    return 2 * n;
}

static bool valueRecordIsNull( const OTData & t, unsigned int off, unsigned int valueFormat )
{
    unsigned int size = valueRecordSize(valueFormat);
    for ( unsigned int i = 0; i < size; i += 2 )
        if ( t.u16(off+i) )
            return false;
    return true;
}

// ===== Rules

// A glyph position in a rule: any glyph, glyphs from a set, or glyphs of a class
struct HBGlyphPredicate {
    enum Kind { SET, CLASS } kind;
    std::vector<hb_codepoint_t> glyphs; // SET (sorted)
    const OTData * table;               // CLASS
    unsigned int classDef;
    unsigned int cls;
    bool test( hb_codepoint_t g ) const {
        if ( kind == SET )
            return std::binary_search(glyphs.begin(), glyphs.end(), g);
        return classOf(*table, classDef, g) == cls;
    }
};

// A rule position matches if any of its predicates match (or always if any)
struct HBRulePosition {
    bool any;
    std::vector<HBGlyphPredicate> preds;
    HBRulePosition() : any(false) { }
    bool admits( const std::vector<hb_codepoint_t> & glyphs ) const {
        if ( any )
            return true;
        for ( size_t p = 0; p < preds.size(); p++ )
            for ( size_t i = 0; i < glyphs.size(); i++ )
                if ( preds[p].test(glyphs[i]) )
                    return true;
        return false;
    }
    static HBRulePosition anyGlyph() { HBRulePosition p; p.any = true; return p; }
    static HBRulePosition glyph( hb_codepoint_t g ) {
        HBRulePosition p;
        HBGlyphPredicate pred;
        pred.kind = HBGlyphPredicate::SET;
        pred.table = NULL; pred.classDef = 0; pred.cls = 0;
        pred.glyphs.push_back(g);
        p.preds.push_back(pred);
        return p;
    }
    static HBRulePosition set( std::vector<hb_codepoint_t> glyphs ) {
        HBRulePosition p;
        HBGlyphPredicate pred;
        pred.kind = HBGlyphPredicate::SET;
        pred.table = NULL; pred.classDef = 0; pred.cls = 0;
        std::sort(glyphs.begin(), glyphs.end());
        pred.glyphs.swap(glyphs);
        p.preds.push_back(pred);
        return p;
    }
    static HBRulePosition ofClass( const OTData * table, unsigned int classDef, unsigned int cls ) {
        HBRulePosition p;
        HBGlyphPredicate pred;
        pred.kind = HBGlyphPredicate::CLASS;
        pred.table = table; pred.classDef = classDef; pred.cls = cls;
        p.preds.push_back(pred);
        return p;
    }
};

// A rule that can match glyphs on both sides of a space: left[k-1] and right[k-1]
// are the k-th positions before and after that space.
struct HBCrossRule {
    std::vector<HBRulePosition> left;
    std::vector<HBRulePosition> right;
    lUInt8 needLeft;  // positions that must be matched (bit k-1 for k-th position)
    lUInt8 needRight;
    lUInt8 anyLeft;   // positions matching any glyph
    lUInt8 anyRight;
};

} // namespace

// ===== Shaping model: what is known about a font for a script and language

class HBShapingModel {
public:
    hb_script_t script;
    hb_language_t language;
    lUInt64 id;
    bool usable;
private:
    hb_font_t * _font;
    hb_face_t * _face;
    hb_blob_t * _gsubBlob;
    hb_blob_t * _gposBlob;
    OTData _gsub;
    OTData _gpos;
    std::vector<bool> _gsubLookups; // active lookups
    std::vector<bool> _gposLookups;
    // GSUB "may become" graph: (from, to) glyph pairs, sorted
    std::vector< std::pair<hb_codepoint_t, hb_codepoint_t> > _edges;
    std::vector<hb_codepoint_t> _spaceGlyphs; // what the space glyph may become
    std::vector<HBCrossRule> _rules;
    // Per char: index in _masks of its masks, or -1 when none. A char's masks are
    // HBWC_MASK_WORDS words of left position bits (one byte per rule) followed by
    // as many of right position bits.
    std::vector<int> _lowCharMasks; // for chars < HBWC_LOW_CHARS (-2: not computed)
    std::unordered_map<lUInt32, int> _charMasks;
    std::vector<lUInt64> _masks;
    lUInt64 _needLeft[HBWC_MASK_WORDS];
    lUInt64 _needRight[HBWC_MASK_WORDS];
    lUInt64 _anyLeft[HBWC_MASK_WORDS];
    lUInt64 _anyRight[HBWC_MASK_WORDS];
    long _boundaries; // boundaries checked, and found sensitive
    long _merges;
    std::vector<hb_codepoint_t> _scratch;

    bool collectLookups( hb_tag_t table, const hb_tag_t * scriptTags, unsigned int scriptCount,
                         const hb_tag_t * langTags, unsigned int langCount,
                         const std::vector<hb_tag_t> & features, std::vector<bool> & lookups, bool & hasKern );
    unsigned int lookupCount( const OTData & t ) const;
    bool forEachSubtable( const OTData & t, unsigned int lookupIndex, unsigned int extensionType,
                          unsigned int & type, std::vector<unsigned int> & subtables, unsigned int & flags );
    void addNested( const OTData & t, unsigned int records, unsigned int count, std::vector<bool> & lookups, std::vector<unsigned int> & todo );
    bool parseGsubGraph();
    void closure( const std::vector<hb_codepoint_t> & from, std::vector<hb_codepoint_t> & out );
    bool admitsSpace( const HBRulePosition & pos ) const { return pos.admits(_spaceGlyphs); }
    void addSequence( const std::vector<HBRulePosition> & seq );
    void parseContext( const OTData & t, unsigned int off, unsigned int type, bool chained );
    bool parseRules( const OTData & t, bool gpos, std::vector<bool> & lookups );
    void finishRules();
    int computeCharMasks( lUInt32 ch );
    int charMaskIndex( lUInt32 ch ) {
        if ( ch < HBWC_LOW_CHARS ) {
            int index = _lowCharMasks[ch];
            return index != -2 ? index : (_lowCharMasks[ch] = computeCharMasks(ch));
        }
        std::unordered_map<lUInt32, int>::const_iterator it = _charMasks.find(ch);
        if ( it != _charMasks.end() )
            return it->second;
        return _charMasks[ch] = computeCharMasks(ch);
    }
    lUInt64 rulesSatisfied( const lUInt64 * acc, const lUInt64 * need ) const;
public:
    HBShapingModel( hb_font_t * font, const hb_feature_t * features, unsigned int num_features,
                    hb_script_t script, hb_language_t language, bool legacyKerning );
    ~HBShapingModel();
    bool hasRules() const { return !_rules.empty(); }
    /** \brief Crossing rules a unit could match on each side of its boundaries
        \param endBits receives the rules it could match before the space ending it
        \param startBits receives the rules it could match after the space preceding it
    */
    void unitBits( const hb_codepoint_t * text, int len, bool endsWithBoundary, lUInt64 & endBits, lUInt64 & startBits );
    /// Account for a boundary check (merged: found sensitive)
    void countBoundary( bool merged ) { _boundaries++; if ( merged ) _merges++; }
    /// False when so many boundaries had to be merged that the cache does not help
    bool isEffective() const {
        return _boundaries < HBWC_MERGES_MIN_BOUNDARIES || _merges * 100 < _boundaries * HBWC_MERGES_MAX_PERCENT;
    }
};

static lUInt64 hbwc_next_model_id = 1;

HBShapingModel::HBShapingModel( hb_font_t * font, const hb_feature_t * features, unsigned int num_features,
                                hb_script_t script_, hb_language_t language_, bool legacyKerning )
    : script(script_), language(language_), id(hbwc_next_model_id++), usable(false),
      _font(font), _face(hb_font_get_face(font)), _gsubBlob(NULL), _gposBlob(NULL), _boundaries(0), _merges(0)
{
    HBWC_STAT(models);
    // AAT shaping and tracking are not analysed
    if ( hb_aat_layout_has_substitution(_face) || hb_aat_layout_has_positioning(_face) || hb_aat_layout_has_tracking(_face) )
        return;
    hb_codepoint_t space;
    if ( !hb_font_get_nominal_glyph(font, 0x20, &space) )
        return;

    // The features HarfBuzz enables for horizontal LTR text with its default shaper
    // (hb-ot-shape.cc hb_ot_shape_collect_features()), minus the automatic fractions
    // ones, which only apply around U+2044 FRACTION SLASH, not on spaces.
    static const char * const default_features[] = {
        "rvrn", "ltra", "ltrm", "rand", "Harf", "HARF", "Buzz", "BUZZ",
        "abvm", "blwm", "ccmp", "locl", "mark", "mkmk", "rlig",
        "calt", "clig", "curs", "dist", "kern", "liga", "rclt",
    };
    std::vector<hb_tag_t> tags;
    for ( size_t i = 0; i < sizeof(default_features)/sizeof(default_features[0]); i++ )
        tags.push_back(hb_tag_from_string(default_features[i], 4));
    // Requested features: only add them (also keeping those disabled, which is conservative)
    for ( unsigned int i = 0; i < num_features; i++ ) {
        if ( features[i].value )
            tags.push_back(features[i].tag);
    }

    hb_tag_t scriptTags[HB_OT_MAX_TAGS_PER_SCRIPT];
    hb_tag_t langTags[HB_OT_MAX_TAGS_PER_LANGUAGE];
    unsigned int scriptCount = HB_OT_MAX_TAGS_PER_SCRIPT;
    unsigned int langCount = HB_OT_MAX_TAGS_PER_LANGUAGE;
    hb_ot_tags_from_script_and_language(script, language, &scriptCount, scriptTags, &langCount, langTags);

    _gsubBlob = hb_face_reference_table(_face, HB_OT_TAG_GSUB);
    _gposBlob = hb_face_reference_table(_face, HB_OT_TAG_GPOS);
    _gsub.set(_gsubBlob);
    _gpos.set(_gposBlob);
    bool hasGsubKern = false, hasGposKern = false;
    if ( !collectLookups(HB_OT_TAG_GSUB, scriptTags, scriptCount, langTags, langCount, tags, _gsubLookups, hasGsubKern) )
        return;
    if ( !collectLookups(HB_OT_TAG_GPOS, scriptTags, scriptCount, langTags, langCount, tags, _gposLookups, hasGposKern) )
        return;
    // Kerning from a legacy 'kern' table, or from FreeType (used when GPOS has no 'kern'
    // feature), is not analysed
    if ( !hasGposKern ) {
        hb_blob_t * kern = hb_face_reference_table(_face, HB_TAG('k','e','r','n'));
        bool hasKernTable = hb_blob_get_length(kern) > 0;
        hb_blob_destroy(kern);
        if ( hasKernTable || legacyKerning )
            return;
    }

    if ( !parseGsubGraph() )
        return;
    std::vector<hb_codepoint_t> from(1, space);
    closure(from, _spaceGlyphs);
    // The space glyph must never be skipped by lookups (we refuse IgnoreBaseGlyphs lookups)
    for ( size_t i = 0; i < _spaceGlyphs.size(); i++ ) {
        hb_ot_layout_glyph_class_t cls = hb_ot_layout_get_glyph_class(_face, _spaceGlyphs[i]);
        if ( cls == HB_OT_LAYOUT_GLYPH_CLASS_MARK || cls == HB_OT_LAYOUT_GLYPH_CLASS_LIGATURE )
            return;
    }
    if ( !parseRules(_gsub, false, _gsubLookups) )
        return;
    if ( !parseRules(_gpos, true, _gposLookups) )
        return;
    finishRules();
    usable = true;
}

HBShapingModel::~HBShapingModel()
{
    if ( _gsubBlob )
        hb_blob_destroy(_gsubBlob);
    if ( _gposBlob )
        hb_blob_destroy(_gposBlob);
}

unsigned int HBShapingModel::lookupCount( const OTData & t ) const
{
    unsigned int lookupList = t.u16(8);
    return lookupList ? t.u16(lookupList) : 0;
}

// Mirrors what HarfBuzz's hb_ot_map_builder_t does to select the lookups to apply
bool HBShapingModel::collectLookups( hb_tag_t table, const hb_tag_t * scriptTags, unsigned int scriptCount,
                                     const hb_tag_t * langTags, unsigned int langCount,
                                     const std::vector<hb_tag_t> & features, std::vector<bool> & lookups, bool & hasKern )
{
    const OTData & t = table == HB_OT_TAG_GSUB ? _gsub : _gpos;
    lookups.assign(lookupCount(t), false);
    unsigned int scriptIndex = 0, languageIndex = HB_OT_LAYOUT_DEFAULT_LANGUAGE_INDEX;
    hb_tag_t chosenScript;
    hb_ot_layout_table_select_script(_face, table, scriptCount, scriptTags, &scriptIndex, &chosenScript);
    hb_ot_layout_script_select_language(_face, table, scriptIndex, langCount, langTags, &languageIndex);

    unsigned int variationsIndex = HB_OT_LAYOUT_NO_VARIATIONS_INDEX;
    unsigned int numCoords = 0;
    const int * coords = hb_font_get_var_coords_normalized(_font, &numCoords);
    hb_ot_layout_table_find_feature_variations(_face, table, coords, numCoords, &variationsIndex);

    std::vector<unsigned int> featureIndexes;
    unsigned int requiredIndex;
    hb_tag_t requiredTag;
    if ( hb_ot_layout_language_get_required_feature(_face, table, scriptIndex, languageIndex, &requiredIndex, &requiredTag) )
        featureIndexes.push_back(requiredIndex);
    for ( size_t i = 0; i < features.size(); i++ ) {
        unsigned int featureIndex;
        if ( hb_ot_layout_language_find_feature(_face, table, scriptIndex, languageIndex, features[i], &featureIndex) ) {
            featureIndexes.push_back(featureIndex);
            if ( features[i] == HB_TAG('k','e','r','n') )
                hasKern = true;
            // Random alternates depend on the position in the buffer: can't cache
            if ( features[i] == HB_TAG('r','a','n','d') )
                return false;
        }
    }
    for ( size_t f = 0; f < featureIndexes.size(); f++ ) {
        unsigned int lookupIndexes[64];
        unsigned int start = 0;
        for ( ;; ) {
            unsigned int count = 64;
            hb_ot_layout_feature_with_variations_get_lookups(_face, table, featureIndexes[f], variationsIndex, start, &count, lookupIndexes);
            for ( unsigned int i = 0; i < count; i++ ) {
                if ( lookupIndexes[i] >= lookups.size() )
                    return false;
                lookups[lookupIndexes[i]] = true;
            }
            if ( count < 64 )
                break;
            start += count;
        }
    }
    return true;
}

// Gives the lookup type and the offsets of its subtables (extensions resolved)
bool HBShapingModel::forEachSubtable( const OTData & t, unsigned int lookupIndex, unsigned int extensionType,
                                      unsigned int & type, std::vector<unsigned int> & subtables, unsigned int & flags )
{
    subtables.clear();
    unsigned int lookupList = t.u16(8);
    unsigned int lookup = lookupList + t.u16(lookupList + 2 + 2*lookupIndex);
    if ( !t.has(lookup, 6) )
        return false;
    type = t.u16(lookup);
    flags = t.u16(lookup+2);
    bool extension = type == extensionType;
    unsigned int count = t.u16(lookup+4);
    for ( unsigned int i = 0; i < count; i++ ) {
        unsigned int sub = lookup + t.u16(lookup + 6 + 2*i);
        if ( extension ) {
            // Extension subtables all point to subtables of a same real type
            if ( t.u16(sub) != 1 )
                return false;
            unsigned int realType = t.u16(sub+2);
            if ( i == 0 )
                type = realType;
            else if ( realType != type )
                return false;
            sub = sub + t.u32(sub+4);
        }
        subtables.push_back(sub);
    }
    return !extension || count > 0;
}

// Marks the lookups referenced by SequenceLookupRecords as active (nested lookups)
void HBShapingModel::addNested( const OTData & t, unsigned int records, unsigned int count,
                                std::vector<bool> & lookups, std::vector<unsigned int> & todo )
{
    for ( unsigned int i = 0; i < count; i++ ) {
        unsigned int index = t.u16(records + 4*i + 2);
        if ( index < lookups.size() && !lookups[index] ) {
            lookups[index] = true;
            todo.push_back(index);
        }
    }
}

// Builds the GSUB "may become" graph from all active lookups (and nested ones)
bool HBShapingModel::parseGsubGraph()
{
    const OTData & t = _gsub;
    std::vector<unsigned int> todo;
    for ( unsigned int i = 0; i < _gsubLookups.size(); i++ )
        if ( _gsubLookups[i] )
            todo.push_back(i);
    std::vector<unsigned int> subtables;
    std::vector<hb_codepoint_t> cov;
    while ( !todo.empty() ) {
        unsigned int lookupIndex = todo.back();
        todo.pop_back();
        unsigned int type, flags;
        if ( !forEachSubtable(t, lookupIndex, 7, type, subtables, flags) )
            return false;
        if ( flags & 0x0002 ) // IgnoreBaseGlyphs: a space could be skipped
            return false;
        for ( size_t s = 0; s < subtables.size(); s++ ) {
            unsigned int sub = subtables[s];
            unsigned int format = t.u16(sub);
            switch ( type ) {
            case 1: // Single
                coverageGlyphs(t, sub + t.u16(sub+2), cov);
                for ( size_t i = 0; i < cov.size(); i++ ) {
                    hb_codepoint_t to = format == 1 ? ((cov[i] + t.u16(sub+4)) & 0xFFFF) : t.u16(sub + 6 + 2*i);
                    _edges.push_back(std::make_pair(cov[i], to));
                }
                break;
            case 2: // Multiple
            case 3: // Alternate
                coverageGlyphs(t, sub + t.u16(sub+2), cov);
                for ( size_t i = 0; i < cov.size(); i++ ) {
                    unsigned int seq = sub + t.u16(sub + 6 + 2*i);
                    unsigned int n = t.u16(seq);
                    for ( unsigned int j = 0; j < n; j++ )
                        _edges.push_back(std::make_pair(cov[i], (hb_codepoint_t)t.u16(seq + 2 + 2*j)));
                }
                break;
            case 4: // Ligature: any component may become the ligature
                coverageGlyphs(t, sub + t.u16(sub+2), cov);
                for ( size_t i = 0; i < cov.size(); i++ ) {
                    unsigned int ligSet = sub + t.u16(sub + 6 + 2*i);
                    unsigned int n = t.u16(ligSet);
                    for ( unsigned int j = 0; j < n; j++ ) {
                        unsigned int lig = ligSet + t.u16(ligSet + 2 + 2*j);
                        hb_codepoint_t ligGlyph = t.u16(lig);
                        unsigned int compCount = t.u16(lig+2);
                        _edges.push_back(std::make_pair(cov[i], ligGlyph));
                        for ( unsigned int c = 1; c < compCount; c++ )
                            _edges.push_back(std::make_pair((hb_codepoint_t)t.u16(lig + 4 + 2*(c-1)), ligGlyph));
                    }
                }
                break;
            case 5: // Context
                if ( format == 1 || format == 2 ) {
                    unsigned int setCount = t.u16(sub + (format == 1 ? 4 : 6));
                    unsigned int sets = sub + (format == 1 ? 6 : 8);
                    for ( unsigned int i = 0; i < setCount; i++ ) {
                        unsigned int setOff = t.u16(sets + 2*i);
                        if ( !setOff ) continue;
                        unsigned int ruleSet = sub + setOff;
                        unsigned int ruleCount = t.u16(ruleSet);
                        for ( unsigned int r = 0; r < ruleCount; r++ ) {
                            unsigned int rule = ruleSet + t.u16(ruleSet + 2 + 2*r);
                            unsigned int glyphCount = t.u16(rule), recCount = t.u16(rule+2);
                            addNested(t, rule + 4 + 2*(glyphCount ? glyphCount-1 : 0), recCount, _gsubLookups, todo);
                        }
                    }
                }
                else if ( format == 3 ) {
                    unsigned int glyphCount = t.u16(sub+2), recCount = t.u16(sub+4);
                    addNested(t, sub + 6 + 2*glyphCount, recCount, _gsubLookups, todo);
                }
                else return false;
                break;
            case 6: // Chained context
                if ( format == 1 || format == 2 ) {
                    unsigned int setCount = t.u16(sub + (format == 1 ? 4 : 10));
                    unsigned int sets = sub + (format == 1 ? 6 : 12);
                    for ( unsigned int i = 0; i < setCount; i++ ) {
                        unsigned int setOff = t.u16(sets + 2*i);
                        if ( !setOff ) continue;
                        unsigned int ruleSet = sub + setOff;
                        unsigned int ruleCount = t.u16(ruleSet);
                        for ( unsigned int r = 0; r < ruleCount; r++ ) {
                            unsigned int p = ruleSet + t.u16(ruleSet + 2 + 2*r);
                            p += 2 + 2*t.u16(p);                    // backtrack
                            unsigned int inputCount = t.u16(p);
                            p += 2 + 2*(inputCount ? inputCount-1 : 0); // input
                            p += 2 + 2*t.u16(p);                    // lookahead
                            addNested(t, p + 2, t.u16(p), _gsubLookups, todo);
                        }
                    }
                }
                else if ( format == 3 ) {
                    unsigned int p = sub + 2;
                    p += 2 + 2*t.u16(p); // backtrack coverages
                    p += 2 + 2*t.u16(p); // input coverages
                    p += 2 + 2*t.u16(p); // lookahead coverages
                    addNested(t, p + 2, t.u16(p), _gsubLookups, todo);
                }
                else return false;
                break;
            case 8: // Reverse chaining single
                {
                    unsigned int p = sub + 4;
                    p += 2 + 2*t.u16(p); // backtrack
                    p += 2 + 2*t.u16(p); // lookahead
                    coverageGlyphs(t, sub + t.u16(sub+2), cov);
                    unsigned int n = t.u16(p);
                    for ( size_t i = 0; i < cov.size() && i < n; i++ )
                        _edges.push_back(std::make_pair(cov[i], (hb_codepoint_t)t.u16(p + 2 + 2*i)));
                }
                break;
            default:
                return false;
            }
        }
    }
    std::sort(_edges.begin(), _edges.end());
    _edges.erase(std::unique(_edges.begin(), _edges.end()), _edges.end());
    return true;
}

// All the glyphs the provided ones may become (including themselves), sorted
void HBShapingModel::closure( const std::vector<hb_codepoint_t> & from, std::vector<hb_codepoint_t> & out )
{
    out = from;
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    std::vector<hb_codepoint_t> todo(out);
    while ( !todo.empty() ) {
        hb_codepoint_t g = todo.back();
        todo.pop_back();
        std::vector< std::pair<hb_codepoint_t, hb_codepoint_t> >::const_iterator it =
            std::lower_bound(_edges.begin(), _edges.end(), std::make_pair(g, (hb_codepoint_t)0));
        for ( ; it != _edges.end() && it->first == g; ++it ) {
            std::vector<hb_codepoint_t>::iterator pos = std::lower_bound(out.begin(), out.end(), it->second);
            if ( pos == out.end() || *pos != it->second ) {
                out.insert(pos, it->second);
                todo.push_back(it->second);
            }
        }
    }
}

// Adds a crossing rule for each position of a sequence that may be a space
// with positions after it
void HBShapingModel::addSequence( const std::vector<HBRulePosition> & seq )
{
    for ( size_t i = 0; i + 1 < seq.size(); i++ ) {
        if ( !admitsSpace(seq[i]) )
            continue;
        HBCrossRule rule;
        for ( size_t k = 1; k <= i; k++ )
            rule.left.push_back(seq[i-k]);
        for ( size_t k = i + 1; k < seq.size(); k++ )
            rule.right.push_back(seq[k]);
        _rules.push_back(rule);
    }
}

// (Chain)Context(Subst|Pos) formats 1, 2 and 3: their sequences, as rule positions
void HBShapingModel::parseContext( const OTData & t, unsigned int sub, unsigned int type, bool chained )
{
    unsigned int format = t.u16(sub);
    std::vector<HBRulePosition> seq;
    std::vector<hb_codepoint_t> cov;
    (void)type; // (same layouts for GSUB and GPOS)
    if ( format == 1 ) {
        coverageGlyphs(t, sub + t.u16(sub+2), cov);
        unsigned int setCount = t.u16(sub+4);
        for ( unsigned int i = 0; i < setCount && i < cov.size(); i++ ) {
            unsigned int setOff = t.u16(sub + 6 + 2*i);
            if ( !setOff ) continue;
            unsigned int ruleSet = sub + setOff;
            unsigned int ruleCount = t.u16(ruleSet);
            for ( unsigned int r = 0; r < ruleCount; r++ ) {
                unsigned int p = ruleSet + t.u16(ruleSet + 2 + 2*r);
                seq.clear();
                if ( chained ) {
                    unsigned int n = t.u16(p);
                    for ( unsigned int k = n; k > 0; k-- ) // stored nearest first
                        seq.push_back(HBRulePosition::glyph(t.u16(p + 2*k)));
                    p += 2 + 2*n;
                }
                // Chained: inputGlyphCount, input... Not chained: glyphCount, seqLookupCount, input...
                unsigned int inputCount = t.u16(p);
                unsigned int inputs = p + (chained ? 2 : 4);
                seq.push_back(HBRulePosition::glyph(cov[i]));
                for ( unsigned int k = 1; k < inputCount; k++ )
                    seq.push_back(HBRulePosition::glyph(t.u16(inputs + 2*(k-1))));
                if ( chained ) {
                    p += 2 + 2*(inputCount ? inputCount-1 : 0);
                    unsigned int n = t.u16(p);
                    for ( unsigned int k = 0; k < n; k++ )
                        seq.push_back(HBRulePosition::glyph(t.u16(p + 2 + 2*k)));
                }
                addSequence(seq);
            }
        }
    }
    else if ( format == 2 ) {
        unsigned int backtrackDef = 0, inputDef, lookaheadDef = 0, setCount, sets;
        if ( chained ) {
            backtrackDef = t.u16(sub+4) ? sub + t.u16(sub+4) : 0;
            inputDef = t.u16(sub+6) ? sub + t.u16(sub+6) : 0;
            lookaheadDef = t.u16(sub+8) ? sub + t.u16(sub+8) : 0;
            setCount = t.u16(sub+10);
            sets = sub + 12;
        }
        else {
            inputDef = t.u16(sub+4) ? sub + t.u16(sub+4) : 0;
            setCount = t.u16(sub+6);
            sets = sub + 8;
        }
        for ( unsigned int i = 0; i < setCount; i++ ) {
            unsigned int setOff = t.u16(sets + 2*i);
            if ( !setOff ) continue;
            unsigned int ruleSet = sub + setOff;
            unsigned int ruleCount = t.u16(ruleSet);
            for ( unsigned int r = 0; r < ruleCount; r++ ) {
                unsigned int p = ruleSet + t.u16(ruleSet + 2 + 2*r);
                seq.clear();
                if ( chained ) {
                    unsigned int n = t.u16(p);
                    for ( unsigned int k = n; k > 0; k-- )
                        seq.push_back(HBRulePosition::ofClass(&t, backtrackDef, t.u16(p + 2*k)));
                    p += 2 + 2*n;
                }
                unsigned int inputCount = t.u16(p);
                unsigned int inputs = p + (chained ? 2 : 4);
                seq.push_back(HBRulePosition::ofClass(&t, inputDef, i));
                for ( unsigned int k = 1; k < inputCount; k++ )
                    seq.push_back(HBRulePosition::ofClass(&t, inputDef, t.u16(inputs + 2*(k-1))));
                if ( chained ) {
                    p += 2 + 2*(inputCount ? inputCount-1 : 0);
                    unsigned int n = t.u16(p);
                    for ( unsigned int k = 0; k < n; k++ )
                        seq.push_back(HBRulePosition::ofClass(&t, lookaheadDef, t.u16(p + 2 + 2*k)));
                }
                addSequence(seq);
            }
        }
    }
    else if ( format == 3 ) {
        if ( chained ) {
            unsigned int p = sub + 2;
            unsigned int n = t.u16(p);
            for ( unsigned int k = n; k > 0; k-- ) {
                coverageGlyphs(t, sub + t.u16(p + 2*k), cov);
                seq.push_back(HBRulePosition::set(cov));
            }
            p += 2 + 2*n;
            for ( int part = 0; part < 2; part++ ) { // input, lookahead
                n = t.u16(p);
                for ( unsigned int k = 0; k < n; k++ ) {
                    coverageGlyphs(t, sub + t.u16(p + 2 + 2*k), cov);
                    seq.push_back(HBRulePosition::set(cov));
                }
                p += 2 + 2*n;
            }
        }
        else {
            unsigned int n = t.u16(sub+2);
            for ( unsigned int k = 0; k < n; k++ ) {
                coverageGlyphs(t, sub + t.u16(sub + 6 + 2*k), cov);
                seq.push_back(HBRulePosition::set(cov));
            }
        }
        addSequence(seq);
    }
}

// Gathers the crossing rules from the active lookups of a table
bool HBShapingModel::parseRules( const OTData & t, bool gpos, std::vector<bool> & lookups )
{
    unsigned int extensionType = gpos ? 9 : 7;
    std::vector<unsigned int> todo;
    for ( unsigned int i = 0; i < lookups.size(); i++ )
        if ( lookups[i] )
            todo.push_back(i);
    std::vector<unsigned int> subtables;
    std::vector<hb_codepoint_t> cov, glyphs;
    std::vector<HBRulePosition> seq;
    while ( !todo.empty() ) {
        unsigned int lookupIndex = todo.back();
        todo.pop_back();
        unsigned int type, flags;
        if ( !forEachSubtable(t, lookupIndex, extensionType, type, subtables, flags) )
            return false;
        if ( flags & 0x0002 ) // IgnoreBaseGlyphs: a space could be skipped
            return false;
        for ( size_t s = 0; s < subtables.size(); s++ ) {
            unsigned int sub = subtables[s];
            unsigned int format = t.u16(sub);
            if ( !gpos ) {
                switch ( type ) {
                case 1: case 2: case 3: // only change a single glyph
                    break;
                case 4: // Ligature: sequences of components
                    coverageGlyphs(t, sub + t.u16(sub+2), cov);
                    for ( size_t i = 0; i < cov.size(); i++ ) {
                        unsigned int ligSet = sub + t.u16(sub + 6 + 2*i);
                        unsigned int n = t.u16(ligSet);
                        for ( unsigned int j = 0; j < n; j++ ) {
                            unsigned int lig = ligSet + t.u16(ligSet + 2 + 2*j);
                            unsigned int compCount = t.u16(lig+2);
                            seq.clear();
                            seq.push_back(HBRulePosition::glyph(cov[i]));
                            for ( unsigned int c = 1; c < compCount; c++ )
                                seq.push_back(HBRulePosition::glyph(t.u16(lig + 4 + 2*(c-1))));
                            addSequence(seq);
                        }
                    }
                    break;
                case 5:
                case 6:
                    if ( format < 1 || format > 3 )
                        return false;
                    parseContext(t, sub, type, type == 6);
                    break;
                case 8: // Reverse chaining single
                    {
                        seq.clear();
                        unsigned int p = sub + 4;
                        unsigned int n = t.u16(p);
                        for ( unsigned int k = n; k > 0; k-- ) {
                            coverageGlyphs(t, sub + t.u16(p + 2*k), cov);
                            seq.push_back(HBRulePosition::set(cov));
                        }
                        coverageGlyphs(t, sub + t.u16(sub+2), cov);
                        seq.push_back(HBRulePosition::set(cov));
                        p += 2 + 2*n;
                        n = t.u16(p);
                        for ( unsigned int k = 0; k < n; k++ ) {
                            coverageGlyphs(t, sub + t.u16(p + 2 + 2*k), cov);
                            seq.push_back(HBRulePosition::set(cov));
                        }
                        addSequence(seq);
                    }
                    break;
                default:
                    return false;
                }
            }
            else {
                switch ( type ) {
                case 1: // Single adjustment: a single glyph
                case 6: // MarkToMark: spaces are not marks
                    break;
                case 2: // Pair: a space followed by a glyph
                    {
                        unsigned int covOff = sub + t.u16(sub+2);
                        unsigned int vf1 = t.u16(sub+4), vf2 = t.u16(sub+6);
                        unsigned int size1 = valueRecordSize(vf1), size2 = valueRecordSize(vf2);
                        for ( size_t sg = 0; sg < _spaceGlyphs.size(); sg++ ) {
                            int index = coverageIndex(t, covOff, _spaceGlyphs[sg]);
                            if ( index < 0 )
                                continue;
                            HBCrossRule rule;
                            HBRulePosition pos;
                            if ( format == 1 ) {
                                unsigned int pairSet = sub + t.u16(sub + 10 + 2*index);
                                unsigned int n = t.u16(pairSet);
                                unsigned int recSize = 2 + size1 + size2;
                                glyphs.clear();
                                for ( unsigned int j = 0; j < n; j++ ) {
                                    unsigned int rec = pairSet + 2 + recSize*j;
                                    // A second glyph value format makes HarfBuzz skip that glyph
                                    // as the first of the next pair, even with null values
                                    if ( vf2 || !valueRecordIsNull(t, rec+2, vf1) )
                                        glyphs.push_back(t.u16(rec));
                                }
                                if ( glyphs.empty() )
                                    continue;
                                pos = HBRulePosition::set(glyphs);
                            }
                            else if ( format == 2 ) {
                                unsigned int classDef1 = sub + t.u16(sub+8), classDef2 = sub + t.u16(sub+10);
                                unsigned int class2Count = t.u16(sub+14);
                                unsigned int c1 = classOf(t, classDef1, _spaceGlyphs[sg]);
                                unsigned int recSize = size1 + size2;
                                unsigned int row = sub + 16 + c1 * class2Count * recSize;
                                for ( unsigned int c2 = 0; c2 < class2Count; c2++ ) {
                                    unsigned int rec = row + c2 * recSize;
                                    if ( vf2 || !valueRecordIsNull(t, rec, vf1) ) {
                                        HBRulePosition cp = HBRulePosition::ofClass(&t, classDef2, c2);
                                        pos.preds.push_back(cp.preds[0]);
                                    }
                                }
                                if ( pos.preds.empty() )
                                    continue;
                            }
                            else
                                return false;
                            rule.right.push_back(pos);
                            _rules.push_back(rule);
                        }
                    }
                    break;
                case 3: // Cursive: a space attached to the next glyph
                case 4: // MarkToBase: a mark attached to a space
                case 5: // MarkToLigature
                    {
                        // Cursive: coverage at +2. Mark attachment: mark coverage at +2, base/ligature coverage at +4
                        coverageGlyphs(t, sub + t.u16(sub + (type == 3 ? 2 : 4)), cov);
                        bool hasSpace = false;
                        std::sort(cov.begin(), cov.end());
                        for ( size_t sg = 0; sg < _spaceGlyphs.size() && !hasSpace; sg++ )
                            hasSpace = std::binary_search(cov.begin(), cov.end(), _spaceGlyphs[sg]);
                        if ( !hasSpace )
                            break;
                        if ( type != 3 )
                            coverageGlyphs(t, sub + t.u16(sub+2), cov); // the marks
                        HBCrossRule rule;
                        rule.right.push_back(HBRulePosition::set(cov));
                        _rules.push_back(rule);
                    }
                    break;
                case 7:
                case 8:
                    if ( format < 1 || format > 3 )
                        return false;
                    parseContext(t, sub, type, type == 8);
                    break;
                default:
                    return false;
                }
            }
            // Nested lookups of contexts
            if ( (!gpos && (type == 5 || type == 6)) || (gpos && (type == 7 || type == 8)) ) {
                bool chained = gpos ? type == 8 : type == 6;
                if ( format == 3 ) {
                    if ( chained ) {
                        unsigned int p = sub + 2;
                        p += 2 + 2*t.u16(p);
                        p += 2 + 2*t.u16(p);
                        p += 2 + 2*t.u16(p);
                        addNested(t, p + 2, t.u16(p), lookups, todo);
                    }
                    else {
                        unsigned int glyphCount = t.u16(sub+2), recCount = t.u16(sub+4);
                        addNested(t, sub + 6 + 2*glyphCount, recCount, lookups, todo);
                    }
                }
                else {
                    unsigned int setCount = t.u16(sub + (format == 1 ? 4 : (chained ? 10 : 6)));
                    unsigned int sets = sub + (format == 1 ? 6 : (chained ? 12 : 8));
                    for ( unsigned int i = 0; i < setCount; i++ ) {
                        unsigned int setOff = t.u16(sets + 2*i);
                        if ( !setOff ) continue;
                        unsigned int ruleSet = sub + setOff;
                        unsigned int ruleCount = t.u16(ruleSet);
                        for ( unsigned int r = 0; r < ruleCount; r++ ) {
                            unsigned int p = ruleSet + t.u16(ruleSet + 2 + 2*r);
                            if ( chained ) {
                                p += 2 + 2*t.u16(p);
                                unsigned int inputCount = t.u16(p);
                                p += 2 + 2*(inputCount ? inputCount-1 : 0);
                                p += 2 + 2*t.u16(p);
                                addNested(t, p + 2, t.u16(p), lookups, todo);
                            }
                            else {
                                unsigned int glyphCount = t.u16(p), recCount = t.u16(p+2);
                                addNested(t, p + 4 + 2*(glyphCount ? glyphCount-1 : 0), recCount, lookups, todo);
                            }
                        }
                    }
                }
            }
        }
    }
    return true;
}

// Merges rules beyond HBWC_MAX_RULES, and computes the positions to check
void HBShapingModel::finishRules()
{
    while ( _rules.size() > HBWC_MAX_RULES ) {
        // Merge the last rule into the one before: it matches if any of them matches
        // (positions required by both, each accepting what either accepts)
        HBCrossRule & a = _rules[_rules.size()-2];
        const HBCrossRule & b = _rules.back();
        for ( int side = 0; side < 2; side++ ) {
            std::vector<HBRulePosition> & pa = side ? a.right : a.left;
            const std::vector<HBRulePosition> & pb = side ? b.right : b.left;
            if ( pb.size() < pa.size() )
                pa.resize(pb.size());
            for ( size_t k = 0; k < pa.size(); k++ ) {
                if ( pb[k].any ) pa[k].any = true;
                pa[k].preds.insert(pa[k].preds.end(), pb[k].preds.begin(), pb[k].preds.end());
            }
        }
        _rules.pop_back();
    }
    for ( size_t r = 0; r < _rules.size(); r++ ) {
        HBCrossRule & rule = _rules[r];
        for ( int side = 0; side < 2; side++ ) {
            std::vector<HBRulePosition> & positions = side ? rule.right : rule.left;
            lUInt8 need = 0, any = 0;
            // Positions past one that may be a space may belong to another unit: they
            // are not checked (which is conservative)
            for ( size_t k = 0; k < positions.size() && k < HBWC_MAX_POSITIONS; k++ ) {
                need |= 1 << k;
                if ( positions[k].any )
                    any |= 1 << k;
                if ( admitsSpace(positions[k]) )
                    break;
            }
            if ( side ) { rule.needRight = need; rule.anyRight = any; }
            else { rule.needLeft = need; rule.anyLeft = any; }
        }
    }
    memset(_needLeft, 0, sizeof(_needLeft));
    memset(_needRight, 0, sizeof(_needRight));
    memset(_anyLeft, 0, sizeof(_anyLeft));
    memset(_anyRight, 0, sizeof(_anyRight));
    for ( size_t r = 0; r < _rules.size(); r++ ) {
        int shift = 8 * (r % 8);
        _needLeft[r / 8] |= (lUInt64)_rules[r].needLeft << shift;
        _needRight[r / 8] |= (lUInt64)_rules[r].needRight << shift;
        _anyLeft[r / 8] |= (lUInt64)_rules[r].anyLeft << shift;
        _anyRight[r / 8] |= (lUInt64)_rules[r].anyRight << shift;
    }
    if ( !_rules.empty() )
        _lowCharMasks.assign(HBWC_LOW_CHARS, -2);
}

// Computes a char's rule position masks: returns their index in _masks, or -1 if none
int HBShapingModel::computeCharMasks( lUInt32 ch )
{
    // The glyphs this char may become: from its own glyph, the glyphs of its
    // decomposition (HarfBuzz decomposes chars missing from the font), or the
    // space glyph (used for spaces missing from the font)
    std::vector<hb_codepoint_t> from;
    hb_unicode_funcs_t * ufuncs = hb_unicode_funcs_get_default();
    std::vector<hb_codepoint_t> chars(1, ch);
    for ( size_t i = 0; i < chars.size() && i < 16; i++ ) {
        hb_codepoint_t g, a, b;
        if ( hb_font_get_nominal_glyph(_font, chars[i], &g) )
            from.push_back(g);
        if ( hb_unicode_decompose(ufuncs, chars[i], &a, &b) ) {
            chars.push_back(a);
            if ( b ) chars.push_back(b);
        }
    }
    if ( hb_unicode_general_category(ufuncs, ch) == HB_UNICODE_GENERAL_CATEGORY_SPACE_SEPARATOR )
        from.insert(from.end(), _spaceGlyphs.begin(), _spaceGlyphs.end());
    closure(from, _scratch);
    lUInt64 left[HBWC_MASK_WORDS] = {0};
    lUInt64 right[HBWC_MASK_WORDS] = {0};
    bool found = false;
    for ( size_t r = 0; r < _rules.size(); r++ ) {
        const HBCrossRule & rule = _rules[r];
        lUInt64 l = 0, rt = 0;
        for ( size_t k = 0; k < rule.left.size() && k < HBWC_MAX_POSITIONS; k++ )
            if ( (rule.needLeft & (1 << k)) && rule.left[k].admits(_scratch) )
                l |= 1 << k;
        for ( size_t k = 0; k < rule.right.size() && k < HBWC_MAX_POSITIONS; k++ )
            if ( (rule.needRight & (1 << k)) && rule.right[k].admits(_scratch) )
                rt |= 1 << k;
        if ( l || rt ) {
            found = true;
            left[r / 8] |= l << (8 * (r % 8));
            right[r / 8] |= rt << (8 * (r % 8));
        }
    }
    if ( !found )
        return -1;
    int index = (int)_masks.size();
    _masks.insert(_masks.end(), left, left + HBWC_MASK_WORDS);
    _masks.insert(_masks.end(), right, right + HBWC_MASK_WORDS);
    return index;
}

static bool isVariationSelector( lUInt32 ch )
{
    return (ch >= 0xFE00 && ch <= 0xFE0F) || (ch >= 0xE0100 && ch <= 0xE01EF) || (ch >= 0x180B && ch <= 0x180D);
}

static bool isMark( hb_unicode_funcs_t * ufuncs, lUInt32 ch )
{
    if ( ch < 0x300 )
        return false;
    hb_unicode_general_category_t gc = hb_unicode_general_category(ufuncs, ch);
    return gc == HB_UNICODE_GENERAL_CATEGORY_NON_SPACING_MARK
        || gc == HB_UNICODE_GENERAL_CATEGORY_SPACING_MARK
        || gc == HB_UNICODE_GENERAL_CATEGORY_ENCLOSING_MARK;
}

// Rules whose required positions are all in acc
lUInt64 HBShapingModel::rulesSatisfied( const lUInt64 * acc, const lUInt64 * need ) const
{
    lUInt64 bits = 0;
    size_t nrules = _rules.size();
    for ( size_t w = 0; w * 8 < nrules; w++ ) {
        lUInt64 missing = (acc[w] & need[w]) ^ need[w];
        for ( size_t b = 0; b < 8 && w * 8 + b < nrules; b++ ) {
            if ( !((missing >> (8 * b)) & 0xFF) )
                bits |= (lUInt64)1 << (w * 8 + b);
        }
    }
    return bits;
}

void HBShapingModel::unitBits( const hb_codepoint_t * text, int len, bool endsWithBoundary, lUInt64 & endBits, lUInt64 & startBits )
{
    // Accumulate the rule positions each char could fill: left positions from
    // the chars before the space ending the unit, right positions from all
    lUInt64 accLeft[HBWC_MASK_WORDS], accRight[HBWC_MASK_WORDS];
    memcpy(accLeft, _anyLeft, sizeof(accLeft));
    memcpy(accRight, _anyRight, sizeof(accRight));
    hb_unicode_funcs_t * ufuncs = hb_unicode_funcs_get_default();
    int leftLen = endsWithBoundary ? len - 1 : 0;
    for ( int i = 0; i < len; i++ ) {
        lUInt32 ch = text[i];
        if ( isVariationSelector(ch) ) {
            endBits = startBits = ~(lUInt64)0;
            return;
        }
        for ( int pass = 0; pass < 2; pass++ ) {
            int index;
            if ( pass == 0 ) {
                index = charMaskIndex(ch);
            }
            else {
                // A mark may be composed with the previous char
                hb_codepoint_t composed;
                if ( i == 0 || !isMark(ufuncs, ch) || !hb_unicode_compose(ufuncs, text[i-1], ch, &composed) )
                    break;
                index = charMaskIndex(composed);
            }
            if ( index < 0 )
                continue;
            const lUInt64 * masks = &_masks[index];
            if ( i < leftLen )
                for ( int w = 0; w < HBWC_MASK_WORDS; w++ )
                    accLeft[w] |= masks[w];
            for ( int w = 0; w < HBWC_MASK_WORDS; w++ )
                accRight[w] |= masks[HBWC_MASK_WORDS + w];
        }
    }
    endBits = endsWithBoundary ? rulesSatisfied(accLeft, _needLeft) : 0;
    startBits = rulesSatisfied(accRight, _needRight);
}

// ===== The cache: global, 4-way set associative, about 2 MB

#define HBWC_SLOT_CHARS 16
#define HBWC_WAYS 4
#define HBWC_SETS 4096

struct HBWordCacheSlot {
    lUInt64 key;        // 0: empty
    lUInt32 age;
    lUInt8  len;
    lUInt8  reserved;
    lUInt16 glyphMask;  // bit i: char i starts a cluster
    lUInt16 anyAdvMask; // bit i: a glyph of that cluster has a non-zero 26.6 advance
    lInt16  advPx[HBWC_SLOT_CHARS];
    lInt32  adv64[HBWC_SLOT_CHARS];
};

static HBWordCacheSlot * hbwc_slots = NULL;
static lUInt32 hbwc_age = 0;

static HBWordCacheSlot * hbwcFind( lUInt64 key, int len )
{
    if ( !hbwc_slots )
        return NULL;
    HBWordCacheSlot * set = hbwc_slots + (size_t)((key ^ (key >> 32)) & (HBWC_SETS - 1)) * HBWC_WAYS;
    for ( int w = 0; w < HBWC_WAYS; w++ ) {
        if ( set[w].key == key && set[w].len == len ) {
            set[w].age = ++hbwc_age;
            return &set[w];
        }
    }
    return NULL;
}

static void hbwcStore( lUInt64 key, int len, const HBCharAdvance * adv )
{
    if ( len > HBWC_SLOT_CHARS )
        return;
    for ( int i = 0; i < len; i++ ) {
        if ( adv[i].advPx < -32768 || adv[i].advPx > 32767 )
            return;
    }
    if ( !hbwc_slots ) {
        hbwc_slots = (HBWordCacheSlot *)calloc((size_t)HBWC_SETS * HBWC_WAYS, sizeof(HBWordCacheSlot));
        if ( !hbwc_slots )
            return;
    }
    HBWordCacheSlot * set = hbwc_slots + (size_t)((key ^ (key >> 32)) & (HBWC_SETS - 1)) * HBWC_WAYS;
    HBWordCacheSlot * slot = &set[0];
    for ( int w = 0; w < HBWC_WAYS; w++ ) {
        if ( !set[w].key ) { slot = &set[w]; break; }
        if ( set[w].age < slot->age ) slot = &set[w];
    }
    slot->key = key;
    slot->age = ++hbwc_age;
    slot->len = (lUInt8)len;
    slot->glyphMask = 0;
    slot->anyAdvMask = 0;
    for ( int i = 0; i < len; i++ ) {
        if ( adv[i].hasGlyphs ) slot->glyphMask |= 1 << i;
        if ( adv[i].anyAdv ) slot->anyAdvMask |= 1 << i;
        slot->advPx[i] = (lInt16)adv[i].advPx;
        slot->adv64[i] = adv[i].adv64;
    }
    HBWC_STAT(stored);
}

static void hbwcLoad( const HBWordCacheSlot * slot, HBCharAdvance * adv )
{
    for ( int i = 0; i < slot->len; i++ ) {
        adv[i].hasGlyphs = (slot->glyphMask >> i) & 1;
        adv[i].anyAdv = (slot->anyAdvMask >> i) & 1;
        adv[i].advPx = slot->advPx[i];
        adv[i].adv64 = slot->adv64[i];
    }
}

static lUInt64 hbwcKey( const hb_codepoint_t * text, int len, lUInt64 modelId, unsigned int flags )
{
    lUInt64 h = 0xcbf29ce484222325ULL; // FNV-1a
    for ( int i = 0; i < len; i++ ) {
        h ^= text[i];
        h *= 0x100000001b3ULL;
    }
    h ^= (modelId * 0x9E3779B97F4A7C15ULL) ^ ((lUInt64)flags << 48) ^ (lUInt64)len;
    // splitmix64 finalizer
    h ^= h >> 30; h *= 0xbf58476d1ce4e5b9ULL;
    h ^= h >> 27; h *= 0x94d049bb133111ebULL;
    h ^= h >> 31;
    return h ? h : 1;
}

// ===== HBWordShaper

HBWordShaper::HBWordShaper() : _buffer(NULL)
{
}

HBWordShaper::~HBWordShaper()
{
    invalidate();
    if ( _buffer )
        hb_buffer_destroy(_buffer);
}

void HBWordShaper::invalidate()
{
    // Cache entries are keyed by model ids, so old ones just won't be found anymore
    for ( size_t i = 0; i < _models.size(); i++ )
        delete _models[i];
    _models.clear();
}

HBShapingModel * HBWordShaper::getModel( hb_font_t * font, const hb_feature_t * features, unsigned int num_features,
                                         const hb_segment_properties_t & props, bool legacyKerning )
{
    for ( size_t i = 0; i < _models.size(); i++ ) {
        if ( _models[i]->script == props.script && _models[i]->language == props.language )
            return _models[i]->usable ? _models[i] : NULL;
    }
    HBShapingModel * model = new HBShapingModel(font, features, num_features, props.script, props.language, legacyKerning);
    _models.push_back(model);
    #ifdef HB_WORD_CACHE_VERIFY
        if ( !model->usable ) HBWC_STAT(unusableModels);
    #endif
    return model->usable ? model : NULL;
}

// Can a unit start with this char (after spaces)? Not if HarfBuzz could make it
// part of the previous char's cluster or treat it specially.
static bool canStartUnit( hb_unicode_funcs_t * ufuncs, lUInt32 ch )
{
    if ( ch < 0x80 )
        return ch > 0x20 && ch < 0x7F;
    if ( (ch >= 0x1F3FB && ch <= 0x1F3FF) || (ch >= 0x1F1E6 && ch <= 0x1F1FF) )
        return false; // emoji modifiers, regional indicators
    switch ( hb_unicode_general_category(ufuncs, ch) ) {
        case HB_UNICODE_GENERAL_CATEGORY_UPPERCASE_LETTER:
        case HB_UNICODE_GENERAL_CATEGORY_LOWERCASE_LETTER:
        case HB_UNICODE_GENERAL_CATEGORY_TITLECASE_LETTER:
        case HB_UNICODE_GENERAL_CATEGORY_MODIFIER_LETTER:
        case HB_UNICODE_GENERAL_CATEGORY_OTHER_LETTER:
        case HB_UNICODE_GENERAL_CATEGORY_DECIMAL_NUMBER:
        case HB_UNICODE_GENERAL_CATEGORY_LETTER_NUMBER:
        case HB_UNICODE_GENERAL_CATEGORY_OTHER_NUMBER:
        case HB_UNICODE_GENERAL_CATEGORY_CONNECT_PUNCTUATION:
        case HB_UNICODE_GENERAL_CATEGORY_DASH_PUNCTUATION:
        case HB_UNICODE_GENERAL_CATEGORY_OPEN_PUNCTUATION:
        case HB_UNICODE_GENERAL_CATEGORY_CLOSE_PUNCTUATION:
        case HB_UNICODE_GENERAL_CATEGORY_INITIAL_PUNCTUATION:
        case HB_UNICODE_GENERAL_CATEGORY_FINAL_PUNCTUATION:
        case HB_UNICODE_GENERAL_CATEGORY_OTHER_PUNCTUATION:
        case HB_UNICODE_GENERAL_CATEGORY_MATH_SYMBOL:
        case HB_UNICODE_GENERAL_CATEGORY_CURRENCY_SYMBOL:
        case HB_UNICODE_GENERAL_CATEGORY_MODIFIER_SYMBOL:
        case HB_UNICODE_GENERAL_CATEGORY_OTHER_SYMBOL:
            return true;
        default:
            return false;
    }
}

// Shapes text[start, end) and puts its advances in out[start, end)
bool HBWordShaper::shapeStretch( hb_font_t * font, const hb_feature_t * features, unsigned int num_features,
                                 const hb_segment_properties_t & props, hb_buffer_flags_t flags,
                                 int start, int end, lInt32 synthAdvance, HBCharAdvance * out )
{
    if ( !_buffer )
        _buffer = hb_buffer_create();
    hb_buffer_clear_contents(_buffer);
    for ( int i = start; i < end; i++ )
        hb_buffer_add(_buffer, _text[i], i);
    hb_buffer_set_content_type(_buffer, HB_BUFFER_CONTENT_TYPE_UNICODE);
    hb_buffer_set_segment_properties(_buffer, &props);
    hb_buffer_set_flags(_buffer, flags);
    hb_shape(font, _buffer, features, num_features);
    for ( int i = start; i < end; i++ ) {
        out[i].adv64 = 0;
        out[i].advPx = 0;
        out[i].hasGlyphs = false;
        out[i].anyAdv = false;
    }
    unsigned int count;
    const hb_glyph_info_t * info = hb_buffer_get_glyph_infos(_buffer, &count);
    const hb_glyph_position_t * pos = hb_buffer_get_glyph_positions(_buffer, NULL);
    int prevCluster = start;
    for ( unsigned int g = 0; g < count; g++ ) {
        int cluster = (int)info[g].cluster;
        if ( info[g].codepoint == 0 ) // .notdef: fallback fonts needed
            return false;
        if ( cluster < prevCluster || cluster >= end )
            return false;
        prevCluster = cluster;
        HBCharAdvance & a = out[cluster];
        a.hasGlyphs = true;
        if ( pos[g].x_advance ) {
            lInt32 advance64 = pos[g].x_advance + synthAdvance;
            a.adv64 += advance64;
            a.advPx += HBWC_METRIC_TO_PX(advance64);
            if ( advance64 != 0 )
                a.anyAdv = true;
        }
    }
    return true;
}

bool HBWordShaper::getAdvances( hb_font_t * font, const hb_feature_t * features, unsigned int num_features,
                                hb_buffer_t * prepared, lInt32 synthAdvance, bool legacyKerning, HBCharAdvance * out )
{
    HBWC_STAT(runs);
    hb_segment_properties_t props;
    hb_buffer_get_segment_properties(prepared, &props);
    if ( props.direction != HB_DIRECTION_LTR || hb_buffer_get_content_type(prepared) != HB_BUFFER_CONTENT_TYPE_UNICODE ) {
        HBWC_STAT(runsRefused);
        return false;
    }
    // Only scripts handled by HarfBuzz's default shaper
    switch ( props.script ) {
        case HB_SCRIPT_LATIN:
        case HB_SCRIPT_GREEK:
        case HB_SCRIPT_CYRILLIC:
        case HB_SCRIPT_COMMON:
        case HB_SCRIPT_INHERITED:
        case HB_SCRIPT_UNKNOWN:
        case HB_SCRIPT_INVALID:
            break;
        default:
            HBWC_STAT(runsRefused);
            return false;
    }
    unsigned int ulen;
    const hb_glyph_info_t * info = hb_buffer_get_glyph_infos(prepared, &ulen);
    int len = (int)ulen;
    if ( len <= 0 ) {
        HBWC_STAT(runsRefused);
        return false;
    }
    HBShapingModel * model = getModel(font, features, num_features, props, legacyKerning);
    if ( !model ) {
        HBWC_STAT(runsRefused);
        return false;
    }

    // Split into units: words, each with its trailing spaces
    _text.resize(len);
    for ( int i = 0; i < len; i++ )
        _text[i] = info[i].codepoint;
    hb_unicode_funcs_t * ufuncs = hb_unicode_funcs_get_default();
    _unitStarts.clear();
    _unitStarts.push_back(0);
    for ( int i = 1; i < len; i++ ) {
        if ( _text[i-1] == ' ' && _text[i] != ' ' && canStartUnit(ufuncs, _text[i]) )
            _unitStarts.push_back(i);
    }
    int nunits = (int)_unitStarts.size();
    _unitStarts.push_back(len);

    // Group units into segments: units are joined when a font rule could
    // match across the boundary between them
    _segStarts.clear();
    _segStarts.push_back(0);
    if ( model->hasRules() ) {
        if ( !model->isEffective() ) {
            HBWC_STAT(runsRefused);
            return false;
        }
        _endBits.resize(nunits);
        _startBits.resize(nunits);
        for ( int u = 0; u < nunits; u++ ) {
            int s = _unitStarts[u], e = _unitStarts[u+1];
            // (the space ending a unit is not part of the rules' left side)
            model->unitBits(&_text[s], e - s, u < nunits - 1, _endBits[u], _startBits[u]);
        }
        for ( int u = 1; u < nunits; u++ ) {
            bool merged = (_endBits[u-1] & _startBits[u]) != 0;
            model->countBoundary(merged);
            if ( merged ) {
                HBWC_STAT(merges);
                continue;
            }
            _segStarts.push_back(_unitStarts[u]);
        }
    }
    else {
        for ( int u = 1; u < nunits; u++ )
            _segStarts.push_back(_unitStarts[u]);
    }
    int nsegs = (int)_segStarts.size();
    _segStarts.push_back(len);

    // Get segments from the cache
    unsigned int bufferFlags = hb_buffer_get_flags(prepared);
    unsigned int edgeFlags = HB_BUFFER_FLAG_BOT | HB_BUFFER_FLAG_EOT;
    unsigned int baseFlags = bufferFlags & ~edgeFlags;
    _segKeys.resize(nsegs);
    _segSlots.resize(nsegs);
    for ( int sg = 0; sg < nsegs; sg++ ) {
        int s = _segStarts[sg], e = _segStarts[sg+1];
        unsigned int flags = baseFlags;
        if ( s == 0 ) flags |= bufferFlags & HB_BUFFER_FLAG_BOT;
        if ( e == len ) flags |= bufferFlags & HB_BUFFER_FLAG_EOT;
        _segKeys[sg] = hbwcKey(&_text[s], e - s, model->id, flags);
        HBWordCacheSlot * slot = e - s <= HBWC_SLOT_CHARS ? hbwcFind(_segKeys[sg], e - s) : NULL;
        _segSlots[sg] = slot ? 1 : 0;
        if ( slot )
            hbwcLoad(slot, out + s);
        #ifdef HB_WORD_CACHE_VERIFY
            hbwc_stats.segments++;
            if ( slot ) hbwc_stats.hits++; else hbwc_stats.misses++;
        #endif
    }

    // Shape the missing ones: consecutive ones together (they are not sensitive
    // to each other, so this gives the same result as shaping them separately)
    for ( int sg = 0; sg < nsegs; ) {
        if ( _segSlots[sg] ) {
            sg++;
            continue;
        }
        int last = sg;
        while ( last + 1 < nsegs && !_segSlots[last+1] )
            last++;
        int s = _segStarts[sg], e = _segStarts[last+1];
        unsigned int flags = baseFlags;
        if ( s == 0 ) flags |= bufferFlags & HB_BUFFER_FLAG_BOT;
        if ( e == len ) flags |= bufferFlags & HB_BUFFER_FLAG_EOT;
        if ( !shapeStretch(font, features, num_features, props, (hb_buffer_flags_t)flags, s, e, synthAdvance, out) ) {
            HBWC_STAT(runsFailed);
            return false;
        }
        for ( int k = sg; k <= last; k++ ) {
            int ks = _segStarts[k], ke = _segStarts[k+1];
            if ( !out[ks].hasGlyphs ) { // a cluster across segments: should not happen
                HBWC_STAT(runsFailed);
                return false;
            }
            hbwcStore(_segKeys[k], ke - ks, out + ks);
        }
        sg = last + 1;
    }
    HBWC_STAT(runsCached);
    return true;
}

#endif // USE_HARFBUZZ==1
