#include <jni.h>
#include "mg_chunk_drawer.h"
#include "../gles/loader.h"

extern "C" {

JNIEXPORT jboolean JNICALL Java_com_denonexus_mgshaders_client_render_MGNativeDrawer_init(JNIEnv* env, jclass clazz) {
    return mg_chunk_drawer::Drawer::get().initialize() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jlong JNICALL Java_com_denonexus_mgshaders_client_render_MGNativeDrawer_createBatch(JNIEnv* env, jclass clazz) {
    auto* batch = new mg_chunk_drawer::Batch();
    return reinterpret_cast<jlong>(batch);
}

JNIEXPORT void JNICALL Java_com_denonexus_mgshaders_client_render_MGNativeDrawer_uploadBatch(
    JNIEnv* env, jclass clazz, jlong batchHandle, jintArray counts, jlongArray offsets, jintArray baseVertices, jint drawCount) 
{
    auto* batch = reinterpret_cast<mg_chunk_drawer::Batch*>(batchHandle);
    if (!batch) return;

    batch->size = drawCount;
    batch->counts.resize(drawCount);
    batch->indices.resize(drawCount);
    batch->baseVertices.resize(drawCount);

    if (drawCount > 0) {
        env->GetIntArrayRegion(counts, 0, drawCount, batch->counts.data());
        env->GetIntArrayRegion(baseVertices, 0, drawCount, batch->baseVertices.data());
        
        jlong* offsetsPtr = env->GetLongArrayElements(offsets, nullptr);
        for (int i = 0; i < drawCount; ++i) {
            batch->indices[i] = reinterpret_cast<const void*>(static_cast<uintptr_t>(offsetsPtr[i]));
        }
        env->ReleaseLongArrayElements(offsets, offsetsPtr, JNI_ABORT);
    }
}

JNIEXPORT void JNICALL Java_com_denonexus_mgshaders_client_render_MGNativeDrawer_drawBatch(
    JNIEnv* env, jclass clazz, jlong batchHandle, jint vao, jint vertexBuffer, jint indexBuffer, jint indexType) 
{
    auto* batch = reinterpret_cast<mg_chunk_drawer::Batch*>(batchHandle);
    if (!batch) return;

    mg_chunk_drawer::Drawer::get().draw(*batch, vao, vertexBuffer, indexBuffer, indexType);
}

JNIEXPORT void JNICALL Java_com_denonexus_mgshaders_client_render_MGNativeDrawer_destroyBatch(
    JNIEnv* env, jclass clazz, jlong batchHandle) 
{
    auto* batch = reinterpret_cast<mg_chunk_drawer::Batch*>(batchHandle);
    delete batch;
}

}
