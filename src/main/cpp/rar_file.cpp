//
// Created by mao on 17-6-4.
//
#include "jni.h"
#include "rar.hpp"
#include "dll.hpp"
#include "jni_util.h"

#include "rar_file.h"

#include <android/log.h>

#ifdef __cplusplus
extern "C" {
#endif
struct user_data {
    JNIEnv *env;
    jobject callback;
    jbyteArray readbuf;
};

static jmethodID callback_process_data;
static jmethodID callback_need_password;

static jclass entryClass;
static jmethodID entry_ctor;

void initIDs(JNIEnv *env) {
    jclass callback_cls = nullptr;

    entryClass = static_cast<jclass>(
            env->NewGlobalRef(env->FindClass("mao/archive/unrar/RarEntry")));
    if (entryClass == nullptr) {
        return;
    }

    callback_cls = env->FindClass("mao/archive/unrar/UnrarCallback");
    if (callback_cls == nullptr) {
        return;
    }

    callback_process_data = env->GetMethodID(callback_cls, "processData", "([BII)V");

    callback_need_password = env->GetMethodID(callback_cls, "needPassword", "()Ljava/lang/String;");


    entry_ctor = env->GetMethodID(entryClass, "<init>", "(Ljava/lang/String;JJJJI)V");

    env->DeleteLocalRef(callback_cls);
}
inline wchar_t *jcharntowcs(wchar *dest, const jchar *src, size_t len) {
    if (dest == nullptr || src == nullptr)
        return nullptr;
    size_t i = 0;
    for (; i < len && src[i] != '\0'; i++) {
        dest[i] = src[i];
    }
    for (; i <= len; i++)
        dest[i] = '\0';

    return dest;
}
inline jchar *wcsntojchar(jchar *dest, const wchar *src, size_t len) {
    if (dest == nullptr || src == nullptr)
        return nullptr;
    size_t i = 0;
    for (; i < len && src[i] != '\0'; i++) {
        dest[i] = (jchar) src[i];
    }
    for (; i <= len; i++)
        dest[i] = '\0';
    return dest;
}

int CALLBACK callbackFunc(UINT msg, LPARAM UserData, LPARAM P1, LPARAM P2) {
    struct user_data *data = (struct user_data *) UserData;
    switch (msg) {
        case UCM_PROCESSDATA: {
            const jbyte *buf = (const jbyte *) P1;

            for (jsize readed = 0, rem = (jsize) P2, len = data->env->GetArrayLength(data->readbuf);
                 rem > 0; readed += len, rem -= len) {
                if (len > rem) {
                    len = rem;
                }
                data->env->SetByteArrayRegion(data->readbuf, 0, len, buf + readed);

                data->env->CallVoidMethod(data->callback, callback_process_data,
                                          data->readbuf, 0, len);
                if (data->env->ExceptionCheck()) {
                    data->env->ExceptionClear();
                    return -1;
                }
            }
            return 1;
        }
        case UCM_NEEDPASSWORDW: {
            jstring jpassword = (jstring) data->env->CallObjectMethod(data->callback,
                                                                      callback_need_password);
            if (jpassword != nullptr) {
                wchar_t *password = (wchar_t *) P1;
                const jchar *chars = data->env->GetStringChars(jpassword, nullptr);
                jcharntowcs(password, chars,
                            static_cast<size_t >(Min(data->env->GetStringLength(jpassword), P2)));
                //保证字符串正确终止
                password[P2 - 1] = '\0';
                data->env->ReleaseStringChars(jpassword, chars);
                return 1;
            }
            return -1;
        }
        case UCM_CHANGEVOLUMEW: {
            if (P2 == RAR_VOL_NOTIFY) {
                return 0;
            }
            //TODO 缺少卷调用,回调java层
//            LOGI("volume %d", P2);
            return -1;
        }
        default:
            break;
    }
    return 1;
}

static jlong Java_mao_archive_unrar_RarFile_openArchive
        (JNIEnv *env, jclass jcls, jstring jfilePath, jint mode) {
    wchar nameW[NM];
    struct RAROpenArchiveDataEx data{};

    const jchar *path = env->GetStringChars(jfilePath, nullptr);

    jcharntowcs(nameW, path, (size_t) env->GetStringLength(jfilePath));

    env->ReleaseStringChars(jfilePath, path);

    data.ArcNameW = &nameW[0];
    data.OpenMode = (unsigned int) mode;

    HANDLE handle = RAROpenArchiveEx(&data);

    if (handle == nullptr || data.OpenResult) {
        if (handle) {
            RARCloseArchive(handle);
        }
        char err_str[128];
        sprintf(err_str, "ErrorCode: %d", data.OpenResult);
        JNU_ThrowByName(env, "mao/archive/unrar/RarException", err_str);
        return 0;
    }
    return reinterpret_cast<jlong>(handle);
}


static jobject Java_mao_archive_unrar_RarFile_readHeader0
        (JNIEnv *env, jclass jcls, jlong jhandle, jobject callback) {

    struct user_data userData{};

    HANDLE handle = reinterpret_cast<void *>(jhandle);

    if (callback != nullptr) {
        userData.env = env;
        userData.callback = env->NewGlobalRef(callback);
        //需要密码回调
        RARSetCallback(handle, callbackFunc, (LPARAM) &userData);
    } else {
        RARSetCallback(handle, nullptr, (LPARAM) nullptr);
    }


    struct RARHeaderDataEx header{};
    memset(&header, 0, sizeof(struct RARHeaderDataEx));
    if (RARReadHeaderEx(handle, &header)) {
        return nullptr;
    }

    if (userData.callback) {
        env->DeleteGlobalRef(userData.callback);
    }

    jchar name[1024];
    size_t len = wcslen(header.FileNameW);
    wcsntojchar(name, header.FileNameW, len);

    jstring jname = env->NewString(name, (jsize) len);


    return env->NewObject(entryClass, entry_ctor, jname,
                          (((uint64_t) header.UnpSizeHigh) << 32) | header.UnpSize,
                          (((uint64_t) header.PackSizeHigh) << 32) | header.PackSize,
                          header.FileCRC, (header.FileTime), header.Flags);
}

#define MAXBUF (1024*64)
static void Java_mao_archive_unrar_RarFile_processFile0
        (JNIEnv *env, jclass jcls, jlong jhandle, jint operation, jstring jdestPath,
         jstring jdestName, jobject callback) {
    struct user_data userData{};
    HANDLE handle = reinterpret_cast<void *>(jhandle);

    wchar_t destPath[NM], destName[NM];

    memset(destPath, 0, sizeof(wchar) * NM);
    memset(destName, 0, sizeof(wchar) * NM);

    if (jdestPath != nullptr) {
        const jchar *chars = env->GetStringChars(jdestPath, nullptr);
        jcharntowcs(destPath, chars, static_cast<size_t>(env->GetStringLength(jdestPath)));
        env->ReleaseStringChars(jdestPath, chars);
    }

    if (jdestName != nullptr) {
        const jchar *chars = env->GetStringChars(jdestName, nullptr);
        jcharntowcs(destName, chars, static_cast<size_t>(env->GetStringLength(jdestName)));
        env->ReleaseStringChars(jdestName, chars);
    }

    if (callback != nullptr) {//UCM_PROCESSDATA
        userData.env = env;
        userData.callback = env->NewGlobalRef(callback);

        userData.readbuf = static_cast<jbyteArray>(env->NewGlobalRef(env->NewByteArray(MAXBUF)));

        RARSetCallback(handle, callbackFunc, (LPARAM) &userData);
    } else {
        RARSetCallback(handle, nullptr, (LPARAM) nullptr);
    }
    int code = RARProcessFileW(handle, operation, destPath, destName);

    if (userData.callback) {
        env->DeleteGlobalRef(userData.callback);
    }
    if (userData.readbuf) {
        env->DeleteGlobalRef(userData.readbuf);
    }
    //检查异常
    switch (code) {
        case ERAR_SUCCESS:
            break;
        case ERAR_BAD_PASSWORD:
            JNU_ThrowIOException(env, "Bad password");
            break;
        case ERAR_MISSING_PASSWORD:
            JNU_ThrowIOException(env, "Missing password");
            break;
        default:
            LOGI("operation %d,process result %d", operation, code);
            JNU_ThrowIOException(env, "");
    }
}

static void Java_mao_archive_unrar_RarFile_closeArchive
        (JNIEnv *env, jclass jcls, jlong jhandle) {
    HANDLE handle = reinterpret_cast<void *>(jhandle);
    if (RARCloseArchive(handle) != ERAR_SUCCESS) {
        JNU_ThrowIOExceptionWithLastError(env, "close error");
    }
}


static JNINativeMethod methods[] = {
        {"openArchive",  "(Ljava/lang/String;I)J",                                                     (void *) Java_mao_archive_unrar_RarFile_openArchive},

        {"readHeader0",  "(JLmao/archive/unrar/UnrarCallback;)Lmao/archive/unrar/RarEntry;",           (void *) Java_mao_archive_unrar_RarFile_readHeader0},

        {"processFile0", "(JILjava/lang/String;Ljava/lang/String;Lmao/archive/unrar/UnrarCallback;)V", (void *) Java_mao_archive_unrar_RarFile_processFile0},

        {"closeArchive", "(J)V",                                                                       (void *) Java_mao_archive_unrar_RarFile_closeArchive},

};

#define NELEM(x) ((int) (sizeof(x) / sizeof((x)[0])))

jboolean registerNativeMethods(JNIEnv *env) {
    jclass clazz = env->FindClass("mao/archive/unrar/RarFile");
    if (clazz == nullptr) {
        return JNI_FALSE;
    }
    if (env->RegisterNatives(clazz, methods, NELEM(methods)) < 0) {
        env->DeleteLocalRef(clazz);
        return JNI_FALSE;
    }
    env->DeleteLocalRef(clazz);

    return JNI_TRUE;
}


#ifdef __cplusplus
}
#endif




