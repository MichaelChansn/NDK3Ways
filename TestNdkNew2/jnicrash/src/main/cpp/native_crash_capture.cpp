#include "com_crashcapture_NativeCrashCapture_JNI.h"

#include "handler/exception_handler.h"
#include <android/log.h>

JavaVM *g_jvm;

jint JNI_OnLoad(JavaVM *vm, void *reserved) {
    g_jvm = vm;
    JNIEnv *env;
    if (JNI_OK != vm->GetEnv(reinterpret_cast<void **> (&env), JNI_VERSION_1_4)) {
        return JNI_ERR;
    }

    return JNI_VERSION_1_4;
}

jclass globalobjclass;
namespace native_jnicrash {
    struct callbackPara {
        const char *file_path;
        int type;
    };

    void *do_call_back(void *para) {
        JNIEnv *env;
        g_jvm->AttachCurrentThread(&env, NULL);

        struct callbackPara *call_para = (struct callbackPara *) para;
        if (call_para->type == 1) {
            jmethodID mid = env->GetStaticMethodID(globalobjclass, "crashDumpEnd",
                                                   "(Ljava/lang/String;)V");

            if (mid != NULL) {
                env->CallStaticVoidMethod(globalobjclass, mid,
                                          env->NewStringUTF(
                                                  const_cast <char *>(call_para->file_path)));
            }
        }
        else {
            jmethodID mid = env->GetStaticMethodID(globalobjclass, "crashDumpBegin",
                                                   "(Ljava/lang/String;)V");
            if (mid != NULL) {
                if (call_para->type == 0) {
                    env->CallStaticVoidMethod(globalobjclass, mid, env->NewStringUTF("0"));
                } else
                {
                    env->CallStaticVoidMethod(globalobjclass, mid, env->NewStringUTF("1"));
                }
            }
        }

        g_jvm->DetachCurrentThread();
    }

    bool dump_callback(int type, const char *path, bool succeeded) {
        if (type != 1 || succeeded) {
            pthread_t thread;
            struct callbackPara para;
            para.file_path = path;
            para.type = type;

            int ret = pthread_create(&thread, NULL, do_call_back, (void *) &para);
            if (ret != 0) {
                return 0;
            }
            pthread_join(thread, NULL);
        }
        return succeeded;
    }
}
JNIEXPORT jint

JNICALL Java_com_disasterrecovery_jnicrash_NativeCrashCapture_nativeInit
        (JNIEnv *env, jobject obj, jstring crash_dump_path) {
    const char *path = (char *) env->GetStringUTFChars(crash_dump_path, NULL);
    static google_breakpad::ExceptionHandler eh(path, native_jnicrash::dump_callback, true);
    env->ReleaseStringUTFChars(crash_dump_path, path);

    jclass objclass = env->FindClass(
            "com/disasterrecovery/jnicrash/NativeCrashCapture");
    globalobjclass = reinterpret_cast<jclass>(env->NewGlobalRef(objclass));
    env->DeleteLocalRef(objclass);

    return 1;
}

JNIEXPORT jint

JNICALL Java_com_disasterrecovery_jnicrash_NativeCrashCapture_nativeCrash
        (JNIEnv *env, jobject obj) {
    int j = 0;
    int i = 10 / j;
//    int* zero = 0;
//    *zero = 0;
    return i;
}
