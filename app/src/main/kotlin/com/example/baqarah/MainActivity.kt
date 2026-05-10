package com.example.baqarah

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import com.example.baqarah.ui.BaqarahTheme
import com.example.baqarah.ui.QuranScreen

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        setContent {
            BaqarahTheme {
                QuranScreen()
            }
        }
    }
}
