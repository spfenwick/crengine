/** \file knuthparagraph.h
    \brief Knuth-Plass paragraph justification: what LVFormatter
    (lvtextfm.cpp) calls to plan a paragraph's line breaks and to
    justify the lines.
*/

#ifndef __KNUTH_PARAGRAPH_H_INCLUDED__
#define __KNUTH_PARAGRAPH_H_INCLUDED__

#include <vector>
#include "lvtextfm.h"
#include "knuthjustify.h"

/// A measured paragraph, as LVFormatter holds it.
struct KnuthParagraph {
    const lChar32 * text;
    const lUInt16 * flags;  // LCHAR_*
    const int * widths;     // cumulative
    src_text_fragment_t * const * srcs;
    const lUInt16 * charindex;
    lUInt16 pad_char_index; // charindex marking inline padding (an object with a width)
    int length;
    const KnuthJustifyConfig * config;
    bool harfbuzz;          // kerning mode: only HarfBuzz drawing can track
    bool hanging_punctuation;
    lUInt32 para_flags;     // LTEXT_* flags of the paragraph's first source
    // Line geometry, only needed by knuthBreakParagraph()
    int line_width;
    int first_indent;
    int following_indent;
    int usable_left_overflow;
    int usable_right_overflow;
};

/// One planned line break.
struct OptimalLineBreak {
    int pos;
    bool hyphenated;
    OptimalLineBreak(int p, bool h) : pos(p), hyphenated(h) {}
};

/// false if some char of the paragraph rules out planning (objects
/// other than inline padding, preformatted or nowrap text, glyphs a
/// HarfBuzz font lacks...)
bool knuthCanBreakParagraph( const KnuthParagraph & para );

/// Find the paragraph's best line breaks. false if no layout fits,
/// to break the paragraph greedily.
bool knuthBreakParagraph( const KnuthParagraph & para, std::vector<OptimalLineBreak> & breaks );

/// Whether a line break after pos makes the next line start with a copy
/// of that real hyphen (same conditions as the greedy line breaking).
bool knuthDuplicatesHyphenAfter( const KnuthParagraph & para, int pos );

/// Justify a line of a planned paragraph, adding extra_width (negative to
/// condense) with one common word space width and line-wide tracking.
void knuthJustifyLine( formatted_line_t * frmline, int extra_width,
                       const formatted_text_fragment_t * pbuffer, bool harfbuzz );

#endif
