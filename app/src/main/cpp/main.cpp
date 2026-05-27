#include <android_native_app_glue.h>
#include <android/log.h>

#define LOG_TAG "CppAndroidTest"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

static void handle_cmd(android_app* app, int32_t cmd) {
    switch (cmd) {
        case APP_CMD_INIT_WINDOW:
            LOGI("Window initialized");
            break;
        case APP_CMD_TERM_WINDOW:
            LOGI("Window terminated");
            break;
        case APP_CMD_GAINED_FOCUS:
            LOGI("Gained focus");
            break;
        case APP_CMD_LOST_FOCUS:
            LOGI("Lost focus");
            break;
        default:
            break;
    }
}

void android_main(android_app* app) {
    app->onAppCmd = handle_cmd;
    LOGI("Hello from C++ Native Activity!");

    while (true) {
        int events;
        android_poll_source* source;
        while (ALooper_pollAll(0, nullptr, &events, reinterpret_cast<void**>(&source)) >= 0) {
            if (source != nullptr) {
                source->process(app, source);
            }
            if (app->destroyRequested != 0) {
                LOGI("Destroy requested, exiting");
                return;
            }
        }
    }
}
