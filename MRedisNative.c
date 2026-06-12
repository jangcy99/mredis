#include <jni.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mredis_types.h"
#include "mredis_core.h"
#include "cmd_dispatch.h"

/* Forward declarations */
static s_replyObject* strings_to_args(JNIEnv* env, jobjectArray jargs, string_t*** out_args, uint32_t* out_argc);
static void free_args(string_t** args, uint32_t argc);
static jobject reply_to_java(JNIEnv* env, s_replyObject* reply);

/* ==================== Native Methods ==================== */

JNIEXPORT jlong JNICALL Java_com_mredis_MRedis_nativeCreate
    (JNIEnv* env, jobject obj __attribute((unused)), jstring jname, jlong size) {
    const char* name = (*env)->GetStringUTFChars(env, jname, NULL);
    MRedisHandle* h = mredis_create(name, (uint64_t)size);
    (*env)->ReleaseStringUTFChars(env, jname, name);
    return (jlong)(intptr_t)h;
}

JNIEXPORT jlong JNICALL Java_com_mredis_MRedis_nativeOpen
    (JNIEnv* env, jobject obj __attribute((unused)), jstring jname) {
    const char* name = (*env)->GetStringUTFChars(env, jname, NULL);
    MRedisHandle* h = mredis_open_existing(name);
    (*env)->ReleaseStringUTFChars(env, jname, name);
    return (jlong)(intptr_t)h;
}

JNIEXPORT void JNICALL Java_com_mredis_MRedis_nativeClose
    (JNIEnv* env __attribute((unused)), jobject obj __attribute((unused)), jlong handle) {
    MRedisHandle* h = (MRedisHandle*)(intptr_t)handle;
    if (h) mredis_close(h);
}

JNIEXPORT void JNICALL Java_com_mredis_MRedis_nativeDestroy
    (JNIEnv* env, jobject obj __attribute((unused)), jstring jname) {
    const char* name = (*env)->GetStringUTFChars(env, jname, NULL);
    mredis_destroy(name);
    (*env)->ReleaseStringUTFChars(env, jname, name);
}

JNIEXPORT jobject JNICALL Java_com_mredis_MRedis_nativeExec
    (JNIEnv* env, jobject obj __attribute((unused)), jlong handle, jobjectArray jargs) {
    MRedisHandle* h = (MRedisHandle*)(intptr_t)handle;
    if (!h) return NULL;

    string_t** args = NULL;
    uint32_t argc = 0;
    s_replyObject* reply = NULL;

    if (strings_to_args(env, jargs, &args, &argc) != NULL) {
        reply = cmd_dispatch(h, args, argc);
        free_args(args, argc);
    }

    jobject jreply = reply_to_java(env, reply);
    if (reply) reply_free(reply);
    return jreply;
}

/* ==================== Helper Functions ==================== */

static s_replyObject* strings_to_args(JNIEnv* env, jobjectArray jargs, string_t*** out_args, uint32_t* out_argc) {
    *out_args = NULL;
    *out_argc = 0;

    jsize len = (*env)->GetArrayLength(env, jargs);
    if (len == 0) return NULL;

    string_t** args = (string_t**)calloc(len + 1, sizeof(string_t*));
    if (!args) return NULL;

    for (jsize i = 0; i < len; i++) {
        jstring jstr = (jstring)(*env)->GetObjectArrayElement(env, jargs, i);
        if (jstr) {
            const char* cstr = (*env)->GetStringUTFChars(env, jstr, NULL);
            uint32_t slen = (uint32_t)(*env)->GetStringUTFLength(env, jstr);
            args[i] = string_new(cstr, slen);
            (*env)->ReleaseStringUTFChars(env, jstr, cstr);
            (*env)->DeleteLocalRef(env, jstr);
        }
    }
    *out_args = args;
    *out_argc = (uint32_t)len;
    return (s_replyObject*)1; // success marker
}

static void free_args(string_t** args, uint32_t argc) {
    if (!args) return;
    for (uint32_t i = 0; i < argc; i++) {
        if (args[i]) string_del(args[i]);
    }
    free(args);
}

/* Convert C reply to Java Reply object */
static jobject reply_to_java(JNIEnv* env, s_replyObject* r) {
//printf ("LINE:%d\n", __LINE__);fflush(stdout);
    if (!r) return NULL;

//printf ("LINE:%d\n", __LINE__);fflush(stdout);
    jclass replyClass = (*env)->FindClass(env, "com/mredis/MRedis$Reply");
//printf ("LINE:%d\n", __LINE__);fflush(stdout);
    if (!replyClass) return NULL;
//printf ("LINE:%d\n", __LINE__);fflush(stdout);

    jmethodID ctor = (*env)->GetMethodID(env, replyClass, "<init>", "()V");
//printf ("LINE:%d\n", __LINE__);fflush(stdout);
    jobject jreply = (*env)->NewObject(env, replyClass, ctor);
//printf ("LINE:%d\n", __LINE__);fflush(stdout);

    jfieldID typeField = (*env)->GetFieldID(env, replyClass, "type", "I");
//printf ("LINE:%d\n", __LINE__);fflush(stdout);
    (*env)->SetIntField(env, jreply, typeField, r->type);
//printf ("LINE:%d\n", __LINE__);fflush(stdout);

    if (r->type == REPLY_INTEGER) {
//printf ("LINE:%d\n", __LINE__);fflush(stdout);
        jfieldID intField = (*env)->GetFieldID(env, replyClass, "integer", "J");
        (*env)->SetLongField(env, jreply, intField, r->integer);
    } else if (r->type == REPLY_DOUBLE) {
//printf ("LINE:%d\n", __LINE__);fflush(stdout);
        jfieldID dField = (*env)->GetFieldID(env, replyClass, "dval", "D");
        (*env)->SetDoubleField(env, jreply, dField, r->dval);
    } else if (r->type == REPLY_STRING) {
//printf ("LINE:%d\n", __LINE__);fflush(stdout);
        jfieldID strField = (*env)->GetFieldID(env, replyClass, "string", "Ljava/lang/String;");
        jstring jstr = (*env)->NewStringUTF(env, (const char*)r->ptr);
        (*env)->SetObjectField(env, jreply, strField, jstr);
    }

//printf ("LINE:%d\n", __LINE__);fflush(stdout);
    if (r->type == REPLY_ARRAY && r->elements) {
//printf ("LINE:%d\n", __LINE__);fflush(stdout);
        jfieldID elemsField = (*env)->GetFieldID(env, replyClass, "elements", "Ljava/util/List;");
//printf ("LINE:%d\n", __LINE__);fflush(stdout);
        jobject list = (*env)->GetObjectField(env, jreply, elemsField);
//printf ("LINE:%d\n", __LINE__);fflush(stdout);

        jclass arrayListClass = (*env)->FindClass(env, "java/util/ArrayList");
//printf ("LINE:%d\n", __LINE__);fflush(stdout);
        jmethodID addMethod = (*env)->GetMethodID(env, arrayListClass, "add", "(Ljava/lang/Object;)Z");
//printf ("LINE:%d\n", __LINE__);fflush(stdout);

        for (size_t i = 0; i < r->elements; i++) {
//printf ("LINE:%d\n", __LINE__);fflush(stdout);
            jobject child = reply_to_java(env, r->element[i]);
//printf ("LINE:%d\n", __LINE__);fflush(stdout);
            if (child) {
//printf ("LINE:%d\n", __LINE__);fflush(stdout);
                (*env)->CallBooleanMethod(env, list, addMethod, child);
//printf ("LINE:%d\n", __LINE__);fflush(stdout);
                (*env)->DeleteLocalRef(env, child);
//printf ("LINE:%d\n", __LINE__);fflush(stdout);
            }
//printf ("LINE:%d\n", __LINE__);fflush(stdout);
        }
//printf ("LINE:%d\n", __LINE__);fflush(stdout);
    }
//printf ("LINE:%d\n", __LINE__);fflush(stdout);

    return jreply;
}
