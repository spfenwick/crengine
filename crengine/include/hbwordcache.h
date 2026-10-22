/** \file hbwordcache.h
    \brief Word-level cache of HarfBuzz shaping results, for text measuring

    Shaping is the main cost of measuring text with HarfBuzz, and most of
    a book's words are repeated many times. This splits text runs into
    words (each with its trailing spaces), and caches the glyph advances
    HarfBuzz gives to each of them.

    Shaping words separately must give the same result as shaping the whole
    run, which is not the case when some font rules look across a space
    (eg. kerning between a space and the next word's first letter, or
    contextual substitutions). Each font is analysed for such rules, and a
    word boundary where they could apply is not split: the words around it
    are shaped (and cached) together.

    CoolReader Engine, (c) Vadim Lopatin, 2000-2006
    This source code is distributed under the terms of
    GNU General Public License.
    See LICENSE file for details.
*/

#ifndef __HBWORDCACHE_H_INCLUDED__
#define __HBWORDCACHE_H_INCLUDED__

#include "crsetup.h"
#include "lvtypes.h"

#if USE_HARFBUZZ==1

#include <hb.h>
#include <vector>

// Define to compare each cached measurement with a full shaping of
// the text, and report statistics and mismatches on stderr.
// #define HB_WORD_CACHE_VERIFY

/// Advances HarfBuzz gave to a char: all the glyphs of a cluster are put on its first char
struct HBCharAdvance {
    lInt32 adv64;      ///< sum of the 26.6 advances of the cluster glyphs (including synthetic weight)
    lInt32 advPx;      ///< sum of these advances, each rounded to pixels
    bool   hasGlyphs;  ///< this char starts a cluster
    bool   anyAdv;     ///< one of these glyphs has a non-zero 26.6 advance
};

class HBShapingModel;

/// Word-level shaping cache front-end for one font instance
class HBWordShaper {
    // One model per script and language met with this font
    std::vector<HBShapingModel *> _models;
    hb_buffer_t * _buffer;
    // Scratch arrays, reused between calls
    std::vector<hb_codepoint_t> _text;
    std::vector<int> _unitStarts;
    std::vector<lUInt64> _endBits;
    std::vector<lUInt64> _startBits;
    std::vector<int> _segStarts;
    std::vector<lUInt64> _segKeys;
    std::vector<int> _segSlots;
    HBShapingModel * getModel( hb_font_t * font, const hb_feature_t * features, unsigned int num_features,
                               const hb_segment_properties_t & props, bool legacyKerning );
    bool shapeStretch( hb_font_t * font, const hb_feature_t * features, unsigned int num_features,
                       const hb_segment_properties_t & props, hb_buffer_flags_t flags,
                       int start, int end, lInt32 synthAdvance, HBCharAdvance * out );
public:
    HBWordShaper();
    ~HBWordShaper();
    /// Forget everything about the font: to be called when anything changing its shaping changes
    void invalidate();
    /** \brief Get the advances HarfBuzz gives to the text in prepared, using the cache
        \param prepared is a buffer filled with the text, with its segment properties set
        \param synthAdvance is added to each non-zero glyph advance (synthetic weight)
        \param legacyKerning is true if FreeType may provide kerning without a GPOS table
        \param out receives one entry per char
        \return false if the cache can't be used for this text (the caller must then shape it)
    */
    bool getAdvances( hb_font_t * font, const hb_feature_t * features, unsigned int num_features,
                      hb_buffer_t * prepared, lInt32 synthAdvance, bool legacyKerning, HBCharAdvance * out );
};

#endif // USE_HARFBUZZ==1

#endif // __HBWORDCACHE_H_INCLUDED__
