#pragma once

#include <GLES3/gl32.h>
#include <EGL/egl.h>
#include <cstdint>
#include <cstdlib>
#include <vector>
#include "glcorearb.h"
#include "../gles/loader.h"

namespace mg_chunk_drawer {

class Batch final {
public:
    std::vector<GLsizei> counts;
    std::vector<const void*> indices;
    std::vector<GLint> baseVertices;
    GLsizei size = 0;
};

class Drawer final {
public:
    static Drawer& get() {
        static Drawer instance;
        return instance;
    }

    bool initialize() noexcept {
        return GLES.glMultiDrawElementsBaseVertexEXT != nullptr;
    }

    void draw(const Batch& batch, GLuint vao, GLuint vertexBuffer, GLuint indexBuffer, GLenum indexType) noexcept {
        if (batch.size == 0) return;
        if (!GLES.glMultiDrawElementsBaseVertexEXT) return;

        // Note: The user mentioned "bind somente se necessário" - a State Cache for VAO and VBOs.
        // We'll keep it simple for this drawer class, or we can use MobileGlues' cache if it exists.
        // But for now, we bind the state for the JNI call.
        GLint prev_vao = 0;
        GLint prev_vbo = 0;
        GLint prev_ibo = 0;
        
        GLES.glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prev_vao);
        GLES.glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prev_vbo);
        GLES.glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &prev_ibo);

        if (prev_vao != vao) GLES.glBindVertexArray(vao);
        if (prev_vbo != vertexBuffer) GLES.glBindBuffer(GL_ARRAY_BUFFER, vertexBuffer);
        if (prev_ibo != indexBuffer) GLES.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indexBuffer);

        GLES.glMultiDrawElementsBaseVertexEXT(
            GL_TRIANGLES,
            batch.counts.data(),
            indexType,
            batch.indices.data(),
            batch.size,
            batch.baseVertices.data()
        );

        // Restore state if needed (or assume the renderer manages it)
        if (prev_vao != vao) GLES.glBindVertexArray(prev_vao);
        if (prev_vbo != vertexBuffer) GLES.glBindBuffer(GL_ARRAY_BUFFER, prev_vbo);
        if (prev_ibo != indexBuffer) GLES.glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, prev_ibo);
    }

private:
    Drawer() = default;
};

}
