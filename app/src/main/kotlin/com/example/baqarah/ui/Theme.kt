package com.example.baqarah.ui

import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color

private val PaperColors = lightColorScheme(
    primary = Color(0xFF0E7C66),
    background = Color(0xFFFBF7EC),
    surface = Color(0xFFFBF7EC),
    onBackground = Color(0xFF111111),
    onSurface = Color(0xFF111111),
)

@Composable
fun BaqarahTheme(content: @Composable () -> Unit) {
    MaterialTheme(colorScheme = PaperColors, content = content)
}
