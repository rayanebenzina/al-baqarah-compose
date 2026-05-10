package com.example.baqarah.vk

import android.view.Surface

class NativeRenderer {

    private var handle: Long = 0L

    init {
        System.loadLibrary("baqarah_vk")
        handle = nCreate()
    }

    fun attachSurface(surface: Surface): Boolean = nAttachSurface(handle, surface)

    fun detachSurface() = nDetachSurface(handle)

    fun drawFrame(): Boolean = nDrawFrame(handle)

    fun release() {
        if (handle != 0L) {
            nDestroy(handle)
            handle = 0L
        }
    }

    private external fun nCreate(): Long
    private external fun nDestroy(handle: Long)
    private external fun nAttachSurface(handle: Long, surface: Surface): Boolean
    private external fun nDetachSurface(handle: Long)
    private external fun nDrawFrame(handle: Long): Boolean
}
