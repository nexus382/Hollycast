/*
	Copyright 2022 flyinghead
	Portions Copyright 2026 The Hollycast Authors

	This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
*/
#pragma once
#include "oslib/http_client.h"
#include "jni_util.h"
#include <cstdint>

namespace http {

    static jobject HttpClient;
    static jmethodID initMid;
    static jmethodID openUrlMid;
    static jmethodID postMid;
    static jmethodID postRawMid;

    void init() {
    	jni::String juserAgent(getUserAgent());
    	jni::env()->CallVoidMethod(HttpClient, initMid, static_cast<jstring>(juserAgent));
    }

    int get(const std::string &url, std::vector<u8> &content, const Headers *reqHeaders, Headers *respHeaders, int timeoutMs,
            size_t maxBytes, const std::atomic_bool* cancelled)
    {
        content.clear();
        if (cancelled != nullptr && cancelled->load())
            return 499;

        jni::String jurl(url);
        jni::ObjectArray<jni::ByteArray> contentOut(1);
        jni::ObjectArray<jni::String> reqHeaderKeys;
        jni::ObjectArray<jni::String> reqHeaderValues;
        jni::ObjectArray<jni::ObjectArray<jni::String>> respHeaderKeys;
        jni::ObjectArray<jni::ObjectArray<jni::String>> respHeaderValues;
        if (reqHeaders != nullptr)
        {
            reqHeaderKeys = jni::ObjectArray<jni::String>(reqHeaders->size());
            reqHeaderValues = jni::ObjectArray<jni::String>(reqHeaders->size());
            for (size_t i = 0; i < reqHeaders->size(); i++) {
            	reqHeaderKeys.setAt(i, jni::String(reqHeaders->at(i).first));
            	reqHeaderValues.setAt(i, jni::String(reqHeaders->at(i).second));
            }
        }
        if (respHeaders != nullptr)
        {
        	jni::ObjectArray<jni::String> tmp(1);
        	respHeaderKeys = jni::ObjectArray<jni::ObjectArray<jni::String>>(1, tmp.getClass());
        	respHeaderValues = jni::ObjectArray<jni::ObjectArray<jni::String>>(1, tmp.getClass());
        }

        const jlong javaMaxBytes = maxBytes == std::numeric_limits<size_t>::max()
                || maxBytes > static_cast<size_t>(std::numeric_limits<jlong>::max())
                ? -1 : static_cast<jlong>(maxBytes);
        // Java polls this borrowed flag only while the synchronous JNI call is active.
        const jlong cancellationToken = static_cast<jlong>(reinterpret_cast<std::intptr_t>(cancelled));
        int httpStatus = jni::env()->CallIntMethod(HttpClient, openUrlMid,
        		static_cast<jstring>(jurl),
				static_cast<jobjectArray>(contentOut),
        		static_cast<jobjectArray>(reqHeaderKeys),
                static_cast<jobjectArray>(reqHeaderValues),
                static_cast<jobjectArray>(respHeaderKeys),
                static_cast<jobjectArray>(respHeaderValues),
                static_cast<jint>(timeoutMs),
                javaMaxBytes,
                cancellationToken);

        if (cancelled != nullptr && cancelled->load())
            return 499;
        if (http::success(httpStatus))
            content = contentOut[0];
        if (respHeaders != nullptr)
        {
        	jni::ObjectArray<jni::String> keys = respHeaderKeys[0];
        	jni::ObjectArray<jni::String> values = respHeaderValues[0];
        	for (size_t i = 0; i < keys.size(); i++)
        		respHeaders->emplace_back(keys[i], values[i]);
        }

        return httpStatus;
    }

    int post(const std::string &url, const char *payload, const char *contentType, std::vector<u8>& reply)
    {
        jni::String jurl(url);
        jni::String jpayload(payload);
        jni::String jcontentType(contentType);
        jni::ObjectArray<jni::ByteArray> replyOut(1);

        int httpStatus = jni::env()->CallIntMethod(HttpClient, postRawMid,
        		static_cast<jstring>(jurl),
				static_cast<jstring>(jpayload),
				static_cast<jstring>(jcontentType),
				static_cast<jobjectArray>(replyOut));
        reply = replyOut[0];

        return httpStatus;
    }

    int post(const std::string &url, const std::vector<PostField>& fields)
    {
        jni::String jurl(url);
        jni::ObjectArray<jni::String> names(fields.size());
        jni::ObjectArray<jni::String> values(fields.size());
        jni::ObjectArray<jni::String> contentTypes(fields.size());

        for (size_t i = 0; i < fields.size(); i++)
        {
        	names.setAt(i, fields[i].name);
        	values.setAt(i, fields[i].value);
        	contentTypes.setAt(i, fields[i].contentType);
        }

        int httpStatus = jni::env()->CallIntMethod(HttpClient, postMid,
        		static_cast<jstring>(jurl),
				static_cast<jobjectArray>(names),
        		static_cast<jobjectArray>(values),
				static_cast<jobjectArray>(contentTypes));

        return httpStatus;
    }

    void term() {
    }

}

extern "C" JNIEXPORT void JNICALL Java_com_hollycast_emulator_emu_HttpClient_nativeInit(JNIEnv *env, jobject obj)
{
    http::HttpClient = env->NewGlobalRef(obj);
    http::initMid = env->GetMethodID(env->GetObjectClass(obj), "init", "(Ljava/lang/String;)V");
    http::openUrlMid = env->GetMethodID(env->GetObjectClass(obj), "openUrl", "(Ljava/lang/String;"	// url
    																		"[[B"					// content (out)
    																		"[Ljava/lang/String;"	// req header names
    																		"[Ljava/lang/String;"	// req header values
    																		"[[Ljava/lang/String;"	// resp header names (out)
    																		"[[Ljava/lang/String;"	// resp header values (out)
																		"I"					// network timeout (ms)
                                                                        "J"                    // maximum response body bytes (-1 means unlimited)
                                                                        "J"                    // cancellation token
    																		")I");
    http::postMid = env->GetMethodID(env->GetObjectClass(obj), "post", "(Ljava/lang/String;[Ljava/lang/String;[Ljava/lang/String;[Ljava/lang/String;)I");
    http::postRawMid = env->GetMethodID(env->GetObjectClass(obj), "post", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;[[B)I");
}

extern "C" JNIEXPORT jboolean JNICALL Java_com_hollycast_emulator_emu_HttpClient_isCancelled(
        JNIEnv*, jclass, jlong cancellationToken)
{
    const auto* cancelled = reinterpret_cast<const std::atomic_bool*>(static_cast<std::intptr_t>(cancellationToken));
    return cancelled != nullptr && cancelled->load() ? JNI_TRUE : JNI_FALSE;
}
