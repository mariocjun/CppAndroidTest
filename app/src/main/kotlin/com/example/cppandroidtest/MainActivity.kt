// MainActivity — user-facing entry. Loads libcppandroidtest.so, drives the
// JNI bridge that returns benchmark / sensor / camera / hwcap JSON, and
// handles crash-dump propagation.
//
// UI layout:
//   filter row:  [ filter text input ] [ Run ]
//   info row:    [ HW Capabilities ] [ Sensors ] [ Cameras ]
//   action row:  [ Upload last result ]
//   output:      scrollable monospace TextView (selectable)
//
// 'Run' uses the filter input — empty = default benchmark suite (everything
// except opt-in: i8mm, sve2, sustained). Type 'stream' / 'latency' /
// 'neon_fma' / 'dot_int8' / 'i8mm' / 'sve2' / 'sustained' to run just one.
// 'sustained,perf_counters' for a comma list.
//
// HW Capabilities shows kernel-permitted ARMv8/ARMv9 extensions
// (getauxval(AT_HWCAP) view). Useful BEFORE running an extension bench to
// know whether it'll SIGILL on this kernel.

package com.example.cppandroidtest

import android.os.Bundle
import android.text.method.ScrollingMovementMethod
import android.view.Gravity
import android.view.View
import android.view.ViewGroup.LayoutParams.MATCH_PARENT
import android.view.ViewGroup.LayoutParams.WRAP_CONTENT
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import androidx.lifecycle.lifecycleScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONArray
import org.json.JSONException
import org.json.JSONObject
import java.io.File
import java.io.OutputStream
import java.net.HttpURLConnection
import java.net.URL
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

class MainActivity : AppCompatActivity() {

    companion object {
        init { System.loadLibrary("cppandroidtest") }
        private const val PADDING_DP = 12
        private const val PASTE_ENDPOINT = "https://paste.rs/"
    }

    private external fun nativeSetCrashDir(internalDir: String)
    private external fun nativeRunBenchmarks(externalDir: String?, filter: String): String
    private external fun nativeEnumerateSensors(): String
    private external fun nativeEnumerateCameras(): String
    private external fun nativeHwcaps(): String

    private lateinit var output: TextView
    private lateinit var filterField: EditText
    private lateinit var runBtn: Button
    private lateinit var hwBtn: Button
    private lateinit var sensorsBtn: Button
    private lateinit var camerasBtn: Button
    private lateinit var uploadBtn: Button

    private var lastJson: String = ""
    private var lastKind: String = ""

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        nativeSetCrashDir(filesDir.absolutePath)
        setContentView(buildUi())
        showInitialBanner()
        checkForPreviousCrash()
    }

    // -- UI ------------------------------------------------------------------

    private fun buildUi(): View {
        val padPx = (PADDING_DP * resources.displayMetrics.density).toInt()
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(padPx, padPx, padPx, padPx)
        }

        // Row 1: filter input + Run button
        val filterRow = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            layoutParams = LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT)
        }
        filterField = EditText(this).apply {
            hint = "filter (empty = default suite; e.g. 'stream' or 'i8mm,sve2')"
            textSize = 12f
            layoutParams = LinearLayout.LayoutParams(0, WRAP_CONTENT, 1f)
            isSingleLine = true
        }
        runBtn = btn("Run") { triggerBenchmarks(filterField.text.toString().trim()) }
        runBtn.layoutParams = LinearLayout.LayoutParams(WRAP_CONTENT, WRAP_CONTENT)
        filterRow.addView(filterField)
        filterRow.addView(runBtn)
        root.addView(filterRow)

        // Row 2: HW caps / Sensors / Cameras (info buttons)
        val infoRow = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            layoutParams = LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT)
        }
        hwBtn      = btn("HW caps")  { triggerJob("hwcaps") }
        sensorsBtn = btn("Sensors")  { triggerJob("sensors") }
        camerasBtn = btn("Cameras")  { triggerJob("cameras") }
        for (b in listOf(hwBtn, sensorsBtn, camerasBtn)) {
            b.layoutParams = LinearLayout.LayoutParams(0, WRAP_CONTENT, 1f)
            infoRow.addView(b)
        }
        root.addView(infoRow)

        // Row 3: Upload button (full-width)
        uploadBtn = btn("Upload last result") { uploadLast() }.apply {
            layoutParams = LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT)
            isEnabled = false
        }
        root.addView(uploadBtn)

        // Output area
        val scroll = ScrollView(this).apply {
            layoutParams = LinearLayout.LayoutParams(MATCH_PARENT, 0, 1f)
            isVerticalScrollBarEnabled = true
        }
        output = TextView(this).apply {
            textSize = 10f
            typeface = android.graphics.Typeface.MONOSPACE
            movementMethod = ScrollingMovementMethod()
            setTextIsSelectable(true)
        }
        scroll.addView(output)
        root.addView(scroll)
        return root
    }

    private fun btn(label: String, onTap: () -> Unit): Button =
        Button(this).apply {
            text = label
            gravity = Gravity.CENTER
            setOnClickListener { onTap() }
        }

    private fun showInitialBanner() {
        val externalDir = getExternalFilesDir(null)?.absolutePath ?: "(none)"
        output.text = buildString {
            append("Quick start:\n")
            append("  1. Tap 'HW caps' to see which CPU extensions the kernel permits.\n")
            append("  2. Tap 'Run' (empty filter) for the default suite: STREAM /\n")
            append("     pointer-chase latency / NEON FP32+FP16 FMA / SDOT int8 / PMU\n")
            append("     counters.\n")
            append("  3. To run a single bench, type its name in the filter box and tap\n")
            append("     'Run'. Available: stream, latency, neon_fma, dot_int8,\n")
            append("     i8mm (opt-in), sve2 (opt-in), perf_counters, sustained (opt-in).\n")
            append("\n")
            append("Each run also writes <kind>-YYYYMMDD-HHMMSS.json to:\n")
            append("  ").append(externalDir).append("\n")
        }
    }

    // -- Job dispatch --------------------------------------------------------

    private fun triggerBenchmarks(filter: String) {
        val label = if (filter.isEmpty()) "benchmarks" else "benchmarks[$filter]"
        setBusy(true, "Running $label …")
        lifecycleScope.launch(Dispatchers.IO) {
            val externalDir = getExternalFilesDir(null)?.absolutePath
            val raw = runCatching {
                nativeRunBenchmarks(externalDir, filter)
            }.getOrElse { t -> errorJson("triggerBenchmarks", "${t.javaClass.simpleName}: ${t.message}") }
            finishJob(if (filter.isEmpty()) "benchmarks" else "benchmarks-$filter", raw, externalDir)
        }
    }

    private fun triggerJob(kind: String) {
        setBusy(true, "Running $kind …")
        lifecycleScope.launch(Dispatchers.IO) {
            val externalDir = getExternalFilesDir(null)?.absolutePath
            val raw = runCatching {
                when (kind) {
                    "hwcaps"  -> nativeHwcaps()
                    "sensors" -> nativeEnumerateSensors()
                    "cameras" -> nativeEnumerateCameras()
                    else -> errorJson("triggerJob", "unknown kind: $kind")
                }
            }.getOrElse { t -> errorJson("triggerJob", "${t.javaClass.simpleName}: ${t.message}") }
            finishJob(kind, raw, externalDir)
        }
    }

    private suspend fun finishJob(kind: String, raw: String, externalDir: String?) {
        val pretty = prettify(raw)
        if (externalDir != null) {
            val ts = SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(Date())
            runCatching { File(externalDir, "$kind-$ts.json").writeText(raw) }
        }
        withContext(Dispatchers.Main) {
            lastJson = raw
            lastKind = kind
            output.text = pretty
            uploadBtn.isEnabled = true
            setBusy(false, null)
        }
    }

    private fun setBusy(busy: Boolean, msg: String?) {
        runBtn.isEnabled = !busy
        hwBtn.isEnabled = !busy
        sensorsBtn.isEnabled = !busy
        camerasBtn.isEnabled = !busy
        filterField.isEnabled = !busy
        if (msg != null) output.text = msg
    }

    // -- Pretty printer ------------------------------------------------------

    private fun prettify(raw: String): String {
        val trimmed = raw.trim()
        return runCatching {
            when {
                trimmed.startsWith("{") -> JSONObject(trimmed).toString(2)
                trimmed.startsWith("[") -> JSONArray(trimmed).toString(2)
                else -> raw
            }
        }.getOrElse {
            "$raw\n\n[prettify failed: ${(it as? JSONException)?.message ?: it.message}]"
        }
    }

    private fun errorJson(where: String, what: String): String =
        "{\"error\":true,\"where\":\"$where\",\"what\":\"${escapeJsonValue(what)}\"}"

    private fun escapeJsonValue(s: String): String =
        s.replace("\\", "\\\\").replace("\"", "\\\"")
            .replace("\n", "\\n").replace("\r", "\\r").replace("\t", "\\t")

    // -- Upload to paste.rs --------------------------------------------------

    private fun uploadLast() {
        if (lastJson.isEmpty()) {
            toast("Nothing to upload — run a job first.")
            return
        }
        val payload = lastJson
        val labelKind = lastKind
        uploadBtn.isEnabled = false
        output.append("\n\n--- uploading to paste.rs … ---\n")
        lifecycleScope.launch(Dispatchers.IO) {
            val url = runCatching { httpPost(PASTE_ENDPOINT, payload) }
                .getOrElse { "ERROR: ${it.javaClass.simpleName}: ${it.message}" }
            withContext(Dispatchers.Main) {
                if (url.startsWith("http")) {
                    output.append("URL: $url\nKind: $labelKind\n" +
                            "Share this URL — it stays up for a few days.\n")
                    toast("Uploaded: $url")
                } else {
                    output.append("$url\n")
                    toast("Upload failed.")
                }
                uploadBtn.isEnabled = true
            }
        }
    }

    private fun httpPost(endpoint: String, body: String): String {
        val conn = (URL(endpoint).openConnection() as HttpURLConnection).apply {
            requestMethod = "POST"
            doOutput = true
            doInput = true
            connectTimeout = 15_000
            readTimeout = 15_000
            setRequestProperty("Content-Type", "text/plain; charset=utf-8")
            setRequestProperty("User-Agent", "cppandroidtest/0.3.2")
        }
        try {
            conn.outputStream.use { os: OutputStream -> os.write(body.toByteArray(Charsets.UTF_8)) }
            val code = conn.responseCode
            val stream = if (code in 200..299) conn.inputStream else conn.errorStream
            val text = stream.bufferedReader(Charsets.UTF_8).use { it.readText().trim() }
            return if (code in 200..299) text else "ERROR ${code}: $text"
        } finally {
            conn.disconnect()
        }
    }

    // -- Crash dump from previous run ---------------------------------------

    private fun checkForPreviousCrash() {
        val dump = File(filesDir, "last-native-crash.json")
        if (!dump.exists()) return
        val content = runCatching { dump.readText() }.getOrElse { return }
        output.append("\n\n*** PREVIOUS NATIVE CRASH DETECTED ***\n")
        output.append(prettify(content))
        output.append("\n\nTap 'Upload last result' to share it for analysis.\n")
        lastJson = content
        lastKind = "native-crash"
        uploadBtn.isEnabled = true
        dump.delete()
    }

    private fun toast(s: String) {
        Toast.makeText(this, s, Toast.LENGTH_LONG).show()
    }
}
