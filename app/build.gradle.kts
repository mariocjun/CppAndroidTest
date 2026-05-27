plugins {
    id("com.android.application")
}

android {
    namespace = "com.example.cppandroidtest"
    compileSdk = 34
    ndkVersion = "26.1.10909125"

    defaultConfig {
        applicationId = "com.example.cppandroidtest"
        // minSdk 29 (Android 10) because:
        //   - ASensor_getHandle was added in API 29 (NDK marks it
        //     __INTRODUCED_IN(29); we use it in bench/sensors).
        //   - ASensorManager_getInstanceForPackage requires API 26.
        // All target devices (Note10+ Exynos/Snapdragon, S24 Ultra, any
        // 2020+ flagship) are >= API 29, so this is no practical loss.
        minSdk = 29
        targetSdk = 34
        versionCode = 1
        versionName = "1.0"

        externalNativeBuild {
            cmake {
                arguments += listOf("-DANDROID_STL=c++_shared")
                // Don't hard-code -std= here: NDK r26b ships Clang 17 which
                // only knows '-std=c++2b' for C++23, while Clang 18+ uses
                // '-std=c++23'. CMakeLists's CMAKE_CXX_STANDARD 23 lets CMake
                // emit the right flag for whichever compiler the NDK ships.
            }
        }
        ndk {
            abiFilters += listOf("arm64-v8a", "armeabi-v7a", "x86_64")
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
            signingConfig = signingConfigs.getByName("debug")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}
