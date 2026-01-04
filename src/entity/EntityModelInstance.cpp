#include "Rigel/Entity/EntityModelInstance.h"

#include "Rigel/Entity/Entity.h"
#include "Rigel/Asset/Types.h"

#include <spdlog/spdlog.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstddef>

namespace Rigel::Entity {

namespace {
constexpr glm::vec3 kCubeCorners[8] = {
    {-0.5f, -0.5f, -0.5f}, // 000
    {-0.5f,  0.5f, -0.5f}, // 010
    { 0.5f, -0.5f, -0.5f}, // 100
    { 0.5f,  0.5f, -0.5f}, // 110
    {-0.5f, -0.5f,  0.5f}, // 001
    {-0.5f,  0.5f,  0.5f}, // 011
    { 0.5f, -0.5f,  0.5f}, // 101
    { 0.5f,  0.5f,  0.5f}  // 111
};

struct FaceUvRect {
    glm::vec2 a{0.0f};
    glm::vec2 b{1.0f};
};

glm::mat4 rotationYawPitchRoll(const glm::vec3& degrees) {
    glm::mat4 rot(1.0f);
    rot = glm::rotate(rot, glm::radians(degrees.y), glm::vec3(0.0f, 1.0f, 0.0f));
    rot = glm::rotate(rot, glm::radians(degrees.x), glm::vec3(1.0f, 0.0f, 0.0f));
    rot = glm::rotate(rot, glm::radians(degrees.z), glm::vec3(0.0f, 0.0f, 1.0f));
    return rot;
}

void setFaceUv(FaceUvRect& rect, const glm::vec2& uvA, const glm::vec2& uvB,
               float invW, float invH) {
    rect.a = glm::vec2(uvA.x * invW, 1.0f - (uvA.y * invH));
    rect.b = glm::vec2(uvB.x * invW, 1.0f - (uvB.y * invH));
}

void computeFaceUvs(const EntityModelCube& cube,
                    const EntityModelAsset& model,
                    std::array<FaceUvRect, 6>& out) {
    if (!cube.hasUv || model.texWidth <= 0.0f || model.texHeight <= 0.0f) {
        for (auto& rect : out) {
            rect = {};
        }
        return;
    }

    float csx = std::floor(cube.size.x);
    float csy = std::floor(cube.size.y);
    float csz = std::floor(cube.size.z);
    float u0 = cube.uv.x;
    float v0 = cube.uv.y;
    float invW = 1.0f / model.texWidth;
    float invH = 1.0f / model.texHeight;

    glm::vec2 uvA;
    glm::vec2 uvB;

    // -Z
    uvB = glm::vec2(u0 + csz, v0 + csz);
    uvA = uvB + glm::vec2(csx, csy);
    if (cube.mirror) {
        uvB = glm::vec2(u0 + csz + csx, v0 + csz);
        uvA = uvB + glm::vec2(-csx, csy);
    }
    setFaceUv(out[0], uvA, uvB, invW, invH);

    // +Z
    uvA = glm::vec2(u0 + csz + csx + csz, v0 + csz);
    uvB = uvA + glm::vec2(csx, csy);
    if (cube.mirror) {
        uvA = glm::vec2(u0 + csz + csx + csx + csz, v0 + csz);
        uvB = uvA + glm::vec2(-csx, csy);
    }
    setFaceUv(out[1], uvA, uvB, invW, invH);

    // -Y
    uvB = glm::vec2(u0 + csz + csx, v0 + csz);
    uvA = uvB + glm::vec2(csx, -csz);
    if (cube.mirror) {
        uvB = glm::vec2(u0 + csz + csx + csx, v0 + csz);
        uvA = uvB + glm::vec2(-csx, -csz);
    }
    setFaceUv(out[2], uvA, uvB, invW, invH);

    // +Y
    uvB = glm::vec2(u0 + csz, v0);
    uvA = uvB + glm::vec2(csx, csz);
    if (cube.mirror) {
        uvB = glm::vec2(u0 + csz + csx, v0);
        uvA = uvB + glm::vec2(-csx, csz);
    }
    setFaceUv(out[3], uvA, uvB, invW, invH);

    // -X
    uvB = glm::vec2(u0 + csx + csz, v0 + csz);
    uvA = uvB + glm::vec2(csz, csy);
    if (cube.mirror) {
        uvB = glm::vec2(u0 + csz, v0 + csz);
        uvA = uvB + glm::vec2(-csz, csy);
    }
    setFaceUv(out[4], uvA, uvB, invW, invH);

    // +X
    uvB = glm::vec2(u0, v0 + csz);
    uvA = uvB + glm::vec2(csz, csy);
    if (cube.mirror) {
        uvA = glm::vec2(u0 + csz + csz + csx, v0 + csz);
        uvB = uvA + glm::vec2(-csz, csy);
    }
    setFaceUv(out[5], uvA, uvB, invW, invH);
}
}

EntityModelInstance::EntityModelInstance(std::shared_ptr<const EntityModelAsset> model,
                                         Asset::Handle<Asset::ShaderAsset> shader,
                                         std::unordered_map<std::string, Asset::Handle<Asset::TextureAsset>> textures)
    : m_model(std::move(model))
    , m_shader(std::move(shader))
    , m_textures(std::move(textures))
{
    if (m_model && m_model->animationSet) {
        m_animationSet = &m_model->animationSet->set;
    }
    if (m_model && !m_model->defaultAnimation.empty() && m_animationSet) {
        const EntityAnimation* anim = m_animationSet->find(m_model->defaultAnimation);
        if (anim) {
            m_activeAnimations.push_back(anim);
        }
    }
}

EntityModelInstance::~EntityModelInstance() {
    releaseGpuResources();
}

void EntityModelInstance::addAnimation(std::string_view name) {
    if (!m_animationSet) {
        return;
    }
    const EntityAnimation* anim = m_animationSet->find(name);
    if (!anim) {
        return;
    }
    if (std::find(m_activeAnimations.begin(), m_activeAnimations.end(), anim) == m_activeAnimations.end()) {
        m_activeAnimations.push_back(anim);
        m_meshDirty = true;
    }
}

void EntityModelInstance::removeAnimation(std::string_view name) {
    if (!m_animationSet) {
        return;
    }
    const EntityAnimation* anim = m_animationSet->find(name);
    if (!anim) {
        return;
    }
    removeAnimation(anim);
}

void EntityModelInstance::removeAnimation(const EntityAnimation* animation) {
    auto it = std::remove(m_activeAnimations.begin(), m_activeAnimations.end(), animation);
    m_activeAnimations.erase(it, m_activeAnimations.end());
    m_meshDirty = true;
}

void EntityModelInstance::render(const EntityRenderContext& ctx,
                                 Entity& entity,
                                 const glm::mat4& modelMatrix,
                                 bool shouldRender) {
    (void)entity;
    updateAnimations(ctx);
    if (m_meshDirty) {
        rebuildMesh(ctx);
    }
    if (!shouldRender || m_cpuVertices.empty() || !m_shader) {
        return;
    }

    ensureGpuResources();
    if (m_vertexCount == 0) {
        return;
    }

    m_shader->bind();
    GLint locViewProjection = m_shader->uniform("u_viewProjection");
    if (locViewProjection >= 0) {
        glUniformMatrix4fv(locViewProjection, 1, GL_FALSE, glm::value_ptr(ctx.viewProjection));
    }
    GLint locModel = m_shader->uniform("u_model");
    if (locModel >= 0) {
        glUniformMatrix4fv(locModel, 1, GL_FALSE, glm::value_ptr(modelMatrix));
    }
    GLint locSun = m_shader->uniform("u_sunDirection");
    if (locSun >= 0) {
        glUniform3fv(locSun, 1, glm::value_ptr(ctx.sunDirection));
    }
    GLint locCameraPos = m_shader->uniform("u_cameraPos");
    if (locCameraPos >= 0) {
        glUniform3fv(locCameraPos, 1, glm::value_ptr(ctx.cameraPos));
    }
    GLint locAmbient = m_shader->uniform("u_ambientStrength");
    if (locAmbient >= 0) {
        glUniform1f(locAmbient, ctx.ambientStrength);
    }
    GLint locAo = m_shader->uniform("u_ao");
    if (locAo >= 0) {
        glUniform1f(locAo, ctx.ambientOcclusion);
    }
    GLint locTint = m_shader->uniform("u_tintColor");
    if (locTint >= 0) {
        glUniform4fv(locTint, 1, glm::value_ptr(m_tint));
    }

    GLint locDiffuse = m_shader->uniform("u_diffuse");
    auto texIt = m_textures.find("diffuse");
    if (locDiffuse >= 0) {
        if (texIt != m_textures.end() && texIt->second) {
            texIt->second->bind(GL_TEXTURE0);
            glUniform1i(locDiffuse, 0);
        }
    }

    GLint locEmission = m_shader->uniform("u_emission");
    GLint locHasEmission = m_shader->uniform("u_hasEmission");
    auto emissionIt = m_textures.find("emission");
    bool hasEmission = emissionIt != m_textures.end() && emissionIt->second;
    if (locEmission >= 0 && hasEmission) {
        emissionIt->second->bind(GL_TEXTURE1);
        glUniform1i(locEmission, 1);
    }
    if (locHasEmission >= 0) {
        glUniform1i(locHasEmission, hasEmission ? 1 : 0);
    }

    GLint locShadowEnabled = m_shader->uniform("u_shadowEnabled");
    GLint locShadowMap = m_shader->uniform("u_shadowMap");
    GLint locShadowTransmittanceMap = m_shader->uniform("u_shadowTransmittanceMap");
    GLint locShadowMatrices = m_shader->uniform("u_shadowMatrices");
    GLint locShadowSplits = m_shader->uniform("u_shadowSplits");
    GLint locShadowCascadeCount = m_shader->uniform("u_shadowCascadeCount");
    GLint locShadowBias = m_shader->uniform("u_shadowBias");
    GLint locShadowNormalBias = m_shader->uniform("u_shadowNormalBias");
    GLint locShadowPcfNear = m_shader->uniform("u_shadowPcfNear");
    GLint locShadowPcfFar = m_shader->uniform("u_shadowPcfFar");
    GLint locShadowStrength = m_shader->uniform("u_shadowStrength");
    GLint locShadowNear = m_shader->uniform("u_shadowNear");
    GLint locShadowFadeStart = m_shader->uniform("u_shadowFadeStart");
    GLint locShadowFadePower = m_shader->uniform("u_shadowFadePower");

    bool shadowsActive = ctx.shadow.enabled && ctx.shadow.cascadeCount > 0 &&
        ctx.shadow.depthMap != 0;
    if (locShadowEnabled >= 0) {
        glUniform1i(locShadowEnabled, shadowsActive ? 1 : 0);
    }
    if (shadowsActive) {
        constexpr int kShadowMapUnit = 2;
        constexpr int kShadowTransmitUnit = 3;

        if (locShadowMap >= 0) {
            glActiveTexture(GL_TEXTURE0 + kShadowMapUnit);
            glBindTexture(GL_TEXTURE_2D_ARRAY, ctx.shadow.depthMap);
            glUniform1i(locShadowMap, kShadowMapUnit);
        }
        if (locShadowTransmittanceMap >= 0) {
            glActiveTexture(GL_TEXTURE0 + kShadowTransmitUnit);
            glBindTexture(GL_TEXTURE_2D_ARRAY, ctx.shadow.transmittanceMap);
            glUniform1i(locShadowTransmittanceMap, kShadowTransmitUnit);
        }
        if (locShadowMatrices >= 0) {
            glUniformMatrix4fv(locShadowMatrices,
                               ctx.shadow.cascadeCount,
                               GL_FALSE,
                               glm::value_ptr(ctx.shadow.matrices[0]));
        }
        if (locShadowSplits >= 0) {
            glUniform1fv(locShadowSplits,
                         ctx.shadow.cascadeCount,
                         ctx.shadow.splits.data());
        }
        if (locShadowCascadeCount >= 0) {
            glUniform1i(locShadowCascadeCount, ctx.shadow.cascadeCount);
        }
        if (locShadowBias >= 0) {
            glUniform1f(locShadowBias, ctx.shadow.bias);
        }
        if (locShadowNormalBias >= 0) {
            glUniform1f(locShadowNormalBias, ctx.shadow.normalBias);
        }
        if (locShadowPcfNear >= 0) {
            glUniform1f(locShadowPcfNear, ctx.shadow.pcfNear);
        }
        if (locShadowPcfFar >= 0) {
            glUniform1f(locShadowPcfFar, ctx.shadow.pcfFar);
        }
        if (locShadowStrength >= 0) {
            glUniform1f(locShadowStrength, ctx.shadow.strength);
        }
        if (locShadowNear >= 0) {
            glUniform1f(locShadowNear, ctx.shadow.nearPlane);
        }
        if (locShadowFadeStart >= 0) {
            glUniform1f(locShadowFadeStart, ctx.shadow.fadeStart);
        }
        if (locShadowFadePower >= 0) {
            glUniform1f(locShadowFadePower, ctx.shadow.fadePower);
        }
    } else if (locShadowCascadeCount >= 0) {
        glUniform1i(locShadowCascadeCount, 0);
    }

    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glBindVertexArray(m_vao);
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(m_vertexCount));

    glDisable(GL_BLEND);
    glEnable(GL_CULL_FACE);
    glBindVertexArray(0);
    glUseProgram(0);
}

void EntityModelInstance::renderShadow(const EntityRenderContext& ctx,
                                       Entity& entity,
                                       const glm::mat4& modelMatrix,
                                       const glm::mat4& lightViewProjection,
                                       const Asset::Handle<Asset::ShaderAsset>& shadowShader,
                                       bool shouldRender) {
    (void)entity;
    if (!shadowShader) {
        return;
    }
    updateAnimations(ctx);
    if (m_meshDirty) {
        rebuildMesh(ctx);
    }
    if (!shouldRender || m_cpuVertices.empty()) {
        return;
    }

    ensureGpuResources();
    if (m_vertexCount == 0) {
        return;
    }

    shadowShader->bind();
    GLint locLightVP = shadowShader->uniform("u_lightViewProjection");
    if (locLightVP >= 0) {
        glUniformMatrix4fv(locLightVP, 1, GL_FALSE, glm::value_ptr(lightViewProjection));
    }
    GLint locModel = shadowShader->uniform("u_model");
    if (locModel >= 0) {
        glUniformMatrix4fv(locModel, 1, GL_FALSE, glm::value_ptr(modelMatrix));
    }
    GLint locDiffuse = shadowShader->uniform("u_diffuse");
    GLint locUseAlphaTest = shadowShader->uniform("u_useAlphaTest");
    auto diffuseIt = m_textures.find("diffuse");
    bool hasDiffuse = diffuseIt != m_textures.end() && diffuseIt->second;
    if (locDiffuse >= 0 && hasDiffuse) {
        diffuseIt->second->bind(GL_TEXTURE0);
        glUniform1i(locDiffuse, 0);
    }
    if (locUseAlphaTest >= 0) {
        glUniform1i(locUseAlphaTest, hasDiffuse ? 1 : 0);
    }

    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);

    glBindVertexArray(m_vao);
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(m_vertexCount));

    glBindVertexArray(0);
    glUseProgram(0);
}

void EntityModelInstance::updateAnimations(const EntityRenderContext& ctx) {
    if (m_activeAnimations.empty()) {
        return;
    }
    if (ctx.frameIndex != 0 && ctx.frameIndex == m_lastAnimFrame) {
        return;
    }
    if (ctx.deltaTime <= 0.0f) {
        return;
    }
    if (ctx.frameIndex != 0) {
        m_lastAnimFrame = ctx.frameIndex;
    }
    m_globalAnimTime += ctx.deltaTime;
    m_meshDirty = true;
}

glm::vec3 EntityModelInstance::sampleTrack(const EntityAnimationTrack& track,
                                           const EntityAnimation& animation,
                                           const glm::vec3& defaultValue) const {
    return track.sample(m_globalAnimTime, animation.loop, animation.duration, defaultValue);
}

void EntityModelInstance::rebuildMesh(const EntityRenderContext& ctx) {
    (void)ctx;
    m_cpuVertices.clear();
    m_vertexCount = 0;
    m_meshDirty = false;

    if (!m_model) {
        return;
    }

    std::vector<glm::mat4> boneTransforms(m_model->bones.size(), glm::mat4(1.0f));
    for (size_t i = 0; i < m_model->bones.size(); ++i) {
        const EntityBone& bone = m_model->bones[i];
        glm::vec3 animPos(0.0f);
        glm::vec3 animRot(0.0f);
        glm::vec3 animScale(1.0f);

        for (const EntityAnimation* animation : m_activeAnimations) {
            if (!animation) {
                continue;
            }
            if (const EntityBoneAnimation* boneAnim = animation->findBone(bone.name)) {
                animPos += sampleTrack(boneAnim->position, *animation, glm::vec3(0.0f));
                animRot += sampleTrack(boneAnim->rotation, *animation, glm::vec3(0.0f));
                glm::vec3 scaleSample = sampleTrack(boneAnim->scale, *animation, glm::vec3(1.0f));
                animScale *= scaleSample;
            }
        }

        glm::vec3 totalRot = bone.rotation + animRot;
        glm::mat4 local(1.0f);
        local = glm::translate(local, bone.pivot);
        local = glm::scale(local, bone.scale * animScale);
        local = glm::rotate(local, glm::radians(totalRot.z), glm::vec3(0.0f, 0.0f, 1.0f));
        local = glm::rotate(local, glm::radians(-totalRot.y), glm::vec3(0.0f, 1.0f, 0.0f));
        local = glm::rotate(local, glm::radians(-totalRot.x), glm::vec3(1.0f, 0.0f, 0.0f));
        local = glm::translate(local, -bone.pivot);
        local = glm::translate(local, animPos);

        if (bone.parentIndex >= 0) {
            boneTransforms[i] = boneTransforms[static_cast<size_t>(bone.parentIndex)] * local;
        } else {
            boneTransforms[i] = local;
        }
    }

    glm::mat4 scaleMat(1.0f);
    if (m_model->modelScale != 1.0f) {
        scaleMat = glm::scale(scaleMat, glm::vec3(m_model->modelScale));
    }

    constexpr size_t kFaceVertices = 6;
    constexpr size_t kCubeFaces = 6;
    m_cpuVertices.reserve(m_model->bones.size() * kFaceVertices * kCubeFaces);

    for (size_t boneIndex = 0; boneIndex < m_model->bones.size(); ++boneIndex) {
        const EntityBone& bone = m_model->bones[boneIndex];
        const glm::mat4& boneTransform = boneTransforms[boneIndex];

        for (const EntityModelCube& cube : bone.cubes) {
            glm::vec3 size = cube.size;
            glm::vec3 pivot = cube.pivot;
            float inflate = cube.inflate;

            glm::mat4 cubeMat(1.0f);
            cubeMat = glm::translate(cubeMat, cube.origin);
            cubeMat = glm::translate(cubeMat, size * 0.5f);
            cubeMat = glm::scale(cubeMat, size + glm::vec3(inflate));

            glm::mat4 rotMat(1.0f);
            rotMat = glm::translate(rotMat, pivot);
            rotMat *= rotationYawPitchRoll(cube.rotation);
            rotMat = glm::translate(rotMat, -pivot);
            cubeMat = rotMat * cubeMat;

            glm::mat4 finalTransform = scaleMat * boneTransform * cubeMat;
            std::array<glm::vec3, 8> corners{};
            for (size_t c = 0; c < corners.size(); ++c) {
                glm::vec4 pos = finalTransform * glm::vec4(kCubeCorners[c], 1.0f);
                corners[c] = glm::vec3(pos);
            }

            std::array<FaceUvRect, 6> faceUvs{};
            computeFaceUvs(cube, *m_model, faceUvs);

            glm::vec3 normalZ = glm::normalize((corners[0] + corners[3]) * 0.5f -
                                               (corners[4] + corners[7]) * 0.5f);
            glm::vec3 normalY = glm::normalize((corners[0] + corners[6]) * 0.5f -
                                               (corners[1] + corners[7]) * 0.5f);
            glm::vec3 normalX = glm::normalize((corners[0] + corners[5]) * 0.5f -
                                               (corners[2] + corners[7]) * 0.5f);

            auto emitRect = [&](const glm::vec3& c00,
                                const glm::vec3& c10,
                                const glm::vec3& c11,
                                const glm::vec3& c01,
                                const glm::vec3& normal,
                                const FaceUvRect& uv) {
                Vertex v0{c00, normal, glm::vec2(uv.a.x, uv.a.y)};
                Vertex v1{c10, normal, glm::vec2(uv.a.x, uv.b.y)};
                Vertex v2{c11, normal, glm::vec2(uv.b.x, uv.b.y)};
                Vertex v3{c01, normal, glm::vec2(uv.b.x, uv.a.y)};
                m_cpuVertices.push_back(v0);
                m_cpuVertices.push_back(v1);
                m_cpuVertices.push_back(v2);
                m_cpuVertices.push_back(v0);
                m_cpuVertices.push_back(v2);
                m_cpuVertices.push_back(v3);
            };

            emitRect(corners[0], corners[1], corners[3], corners[2], normalZ, faceUvs[0]);       // -Z
            emitRect(corners[5], corners[4], corners[6], corners[7], -normalZ, faceUvs[1]);      // +Z
            emitRect(corners[4], corners[0], corners[2], corners[6], normalY, faceUvs[2]);       // -Y
            emitRect(corners[1], corners[5], corners[7], corners[3], -normalY, faceUvs[3]);      // +Y
            emitRect(corners[4], corners[5], corners[1], corners[0], normalX, faceUvs[4]);       // -X
            emitRect(corners[2], corners[3], corners[7], corners[6], -normalX, faceUvs[5]);      // +X
        }
    }

    m_vertexCount = m_cpuVertices.size();

    if (m_vao != 0 && m_vbo != 0) {
        glBindVertexArray(m_vao);
        glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(m_cpuVertices.size() * sizeof(Vertex)),
                     m_cpuVertices.data(),
                     GL_DYNAMIC_DRAW);
    }
}

void EntityModelInstance::ensureGpuResources() {
    if (m_vao == 0) {
        glGenVertexArrays(1, &m_vao);
    }
    if (m_vbo == 0) {
        glGenBuffers(1, &m_vbo);
    }

    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(m_cpuVertices.size() * sizeof(Vertex)),
                 m_cpuVertices.data(),
                 GL_DYNAMIC_DRAW);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          reinterpret_cast<void*>(offsetof(Vertex, position)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          reinterpret_cast<void*>(offsetof(Vertex, normal)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          reinterpret_cast<void*>(offsetof(Vertex, uv)));

    glBindVertexArray(0);
}

void EntityModelInstance::releaseGpuResources() {
    if (m_vao != 0) {
        glDeleteVertexArrays(1, &m_vao);
        m_vao = 0;
    }
    if (m_vbo != 0) {
        glDeleteBuffers(1, &m_vbo);
        m_vbo = 0;
    }
    m_vertexCount = 0;
}

} // namespace Rigel::Entity
