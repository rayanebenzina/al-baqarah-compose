package com.example.baqarah.ui

import android.graphics.Typeface
import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.gestures.detectVerticalDragGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyListState
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.Settings
import androidx.compose.material3.Button
import androidx.compose.material3.CenterAlignedTopAppBar
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.lifecycle.viewmodel.compose.viewModel
import com.example.baqarah.R
import com.example.baqarah.data.GlyphAtlas
import com.example.baqarah.data.LayoutPlan
import com.example.baqarah.data.Verse
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.launch

private const val USE_V5 = true

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun BaqarahScreen(
    viewModel: BaqarahViewModel = viewModel(factory = BaqarahViewModel.Factory),
) {
    val state by viewModel.state.collectAsStateWithLifecycle()
    val fontSize by viewModel.fontSize.collectAsStateWithLifecycle()
    var showSettings by remember { mutableStateOf(false) }

    Scaffold(
        modifier = Modifier.fillMaxSize(),
        topBar = {
            CenterAlignedTopAppBar(
                title = { Text(stringResource(R.string.title)) },
                actions = {
                    IconButton(onClick = { showSettings = true }) {
                        Icon(Icons.Outlined.Settings, contentDescription = "Settings")
                    }
                },
            )
        },
    ) { padding ->
        Box(
            modifier = Modifier
                .fillMaxSize()
                .padding(padding)
                .background(MaterialTheme.colorScheme.background),
            contentAlignment = Alignment.Center,
        ) {
            when (val s = state) {
                is BaqarahUiState.Loading -> CircularProgressIndicator()
                is BaqarahUiState.Error -> Column(
                    horizontalAlignment = Alignment.CenterHorizontally,
                    verticalArrangement = Arrangement.spacedBy(12.dp),
                ) {
                    Text(s.message, color = MaterialTheme.colorScheme.error)
                    Button(onClick = { viewModel.reload() }) { Text("Retry") }
                }
                is BaqarahUiState.Ready -> VerseList(
                    verseIds = s.verseIds,
                    verses = s.verses,
                    typefaces = s.typefaces,
                    atlas = s.atlas,
                    fontSizePx = s.fontSizePx,
                    prebuiltPlans = s.prebuiltPlans,
                    prebuiltWidthPx = s.prebuiltWidthPx,
                )
            }
        }
    }

    if (showSettings) {
        SettingsSheet(
            currentFontSize = fontSize,
            onFontSizeChange = { viewModel.setFontSize(it) },
            onDismiss = { showSettings = false },
        )
    }
}

@Composable
private fun VerseList(
    verseIds: List<Int>,
    verses: List<Verse>,
    typefaces: Map<Int, Typeface>,
    atlas: GlyphAtlas,
    fontSizePx: Float,
    prebuiltPlans: Map<Int, LayoutPlan>,
    prebuiltWidthPx: Int,
) {
    val listState = rememberLazyListState()
    val scope = rememberCoroutineScope()
    val versesById = remember(verses) { verses.associateBy { it.id } }

    Row(modifier = Modifier.fillMaxSize()) {
        LeftScrollbar(
            state = listState,
            totalCount = verseIds.size,
            scope = scope,
            modifier = Modifier
                .width(22.dp)
                .fillMaxHeight(),
        )
        LazyColumn(
            state = listState,
            modifier = Modifier
                .weight(1f)
                .fillMaxHeight(),
            contentPadding = PaddingValues(start = 8.dp, end = 20.dp, top = 16.dp, bottom = 16.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            items(verseIds, key = { it }) { verseId ->
                if (USE_V5) {
                    V5AyahText(
                        verseId = verseId,
                        atlas = atlas,
                        fontSizePx = fontSizePx,
                        prebuiltPlans = prebuiltPlans,
                        prebuiltWidthPx = prebuiltWidthPx,
                        modifier = Modifier
                            .fillMaxWidth()
                            .padding(vertical = 8.dp),
                    )
                } else {
                    versesById[verseId]?.let { verse ->
                        LegacyAyahText(
                            verse = verse,
                            typefaces = typefaces,
                            fontSizePx = fontSizePx,
                            modifier = Modifier
                                .fillMaxWidth()
                                .padding(vertical = 8.dp),
                        )
                    }
                }
                HorizontalDivider(color = MaterialTheme.colorScheme.onBackground.copy(alpha = 0.08f))
            }
        }
    }
}

@Composable
private fun LeftScrollbar(
    state: LazyListState,
    totalCount: Int,
    scope: CoroutineScope,
    modifier: Modifier = Modifier,
) {
    val trackColor = MaterialTheme.colorScheme.onBackground.copy(alpha = 0.06f)
    val thumbColor = MaterialTheme.colorScheme.primary.copy(alpha = 0.6f)

    fun jumpTo(yPx: Float, heightPx: Float) {
        if (totalCount == 0 || heightPx <= 0f) return
        val fraction = (yPx / heightPx).coerceIn(0f, 1f)
        val target = (fraction * (totalCount - 1)).toInt().coerceIn(0, totalCount - 1)
        scope.launch { state.scrollToItem(target) }
    }

    BoxWithConstraints(
        modifier = modifier
            .background(trackColor)
            .pointerInput(totalCount) {
                detectTapGestures { offset -> jumpTo(offset.y, size.height.toFloat()) }
            }
            .pointerInput(totalCount) {
                detectVerticalDragGestures(
                    onDragStart = { offset -> jumpTo(offset.y, size.height.toFloat()) },
                    onVerticalDrag = { change, _ ->
                        change.consume()
                        jumpTo(change.position.y, size.height.toFloat())
                    },
                )
            },
    ) {
        val trackHeight = maxHeight
        val visibleCount = state.layoutInfo.visibleItemsInfo.size.coerceAtLeast(1)
        val visibleFraction = (visibleCount.toFloat() / totalCount.coerceAtLeast(1)).coerceIn(0.04f, 1f)
        val thumbHeight = trackHeight * visibleFraction
        val progress = if (totalCount > 1)
            state.firstVisibleItemIndex.toFloat() / (totalCount - 1)
        else 0f
        val thumbOffset = (trackHeight - thumbHeight) * progress.coerceIn(0f, 1f)

        Box(
            modifier = Modifier
                .align(Alignment.TopCenter)
                .offset(y = thumbOffset)
                .width(8.dp)
                .height(thumbHeight)
                .background(thumbColor, RoundedCornerShape(4.dp)),
        )
    }
}
