// MainActivity — user-facing entry. Loads libcppandroidtest.so, drives the
// JNI bridge that returns benchmark / sensor / camera / hwcap JSON, and
// handles crash-dump propagation.
//
// UI layout:
//   url history:  always-visible scrollable history of upload URLs
//   filter row:   [ filter text input ] [ Run ]
//   info row:     [ HW caps ] [ Sensors ] [ Cameras ]
//   action row:   [ Upload last result ]
//   output:       scrollable monospace TextView (selectable)
//
// 'Upload' POSTs the latest output to paste.rs. The returned URL is:
//   1. Copied to the system clipboard automatically.
//   2. Prepended to a persistent 'Recent uploads' header that survives
//      subsequent Run taps (so you don't lose URLs when starting a new
//      benchmark).
//   3. Appended to a log file in filesDir/upload-log.txt.

package com.example.cppandroidtest

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.os.Bundle
import android.text.method.LinkMovementMethod
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
        private const val MAX_URL_HISTORY = 8
    }

    private external fun nativeSetCrashDir(internalDir: String)
    private external fun nativeRunBenchmarks(externalDir: String?, filter: String): String
    private external fun nativeEnumerateSensors(): String
    private external fun nativeEnumerateCameras(): String
    private external fun nativeHwcaps(): String

    private lateinit var urlHistoryView: TextView
    private lateinit var output: TextView
    private lateinit var filterField: EditText
    private lateinit var runBtn: Button
    private lateinit var hwBtn: Button
    private lateinit var sensorsBtn: Button
    private lateinit var camerasBtn: Button
    private lateinit var uploadBtn: Button

    private val urlHistory: ArrayDeque<String> = ArrayDeque()
    private var lastJson: String = ""
    private var lastKind: String = ""

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        nativeSetCrashDir(filesDir.absolutePath)
        setContentView(buildUi())
        loadUrlHistory()
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

        // Row 0: persistent URL history (sticky at top, survives Run taps)
        urlHistoryView = TextView(this).apply {
            textSize = 11f
            typeface = android.graphics.Typeface.MONOSPACE
            setTextIsSelectable(true)
            movementMethod = LinkMovementMethod.getInstance()
            autoLinkMask = android.text.util.Linkify.WEB_URLS
            text = "(no uploads yet)"
            setPadding(0, 0, 0, padPx / 2)
        }
        root.addView(urlHistoryView)

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
            // Stable test locator — UI automation finds this by content-desc,
            // never by pixel coordinates, so it survives layout/resolution changes.
            contentDescription = "field_filter"
        }
        runBtn = btn("Run", "btn_run") { triggerBenchmarks(filterField.text.toString().trim()) }
        runBtn.layoutParams = LinearLayout.LayoutParams(WRAP_CONTENT, WRAP_CONTENT)
        filterRow.addView(filterField)
        filterRow.addView(runBtn)
        root.addView(filterRow)

        // Row 2: HW caps / Sensors / Cameras (info buttons)
        val infoRow = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            layoutParams = LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT)
        }
        hwBtn      = btn("HW caps", "btn_hwcaps")   { triggerJob("hwcaps") }
        sensorsBtn = btn("Sensors", "btn_sensors")  { triggerJob("sensors") }
        camerasBtn = btn("Cameras", "btn_cameras")  { triggerJob("cameras") }
        for (b in listOf(hwBtn, sensorsBtn, camerasBtn)) {
            b.layoutParams = LinearLayout.LayoutParams(0, WRAP_CONTENT, 1f)
            infoRow.addView(b)
        }
        root.addView(infoRow)

        // Row 3: Upload button (full-width)
        uploadBtn = btn("Upload last result", "btn_upload") { uploadLast() }.apply {
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

    // testId becomes the View's contentDescription — a stable locator that
    // UI automation (scripts/ui_tap.py) matches against, so taps don't depend
    // on pixel coordinates or on the visible label text.
    private fun btn(label: String, testId: String, onTap: () -> Unit): Button =
        Button(this).apply {
            text = label
            contentDescription = testId
            gravity = Gravity.CENTER
            setOnClickListener { onTap() }
        }

    private fun showInitialBanner() {
        val externalDir = getExternalFilesDir(null)?.absolutePath ?: "(none)"
        output.text = buildString {
            append("Quick start:\n")
            append("  1. Tap 'HW caps' first — shows kernel-permitted ARM extensions.\n")
            append("  2. Tap 'Run' (empty filter) for the default suite (~15s).\n")
            append("  3. To isolate one bench, type its name and tap Run.\n")
            append("     Names: stream, latency, neon_fma, dot_int8, i8mm (opt-in),\n")
            append("     sve2 (opt-in), perf_counters, sustained (opt-in).\n")
            append("\n")
            append("After a job, tap 'Upload last result' to share via paste.rs.\n")
            append("URLs are auto-copied to clipboard AND kept in the history bar\n")
            append("above so they survive subsequent Run taps.\n")
            append("\n")
            append("Local copies of every job also save to:\n")
            append("  ").append(externalDir)
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
                    recordUpload(labelKind, url)
                    output.append("URL: $url\nKind: $labelKind\n" +
                            "(copied to clipboard; also in history bar above)\n")
                    toast("Uploaded — URL copied to clipboard")
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
            setRequestProperty("User-Agent", "cppandroidtest/0.3.3")
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

    // -- URL history persistence --------------------------------------------

    private fun recordUpload(kind: String, url: String) {
        val ts = SimpleDateFormat("HH:mm:ss", Locale.US).format(Date())
        val entry = "[$ts] $kind  $url"
        urlHistory.addFirst(entry)
        while (urlHistory.size > MAX_URL_HISTORY) urlHistory.removeLast()
        renderUrlHistory()
        // Auto-copy to system clipboard so the URL survives anything that
        // happens to the in-app UI.
        runCatching {
            val cm = getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
            cm.setPrimaryClip(ClipData.newPlainText("paste.rs upload", url))
        }
        // Append to persistent log so even crashing/uninstalling leaves a trace.
        runCatching {
            File(filesDir, "upload-log.txt")
                .appendText("${SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US).format(Date())}  $kind  $url\n")
        }
    }

    private fun renderUrlHistory() {
        if (urlHistory.isEmpty()) {
            urlHistoryView.text = "(no uploads yet)"
            return
        }
        urlHistoryView.text = "Recent uploads (latest first):\n" +
                urlHistory.joinToString("\n") { "  $it" }
    }

    private fun loadUrlHistory() {
        val f = File(filesDir, "upload-log.txt")
        if (!f.exists()) {
            renderUrlHistory()
            return
        }
        runCatching {
            // Pull the last MAX_URL_HISTORY URL lines from the log.
            val lines = f.readLines().asReversed().take(MAX_URL_HISTORY)
            urlHistory.clear()
            for (line in lines) {
                // Format on disk: "YYYY-MM-DD HH:MM:SS  kind  url"
                // Render as in-memory format (HH:MM:SS  kind  url).
                val parts = line.split("  ", limit = 3)
                val short = if (parts.size == 3) {
                    val timeOnly = parts[0].substringAfter(' ')
                    "[$timeOnly] ${parts[1]}  ${parts[2]}"
                } else line
                urlHistory.addLast(short)
            }
        }
        renderUrlHistory()
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
