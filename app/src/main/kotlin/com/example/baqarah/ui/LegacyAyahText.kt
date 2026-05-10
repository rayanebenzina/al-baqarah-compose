package com.example.baqarah.ui

import android.graphics.Typeface
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.SpanStyle
import androidx.compose.ui.text.buildAnnotatedString
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextDirection
import androidx.compose.ui.unit.TextUnit
import com.example.baqarah.data.Verse
import com.example.baqarah.data.Word

@Composable
fun LegacyAyahText(
    verse: Verse,
    typefaces: Map<Int, Typeface>,
    fontSizePx: Float,
    modifier: Modifier = Modifier,
) {
    val density = LocalDensity.current
    val fontSize: TextUnit = with(density) { fontSizePx.toSp() }
    val lineHeight: TextUnit = with(density) { (fontSizePx * 1.6f).toSp() }
    val families = remember(typefaces) {
        typefaces.mapValues { (_, tf) ->
            FontFamily(androidx.compose.ui.text.font.Typeface(tf))
        }
    }
    Text(
        text = buildLegacyText(verse.words, families),
        modifier = modifier.fillMaxWidth(),
        color = MaterialTheme.colorScheme.onBackground,
        fontSize = fontSize,
        lineHeight = lineHeight,
        textAlign = TextAlign.Center,
        style = MaterialTheme.typography.bodyLarge.copy(
            textDirection = TextDirection.Rtl,
        ),
    )
}

private fun buildLegacyText(
    words: List<Word>,
    families: Map<Int, FontFamily>,
): AnnotatedString = buildAnnotatedString {
    words.forEach { w ->
        val glyph = w.codeV2 ?: return@forEach
        val family = families[w.pageNumber] ?: FontFamily.Default
        val start = length
        append(glyph)
        addStyle(SpanStyle(fontFamily = family), start, length)
        if (w.charTypeName != "end") append(' ')
    }
}
