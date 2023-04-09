#include "config.h"
#include "FontCascade.h"
#include "FontCascadeDescription.h"
#include "NotImplemented.h"
#include "platform/graphics/BitmapImage.h"
#include "SurrogatePairAwareTextIterator.h"
#include "GraphicsContextUltralight.h"
#include "CharacterProperties.h"
#include "GlyphBuffer.h"
#include "FontCache.h"
#include "DrawGlyphsRecorder.h"
#include "FloatPoint.h"
#include "Font.h"
//#include "HarfBuzzShaper.h"
#include <Ultralight/Bitmap.h>
#include <Ultralight/private/Canvas.h>
#include <Ultralight/private/tracy/Tracy.hpp>
#include <Ultralight/platform/Platform.h>
#include <Ultralight/platform/Config.h>
#include <unicode/normlzr.h>
#include <math.h>
#include <memory>
#include "ft2build.h"
#include FT_FREETYPE_H
#include FT_OUTLINE_H

/*
static int TwentySixDotSix2Pixel(const int i)
{
  return (i >> 6) + (32 < (i & 63));
}
*/

inline float TwentySixDotSix2Float(const int i)
{
  return i / 64.0f;
}

namespace WebCore {

bool FontCascade::canReturnFallbackFontsForComplexText()
{
  return true;
}

bool FontCascade::canExpandAroundIdeographsInComplexText()
{
  return false;
}

void FontCascade::drawGlyphs(GraphicsContext& context, const Font& font, const GlyphBufferGlyph* glyphs,
  const GlyphBufferAdvance* advances, unsigned numGlyphs, const FloatPoint& point, FontSmoothingMode smoothing)
{
  ProfiledZone;
  WebCore::FontPlatformData& platform_font = const_cast<WebCore::FontPlatformData&>(font.platformData());
  auto canvas = context.platformContext();
  platform_font.font()->set_device_scale_hint((float)canvas->DeviceScaleHint());
  FT_Face face = platform_font.face();
  FT_GlyphSlot slot = face->glyph;
  FT_Bool use_kerning = FT_HAS_KERNING(face);
  FT_UInt glyph_index = 0;
  FT_UInt previous = 0;
  ultralight::Glyph glyph;
  ultralight::Point origin(point.x(), point.y());
  ultralight::RefPtr<ultralight::Font> ultraFont = platform_font.font();

  Vector<ultralight::Glyph>& glyphBuf = platform_font.glyphBuffer();
  glyphBuf.reserveCapacity(numGlyphs);

  for (unsigned i = 0; i < numGlyphs; i++) {
    glyph_index = glyphs[i];
    glyph.index = glyph_index;
    glyph.advance_x = advances[i].width();

    if (use_kerning && previous && glyph_index) {
      FT_Vector delta;
      FT_Get_Kerning(face, previous, glyph_index, FT_KERNING_DEFAULT, &delta);
      glyph.kerning_x = TwentySixDotSix2Float(delta.x);
    } else {
      glyph.kerning_x = 0.0f;
    }
    
    glyphBuf.append(glyph);
    previous = glyph_index;
  }

  if (glyphBuf.size()) {
    if (context.hasVisibleShadow()) {

      // TODO: Handle text shadows properly with offscreen blur filter
      // We are just drawing text shadows ignoring blur right now.

      WebCore::FloatSize shadow_size;
      float shadow_blur;
      WebCore::Color shadow_color;
      context.getShadow(shadow_size, shadow_blur, shadow_color);

      ultralight::Point shadow_offset = { shadow_size.width(), shadow_size.height() };
      canvas->DrawGlyphs(ultraFont, shadow_color, origin, glyphBuf.data(), glyphBuf.size(), shadow_offset);
    }

    canvas->DrawGlyphs(ultraFont, context.fillColor(), origin, glyphBuf.data(), glyphBuf.size(), ultralight::Point(0.0f, 0.0f));
    glyphBuf.resize(0);
  }
}

// From platform/graphics/cairo/FontCairoHarfbuzzNG.cpp
static bool characterSequenceIsEmoji(SurrogatePairAwareTextIterator& iterator, UChar32 firstCharacter, unsigned firstClusterLength)
{
	UChar32 character = firstCharacter;
	unsigned clusterLength = firstClusterLength;
	if (!iterator.consume(character, clusterLength))
		return false;

	if (isEmojiKeycapBase(character)) {
		iterator.advance(clusterLength);
		UChar32 nextCharacter;
		if (!iterator.consume(nextCharacter, clusterLength))
			return false;

		if (nextCharacter == combiningEnclosingKeycap)
			return true;

		// Variation selector 16.
		if (nextCharacter == 0xFE0F) {
			iterator.advance(clusterLength);
			if (!iterator.consume(nextCharacter, clusterLength))
				return false;

			if (nextCharacter == combiningEnclosingKeycap)
				return true;
		}

		return false;
	}

	// Regional indicator.
	if (isEmojiRegionalIndicator(character)) {
		iterator.advance(clusterLength);
		UChar32 nextCharacter;
		if (!iterator.consume(nextCharacter, clusterLength))
			return false;

		if (isEmojiRegionalIndicator(nextCharacter))
			return true;

		return false;
	}

	if (character == combiningEnclosingKeycap)
		return true;

	if (isEmojiWithPresentationByDefault(character)
		|| isEmojiModifierBase(character)
		|| isEmojiFitzpatrickModifier(character))
		return true;

	return false;
}

// From platform/graphics/cairo/FontCairoHarfbuzzNG.cpp
const Font* FontCascade::fontForCombiningCharacterSequence(const UChar* originalCharacters, size_t originalLength) const
{
    ProfiledZone;
	auto normalizedString = normalizedNFC(StringView{ originalCharacters, static_cast<unsigned>(originalLength) });

	// Code below relies on normalizedNFC never narrowing a 16-bit input string into an 8-bit output string.
	// At the time of this writing, the function never does this, but in theory a future version could, and
	// we would then need to add code paths here for the simpler 8-bit case.
	auto characters = normalizedString.view.characters16();
	auto length = normalizedString.view.length();

	UChar32 character;
	unsigned clusterLength = 0;
	SurrogatePairAwareTextIterator iterator(characters, 0, length, length);
	if (!iterator.consume(character, clusterLength))
		return nullptr;

	bool isEmoji = characterSequenceIsEmoji(iterator, character, clusterLength);
	bool preferColoredFont = isEmoji;
	// U+FE0E forces text style.
	// U+FE0F forces emoji style.
	if (characters[length - 1] == 0xFE0E)
		preferColoredFont = false;
	else if (characters[length - 1] == 0xFE0F)
		preferColoredFont = true;

	const Font* baseFont = glyphDataForCharacter(character, false, NormalVariant).font;
	if (baseFont
		&& (clusterLength == length || baseFont->canRenderCombiningCharacterSequence(characters, length))
		&& (!preferColoredFont || baseFont->platformData().isColorBitmapFont()))
		return baseFont;

	for (unsigned i = 0; !fallbackRangesAt(i).isNull(); ++i) {
		const Font* fallbackFont = fallbackRangesAt(i).fontForCharacter(character);
		if (!fallbackFont || fallbackFont == baseFont)
			continue;

		if (fallbackFont->canRenderCombiningCharacterSequence(characters, length) && (!preferColoredFont || fallbackFont->platformData().isColorBitmapFont()))
			return fallbackFont;
	}

	if (auto systemFallback = FontCache::forCurrentThread().systemFallbackForCharacters(m_fontDescription, *baseFont, IsForPlatformFont::No, preferColoredFont ? FontCache::PreferColoredFont::Yes : FontCache::PreferColoredFont::No, characters, length)) {
		if (systemFallback->canRenderCombiningCharacterSequence(characters, length) && (!preferColoredFont || systemFallback->platformData().isColorBitmapFont()))
			return systemFallback.get();

		// In case of emoji, if fallback font is colored try again without the variation selector character.
		if (isEmoji && characters[length - 1] == 0xFE0F && systemFallback->platformData().isColorBitmapFont() && systemFallback->canRenderCombiningCharacterSequence(characters, length - 1))
			return systemFallback.get();
	}

	return baseFont;
}

unsigned FontCascadeDescription::effectiveFamilyCount() const
{
  return familyCount();
}

FontFamilySpecification FontCascadeDescription::effectiveFamilyAt(unsigned index) const
{
  return familyAt(index);
}

void Dump_SizeMetrics(const FT_Size_Metrics* metrics)
{
  printf("FT_Size_Metrics (scaled)\n");
  printf("\tx_ppem      = %d\n", metrics->x_ppem);
  printf("\ty_ppem      = %d\n", metrics->y_ppem);
  printf("\tx_scale     = %d\n", metrics->x_scale);
  printf("\ty_scale     = %d\n", metrics->y_scale);
  printf("\tascender    = %d (26.6 frac. pixel)\n", metrics->ascender);
  printf("\tdescender   = %d (26.6 frac. pixel)\n", metrics->descender);
  printf("\theight      = %d (26.6 frac. pixel)\n", metrics->height);
  //	printf("\tmax_advance = %d (26.6 frac. pixel)\n",	metrics->max_advance);
  printf("\n");
}



void Font::platformInit()
{
  ProfiledZone;
  // TODO, handle complex fonts. We force the code path to simple here because Harfbuzz isn't
  // returning proper widths/shape for complex fonts (tested on Stripe).
  //FontCascade::setCodePath(FontCascade::Complex);

  float fontSize = m_platformData.size();
  FT_Face face = m_platformData.face();
  const FT_Size_Metrics& metrics = face->size->metrics;
  //Dump_SizeMetrics(&metrics);

  FT_Select_Charmap(face, ft_encoding_unicode);

  float ascent = TwentySixDotSix2Float(metrics.ascender);
  float descent = TwentySixDotSix2Float(-metrics.descender);
  float capHeight = TwentySixDotSix2Float(metrics.height);
  float lineGap = capHeight - ascent - descent;

  m_fontMetrics.setAscent(ascent);
  m_fontMetrics.setDescent(descent);
  m_fontMetrics.setCapHeight(capHeight);
  m_fontMetrics.setLineSpacing(ascent + descent + lineGap);
  m_fontMetrics.setLineGap(lineGap);

  m_fontMetrics.setUnitsPerEm(face->units_per_EM);

  FT_Error error;
  error = FT_Load_Char(face, (FT_ULong)'x', FT_LOAD_DEFAULT);
  assert(error == 0);
  float xHeight = TwentySixDotSix2Float(face->glyph->metrics.height);
  m_fontMetrics.setXHeight(xHeight);

  error = FT_Load_Char(face, (FT_ULong)' ', FT_LOAD_DEFAULT);
  assert(error == 0);
  m_spaceWidth = TwentySixDotSix2Float(face->glyph->metrics.horiAdvance);

  /*
  printf("From FaceMetrics\n");
  printf("\tascender     = %d\t(from FT_Face)\n", TwentySixDotSix2Pixel(FT_MulFix(face->ascender, face->size->metrics.y_scale)));
  printf("\tdescender    = %d\t(from FT_Face)\n", TwentySixDotSix2Pixel(FT_MulFix(-face->descender, face->size->metrics.y_scale)));
  printf("\theight       = %d\t(from FT_Face)\n", TwentySixDotSix2Pixel(FT_MulFix(face->height, face->size->metrics.y_scale)));
  printf("\tMaxAscender  = %d\t(from bbox.yMax)\n", TwentySixDotSix2Pixel(FT_MulFix(face->bbox.yMax, face->size->metrics.y_scale)));
  printf("\tMinDescender = %d\t(from bbox.yMin)\n", TwentySixDotSix2Pixel(FT_MulFix(-face->bbox.yMin, face->size->metrics.y_scale)));
  printf("\n");
  printf("From SizeMetrics\n");
  printf("\tascender     = %d\t(from FT_Size_Metrics)\n", TwentySixDotSix2Pixel(face->size->metrics.ascender));
  printf("\tdescender    = %d\t(from FT_Size_Metrics)\n", TwentySixDotSix2Pixel(-face->size->metrics.descender));
  printf("\theight       = %d\t(from FT_Size_Metrics)\n", TwentySixDotSix2Pixel(face->size->metrics.height));
  printf("\n");
  */

  // TODO
  //notImplemented();
}

float Font::platformWidthForGlyph(Glyph glyph) const
{
  auto& platformData = const_cast<WebCore::FontPlatformData&>(m_platformData);
  return platformData.glyphWidth(glyph);
}

FloatRect Font::platformBoundsForGlyph(Glyph glyph) const
{
  auto& platformData = const_cast<WebCore::FontPlatformData&>(m_platformData);
  return platformData.glyphExtents(glyph);
}

void Font::platformCharWidthInit()
{
  m_avgCharWidth = 0.f;
  m_maxCharWidth = 0.f;
  initCharWidths();
}

void Font::determinePitch()
{
  m_treatAsFixedPitch = m_platformData.isFixedPitch();
}

RefPtr<Font> Font::platformCreateScaledFont(const FontDescription& fontDescription, float scaleFactor) const
{
  // TODO
  notImplemented();
  return nullptr;
}

Path Font::platformPathForGlyph(Glyph glyph) const
{
  ProfiledZone;
  ProfiledMemoryZone(MemoryTag::Font);
  auto& platformData = const_cast<WebCore::FontPlatformData&>(m_platformData); 

  Path result;
  ultralight::RefPtr<ultralight::Path> platformPath;
  platformData.font()->GetGlyphPath(glyph, platformPath);
  if (platformPath)
      result.ensurePlatformPath()->Set(platformPath);

  return result;
}

bool Font::platformSupportsCodePoint(UChar32 character, std::optional<UChar32> variation) const
{
  if (FT_Face face = m_platformData.face())
      return variation ? !!FT_Face_GetCharVariantIndex(face, character, variation.value()) : !!FT_Get_Char_Index(face, character);

  return false;
}

DrawGlyphsRecorder::DrawGlyphsRecorder(GraphicsContext& owner, float, DeriveFontFromContext deriveFontFromContext)
    : m_owner(owner)
    , m_deriveFontFromContext(deriveFontFromContext)
{
}

void DrawGlyphsRecorder::drawGlyphs(const Font& font, const GlyphBufferGlyph* glyphs, const GlyphBufferAdvance* advances, unsigned numGlyphs, const FloatPoint& startPoint, FontSmoothingMode smoothingMode)
{
  m_owner.drawGlyphsAndCacheResources(font, glyphs, advances, numGlyphs, startPoint, smoothingMode);
}

} // namespace WebCore
