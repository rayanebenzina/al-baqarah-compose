package com.example.baqarah.vk

import android.app.Activity
import android.os.Bundle
import android.view.WindowManager

class VulkanDebugActivity : Activity() {

    private var view: VulkanCanvasView? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        val v = VulkanCanvasView(this)
        view = v
        setContentView(v)
    }

    override fun onDestroy() {
        view?.release()
        view = null
        super.onDestroy()
    }
}
