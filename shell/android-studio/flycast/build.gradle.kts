// Portions Copyright 2026 The Hollycast Authors
plugins {
    alias(libs.plugins.android.application)
}

fun gitVersionName(): String {
    return providers.exec {
        commandLine("git", "describe", "--tags", "--always")
    }.standardOutput.asText.get().trim()
}

val githubUpdates = providers.gradleProperty("githubUpdates").orNull == "true"

android {
    namespace = "com.hollycast.emulator"
    buildFeatures { buildConfig = true }
    ndkVersion = "29.0.14206865"
    compileSdk {
        version = release(36) {
            minorApiLevel = 1
        }
    }

    defaultConfig {
        buildConfigField("boolean", "GITHUB_UPDATES", githubUpdates.toString())
        applicationId = "com.hollycast.emu"
        minSdk = 21
        targetSdk = 36
        versionCode = 8
        versionName = gitVersionName()
        vectorDrawables.useSupportLibrary = true

        externalNativeBuild {
            cmake {
                arguments += "-DANDROID_ARM_MODE=arm"
                arguments += "-DUSE_OPENMP=OFF"
                arguments += "-DANDROID_WEAK_API_DEFS=ON"
            }
        }

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
    }

    // GitHub APKs use Android's package installer. Store bundles omit this permission.
    if (githubUpdates) {
        sourceSets.configureEach {
            if (name in listOf("debug", "release", "developerRelease"))
                manifest.srcFile("src/github/AndroidManifest.xml")
        }
    }

    signingConfigs {
        getByName("debug") {
            storeFile = file("../debug.keystore")
        }
        create("release") {
            storeFile = file("../playstore.jks")
            storePassword = System.getenv("ANDROID_KEYSTORE_PASSWORD")
            keyAlias = "uploadkey"
            keyPassword = System.getenv("ANDROID_KEYSTORE_PASSWORD")
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
            signingConfig = signingConfigs.getByName("release")
        }

        // Release build for local development.
        create("developerRelease") {
            initWith(getByName("release"))
            signingConfig = signingConfigs.getByName("debug")
            // Causes error "Build type isn't debuggable"
            // to prevent accidentally debugging a painful-to-debug build.
            // Enable this locally if you need it.
            isDebuggable = false
            isJniDebuggable = false
            matchingFallbacks += "release"
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_11
        targetCompatibility = JavaVersion.VERSION_11
    }
    externalNativeBuild {
        cmake {
            path = file("../../../CMakeLists.txt")
            version = "3.22.1"
        }
    }
    packaging {
        jniLibs {
            // not used
            excludes += "lib/*/libz.so"
            // This is necessary for libadrenotools custom driver loading
            useLegacyPackaging = true
        }
        resources {
            excludes += "META-INF/DEPENDENCIES"
        }
    }
}

androidComponents {
    onVariants { variant ->
        variant.outputs.forEach { output ->
            output.outputFileName.set("Hollycast-${variant.name}.apk")
        }
    }
}

dependencies {
    implementation("com.google.android.play:app-update:2.1.0")
    implementation(libs.appcompat)
    implementation(libs.commons.lang3)
    implementation(libs.httpclient5)
    implementation(libs.slf4j.android)
    implementation(fileTree("libs") { include("*.aar", "*.jar") })
    implementation(libs.documentfile)
    testImplementation(libs.junit)
    testImplementation("org.mockito:mockito-core:5.14.2")
    testImplementation("org.json:json:20240303")
    androidTestImplementation(libs.espresso.core)
    androidTestImplementation(libs.ext.junit)
}
