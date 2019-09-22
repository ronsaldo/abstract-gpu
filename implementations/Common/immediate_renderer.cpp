#include "immediate_renderer.hpp"
#include <stddef.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265359
#endif

namespace AgpuCommon
{
#include "shaders/compiled.inc"

LightState::LightState()
    :
    ambientColor(0.0f, 0.0f, 0.0f, 1.0f),
    diffuseColor(0.0f, 0.0f, 0.0f, 1.0f),
    specularColor(0.0f, 0.0f, 0.0f, 1.0f),

    position(0.0f, 0.0f, 1.0f, 0.0f),

    spotDirection(0.0f, 0.0f, -1.0f),
    spotCosCutoff(-1.0f),

    spotExponent(0.0f),
    constantAttenuation(1.0f),
    linearAttenuation(0.0f),
    quadraticAttenuation(0.0f)
{
}

size_t LightState::hash() const
{
    return
        ambientColor.hash() ^
        diffuseColor.hash() ^
        specularColor.hash() ^

        position.hash() ^
        spotDirection.hash() ^
        std::hash<float> ()(spotCosCutoff) ^

        std::hash<float> () (spotExponent) ^
        std::hash<float> () (constantAttenuation) ^
        std::hash<float> () (linearAttenuation) ^
        std::hash<float> () (quadraticAttenuation);
    ;
}

bool LightState::operator==(const LightState &other) const
{
    return
        ambientColor == other.ambientColor &&
        diffuseColor == other.diffuseColor &&
        specularColor == other.specularColor &&

        position == other.position &&
        spotDirection == other.spotDirection &&
        spotCosCutoff == other.spotCosCutoff &&

        spotExponent == other.spotExponent &&
        constantAttenuation == other.constantAttenuation &&
        linearAttenuation == other.linearAttenuation &&
        quadraticAttenuation == other.quadraticAttenuation;
}

LightingState::LightingState()
    : ambientLighting(0.2f, 0.2f, 0.2f, 1.0f), enabledLightMask(1)
{
    lights[0].diffuseColor = Vector4F(1.0f, 1.0f, 1.0f, 1.0f);
    lights[0].specularColor = Vector4F(1.0f, 1.0f, 1.0f, 1.0f);
}

size_t LightingState::hash() const
{
    // TODO: Implement this.
    size_t result = ambientLighting.hash();
    for(auto & light : lights)
        result ^= light.hash();
    return result;
}

bool LightingState::operator==(const LightingState &other) const
{
    return ambientLighting == other.ambientLighting &&
        lights == other.lights;
}

MaterialState::MaterialState()
    :
    emission(0.0f, 0.0f, 0.0f, 1.0f),
    ambient(0.2f, 0.2f, 0.2f, 1.0f),
    diffuse(0.8f, 0.8f, 0.8f, 1.0f),
    specular(0.0f, 0.0f, 0.0f, 1.0f),
    shininess(0.0f)
{}

size_t MaterialState::hash() const
{
    return
        emission.hash() ^
        ambient.hash() ^
        diffuse.hash() ^
        specular.hash() ^
        std::hash<float> ()(shininess);
}

bool MaterialState::operator==(const MaterialState &other) const
{
    return
        emission == other.emission &&
        ambient == other.ambient &&
        diffuse == other.diffuse &&
        specular == other.specular &&
        shininess == other.shininess;
}

agpu_vertex_attrib_description ImmediateVertexAttributes[] = {
    {0, 0, AGPU_TEXTURE_FORMAT_R32G32B32_FLOAT, 1, offsetof(ImmediateRendererVertex, position), 0},
    {0, 1, AGPU_TEXTURE_FORMAT_R32G32B32A32_FLOAT, 1, offsetof(ImmediateRendererVertex, color), 0},
    {0, 2, AGPU_TEXTURE_FORMAT_R32G32B32_FLOAT, 1, offsetof(ImmediateRendererVertex, normal), 0},
    {0, 3, AGPU_TEXTURE_FORMAT_R32G32_FLOAT, 1, offsetof(ImmediateRendererVertex, texcoord), 0},
};

inline size_t nextPowerOfTwo(size_t v)
{
    // https://graphics.stanford.edu/~seander/bithacks.html#RoundUpPowerOf2
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    v++;
    return v;
}

static agpu::shader_ref loadSpirVShader(const agpu::device_ref &device, agpu_shader_type shaderType, const uint8_t *data, size_t dataSize)
{
    auto shader = agpu::shader_ref(device->createShader(shaderType));
    shader->setShaderSource(AGPU_SHADER_LANGUAGE_SPIR_V, (agpu_string)data, dataSize);
    auto error = shader->compileShader("");
    if(error)
        return agpu::shader_ref();

    return shader;
}

bool StateTrackerCache::ensureImmediateRendererObjectsExists()
{
    std::unique_lock<std::mutex> l(immediateRendererObjectsMutex);

    if(immediateRendererObjectsInitialized)
        return true;

    // Create the shader signature.
    {
        auto builder = agpu::shader_signature_builder_ref(device->createShaderSignatureBuilder());
        if(!builder) return false;

        builder->beginBindingBank(1);
        builder->addBindingBankElement(AGPU_SHADER_BINDING_TYPE_SAMPLER, 2);

        builder->beginBindingBank(100);
        builder->addBindingBankElement(AGPU_SHADER_BINDING_TYPE_STORAGE_BUFFER, 4);

        builder->beginBindingBank(1000);
        builder->addBindingBankElement(AGPU_SHADER_BINDING_TYPE_SAMPLED_IMAGE, 1);

        for(size_t i = 0; i < sizeof(ImmediatePushConstants)/4; ++i)
            builder->addBindingConstant();

        immediateShaderSignature = agpu::shader_signature_ref(builder->build());
        if(!immediateShaderSignature) return false;
    }

    // Create the immediate shader library.
    {
        immediateShaderLibrary.reset(new ImmediateShaderLibrary());
        if(!immediateShaderLibrary) return false;

        immediateShaderLibrary->flatColorVertex = loadSpirVShader(device, AGPU_VERTEX_SHADER, flatColor_vert_spv, flatColor_vert_spv_len);
        immediateShaderLibrary->flatLightedColorVertex = loadSpirVShader(device, AGPU_VERTEX_SHADER, flatLightedColor_vert_spv, flatLightedColor_vert_spv_len);
        immediateShaderLibrary->flatColorFragment = loadSpirVShader(device, AGPU_FRAGMENT_SHADER, flatColor_frag_spv, flatColor_frag_spv_len);

        immediateShaderLibrary->flatTexturedVertex = loadSpirVShader(device, AGPU_VERTEX_SHADER, flatTextured_vert_spv, flatTextured_vert_spv_len);
        immediateShaderLibrary->flatLightedTexturedVertex = loadSpirVShader(device, AGPU_VERTEX_SHADER, flatLightedTextured_vert_spv, flatLightedTextured_vert_spv_len);
        immediateShaderLibrary->flatTexturedFragment = loadSpirVShader(device, AGPU_FRAGMENT_SHADER, flatTextured_frag_spv, flatTextured_frag_spv_len);

        immediateShaderLibrary->smoothColorVertex = loadSpirVShader(device, AGPU_VERTEX_SHADER, smoothColor_vert_spv, smoothColor_vert_spv_len);
        immediateShaderLibrary->smoothLightedColorVertex = loadSpirVShader(device, AGPU_VERTEX_SHADER, smoothLightedColor_vert_spv, smoothLightedColor_vert_spv_len);
        immediateShaderLibrary->smoothColorFragment = loadSpirVShader(device, AGPU_FRAGMENT_SHADER, smoothColor_frag_spv, smoothColor_frag_spv_len);

        immediateShaderLibrary->smoothTexturedVertex = loadSpirVShader(device, AGPU_VERTEX_SHADER, smoothTextured_vert_spv, smoothTextured_vert_spv_len);
        immediateShaderLibrary->smoothLightedTexturedVertex = loadSpirVShader(device, AGPU_VERTEX_SHADER, smoothLightedTextured_vert_spv, smoothLightedTextured_vert_spv_len);
        immediateShaderLibrary->smoothTexturedFragment = loadSpirVShader(device, AGPU_FRAGMENT_SHADER, smoothTextured_frag_spv, smoothTextured_frag_spv_len);
    }

    {
        immediateSharedRenderingStates.reset(new ImmediateSharedRenderingStates());
        if(!immediateSharedRenderingStates) return false;

        agpu_sampler_description samplerDescription = {};
        samplerDescription.filter = AGPU_FILTER_MIN_LINEAR_MAG_LINEAR_MIPMAP_NEAREST;
        samplerDescription.max_lod = 1000.0f;
        auto sampler = agpu::sampler_ref(device->createSampler(&samplerDescription));
        if(!sampler) return false;

        auto samplerBinding = agpu::shader_resource_binding_ref(immediateShaderSignature->createShaderResourceBinding(0));
        samplerBinding->bindSampler(0, sampler);

        immediateSharedRenderingStates->linearSampler.sampler = sampler;
        immediateSharedRenderingStates->linearSampler.binding = samplerBinding;
    }

    // Create the immediate vertex layout.
    {
        immediateVertexLayout = agpu::vertex_layout_ref(device->createVertexLayout());
        if(!immediateVertexLayout) return false;

        agpu_size strides = sizeof(ImmediateRendererVertex);
        auto error = immediateVertexLayout->addVertexAttributeBindings(1, &strides,
            sizeof(ImmediateVertexAttributes) / sizeof(ImmediateVertexAttributes[0]),
            ImmediateVertexAttributes);
        if(error) return false;
    }

    immediateRendererObjectsInitialized = true;
    return true;
}

ImmediateRenderer::ImmediateRenderer(const agpu::state_tracker_cache_ref &stateTrackerCache)
    : stateTrackerCache(stateTrackerCache)
{
    vertexBufferCapacity = 0;
    indexBufferCapacity = 0;
    matrixBufferCapacity = 0;
    lightingStateBufferCapacity = 0;
    materialStateBufferCapacity = 0;
    usedTextureBindingCount = 0;
    userClipPlaneBufferCapacity = 0;
    activeMatrixStack = nullptr;

    auto impl = stateTrackerCache.as<StateTrackerCache> ();
    device = impl->device;

    immediateShaderSignature = impl->immediateShaderSignature;
    immediateShaderLibrary = impl->immediateShaderLibrary.get();
    immediateSharedRenderingStates = impl->immediateSharedRenderingStates.get();
    immediateVertexLayout = impl->immediateVertexLayout;
}

ImmediateRenderer::~ImmediateRenderer()
{
}

agpu::immediate_renderer_ref ImmediateRenderer::create(const agpu::state_tracker_cache_ref &cache)
{
    auto cacheImpl = cache.as<StateTrackerCache> ();
    if(!cacheImpl->ensureImmediateRendererObjectsExists())
        return agpu::immediate_renderer_ref();

    auto vertexBinding = agpu::vertex_binding_ref(cacheImpl->device->createVertexBinding(cacheImpl->immediateVertexLayout));
    if(!vertexBinding)
        return agpu::immediate_renderer_ref();

    auto uniformResourceBindings = agpu::shader_resource_binding_ref(cacheImpl->immediateShaderSignature->createShaderResourceBinding(1));
    if(!uniformResourceBindings)
        return agpu::immediate_renderer_ref();

    auto result = agpu::makeObject<ImmediateRenderer> (cache);
    result.as<ImmediateRenderer> ()->vertexBinding = vertexBinding;
    result.as<ImmediateRenderer> ()->uniformResourceBindings = uniformResourceBindings;

    return result;
}

agpu_error ImmediateRenderer::beginRendering(const agpu::state_tracker_ref &state_tracker)
{
    if(!state_tracker)
        return AGPU_NULL_POINTER;
    if(currentStateTracker)
        return AGPU_INVALID_OPERATION;

    currentStateTracker = state_tracker;
    activeMatrixStack = nullptr;
    activeMatrixStackDirtyFlag = nullptr;

    // Reset the matrices.
    projectionMatrixStack.clear();
    projectionMatrixStack.push_back(Matrix4F::identity());
    projectionMatrixStackDirtyFlag = false;

    modelViewMatrixStack.clear();
    modelViewMatrixStack.push_back(Matrix4F::identity());
    modelViewMatrixStackDirtyFlag = false;

    textureMatrixStack.clear();
    textureMatrixStack.push_back(Matrix4F::identity());
    textureMatrixStackDirtyFlag = false;

    matrixBufferData.clear();
    matrixBufferData.push_back(Matrix4F::identity());

    currentPushConstants.projectionMatrixIndex = 0;
    currentPushConstants.modelViewMatrixIndex = 0;
    currentPushConstants.textureMatrixIndex = 0;

    // Reset the rendering state.
    currentRenderingState.activePrimitiveTopology = AGPU_POINTS;
    currentRenderingState.flatShading = false;
    currentRenderingState.lightingEnabled = false;
    currentRenderingState.texturingEnabled = false;
    currentRenderingState.activeTexture.reset();

    // Reset the texture bindings.
    usedTextureBindingMap.clear();
    usedTextureBindingCount = 0;

    // Reset the lighting state.
    currentLightingState = LightingState();
    currentLightingStateDirty = true;
    lightingStateBufferData.clear();

    // Reset the material state.
    currentMaterialState = MaterialState();
    currentMaterialStateDirty = true;
    materialStateBufferData.clear();

    // Reset the user clip planes.
    currentUserClipPlane = Vector4F();
    currentUserClipPlaneDirty = true;
    userClipPlaneBufferData.clear();

    // Reset the vertices.
    lastDrawnVertexIndex = 0;
    vertices.clear();
    currentVertex.color = Vector4F(1.0f, 1.0f, 1.0f, 1.0f);
    currentVertex.normal = Vector3F(0.0f, 0.0f, 1.0f);
    currentVertex.texcoord = Vector2F(0.0f, 0.0f);

    // Reset the indices.
    indices.clear();

    // Reset the immediate meshes.
    renderingImmediateMesh = false;
    currentImmediateMeshBaseVertex = 0;
    currentImmediateMeshVertexCount = 0;

    return AGPU_OK;
}

agpu_error ImmediateRenderer::endRendering()
{
    if(!currentStateTracker)
        return AGPU_INVALID_OPERATION;

    flushRenderingData();
    for(auto &command : pendingRenderingCommands)
        command();

    pendingRenderingCommands.clear();
    currentStateTracker.reset();
    return AGPU_OK;
}

agpu_error ImmediateRenderer::setBlendState(agpu_int renderTargetMask, agpu_bool enabled)
{
    return delegateToStateTracker([=]{
        currentStateTracker->setBlendState(renderTargetMask, enabled);
    });
}

agpu_error ImmediateRenderer::setBlendFunction(agpu_int renderTargetMask, agpu_blending_factor sourceFactor, agpu_blending_factor destFactor, agpu_blending_operation colorOperation, agpu_blending_factor sourceAlphaFactor, agpu_blending_factor destAlphaFactor, agpu_blending_operation alphaOperation)
{
    return delegateToStateTracker([=]{
        currentStateTracker->setBlendFunction(renderTargetMask, sourceFactor, destFactor, colorOperation, sourceAlphaFactor, destAlphaFactor, alphaOperation);
    });
}

agpu_error ImmediateRenderer::setColorMask(agpu_int renderTargetMask, agpu_bool redEnabled, agpu_bool greenEnabled, agpu_bool blueEnabled, agpu_bool alphaEnabled)
{
    return delegateToStateTracker([=]{
        currentStateTracker->setColorMask(renderTargetMask, redEnabled, greenEnabled, blueEnabled, alphaEnabled);
    });
}

agpu_error ImmediateRenderer::setFrontFace(agpu_face_winding winding)
{
    return delegateToStateTracker([=]{
        currentStateTracker->setFrontFace(winding);
    });
}

agpu_error ImmediateRenderer::setCullMode(agpu_cull_mode mode)
{
    return delegateToStateTracker([=]{
        currentStateTracker->setCullMode(mode);
    });
}

agpu_error ImmediateRenderer::setDepthBias(agpu_float constant_factor, agpu_float clamp, agpu_float slope_factor)
{
    return delegateToStateTracker([=]{
        currentStateTracker->setDepthBias(constant_factor, clamp, slope_factor);
    });
}

agpu_error ImmediateRenderer::setDepthState(agpu_bool enabled, agpu_bool writeMask, agpu_compare_function function)
{
    return delegateToStateTracker([=]{
        currentStateTracker->setDepthState(enabled, writeMask, function);
    });
}

agpu_error ImmediateRenderer::setPolygonMode(agpu_polygon_mode mode)
{
    return delegateToStateTracker([=]{
        currentStateTracker->setPolygonMode(mode);
    });
}

agpu_error ImmediateRenderer::setStencilState(agpu_bool enabled, agpu_int writeMask, agpu_int readMask)
{
    return delegateToStateTracker([=]{
        currentStateTracker->setStencilState(enabled, writeMask, readMask);
    });
}

agpu_error ImmediateRenderer::setStencilFrontFace(agpu_stencil_operation stencilFailOperation, agpu_stencil_operation depthFailOperation, agpu_stencil_operation stencilDepthPassOperation, agpu_compare_function stencilFunction)
{
    return delegateToStateTracker([=]{
        currentStateTracker->setStencilFrontFace(stencilFailOperation, depthFailOperation, stencilDepthPassOperation, stencilFunction);
    });
}

agpu_error ImmediateRenderer::setStencilBackFace(agpu_stencil_operation stencilFailOperation, agpu_stencil_operation depthFailOperation, agpu_stencil_operation stencilDepthPassOperation, agpu_compare_function stencilFunction)
{
    return delegateToStateTracker([=]{
        currentStateTracker->setStencilBackFace(stencilFailOperation, depthFailOperation, stencilDepthPassOperation, stencilFunction);
    });
}

agpu_error ImmediateRenderer::setViewport(agpu_int x, agpu_int y, agpu_int w, agpu_int h)
{
    return delegateToStateTracker([=]{
        currentStateTracker->setViewport(x, y, w, h);
    });
}

agpu_error ImmediateRenderer::setScissor(agpu_int x, agpu_int y, agpu_int w, agpu_int h)
{
    return delegateToStateTracker([=]{
        currentStateTracker->setScissor(x, y, w, h);
    });
}

agpu_error ImmediateRenderer::setStencilReference(agpu_uint reference)
{
    return delegateToStateTracker([=]{
        currentStateTracker->setStencilReference(reference);
    });
}

agpu_error ImmediateRenderer::setFlatShading(agpu_bool enabled)
{
    currentRenderingState.flatShading = enabled;
    return AGPU_OK;
}

agpu_error ImmediateRenderer::setLightingEnabled(agpu_bool enabled)
{
    currentRenderingState.lightingEnabled = enabled;
    return AGPU_OK;
}

agpu_error ImmediateRenderer::clearLights()
{
    currentLightingState.lights = LightingState().lights;
    currentLightingState.enabledLightMask = 0;
    currentLightingStateDirty = true;
    return AGPU_OK;
}

agpu_error ImmediateRenderer::setAmbientLighting(agpu_float r, agpu_float g, agpu_float b, agpu_float a)
{
    currentLightingState.ambientLighting = Vector4F(r, g, b, a);
    currentLightingStateDirty = true;
    return AGPU_OK;
}

agpu_error ImmediateRenderer::setLight(agpu_uint index, agpu_bool enabled, agpu_immediate_renderer_light* state)
{
    if(index >= LightingState::MaxLightCount)
        return AGPU_OK;

    uint32_t bit = 1<<index;
    if(enabled)
    {
        currentLightingState.enabledLightMask |= bit;

        if(state)
        {
            auto &light = currentLightingState.lights[index];
            light.ambientColor = Vector4F(state->ambient);
            light.diffuseColor = Vector4F(state->diffuse);
            light.specularColor = Vector4F(state->specular);
            light.position = modelViewMatrixStack.back()*Vector4F(state->position);
            light.spotDirection = modelViewMatrixStack.back().transformDirection3(Vector3F(state->spot_direction));
            light.spotExponent = state->spot_exponent;
            light.spotCosCutoff = cos(state->spot_cutoff*M_PI/180.0);
            light.constantAttenuation = state->constant_attenuation;
            light.linearAttenuation = state->linear_attenuation;
            light.quadraticAttenuation = state->quadratic_attenuation;
        }
    }
    else
    {
        currentLightingState.enabledLightMask &= ~bit;
    }
    currentLightingStateDirty = true;

    return AGPU_OK;
}

agpu_error ImmediateRenderer::setClipPlane(agpu_uint index, agpu_bool enabled, agpu_float p1, agpu_float p2, agpu_float p3, agpu_float p4)
{
    // We only support a single user clip plane, for now.
    if(index > 0)
        return AGPU_OK;

    Vector4F newClipPlane;
    if(enabled)
    {
        auto modelViewMatrix = modelViewMatrixStack.back();
        auto inverseModelViewMatrix = modelViewMatrix.inverted();

        newClipPlane = Vector4F(p1, p2, p3, p4)*inverseModelViewMatrix;
    }

    currentUserClipPlaneDirty = currentUserClipPlaneDirty || (currentUserClipPlane != newClipPlane);
    currentUserClipPlane = newClipPlane;

    return AGPU_OK;
}
agpu_error ImmediateRenderer::setMaterial(agpu_immediate_renderer_material* state)
{
    if(!state)
        return AGPU_NULL_POINTER;

    currentMaterialState.emission = Vector4F(state->emission);
    currentMaterialState.ambient = Vector4F(state->ambient);
    currentMaterialState.diffuse = Vector4F(state->diffuse);
    currentMaterialState.specular = Vector4F(state->specular);
    currentMaterialState.shininess = state->shininess;
    return AGPU_OK;
}

agpu_error ImmediateRenderer::setTexturingEnabled(agpu_bool enabled)
{
    currentRenderingState.texturingEnabled = enabled;
    return AGPU_OK;
}
agpu_error ImmediateRenderer::bindTexture(const agpu::texture_ref &texture)
{
    currentRenderingState.activeTexture = texture;
    return AGPU_OK;
}

agpu::shader_resource_binding_ref ImmediateRenderer::getValidTextureBindingFor(const agpu::texture_ref &texture)
{
    if(!texture)
    {
        abort();
    }

    // Do we have an existing binding.
    auto it = usedTextureBindingMap.find(texture);
    if(it != usedTextureBindingMap.end())
        return it->second;

    // Get or create an available texture binding.
    agpu::shader_resource_binding_ref textureBinding;
    if(usedTextureBindingCount < allocatedTextureBindings.size())
    {
        textureBinding = allocatedTextureBindings[usedTextureBindingCount++];
    }
    else
    {
        textureBinding = agpu::shader_resource_binding_ref(stateTrackerCache.as<StateTrackerCache> ()->immediateShaderSignature->createShaderResourceBinding(2));
        allocatedTextureBindings.push_back(textureBinding);
        ++usedTextureBindingCount;
    }

    // Bind the texture on the binding point.
    textureBinding->bindSampledTextureView(0, agpu::texture_view_ref(texture->getOrCreateFullView()));
    usedTextureBindingMap[texture] = textureBinding;

    return textureBinding;
}

agpu_error ImmediateRenderer::projectionMatrixMode()
{
    activeMatrixStack = &projectionMatrixStack;
    activeMatrixStackDirtyFlag = &projectionMatrixStackDirtyFlag;
    return AGPU_OK;
}

agpu_error ImmediateRenderer::modelViewMatrixMode()
{
    activeMatrixStack = &modelViewMatrixStack;
    activeMatrixStackDirtyFlag = &modelViewMatrixStackDirtyFlag;
    return AGPU_OK;
}

agpu_error ImmediateRenderer::textureMatrixMode()
{
    activeMatrixStack = &textureMatrixStack;
    activeMatrixStackDirtyFlag = &textureMatrixStackDirtyFlag;
    return AGPU_OK;
}

agpu_error ImmediateRenderer::loadIdentity()
{
    if(!activeMatrixStack)
        return AGPU_INVALID_OPERATION;

    activeMatrixStack->back() = Matrix4F::identity();
    invalidateMatrix();
    return AGPU_OK;
}

agpu_error ImmediateRenderer::pushMatrix()
{
    if(!activeMatrixStack)
        return AGPU_INVALID_OPERATION;

    activeMatrixStack->push_back(activeMatrixStack->back());
    return AGPU_OK;
}

agpu_error ImmediateRenderer::popMatrix()
{
    if(!activeMatrixStack || activeMatrixStack->size() <= 1)
        return AGPU_INVALID_OPERATION;

    activeMatrixStack->pop_back();
    return AGPU_OK;
}

agpu_error ImmediateRenderer::loadMatrix(agpu_float* elements)
{
    activeMatrixStack->back() = Matrix4F(
        Vector4F(elements[0], elements[1], elements[2], elements[3]),
        Vector4F(elements[4], elements[5], elements[6], elements[7]),
        Vector4F(elements[8], elements[9], elements[10], elements[11]),
        Vector4F(elements[12], elements[13], elements[14], elements[15])
    );

    return AGPU_OK;
}

agpu_error ImmediateRenderer::loadTransposeMatrix(agpu_float* elements)
{
    activeMatrixStack->back() = Matrix4F(
        Vector4F(elements[0], elements[4], elements[8], elements[12]),
        Vector4F(elements[1], elements[5], elements[9], elements[13]),
        Vector4F(elements[2], elements[6], elements[10], elements[14]),
        Vector4F(elements[3], elements[7], elements[11], elements[15])
    );

    return AGPU_OK;
}

agpu_error ImmediateRenderer::multiplyMatrix(agpu_float* elements)
{
    applyMatrix(Matrix4F(
        Vector4F(elements[0], elements[1], elements[2], elements[3]),
        Vector4F(elements[4], elements[5], elements[6], elements[7]),
        Vector4F(elements[8], elements[9], elements[10], elements[11]),
        Vector4F(elements[12], elements[13], elements[14], elements[15])
    ));
    return AGPU_OK;
}

agpu_error ImmediateRenderer::multiplyTransposeMatrix(agpu_float* elements)
{
    /*printf("multiplyTransposeMatrix:\n");
    for(int j = 0; j < 4; ++j)
    {
        for(int i = 0; i < 4; ++i)
            printf("%f ", elements[j*4 + i]);
        printf("\n");
    }*/

    applyMatrix(Matrix4F(
        Vector4F(elements[0], elements[4], elements[8], elements[12]),
        Vector4F(elements[1], elements[5], elements[9], elements[13]),
        Vector4F(elements[2], elements[6], elements[10], elements[14]),
        Vector4F(elements[3], elements[7], elements[11], elements[15])
    ));
    return AGPU_OK;
}

agpu_error ImmediateRenderer::ortho(agpu_float left, agpu_float right, agpu_float bottom, agpu_float top, agpu_float near, agpu_float far)
{
    if(!activeMatrixStack)
        return AGPU_INVALID_OPERATION;

    auto tx = -(right + left)/(right - left);
    auto ty = -(top + bottom)/(top - bottom);
    auto tz = -near/(far - near);

    auto matrix = Matrix4F(
        Vector4F(2.0f/(right - left), 0.0f, 0.0f, 0.0f),
        Vector4F(0.0f, 2.0f/(top - bottom), 0.0f, 0.0f),
        Vector4F(0.0f, 0.0f, -1.0f/(far - near), 0.0f),
        Vector4F(tx, ty, tz, 1.0f)
    );

    // Flip the Y axis
    if(device->hasTopLeftNdcOrigin() != device->hasBottomLeftTextureCoordinates())
    {
        matrix.c2.y = -matrix.c2.y;
        matrix.c4.y = -matrix.c4.y;
    }

    applyMatrix(matrix);
    return AGPU_OK;
}

agpu_error ImmediateRenderer::frustum(agpu_float left, agpu_float right, agpu_float bottom, agpu_float top, agpu_float near, agpu_float far)
{
    if(!activeMatrixStack)
        return AGPU_INVALID_OPERATION;

    auto matrix = Matrix4F(
        Vector4F(2.0f*near / (right - left), 0.0f, 0.0f, 0.0f),
        Vector4F(0.0f, 2.0*near / (top - bottom), 0.0f, 0.0f),
        Vector4F(0.0f, 0.0f, -far/(far - near), -1.0f),
        Vector4F((right + left) / (right - left), (top + bottom) / (top - bottom),  -near * far / (far - near), 0.0f)
    );

    // Flip the Y axis
    if (device->hasTopLeftNdcOrigin() != device->hasBottomLeftTextureCoordinates())
    {
        matrix.c2.y = -matrix.c2.y;
        matrix.c3.y = -matrix.c3.y;
    }

    applyMatrix(matrix);
    return AGPU_OK;
}

agpu_error ImmediateRenderer::perspective(agpu_float fovy, agpu_float aspect, agpu_float near, agpu_float far)
{
    auto radians = fovy*(M_PI/180.0f*0.5f);
    auto top = near * tan(radians);
    auto right = top * aspect;
    return frustum(-right, right, -top, top, near, far);
}

agpu_error ImmediateRenderer::rotate(agpu_float angle, agpu_float vx, agpu_float vy, agpu_float vz)
{
    if(!activeMatrixStack)
        return AGPU_INVALID_OPERATION;

    auto radians = angle*M_PI/180.0f;
    auto c = cos(radians);
    auto s = sin(radians);

    auto l = sqrt(vx*vx + vy*vy + vz*vz);
    auto x = vx / l;
    auto y = vy / l;
    auto z = vz / l;

    // Formula from: https://www.khronos.org/registry/OpenGL-Refpages/gl2.1/xhtml/glRotate.xml
    applyMatrix(Matrix4F(
        Vector4F(x*x*(1 - c) +   c, y*x*(1 - c) + z*s, z*x*(1 - c) - y*s, 0.0f),
        Vector4F(x*y*(1 - c) - z*s, y*y*(1 - c) +   c, z*y*(1 - c) + x*s, 0.0f),
        Vector4F(x*z*(1 - c) + y*s, y*z*(1 - c) - x*s, z*z*(1 - c) +   c, 0.0f),
        Vector4F(0.0f, 0.0f, 0.0f, 1.0f)
    ));
    return AGPU_OK;
}

agpu_error ImmediateRenderer::translate(agpu_float x, agpu_float y, agpu_float z)
{
    if(!activeMatrixStack)
        return AGPU_INVALID_OPERATION;

    applyMatrix(Matrix4F(
        Vector4F(1.0f, 0.0f, 0.0f, 0.0f),
        Vector4F(0.0f, 1.0f, 0.0f, 0.0f),
        Vector4F(0.0f, 0.0f, 1.0f, 0.0f),
        Vector4F(x, y, z, 1.0f)
    ));
    return AGPU_OK;
}

agpu_error ImmediateRenderer::scale(agpu_float x, agpu_float y, agpu_float z)
{
    if(!activeMatrixStack)
        return AGPU_INVALID_OPERATION;

    applyMatrix(Matrix4F(
        Vector4F(x, 0.0f, 0.0f, 0.0f),
        Vector4F(0.0f, y, 0.0f, 0.0f),
        Vector4F(0.0f, 0.0f, z, 0.0f),
        Vector4F(0.0f, 0.0f, 0.0f, 1.0f)
    ));
    return AGPU_OK;
}

agpu_error ImmediateRenderer::beginPrimitives(agpu_primitive_topology type)
{
    if(!currentStateTracker)
        return AGPU_INVALID_OPERATION;

    auto error = validateRenderingStates();
    if(error) return error;
    currentRenderingState.activePrimitiveTopology = type;
    lastDrawnVertexIndex = vertices.size();

    return AGPU_OK;
}


agpu_error ImmediateRenderer::validateRenderingStates()
{
    auto error = validateMatrices();
    if(error) return error;

    error = validateLightingState();
    if(error) return error;

    error = validateMaterialState();
    if(error) return error;

    error = validateUserClipPlaneState();
    if(error) return error;

    return AGPU_OK;
}

agpu_error ImmediateRenderer::endPrimitives()
{
    if(!currentStateTracker)
        return AGPU_INVALID_OPERATION;

    auto vertexCount = vertices.size() - lastDrawnVertexIndex;
    if(vertexCount > 0)
    {
        auto vertexStart = lastDrawnVertexIndex;
        auto stateToRender = currentRenderingState;
        pendingRenderingCommands.push_back([=]{
            auto error = flushRenderingState(stateToRender);
            if(!error)
                currentStateTracker->drawArrays(vertexCount, 1, vertexStart, 0);
        });
    }

    return AGPU_OK;
}

agpu_error ImmediateRenderer::color(agpu_float r, agpu_float g, agpu_float b, agpu_float a)
{
    if(!currentStateTracker)
        return AGPU_INVALID_OPERATION;

    currentVertex.color = Vector4F(r, g, b, a);
    return AGPU_OK;
}

agpu_error ImmediateRenderer::texcoord(agpu_float x, agpu_float y)
{
    if(!currentStateTracker)
        return AGPU_INVALID_OPERATION;

    currentVertex.texcoord = Vector2F(x, y);
    return AGPU_OK;
}

agpu_error ImmediateRenderer::normal(agpu_float x, agpu_float y, agpu_float z)
{
    if(!currentStateTracker)
        return AGPU_INVALID_OPERATION;

    currentVertex.normal = Vector3F(x, y, z);
    return AGPU_OK;
}

agpu_error ImmediateRenderer::vertex(agpu_float x, agpu_float y, agpu_float z)
{
    if(!currentStateTracker)
        return AGPU_INVALID_OPERATION;

    currentVertex.position = Vector3F(x, y, z);
    vertices.push_back(currentVertex);
    return AGPU_OK;
}

void ImmediateRenderer::applyMatrix(const Matrix4F &matrix)
{
    activeMatrixStack->back() *= matrix;
    invalidateMatrix();
}

void ImmediateRenderer::invalidateMatrix()
{
    *activeMatrixStackDirtyFlag = true;
}

agpu_error ImmediateRenderer::validateLightingState()
{
    if((currentRenderingState.lightingEnabled || lightingStateBufferData.empty()) && currentLightingStateDirty)
    {
        currentPushConstants.lightingStateIndex = lightingStateBufferData.size();
        lightingStateBufferData.push_back(currentLightingState);
        uploadPushConstants();
    }

    return AGPU_OK;
}

agpu_error ImmediateRenderer::validateMaterialState()
{
    if((currentRenderingState.lightingEnabled || materialStateBufferData.empty()) && currentMaterialStateDirty)
    {
        currentPushConstants.materialStateIndex = materialStateBufferData.size();
        materialStateBufferData.push_back(currentMaterialState);
        uploadPushConstants();
    }

    return AGPU_OK;
}

agpu_error ImmediateRenderer::validateMatrices()
{
    bool matricesChanged = false;
    if(projectionMatrixStackDirtyFlag)
    {
        currentPushConstants.projectionMatrixIndex = matrixBufferData.size();
        matrixBufferData.push_back(projectionMatrixStack.back());
        projectionMatrixStackDirtyFlag = false;
        matricesChanged = true;
    }

    if(modelViewMatrixStackDirtyFlag)
    {
        currentPushConstants.modelViewMatrixIndex = matrixBufferData.size();
        matrixBufferData.push_back(modelViewMatrixStack.back());
        modelViewMatrixStackDirtyFlag = false;
        matricesChanged = true;
    }

    if(textureMatrixStackDirtyFlag)
    {
        currentPushConstants.textureMatrixIndex = matrixBufferData.size();
        matrixBufferData.push_back(textureMatrixStack.back());
        textureMatrixStackDirtyFlag = false;
        matricesChanged = true;
    }

    if(matricesChanged)
        uploadPushConstants();

    return AGPU_OK;
}

agpu_error ImmediateRenderer::validateUserClipPlaneState()
{
    if(currentUserClipPlaneDirty)
    {
        currentPushConstants.clipPlaneIndex = userClipPlaneBufferData.size();
        userClipPlaneBufferData.push_back(currentUserClipPlane);
        currentUserClipPlaneDirty = false;
        uploadPushConstants();
    }

    return AGPU_OK;
}

void ImmediateRenderer::uploadPushConstants()
{
    auto constantsToUpdate = currentPushConstants;
    pendingRenderingCommands.push_back([=]{
        currentStateTracker->setShaderSignature(immediateShaderSignature);
        currentStateTracker->pushConstants(0, sizeof(constantsToUpdate), (void*)&constantsToUpdate);
    });
}

agpu_error ImmediateRenderer::flushRenderingState(const ImmediateRenderingState &state)
{
    currentStateTracker->setShaderSignature(immediateShaderSignature);
    if(state.flatShading)
    {
        if(state.texturingEnabled)
        {
            if(state.lightingEnabled)
                currentStateTracker->setVertexStage(immediateShaderLibrary->flatLightedTexturedVertex, "main");
            else
                currentStateTracker->setVertexStage(immediateShaderLibrary->flatTexturedVertex, "main");
            currentStateTracker->setFragmentStage(immediateShaderLibrary->flatTexturedFragment, "main");
        }
        else
        {
            if(state.lightingEnabled)
                currentStateTracker->setVertexStage(immediateShaderLibrary->flatLightedColorVertex, "main");
            else
                currentStateTracker->setVertexStage(immediateShaderLibrary->flatColorVertex, "main");
            currentStateTracker->setFragmentStage(immediateShaderLibrary->flatColorFragment, "main");
        }
    }
    else
    {
        if(state.texturingEnabled)
        {
            if(state.lightingEnabled)
                currentStateTracker->setVertexStage(immediateShaderLibrary->smoothLightedTexturedVertex, "main");
            else
                currentStateTracker->setVertexStage(immediateShaderLibrary->smoothTexturedVertex, "main");
            currentStateTracker->setFragmentStage(immediateShaderLibrary->smoothTexturedFragment, "main");
        }
        else
        {
            if(state.lightingEnabled)
                currentStateTracker->setVertexStage(immediateShaderLibrary->smoothLightedColorVertex, "main");
            else
                currentStateTracker->setVertexStage(immediateShaderLibrary->smoothColorVertex, "main");
            currentStateTracker->setFragmentStage(immediateShaderLibrary->smoothColorFragment, "main");
        }
    }

    currentStateTracker->setPrimitiveType(state.activePrimitiveTopology);
    currentStateTracker->setVertexLayout(immediateVertexLayout);
    currentStateTracker->useVertexBinding(vertexBinding);
    currentStateTracker->useShaderResources(uniformResourceBindings);
    if(state.texturingEnabled)
    {
        currentStateTracker->useShaderResources(immediateSharedRenderingStates->linearSampler.binding);
        currentStateTracker->useShaderResources(getValidTextureBindingFor(state.activeTexture));
    }

    return AGPU_OK;
}

agpu_error ImmediateRenderer::flushRenderingData()
{
    // Upload the vertices.
    agpu_error error = AGPU_OK;
    if(!vertexBuffer || vertexBufferCapacity < vertices.size())
    {
        vertexBufferCapacity = nextPowerOfTwo(vertices.size());
        if(vertexBufferCapacity < 32)
            vertexBufferCapacity = 32;

        auto requiredSize = vertexBufferCapacity*sizeof(ImmediateRendererVertex);
        agpu_buffer_description bufferDescription = {};
        bufferDescription.size = requiredSize;
        bufferDescription.heap_type = requiredSize >= GpuBufferDataThreshold
            ? AGPU_MEMORY_HEAP_TYPE_DEVICE_LOCAL : AGPU_MEMORY_HEAP_TYPE_HOST_TO_DEVICE;
        bufferDescription.binding = AGPU_ARRAY_BUFFER;
        bufferDescription.mapping_flags = AGPU_MAP_DYNAMIC_STORAGE_BIT;
        bufferDescription.stride = sizeof(ImmediateRendererVertex);

        vertexBuffer = agpu::buffer_ref(device->createBuffer(&bufferDescription, nullptr));
        if(!vertexBuffer)
            return AGPU_OUT_OF_MEMORY;

        vertexBinding->bindVertexBuffers(1, &vertexBuffer);
    }

    if(!vertices.empty())
    {
        error = vertexBuffer->uploadBufferData(0, vertices.size()*sizeof(ImmediateRendererVertex), &vertices[0]);
        if(error)
            return error;
    }

    // Upload the indices.
    if(!indices.empty() && (!indexBuffer || indexBufferCapacity < indices.size()))
    {
        indexBufferCapacity = nextPowerOfTwo(indices.size());
        if(indexBufferCapacity < 32)
            indexBufferCapacity = 32;

        auto requiredSize = indexBufferCapacity*sizeof(uint32_t);
        agpu_buffer_description bufferDescription = {};
        bufferDescription.size = requiredSize;
        bufferDescription.heap_type = requiredSize >= GpuBufferDataThreshold
            ? AGPU_MEMORY_HEAP_TYPE_DEVICE_LOCAL : AGPU_MEMORY_HEAP_TYPE_HOST_TO_DEVICE;
        bufferDescription.binding = AGPU_ELEMENT_ARRAY_BUFFER;
        bufferDescription.mapping_flags = AGPU_MAP_DYNAMIC_STORAGE_BIT;
        bufferDescription.stride = sizeof(uint32_t);

        indexBuffer = agpu::buffer_ref(device->createBuffer(&bufferDescription, nullptr));
        if(!indexBuffer)
            return AGPU_OUT_OF_MEMORY;
    }

    if(!indices.empty())
    {
        error = indexBuffer->uploadBufferData(0, indices.size()*sizeof(uint32_t), &indices[0]);
        if(error)
            return error;
    }

    // Upload the matrices.
    if(!matrixBuffer || matrixBufferCapacity < matrixBufferData.size())
    {
        matrixBufferCapacity = nextPowerOfTwo(matrixBufferData.size());
        if(matrixBufferCapacity < 32)
            matrixBufferCapacity = 32;

        auto requiredSize = matrixBufferCapacity*sizeof(Matrix4F);
        agpu_buffer_description bufferDescription = {};
        bufferDescription.size = requiredSize;
        bufferDescription.heap_type = requiredSize >= GpuBufferDataThreshold
            ? AGPU_MEMORY_HEAP_TYPE_DEVICE_LOCAL : AGPU_MEMORY_HEAP_TYPE_HOST_TO_DEVICE;
        bufferDescription.binding = AGPU_STORAGE_BUFFER;
        bufferDescription.mapping_flags = AGPU_MAP_DYNAMIC_STORAGE_BIT;
        bufferDescription.stride = sizeof(Matrix4F);

        matrixBuffer = agpu::buffer_ref(device->createBuffer(&bufferDescription, nullptr));
        if(!matrixBuffer)
            return AGPU_OUT_OF_MEMORY;

        uniformResourceBindings->bindStorageBuffer(0, matrixBuffer);
    }

    if(!matrixBufferData.empty())
    {
        error = matrixBuffer->uploadBufferData(0, matrixBufferData.size()*sizeof(Matrix4F), &matrixBufferData[0]);
        if(error)
            return error;
    }

    // Upload the lighting states.
    if(!lightingStateBuffer || lightingStateBufferCapacity < lightingStateBufferData.size())
    {
        lightingStateBufferCapacity = nextPowerOfTwo(lightingStateBufferData.size());
        if(lightingStateBufferCapacity < 32)
            lightingStateBufferCapacity = 32;

        auto requiredSize = lightingStateBufferCapacity*sizeof(LightingState);
        agpu_buffer_description bufferDescription = {};
        bufferDescription.size = requiredSize;
        bufferDescription.heap_type = requiredSize >= GpuBufferDataThreshold
            ? AGPU_MEMORY_HEAP_TYPE_DEVICE_LOCAL : AGPU_MEMORY_HEAP_TYPE_HOST_TO_DEVICE;
        bufferDescription.binding = AGPU_STORAGE_BUFFER;
        bufferDescription.mapping_flags = AGPU_MAP_DYNAMIC_STORAGE_BIT;
        bufferDescription.stride = sizeof(LightingState);

        lightingStateBuffer = agpu::buffer_ref(device->createBuffer(&bufferDescription, nullptr));
        if(!lightingStateBuffer)
            return AGPU_OUT_OF_MEMORY;

        uniformResourceBindings->bindStorageBuffer(1, lightingStateBuffer);
    }

    if(!lightingStateBufferData.empty())
    {
        error = lightingStateBuffer->uploadBufferData(0, lightingStateBufferData.size()*sizeof(LightingState), &lightingStateBufferData[0]);
        if(error)
            return error;
    }

    // Upload the material states.
    if(!materialStateBuffer || materialStateBufferCapacity < materialStateBufferData.size())
    {
        materialStateBufferCapacity = nextPowerOfTwo(materialStateBufferData.size());
        if(materialStateBufferCapacity < 32)
            materialStateBufferCapacity = 32;

        auto requiredSize = materialStateBufferCapacity*sizeof(MaterialState);
        agpu_buffer_description bufferDescription = {};
        bufferDescription.size = requiredSize;
        bufferDescription.heap_type = requiredSize >= GpuBufferDataThreshold
            ? AGPU_MEMORY_HEAP_TYPE_DEVICE_LOCAL : AGPU_MEMORY_HEAP_TYPE_HOST_TO_DEVICE;
        bufferDescription.binding = AGPU_STORAGE_BUFFER;
        bufferDescription.mapping_flags = AGPU_MAP_DYNAMIC_STORAGE_BIT;
        bufferDescription.stride = sizeof(MaterialState);

        materialStateBuffer = agpu::buffer_ref(device->createBuffer(&bufferDescription, nullptr));
        if(!materialStateBuffer)
            return AGPU_OUT_OF_MEMORY;

        uniformResourceBindings->bindStorageBuffer(2, materialStateBuffer);
    }

    if(!materialStateBufferData.empty())
    {
        error = materialStateBuffer->uploadBufferData(0, materialStateBufferData.size()*sizeof(MaterialState), &materialStateBufferData[0]);
        if(error)
            return error;
    }

    // Upload the user clip plane.
    if(!userClipPlaneBuffer || userClipPlaneBufferCapacity < userClipPlaneBufferData.size())
    {
        userClipPlaneBufferCapacity = nextPowerOfTwo(userClipPlaneBufferData.size());
        if(userClipPlaneBufferCapacity < 32)
            userClipPlaneBufferCapacity = 32;

        auto requiredSize = userClipPlaneBufferCapacity*sizeof(Vector4F);
        agpu_buffer_description bufferDescription = {};
        bufferDescription.size = requiredSize;
        bufferDescription.heap_type = requiredSize >= GpuBufferDataThreshold
            ? AGPU_MEMORY_HEAP_TYPE_DEVICE_LOCAL : AGPU_MEMORY_HEAP_TYPE_HOST_TO_DEVICE;
        bufferDescription.binding = AGPU_STORAGE_BUFFER;
        bufferDescription.mapping_flags = AGPU_MAP_DYNAMIC_STORAGE_BIT;
        bufferDescription.stride = sizeof(Vector4F);

        userClipPlaneBuffer = agpu::buffer_ref(device->createBuffer(&bufferDescription, nullptr));
        if(!userClipPlaneBuffer)
            return AGPU_OUT_OF_MEMORY;

        uniformResourceBindings->bindStorageBuffer(3, userClipPlaneBuffer);
    }

    if(!userClipPlaneBufferData.empty())
    {
        error = userClipPlaneBuffer->uploadBufferData(0, userClipPlaneBufferData.size()*sizeof(Vector4F), &userClipPlaneBufferData[0]);
        if(error)
            return error;
    }

    return AGPU_OK;
}

agpu_error ImmediateRenderer::beginMeshWithVertices(agpu_size vertexCount, agpu_size stride, agpu_size elementCount, agpu_pointer positionsPointer)
{
    if(renderingImmediateMesh)
        return AGPU_INVALID_OPERATION;

    auto error = validateRenderingStates();
    if(error) return error;

    renderingImmediateMesh = true;
    currentImmediateMeshBaseVertex = vertices.size();
    currentImmediateMeshVertexCount = vertexCount;
    //printf("beginMeshWithVertices baseVertex %d\n", (int)currentImmediateMeshBaseVertex);
    vertices.reserve(vertexCount);

    auto positionsBytes = reinterpret_cast<const uint8_t*> (positionsPointer);
    for(size_t i = 0; i < vertexCount; ++i)
    {
        auto vertexPositions = reinterpret_cast<const float*> (positionsBytes);
        vertices.push_back(this->currentVertex);

        auto &destPositions = vertices.back().position;
        destPositions = Vector3F(vertexPositions[0]);
        if(elementCount > 1)
        {
            destPositions.y = vertexPositions[1];
            if(elementCount > 2)
                destPositions.z = vertexPositions[2];
        }
        positionsBytes += stride;
    }

    auto stateToRender = currentRenderingState;
    pendingRenderingCommands.push_back([=]{
        auto error = flushRenderingState(stateToRender);
        if(!error)
        {
            currentStateTracker->useIndexBuffer(indexBuffer);
        }
    });

    return AGPU_OK;
}

agpu_error ImmediateRenderer::setCurrentMeshColors(agpu_size stride, agpu_size elementCount, agpu_pointer colors)
{
    if(!renderingImmediateMesh)
        return AGPU_INVALID_OPERATION;

    auto colorBytes = reinterpret_cast<const uint8_t*> (colors);
    for(size_t i = 0; i < currentImmediateMeshVertexCount; ++i)
    {
        auto colorValues = reinterpret_cast<const float*> (colorBytes);
        auto &destColor = vertices[currentImmediateMeshBaseVertex + i].color;
        destColor = Vector4F(colorValues[0]);
        if(elementCount > 1)
        {
            destColor.y = colorValues[1];
            if(elementCount > 2)
            {
                destColor.z = colorValues[2];
                if(elementCount > 3)
                    destColor.w = colorValues[3];
            }
        }

        colorBytes += stride;
    }

    return AGPU_OK;
}

agpu_error ImmediateRenderer::setCurrentMeshNormals(agpu_size stride, agpu_size elementCount, agpu_pointer normals)
{
    if(!renderingImmediateMesh)
        return AGPU_INVALID_OPERATION;

    auto normalBytes = reinterpret_cast<const uint8_t*> (normals);
    for(size_t i = 0; i < currentImmediateMeshVertexCount; ++i)
    {
        auto normalValues = reinterpret_cast<const float*> (normalBytes);
        auto &destNormal = vertices[currentImmediateMeshBaseVertex + i].normal;
        destNormal = Vector3F(normalValues[0]);
        if(elementCount > 1)
        {
            destNormal.y = normalValues[1];
            if(elementCount > 2)
                destNormal.z = normalValues[2];
        }

        normalBytes += stride;
    }

    return AGPU_OK;
}

agpu_error ImmediateRenderer::setCurrentMeshTexCoords(agpu_size stride, agpu_size elementCount, agpu_pointer texcoords)
{
    if(!renderingImmediateMesh)
        return AGPU_INVALID_OPERATION;

    auto texcoordBytes = reinterpret_cast<const uint8_t*> (texcoords);
    for(size_t i = 0; i < currentImmediateMeshVertexCount; ++i)
    {
        auto texcoordValues = reinterpret_cast<const float*> (texcoordBytes);
        auto &destTexcoord = vertices[currentImmediateMeshBaseVertex + i].texcoord;
        destTexcoord = Vector2F(texcoordValues[0]);
        if(elementCount > 1)
            destTexcoord.y = texcoordValues[1];

        texcoordBytes += stride;
    }

    return AGPU_OK;
}

agpu_error ImmediateRenderer::drawElementsWithIndices(agpu_primitive_topology mode, agpu_pointer indicesPointer, agpu_uint index_count, agpu_uint instance_count, agpu_uint first_index, agpu_int base_vertex, agpu_uint base_instance)
{
    if(!renderingImmediateMesh)
        return AGPU_INVALID_OPERATION;

    size_t baseIndex = indices.size();
    auto indicesValues = reinterpret_cast<uint32_t*> (indicesPointer);
    auto actualBaseVertex = currentImmediateMeshBaseVertex + base_vertex;
    switch(mode)
    {
    // Directly supported modes. Just copy the data.
    case AGPU_POINTS:
	case AGPU_LINES:
	case AGPU_LINES_ADJACENCY:
	case AGPU_LINE_STRIP:
	case AGPU_LINE_STRIP_ADJACENCY:
	case AGPU_TRIANGLES:
	case AGPU_TRIANGLES_ADJACENCY:
	case AGPU_TRIANGLE_STRIP:
	case AGPU_TRIANGLE_STRIP_ADJACENCY:
	case AGPU_PATCHES:
        {
            indices.insert(indices.end(), indicesValues, indicesValues + index_count);
            pendingRenderingCommands.push_back([=]{
                currentStateTracker->setPrimitiveType(mode);
                currentStateTracker->drawElements(index_count, instance_count, baseIndex + first_index, actualBaseVertex, base_instance);
            });
        }
        break;
    case AGPU_IMMEDIATE_POLYGON:
    case AGPU_IMMEDIATE_TRIANGLE_FAN:
        if(index_count > 2)
        {
            indicesValues += first_index;
            auto i0 = indicesValues[0];
            auto convertedIndexCount = 0;
            for(size_t i = 2; i < index_count; ++i)
            {
                indices.push_back(i0);
                indices.push_back(indicesValues[i - 1]);
                indices.push_back(indicesValues[i]);
                convertedIndexCount += 3;
            }

            pendingRenderingCommands.push_back([=]{
                currentStateTracker->setPrimitiveType(AGPU_TRIANGLES);
                currentStateTracker->drawElements(convertedIndexCount, instance_count, baseIndex, actualBaseVertex, base_instance);
            });
        }
        return AGPU_OK;
    case AGPU_IMMEDIATE_QUADS:
        if(index_count >= 4)
        {
            indicesValues += first_index;
            size_t quadCount = index_count / 4;
            auto convertedIndexCount = 0;
            for(size_t i = 0; i < quadCount; ++i)
            {
                auto qi0 = indicesValues[0];
                auto qi1 = indicesValues[1];
                auto qi2 = indicesValues[2];
                auto qi3 = indicesValues[3];

                indices.push_back(qi0);
                indices.push_back(qi1);
                indices.push_back(qi2);

                indices.push_back(qi2);
                indices.push_back(qi3);
                indices.push_back(qi0);

                indicesValues += 4;
                convertedIndexCount += 6;
            }


            pendingRenderingCommands.push_back([=]{
                currentStateTracker->setPrimitiveType(AGPU_TRIANGLES);
                currentStateTracker->drawElements(convertedIndexCount, instance_count, baseIndex, actualBaseVertex, base_instance);
            });
        }
        return AGPU_OK;
    default:
        return AGPU_OK;
    }

    return AGPU_OK;
}

agpu_error ImmediateRenderer::endMesh()
{
    if(!renderingImmediateMesh)
        return AGPU_INVALID_OPERATION;

    renderingImmediateMesh = false;
    return AGPU_OK;
}

} // End of namespace AgpuCommon