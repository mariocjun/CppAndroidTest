// MainActivity — the user-facing entry point. Loads libcppandroidtest.so and
// surfaces three actions via a programmatic UI (no XML layouts on purpose,
// keeps the Java surface tiny and predictable):
//
//   - Run benchmarks (the full default suite — stream, latency, neon_fma,
//     dot_int8, i8mm, sve2, perf_counters; ~15-20s on S24 Ultra)
//   - Enumerate sensors  (every ASensorManager-visible sensor with metadata)
//   - Enumerate cameras  (every Camera2 NDK camera ID with characteristics)
//
// Results are shown in a scrollable monospace TextView AND persisted as JSON
// to the app's external-files directory (getExternalFilesDir(null), which
// resolves to /storage/emulated/0/Android/data/com.example.cppandroidtest/files/
// — no storage permission needed, removed cleanly on uninstall).
package com.example.cppandroidtest

import android.os.Bundle
import android.text.method.ScrollingMovementMethod
import android.view.Gravity
import android.view.View
import android.view.ViewGroup.LayoutParams.MATCH_PARENT
import android.view.ViewGroup.LayoutParams.WRAP_CONTENT
import android.widget.Button
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import androidx.lifecycle.lifecycleScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

class MainActivity : AppCompatActivity() {

    companion object {
        init { System.loadLibrary("cppandroidtest") }
        private const val PADDING_DP = 12
    }

    // JNI surface — implemented in app/src/main/cpp/jni.cpp.
    // Each returns a JSON string. externalDir is where results.json is also
    // persisted as a side effect; pass null to skip the file write.
    private external fun nativeRunBenchmarks(externalDir: String?): String
    private external fun nativeEnumerateSensors(): String
    private external fun nativeEnumerateCameras(): String

    private lateinit var output: TextView
    private lateinit var runBtn: Button
    private lateinit var sensorsBtn: Button
    private lateinit var camerasBtn: Button

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(buildUi())
        output.text = "Tap a button to start. Default suite (~15-20s on a flagship) " +
                "covers STREAM bandwidth, pointer-chase latency, NEON FP32/FP16 FMA, " +
                "SDOT int8, SMMLA i8mm (ARMv8.6), SVE2 FP32 (ARMv9), PMU counters.\n\n" +
                "Results also save to: ${getExternalFilesDir(null)?.absolutePath ?: "(no external dir)"}"
    }

    private fun buildUi(): View {
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            val padPx = (PADDING_DP * resources.displayMetrics.density).toInt()
            setPadding(padPx, padPx, padPx, padPx)
        }

        runBtn = button("Run benchmarks") { triggerJob("benchmarks") }
        sensorsBtn = button("Enumerate sensors") { triggerJob("sensors") }
        camerasBtn = button("Enumerate cameras") { triggerJob("cameras") }

        root.addView(runBtn)
        root.addView(sensorsBtn)
        root.addView(camerasBtn)

        val scroll = ScrollView(this).apply {
            layoutParams = LinearLayout.LayoutParams(MATCH_PARENT, 0, 1f)
            isVerticalScrollBarEnabled = true
        }
        output = TextView(this).apply {
            textSize = 10f
            typeface = android.graphics.Typeface.MONOSPACE
            setHorizontallyScrolling(true)
            movementMethod = ScrollingMovementMethod()
        }
        scroll.addView(output)
        root.addView(scroll)
        return root
    }

    private fun button(label: String, onTap: () -> Unit): Button =
        Button(this).apply {
            text = label
            gravity = Gravity.CENTER
            layoutParams = LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT)
            setOnClickListener { onTap() }
        }

    private fun triggerJob(kind: String) {
        setBusy(true, "Running $kind…")
        lifecycleScope.launch(Dispatchers.IO) {
            val externalDir = getExternalFilesDir(null)?.absolutePath
            val result: String = try {
                when (kind) {
                    "benchmarks" -> nativeRunBenchmarks(externalDir)
                    "sensors"    -> nativeEnumerateSensors()
                    "cameras"    -> nativeEnumerateCameras()
                    else -> "{\"error\":\"unknown job: $kind\"}"
                }
            } catch (t: Throwable) {
                "{\"error\":\"${t.javaClass.simpleName}: ${t.message}\"}"
            }
            // Persist for offline inspection / sharing.
            if (externalDir != null) {
                val ts = SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(Date())
                File(externalDir, "$kind-$ts.json").writeText(result)
            }
            withContext(Dispatchers.Main) {
                output.text = result
                setBusy(false, null)
            }
        }
    }

    private fun setBusy(busy: Boolean, msg: String?) {
        runBtn.isEnabled = !busy
        sensorsBtn.isEnabled = !busy
        camerasBtn.isEnabled = !busy
        if (msg != null) output.text = msg
    }
}
