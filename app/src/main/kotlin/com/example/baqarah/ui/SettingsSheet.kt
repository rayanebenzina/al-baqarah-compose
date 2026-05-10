package com.example.baqarah.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.Text
import androidx.compose.material3.rememberModalBottomSheetState
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.example.baqarah.data.SettingsRepository

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun SettingsSheet(
    currentFontSize: Int,
    onFontSizeChange: (Int) -> Unit,
    onDismiss: () -> Unit,
) {
    val sheetState = rememberModalBottomSheetState(skipPartiallyExpanded = true)
    ModalBottomSheet(
        onDismissRequest = onDismiss,
        sheetState = sheetState,
        containerColor = MaterialTheme.colorScheme.surface,
    ) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 24.dp, vertical = 8.dp),
            verticalArrangement = Arrangement.spacedBy(16.dp),
        ) {
            Text(
                text = "Font size",
                fontSize = 18.sp,
                fontWeight = FontWeight.Medium,
                color = MaterialTheme.colorScheme.onSurface,
            )
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(12.dp),
            ) {
                SettingsRepository.PRESETS.forEach { (label, sizePx) ->
                    PresetTile(
                        label = label,
                        sizePx = sizePx,
                        selected = sizePx == currentFontSize,
                        onClick = {
                            onFontSizeChange(sizePx)
                            onDismiss()
                        },
                        modifier = Modifier
                            .weight(1f)
                            .height(72.dp),
                    )
                }
            }
        }
    }
}

@Composable
private fun PresetTile(
    label: String,
    sizePx: Int,
    selected: Boolean,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
) {
    val container = if (selected) MaterialTheme.colorScheme.primary
        else MaterialTheme.colorScheme.onSurface.copy(alpha = 0.06f)
    val content = if (selected) MaterialTheme.colorScheme.onPrimary
        else MaterialTheme.colorScheme.onSurface
    Column(
        modifier = modifier
            .clip(RoundedCornerShape(12.dp))
            .background(container)
            .pointerInput(selected) { detectTapGestures { onClick() } }
            .padding(8.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.Center,
    ) {
        Text(label, color = content, fontWeight = FontWeight.SemiBold, fontSize = 16.sp)
        Text("${sizePx}px", color = content.copy(alpha = 0.7f), fontSize = 11.sp)
    }
}
