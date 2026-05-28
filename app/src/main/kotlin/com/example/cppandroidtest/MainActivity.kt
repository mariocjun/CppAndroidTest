// MainActivity — the user-facing entry point. Loads libcppandroidtest.so,
// drives the JNI bridge that returns benchmark / sensor / camera JSON.
//
// Resilience features added after a crash report from S24 Ultra:
//   - Every native call is wrapped in try/catch so a Kotlin-side exception
//     can't tear down the activity silently.
//   - JSON output is pretty-printed via org.json.JSONObject.toString(2)
//     so it's readable in the on-screen TextView.
//   - A native crash dump (libcppandroidtest.so's signal handlers write to
//     <internalFilesDir>/last-native-crash.json on SIGSEGV/SIGABRT/...) is
//     checked on every onCreate; if present, the user is offered an Upload
//     button next to the results pane.
//   - Upload posts the current result text to paste.rs (an authentication-
//     free public paste service) and shows the returned URL.
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
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import androidx.lifecycle.lifecycleScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONArray
import org.json.JSONException
import org.json.JSONObject
import java.io.BufferedReader
import java.io.File
import java.io.InputStreamReader
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

    // JNI surface — see app/src/main/cpp/jni.cpp.
    private external fun nativeSetCrashDir(internalDir: String)
    private external fun nativeRunBenchmarks(externalDir: String?): String
    private external fun nativeEnumerateSensors(): String
    private external fun nativeEnumerateCameras(): String

    private lateinit var output: TextView
    private lateinit var runBtn: Button
    private lateinit var sensorsBtn: Button
    private lateinit var camerasBtn: Button
    private lateinit var uploadBtn: Button

    private var lastJson: String = ""
    private var lastKind: String = ""

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        // Hand the JNI side a path for its native crash handler to write to.
        // Has to happen before the user triggers any native work.
        nativeSetCrashDir(filesDir.absolutePath)
        setContentView(buildUi())
        showInitialBanner()
        checkForPreviousCrash()
    }

    // -- UI ------------------------------------------------------------------

    private fun buildUi(): View {
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            val padPx = (PADDING_DP * resources.displayMetrics.density).toInt()
            setPadding(padPx, padPx, padPx, padPx)
        }

        val buttonRow = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            layoutParams = LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT)
        }
        runBtn     = btn("Run benchmarks") { triggerJob("benchmarks") }
        sensorsBtn = btn("Sensors")        { triggerJob("sensors") }
        camerasBtn = btn("Cameras")        { triggerJob("cameras") }
        for (b in listOf(runBtn, sensorsBtn, camerasBtn)) {
            b.layoutParams = LinearLayout.LayoutParams(0, WRAP_CONTENT, 1f)
            buttonRow.addView(b)
        }
        root.addView(buttonRow)

        uploadBtn = btn("Upload last result") { uploadLast() }.apply {
            layoutParams = LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT)
            isEnabled = false
        }
        root.addView(uploadBtn)

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
        output.text =
            "Tap a button to start.\n\n" +
            "• Run benchmarks — STREAM bandwidth, pointer-chase latency,\n" +
            "  NEON FP32/FP16 FMA, SDOT int8, SMMLA i8mm, SVE2 FP32,\n" +
            "  PMU counters (~15-20s on a flagship)\n" +
            "• Sensors — every ASensorManager-visible sensor + metadata\n" +
            "• Cameras — every Camera2 NDK camera + characteristics\n\n" +
            "Output is JSON, pretty-printed below. Each run also saves to:\n" +
            externalDir
    }

    // -- Job dispatch --------------------------------------------------------

    private fun triggerJob(kind: String) {
        setBusy(true, "Running $kind…")
        lifecycleScope.launch(Dispatchers.IO) {
            val externalDir = getExternalFilesDir(null)?.absolutePath
            val rawJson: String = runCatching {
                when (kind) {
                    "benchmarks" -> nativeRunBenchmarks(externalDir)
                    "sensors"    -> nativeEnumerateSensors()
                    "cameras"    -> nativeEnumerateCameras()
                    else -> errorJson("triggerJob", "unknown kind: $kind")
                }
            }.getOrElse { t ->
                errorJson("triggerJob", "${t.javaClass.simpleName}: ${t.message}")
            }

            val pretty = prettify(rawJson)
            // Persist the COMPACT form so file size stays small.
            if (externalDir != null) {
                val ts = SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(Date())
                runCatching {
                    File(externalDir, "$kind-$ts.json").writeText(rawJson)
                }
            }

            withContext(Dispatchers.Main) {
                lastJson = rawJson
                lastKind = kind
                output.text = pretty
                uploadBtn.isEnabled = true
                setBusy(false, null)
            }
        }
    }

    private fun setBusy(busy: Boolean, msg: String?) {
        runBtn.isEnabled = !busy
        sensorsBtn.isEnabled = !busy
        camerasBtn.isEnabled = !busy
        // Don't disable upload — uploading is independent.
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
            // org.json failed (corrupted JSON?). Show raw with the parse
            // error appended so the user has SOMETHING to diagnose with.
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
        val payload = lastJson  // upload the compact form, paste.rs handles it
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
                    output.append(url + "\n")
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
            setRequestProperty("User-Agent", "cppandroidtest/0.3")
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
        output.append("\n\n*** PREVIOUS NATIVE CRASH DETECTED ***\n$content\n" +
                "Tap 'Upload last result' to share it for analysis.\n")
        // Stash it as the "last result" so the upload button targets it.
        lastJson = content
        lastKind = "native-crash"
        uploadBtn.isEnabled = true
        // Delete so we don't keep re-prompting forever.
        dump.delete()
    }

    private fun toast(s: String) {
        Toast.makeText(this, s, Toast.LENGTH_LONG).show()
    }
}
