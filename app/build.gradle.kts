plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "com.hozinking.wildhunterinjector"
    compileSdk = 34

    defaultConfig {
        applicationId = "com.hozinking.wildhunterinjector"
        minSdk = 24
        targetSdk = 34
        versionCode = 2
        versionName = "2.0"

        ndk {
            abiFilters += "arm64-v8a"
        }
        externalNativeBuild {
            cmake {
                cppFlags += "-std=c++20"
                arguments += "-DANDROID_STL=c++_static"
                // Cek lokal saja: bila env FETCHCONTENT_SOURCE_DIR_DOBBY
                // menunjuk ke salinan Dobby yang sudah ter-patch, pakai itu
                // (proxy egress VM menolak download arsip GitHub — 407).
                // Di CI env ini tidak ada → download normal dari GitHub.
                val dobbyLocal = System.getenv("FETCHCONTENT_SOURCE_DIR_DOBBY")
                if (!dobbyLocal.isNullOrBlank()) {
                    arguments += "-DFETCHCONTENT_SOURCE_DIR_DOBBY=$dobbyLocal"
                }
            }
        }
    }

    // Keystore debug PERSISTEN (pola Hozin Dompet / HCR v1.11): bila file
    // ../debug.keystore ada (lokal), pakai itu supaya signer sama tiap
    // versi; di CI file itu di-decode dari secret DEBUG_KEYSTORE_BASE64
    // oleh workflow sebelum build. Bila tidak ada, AGP memakai debug
    // keystore ephemeral bawaan (fallback).
    signingConfigs {
        if (file("../debug.keystore").exists()) {
            getByName("debug") {
                storeFile = file("../debug.keystore")
                storePassword = "android"
                keyAlias = "androiddebugkey"
                keyPassword = "android"
            }
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
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }
    buildFeatures {
        viewBinding = true
        // v1.0.5: BuildConfig.VERSION_NAME dipakai menu + log supaya
        // string versi selalu sama dengan versionName (AGP 8.x tidak
        // men-generate BuildConfig secara default).
        buildConfig = true
    }
    packaging {
        // Dobby membangun dobby SHARED + dobby_static; payload hanya
        // memakai yang STATIC — buang libdobby.so dari APK (403 KB,
        // tak dipakai siapa pun, dan AGP membundel semua .so hasil CMake).
        jniLibs {
            excludes += "lib/arm64-v8a/libdobby.so"
        }
    }
}

dependencies {
    implementation("androidx.core:core-ktx:1.13.1")
    implementation("androidx.appcompat:appcompat:1.7.0")
    implementation("com.google.android.material:material:1.12.0")
}
