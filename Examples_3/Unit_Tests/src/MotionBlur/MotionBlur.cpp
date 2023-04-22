// Motion blur based on this paper: https://casual-effects.com/research/McGuire2012Blur/McGuire12Blur.pdf
// The implemented method is called "A Reconstruction Filter for Plausible Motion Blur".

//Interfaces
#include "../../../../Common_3/OS/Interfaces/ICameraController.h"
#include "../../../../Common_3/OS/Interfaces/IApp.h"
#include "../../../../Common_3/OS/Interfaces/ILog.h"
#include "../../../../Common_3/OS/Interfaces/IInput.h"
#include "../../../../Common_3/OS/Interfaces/IFileSystem.h"
#include "../../../../Common_3/OS/Interfaces/ITime.h"
#include "../../../../Common_3/OS/Interfaces/IProfiler.h"
#include "../../../../Middleware_3/UI/AppUI.h"
#include "../../../../Common_3/Renderer/IRenderer.h"
#include "../../../../Common_3/Renderer/IResourceLoader.h"

//Math
#include "../../../../Common_3/OS/Math/MathTypes.h"

#include "../../../../Common_3/OS/Interfaces/IMemory.h"

/// Demo structures
// Settings
float gTileSize     = 20.0f;    // Tile size, suggested amount by the paper - 45.0 to see the effect better
float gSampleCount  = 15.0f;    // Sample taps, suggested amount by the paper
float gExposure     = 0.01f;   //  0.03 to see the effect better

// General
VirtualJoystickUI	gVirtualJoystick;
ProfileToken		gGpuProfileToken	= PROFILE_INVALID_TOKEN;
ICameraController *	pCameraController	= NULL;
GuiComponent *		pGuiWindow;

uint32_t const		gImageCount		= 3;
uint32_t			gFrameIndex		= 0;
bool				bToggleVSync	= false;
uint const			gTimeOffset		= 600000; // For visually better starting locations
float               gDeltaTime      = 0.0f;

Queue *				pGraphicsQueue			= NULL;
CmdPool *			pCmdPools[gImageCount]	= {NULL};
Cmd *				pCmds[gImageCount]		= {NULL};
Renderer *			pRenderer               = NULL;

SwapChain *			pSwapChain								= NULL;
Fence *				pRenderCompleteFences[gImageCount]		= {NULL};
Semaphore *			pImageAcquiredSemaphore					= NULL;
Semaphore *			pRenderCompleteSemaphores[gImageCount]	= {NULL};

uint32_t const      SAMPLERS_COUNT                       = 2;
Sampler	*			pStaticSamplers[SAMPLERS_COUNT]      = {NULL};
char const *		pStaticSamplersNames[]               = {"uSampler", "uSamplerLinear"};

VertexLayout		gVertexLayout;

struct ObjectInfo
{
    mat4 mToWorldMat;
    mat4 mToWorldMatPrev;
};

// Sponza
class Sponza
{
public: // must match with the shader
    static constexpr uint32_t TOTAL_MODELS = 2;
    static constexpr uint32_t TOTAL_IMAGES = 84;

public:
    void                assignTextures();

public:
    Buffer *		    pUniformBuffer[gImageCount]	    = {NULL};
    Texture *           pMaterialTextures[TOTAL_IMAGES] = {NULL};

    eastl::vector<int>  mTextureIndexforMaterial;

    const char *        mModelNames[TOTAL_MODELS] = { "Sponza.gltf", "lion.gltf", };
    Geometry *          mModels[TOTAL_MODELS];
    uint32_t            mMaterialIds[103] = 
    {
        0,  3,  1,  4,  5,  6,  7,  8,  6,  9,  7,  6, 10, 5, 7,  5, 6, 7,  6, 7,  6,  7,  6,  7,  6,  7,  6,  7,  6,  7,  6,  7,  6,  7,  6,
        5,  6,  5,  11, 5,  11, 5,  11, 5,  10, 5,  9, 8,  6, 12, 2, 5, 13, 0, 14, 15, 16, 14, 15, 14, 16, 15, 13, 17, 18, 19, 18, 19, 18, 17,
        19, 18, 17, 20, 21, 20, 21, 20, 21, 20, 21, 3, 1,  3, 1,  3, 1, 3,  1, 3,  1,  3,  1,  3,  1,  22, 23, 4,  23, 4,  5,  24, 5,
    };

    struct UniformBlock
    {
        ObjectInfo mBuilding;
        ObjectInfo mLion;
    } mUniformBlock;

} gSponza;

// UI
UIApp gAppUI;
TextDrawDesc gFrameTimeDraw = TextDrawDesc(0, 0xff00ffff, 18);

struct
{
    vec2  tileSize		    = {};
    float kFactor			= gTileSize;
    float sFactor			= gSampleCount;
} gPushConstant;

// Environment block
struct Environment
{
    struct UniformBlock
    {
        mat4 mView				= {};
        mat4 mProject			= {};
        mat4 mViewPrev			= {};
        mat4 mProjectPrev		= {};
        vec4 mLightDirection	= { -1.0f, -1.0f, -1.0f, 0.0f };
        vec4 mLightColor		= { 1.0f, 1.0f, 1.0f, 1.0f };
    } mUniformData;
    Buffer * pUniformBuffer[gImageCount]	= {NULL};
} gEnv;

// First pass (draws the objects, and calcs the velocity vectors)
struct GBufferPass
{	
    Shader *		pShader						= NULL;
    DescriptorSet * pDescriptorSets_NonFreq	    = {NULL}; // textures
    DescriptorSet * pDescriptorSets_PerFrame	= {NULL}; // object info
    RootSignature * pRootSignature				= NULL;
    Pipeline *		pPipeline					= NULL;

    RenderTarget *	pColorRT;
    RenderTarget *	pNormRT;
    RenderTarget *	pVelocityRT;
    RenderTarget *	pDepthBuffer;

    struct 
    {
        vec2  viewport		= {};
        float kFactor       = gTileSize;
        uint  textureMapIds = 0;
        uint  objectIndex   = 0;
        float exposure	    = gExposure;
        float deltaTime		= 0.0f;
    } mPushConstant;

} gGBufferPass;

// Second pass (finds the most dominant velocity for each tile)
struct TilePass
{
    Shader *		pShader						= NULL;
    DescriptorSet * pDescriptorSets_PerFrame    = {NULL};
    RootSignature * pRootSignature				= NULL;
    Pipeline *		pPipeline					= NULL;
    Texture *	    pTileTexture	            = {NULL};

} gTilePass;

// Third pass (computes the maximum velocity in any adjacent tile)
struct NeighborPass
{
    Shader *		pShader						= NULL;
    DescriptorSet * pDescriptorSets_PerFrame	= {NULL};
    RootSignature * pRootSignature				= NULL;
    Pipeline *		pPipeline					= NULL;
    Texture *	    pNeighborTexture            = {NULL};
} gNeighborPass;

// Forth pass (reconstruction filter)
struct ReconstructPass
{
    Shader *		pShader			 = NULL;
    DescriptorSet * pDescriptorSets	 = {NULL};
    RootSignature * pRootSignature	 = NULL;
    Pipeline *		pPipeline		 = NULL;
} gReconstructPass;

class MotionBlur : public IApp
{
public:
    MotionBlur()
    {
        bToggleVSync = mSettings.mDefaultVSyncEnabled;
    }

    bool Init()
    {
        // File paths
        {
            fsSetPathForResourceDir(pSystemFileIO, RM_CONTENT,	RD_SHADER_SOURCES,	"Shaders");
            fsSetPathForResourceDir(pSystemFileIO, RM_DEBUG,	RD_SHADER_BINARIES, "CompiledShaders");
            fsSetPathForResourceDir(pSystemFileIO, RM_CONTENT,	RD_GPU_CONFIG,		"GPUCfg");
            fsSetPathForResourceDir(pSystemFileIO, RM_CONTENT,	RD_TEXTURES,		"Textures");
            fsSetPathForResourceDir(pSystemFileIO, RM_CONTENT,  RD_MESHES,          "Meshes");
            fsSetPathForResourceDir(pSystemFileIO, RM_CONTENT,	RD_FONTS,			"Fonts");
        }

        // Renderer initialization
        {
            // Window and renderer setup
            RendererDesc settings = {0};
            initRenderer(GetName(), &settings, &pRenderer);

            // Check for init success
            if (!pRenderer)
                return false;

            QueueDesc queueDesc = {};
            queueDesc.mType = QUEUE_TYPE_GRAPHICS;
            queueDesc.mFlag = QUEUE_FLAG_INIT_MICROPROFILE;
            addQueue(pRenderer, &queueDesc, &pGraphicsQueue);

            for (uint32_t i = 0; i < gImageCount; ++i)
            {
                CmdPoolDesc cmdPoolDesc = {};
                cmdPoolDesc.pQueue = pGraphicsQueue;
                addCmdPool(pRenderer, &cmdPoolDesc, &pCmdPools[i]);
                CmdDesc cmdDesc = {};
                cmdDesc.pPool = pCmdPools[i];
                addCmd(pRenderer, &cmdDesc, &pCmds[i]);

                addFence(pRenderer, &pRenderCompleteFences[i]);
                addSemaphore(pRenderer, &pRenderCompleteSemaphores[i]);
            }
            addSemaphore(pRenderer, &pImageAcquiredSemaphore);

            initResourceLoaderInterface(pRenderer);

            if (!gVirtualJoystick.Init(pRenderer, "circlepad"))
            {
                LOGF(LogLevel::eERROR, "Could not initialize Virtual Joystick.");
                return false;
            }
        }
        
        // Common static samplers for all pipelines
        {
            SamplerDesc samplerDesc = 
            {
                FILTER_LINEAR,
                FILTER_LINEAR,
                MIPMAP_MODE_NEAREST,
                ADDRESS_MODE_REPEAT,
                ADDRESS_MODE_REPEAT,
                ADDRESS_MODE_REPEAT,
            };
            addSampler(pRenderer, &samplerDesc, &pStaticSamplers[0]);

            samplerDesc = 
            {
                FILTER_LINEAR,
                FILTER_LINEAR,
                MIPMAP_MODE_NEAREST,
                ADDRESS_MODE_CLAMP_TO_EDGE,
                ADDRESS_MODE_CLAMP_TO_EDGE,
                ADDRESS_MODE_CLAMP_TO_EDGE,
            };
            addSampler(pRenderer, &samplerDesc, &pStaticSamplers[1]);
        }
        
        // Vertex layout for all pipelines
        {
            gVertexLayout = {};
            gVertexLayout.mAttribCount = 3;
            gVertexLayout.mAttribs[0].mSemantic = SEMANTIC_POSITION;
            gVertexLayout.mAttribs[0].mFormat = TinyImageFormat_R32G32B32_SFLOAT;
            gVertexLayout.mAttribs[0].mBinding = 0;
            gVertexLayout.mAttribs[0].mLocation = 0;
            gVertexLayout.mAttribs[0].mOffset = 0;
            gVertexLayout.mAttribs[1].mSemantic = SEMANTIC_NORMAL;
            gVertexLayout.mAttribs[1].mFormat = TinyImageFormat_R32G32B32_SFLOAT;
            gVertexLayout.mAttribs[1].mBinding = 0;
            gVertexLayout.mAttribs[1].mLocation = 1;
            gVertexLayout.mAttribs[1].mOffset = 3 * sizeof(float);
            gVertexLayout.mAttribs[2].mSemantic = SEMANTIC_TEXCOORD0;
            gVertexLayout.mAttribs[2].mFormat = TinyImageFormat_R32G32_SFLOAT;
            gVertexLayout.mAttribs[2].mBinding = 0;
            gVertexLayout.mAttribs[2].mLocation = 2;
            gVertexLayout.mAttribs[2].mOffset = 6 * sizeof(float);
        }
        
        // Loading Sponza
        {
            for (size_t i = 0; i < Sponza::TOTAL_IMAGES; i += 1)
            {
                loadTexture(i);
            }

            for (size_t i = 0; i < Sponza::TOTAL_MODELS; i += 1)
            {
                loadMesh(i);
            }

            gSponza.assignTextures();
        }

        waitForAllResourceLoads();

        createEnvironmentBlock();
        createGBufferPass();
        createTilePass();
        createNeighborPass();
        createReconstructPass();

        if (!gAppUI.Init(pRenderer))
            return false;

        gAppUI.LoadFont("TitilliumText/TitilliumText-Bold.otf");

        // Create the camera
        {
            vec3 lookAt{ 0.0f, -2.0f, 0.9f };
            vec3 camPos{ 20.0f, -2.0f, 0.9f };
            CameraMotionParameters camParameters{ 100.0f, 150.0f, 300.0f };
            pCameraController = createFpsCameraController(camPos, lookAt);
            pCameraController->setMotionParameters(camParameters);
        }

        if (!initInputSystem(pWindow))
            return false;

        // Initialize microprofiler and it's UI.
        initProfiler();

        // Gpu profiler can only be added after initProfile.
        gGpuProfileToken = addGpuProfiler(pRenderer, pGraphicsQueue, "Graphics");

        // GUI - coppied from the first unit test
        {
            GuiDesc guiDesc = {};
            guiDesc.mStartPosition = vec2(mSettings.mWidth * 0.01f, mSettings.mHeight * 0.2f);
            pGuiWindow = gAppUI.AddGuiComponent(GetName(), &guiDesc);
    #if !defined(TARGET_IOS)
            pGuiWindow->AddWidget(CheckboxWidget("Toggle VSync\t\t\t\t\t", &bToggleVSync));
    #endif
            pGuiWindow->AddWidget(SliderFloatWidget("K (Tile size/radius)", &gTileSize,     5.0f,  100.0f, 1.0f));
            pGuiWindow->AddWidget(SliderFloatWidget("S (Sample count)",     &gSampleCount,  1.0f,  100.0f, 1.0f));
            pGuiWindow->AddWidget(SliderFloatWidget("Exposure time",        &gExposure,     0.01f, 0.4f,   0.00001f));
        }

        // App Actions
        {
            InputActionDesc actionDesc = {InputBindings::BUTTON_DUMP, [](InputActionContext *ctx) {  dumpProfileData(((Renderer*)ctx->pUserData), ((Renderer*)ctx->pUserData)->pName); return true; }, pRenderer};
            addInputAction(&actionDesc);
            actionDesc = {InputBindings::BUTTON_FULLSCREEN, [](InputActionContext *ctx) { toggleFullscreen(((IApp*)ctx->pUserData)->pWindow); return true; }, this};
            addInputAction(&actionDesc);
            actionDesc = {InputBindings::BUTTON_EXIT, [](InputActionContext *ctx) { requestShutdown(); return true; }};
            addInputAction(&actionDesc);
            actionDesc = {
                InputBindings::BUTTON_ANY, [](InputActionContext *ctx) {
                    bool capture = gAppUI.OnButton(ctx->mBinding, ctx->mBool, ctx->pPosition);
                    setEnableCaptureInput(capture && INPUT_ACTION_PHASE_CANCELED != ctx->mPhase);
                    return true;
                }, this
            };

            addInputAction(&actionDesc);
            typedef bool (*CameraInputHandler)(InputActionContext * ctx, uint32_t index);
            static CameraInputHandler onCameraInput = [](InputActionContext *ctx, uint32_t index) {
                if (!gAppUI.IsFocused() && *ctx->pCaptured)
                {
                    gVirtualJoystick.OnMove(index, ctx->mPhase != INPUT_ACTION_PHASE_CANCELED, ctx->pPosition);
                    index ? pCameraController->onRotate(ctx->mFloat2) : pCameraController->onMove(ctx->mFloat2);
                }
                return true;
            };
            actionDesc = {InputBindings::FLOAT_RIGHTSTICK, [](InputActionContext *ctx) { return onCameraInput(ctx, 1); }, NULL, 20.0f, 200.0f, 0.5f};
            addInputAction(&actionDesc);
            actionDesc = {InputBindings::FLOAT_LEFTSTICK, [](InputActionContext *ctx) { return onCameraInput(ctx, 0); }, NULL, 20.0f, 200.0f, 1.0f};
            addInputAction(&actionDesc);
            actionDesc = {InputBindings::BUTTON_NORTH, [](InputActionContext *ctx) { pCameraController->resetView(); return true; }};
            addInputAction(&actionDesc);
        }

        return true;
    }

    void Exit()
    {
        waitQueueIdle(pGraphicsQueue);

        exitInputSystem();
        destroyCameraController(pCameraController);
        gVirtualJoystick.Exit();
        gAppUI.Exit();
        exitProfiler();

        destroyReconstructPass();
        destroyNeighborPass();
        destroyTilePass();
        destroyGBufferPass();
        destroyEnvironmentBlock();
        
        for (uint32_t i = 0; i < 2; ++i)
            removeSampler(pRenderer, pStaticSamplers[i]);

        for (uint32_t i = 0; i < gImageCount; ++i)
        {
            removeFence(pRenderer, pRenderCompleteFences[i]);
            removeSemaphore(pRenderer, pRenderCompleteSemaphores[i]);

            removeCmd(pRenderer, pCmds[i]);
            removeCmdPool(pRenderer, pCmdPools[i]);
        }
        removeSemaphore(pRenderer, pImageAcquiredSemaphore);

        exitResourceLoaderInterface(pRenderer);
        removeQueue(pRenderer, pGraphicsQueue);
        removeRenderer(pRenderer);
    }

    bool Load()
    {
        if (!addSwapChain())
            return false;

        if (!loadGBufferPass())
            return false;

        if (!loadTilePass())
            return false;
        
        if (!loadNeighborPass())
            return false;

        if (!loadReconstructPass())
            return false;

        if (!gAppUI.Load(pSwapChain->ppRenderTargets, 1))
            return false;

        if (!gVirtualJoystick.Load(pSwapChain->ppRenderTargets[0]))
            return false;

        loadProfilerUI(&gAppUI, mSettings.mWidth, mSettings.mHeight);

        return true;
    }

    void Unload()
    {
        waitQueueIdle(pGraphicsQueue);
        unloadProfilerUI();
        gAppUI.Unload();
        gVirtualJoystick.Unload();

        unloadNeighborPass();
        unloadTilePass();
        unloadGBufferPass();
        unloadReconstructPass();
    }

    void Update(float deltaTime)
    {
#if !defined(TARGET_IOS)
        if (pSwapChain->mEnableVsync != bToggleVSync)
        {
            waitQueueIdle(pGraphicsQueue);
            gFrameIndex = 0;
            ::toggleVSync(pRenderer, &pSwapChain);
        }
#endif
        updateInputSystem(mSettings.mWidth, mSettings.mHeight);

        // Scene update
        {
            // Update the environement
            {
                // Camera
                float const horizontal_fov = PI / 2.0f;
                float const aspectInverse = float(mSettings.mHeight) / float(mSettings.mWidth);
                {	
                    // Copy the last frame's project and view
                    {
                        gEnv.mUniformData.mViewPrev = gEnv.mUniformData.mView;
                        gEnv.mUniformData.mProjectPrev = gEnv.mUniformData.mProject;
                    }

                    // Update the camera
                    pCameraController->update(deltaTime);

                    // Update current frame's project and view
                    {
                        mat4 viewMat = pCameraController->getViewMatrix();
                        gEnv.mUniformData.mView = viewMat;

                        mat4 projMat = mat4::perspective(horizontal_fov, aspectInverse, 0.1f, 1000.f);
                        gEnv.mUniformData.mProject = projMat;
                    }
                }

                // Point light parameters
                gEnv.mUniformData.mLightDirection = { -1.0f, -1.0f, -1.0f, 0.0f };
                gEnv.mUniformData.mLightColor = { 1.0f, 1.0f, 1.0f, 1.0f };
            }
        }

        // Update one object and copy the lastest MVP
        {
            static float vel = 50.0f;
            static vec3 pos = { 0.0f, -6.0f, 1.0f };
            static float rot = 0.0f;
            static mat4 lionTransform = mat4::scale({0.2f, 0.2f, -0.2f}) * mat4::identity();

            if (pos.getX() >= 10.0f)
            {
                vel = ::abs(vel) * -1.0f;
            }else if (pos.getX() <= -10.0f)
            {
                vel = ::abs(vel);
            }
            pos.setX(pos.getX() + (deltaTime * vel));
            rot += deltaTime * (::abs(vel)/5.0f);

            auto & obj = gSponza.mUniformBlock.mLion;				
            obj.mToWorldMatPrev = obj.mToWorldMat;
            obj.mToWorldMat = mat4::translation(pos) * mat4::rotationY(rot) * lionTransform;
        }

        // Update fps
        gDeltaTime = deltaTime;

        // Update GUI
        gAppUI.Update(deltaTime);
    }

    void Draw()
    {
        uint32_t swapchainImageIndex;
        acquireNextImage(pRenderer, pSwapChain, pImageAcquiredSemaphore, NULL, &swapchainImageIndex);

        RenderTarget *	pRenderTarget				= pSwapChain->ppRenderTargets[swapchainImageIndex];
        Semaphore *		pRenderCompleteSemaphore	= pRenderCompleteSemaphores[gFrameIndex];
        Fence *			pRenderCompleteFence		= pRenderCompleteFences[gFrameIndex];

        // Stall if CPU is running "Swap Chain Buffer Count" frames ahead of GPU
        FenceStatus fenceStatus;
        getFenceStatus(pRenderer, pRenderCompleteFence, &fenceStatus);
        if (FENCE_STATUS_INCOMPLETE == fenceStatus)
            waitForFences(pRenderer, 1, &pRenderCompleteFence);

        // Update uniform buffers
        {
            // Environment
            {
                BufferUpdateDesc envBuffer = { gEnv.pUniformBuffer[gFrameIndex] };
                beginUpdateResource(&envBuffer);
                *(Environment::UniformBlock *)envBuffer.pMappedData = gEnv.mUniformData;
                endUpdateResource(&envBuffer, NULL);
            }

            // Update objects buffer
            {
                BufferUpdateDesc buffer = { gSponza.pUniformBuffer[gFrameIndex] };
                beginUpdateResource(&buffer);
                *(Sponza::UniformBlock *)buffer.pMappedData = gSponza.mUniformBlock;
                endUpdateResource(&buffer, NULL);
            }
        }

        // Reset cmd pool for this frame
        resetCmdPool(pRenderer, pCmdPools[gFrameIndex]);

        Cmd * cmd = pCmds[gFrameIndex];
        beginCmd(cmd);
        {
cmdBeginGpuFrameProfile(cmd, gGpuProfileToken);

            // Draw each pass
            {
                // 1. GBuffer pass
                drawGBufferPass(cmd);

                // 2. Tile pass
                drawTilePass(cmd);

                // 3. Neighbor pass
                drawNeighborPass(cmd);

                // 2. Reconstruct pass
                drawReconstructPass(cmd, swapchainImageIndex);
            }

            // UI and finilization
            {
                LoadActionsDesc loadActions = {};
                loadActions.mLoadActionsColor[0] = LOAD_ACTION_LOAD;
                cmdBindRenderTargets(cmd, 1, &pRenderTarget, NULL, &loadActions, NULL, NULL, -1, -1);
                cmdBeginGpuTimestampQuery(cmd, gGpuProfileToken, "Draw UI");

                gVirtualJoystick.Draw(cmd, {1.0f, 1.0f, 1.0f, 1.0f});
                const float txtIndent = 8.f;
                float2 txtSizePx = cmdDrawCpuProfile(cmd, float2(txtIndent, 15.f), &gFrameTimeDraw);
                cmdDrawGpuProfile(cmd, float2(txtIndent, txtSizePx.y + 30.f), gGpuProfileToken, &gFrameTimeDraw);
                cmdDrawProfilerUI();
                gAppUI.Gui(pGuiWindow);
                gAppUI.Draw(cmd);
                cmdBindRenderTargets(cmd, 0, NULL, NULL, NULL, NULL, NULL, -1, -1);
cmdEndGpuTimestampQuery(cmd, gGpuProfileToken);

                RenderTargetBarrier barriers[] =
                {
                    { pRenderTarget, RESOURCE_STATE_RENDER_TARGET, RESOURCE_STATE_PRESENT },
                };
                cmdResourceBarrier(cmd, 0, NULL, 0, NULL, 1, barriers);
cmdEndGpuFrameProfile(cmd, gGpuProfileToken);
            }
        }
        endCmd(cmd);

        // Sumbit and present the queue
        {
            QueueSubmitDesc submitDesc = {};
            submitDesc.mCmdCount = 1;
            submitDesc.mSignalSemaphoreCount = 1;
            submitDesc.mWaitSemaphoreCount = 1;
            submitDesc.ppCmds = &cmd;
            submitDesc.ppSignalSemaphores = &pRenderCompleteSemaphore;
            submitDesc.ppWaitSemaphores = &pImageAcquiredSemaphore;
            submitDesc.pSignalFence = pRenderCompleteFence;
            queueSubmit(pGraphicsQueue, &submitDesc);

            QueuePresentDesc presentDesc = {};
            presentDesc.mIndex = swapchainImageIndex;
            presentDesc.mWaitSemaphoreCount = 1;
            presentDesc.pSwapChain = pSwapChain;
            presentDesc.ppWaitSemaphores = &pRenderCompleteSemaphore;
            presentDesc.mSubmitDone = true;
            queuePresent(pGraphicsQueue, &presentDesc);
            flipProfiler();
        }

        gFrameIndex = (gFrameIndex + 1) % gImageCount;
    }

    // Environment
    void createEnvironmentBlock()
    {
        BufferLoadDesc ubDesc = {};
        ubDesc.mDesc.mDescriptors = DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        ubDesc.mDesc.mMemoryUsage = RESOURCE_MEMORY_USAGE_CPU_TO_GPU;
        ubDesc.mDesc.mSize = sizeof(Environment::UniformBlock);
        ubDesc.mDesc.mFlags = BUFFER_CREATION_FLAG_PERSISTENT_MAP_BIT;
        ubDesc.pData = NULL;

        for (uint32_t i = 0; i < gImageCount; ++i)
        {
            ubDesc.ppBuffer = &gEnv.pUniformBuffer[i];
            addResource(&ubDesc, NULL);
        }
    }
    void destroyEnvironmentBlock()
    {
        for (uint32_t i = 0; i < gImageCount; ++i)
        {
            removeResource(gEnv.pUniformBuffer[i]);
        }
    }
    
    // GBuffer pass
    void createGBufferPass()
    {
        // Root Sig, sets and shaders
        {
            ShaderLoadDesc shader = {};
            shader.mStages[0] = {"gbuffer.vert", NULL, 0};
            shader.mStages[1] = {"gbuffer.frag", NULL, 0};
            addShader(pRenderer, &shader, &gGBufferPass.pShader);
            Shader * shaders[] = { gGBufferPass.pShader };

            RootSignatureDesc rootDesc = {};
            rootDesc.mStaticSamplerCount = 1;
            rootDesc.ppStaticSamplerNames = &pStaticSamplersNames[0];
            rootDesc.ppStaticSamplers = &pStaticSamplers[0];
            rootDesc.mShaderCount = 1;
            rootDesc.ppShaders = shaders;
            addRootSignature(pRenderer, &rootDesc, &gGBufferPass.pRootSignature);

            DescriptorSetDesc desc = { gGBufferPass.pRootSignature, DESCRIPTOR_UPDATE_FREQ_NONE, 1 };
            addDescriptorSet(pRenderer, &desc, &gGBufferPass.pDescriptorSets_NonFreq);

            desc = { gGBufferPass.pRootSignature, DESCRIPTOR_UPDATE_FREQ_PER_FRAME, gImageCount };
            addDescriptorSet(pRenderer, &desc, &gGBufferPass.pDescriptorSets_PerFrame);
        }

        // Setup pass uniform blocks
        {
            BufferLoadDesc ubDesc = {};
            ubDesc.mDesc.mDescriptors = DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            ubDesc.mDesc.mMemoryUsage = RESOURCE_MEMORY_USAGE_CPU_TO_GPU;
            ubDesc.mDesc.mSize = sizeof(Sponza::UniformBlock);
            ubDesc.mDesc.mFlags = BUFFER_CREATION_FLAG_PERSISTENT_MAP_BIT;
            ubDesc.pData = NULL;
            for (uint32_t i = 0; i < gImageCount; ++i)
            {
                ubDesc.ppBuffer = &gSponza.pUniformBuffer[i];
                addResource(&ubDesc, NULL);
            }
        }

        // Setup building positions
        {
            auto & building = gSponza.mUniformBlock.mBuilding;
            building.mToWorldMat = mat4::translation({0.0f, -6.0f, 0.0f}) * mat4::scale({0.02f, 0.02f, 0.02f}) * mat4::identity();
            building.mToWorldMatPrev = building.mToWorldMat;
        }
    }
    bool loadGBufferPass()
    {
        if (!addGBuffers())
            return false;

        if (!addDepthBuffer())
            return false;

        // Create the pipeline
        {		
            constexpr uint32_t GBUFFER_RT_COUNT = 3;
            TinyImageFormat colorFormats[GBUFFER_RT_COUNT] = 
            {
                gGBufferPass.pColorRT->mFormat,
                gGBufferPass.pNormRT->mFormat,
                gGBufferPass.pVelocityRT->mFormat,
            };

            DepthStateDesc depthStateDesc = {};
            depthStateDesc.mDepthTest = true;
            depthStateDesc.mDepthWrite = true;
            depthStateDesc.mDepthFunc = CompareMode::CMP_LEQUAL;

            RasterizerStateDesc rasterizerStateDesc = {};
            rasterizerStateDesc.mCullMode = CULL_MODE_NONE;

            PipelineDesc pipelineDesc = {};
            pipelineDesc.pName = "GBuffer Pipeline";
            pipelineDesc.mType = PIPELINE_TYPE_GRAPHICS;

            GraphicsPipelineDesc & graphicsPipelineDesc = pipelineDesc.mGraphicsDesc;
            graphicsPipelineDesc.mPrimitiveTopo = PRIMITIVE_TOPO_TRI_LIST;
            graphicsPipelineDesc.mRenderTargetCount = GBUFFER_RT_COUNT;
            graphicsPipelineDesc.pDepthState = &depthStateDesc;
            graphicsPipelineDesc.pColorFormats = colorFormats;
            graphicsPipelineDesc.mSampleCount = gGBufferPass.pColorRT->mSampleCount;
            graphicsPipelineDesc.mSampleQuality = gGBufferPass.pColorRT->mSampleQuality;
            graphicsPipelineDesc.mDepthStencilFormat = gGBufferPass.pDepthBuffer->mFormat;
            graphicsPipelineDesc.pRootSignature = gGBufferPass.pRootSignature;
            graphicsPipelineDesc.pShaderProgram = gGBufferPass.pShader;
            graphicsPipelineDesc.pVertexLayout = &gVertexLayout;
            graphicsPipelineDesc.pRasterizerState = &rasterizerStateDesc;
            addPipeline(pRenderer, &pipelineDesc, &gGBufferPass.pPipeline);
        }

        // Prepare descriptor sets
        {
            // No freq
            {             
                constexpr uint32_t PARAMS_COUNT = 1;
                DescriptorData params[PARAMS_COUNT] = {};
                params[0].pName = "textureMaps";
                params[0].ppTextures = gSponza.pMaterialTextures;
                params[0].mCount = Sponza::TOTAL_IMAGES;
                updateDescriptorSet(pRenderer, 0, gGBufferPass.pDescriptorSets_NonFreq, PARAMS_COUNT, params);
            }

            // Per frame
            {
                for (uint32_t i = 0; i < gImageCount; ++i)
                {
                    constexpr uint32_t PARAMS_COUNT = 2;
                    DescriptorData params[PARAMS_COUNT] = {};
                    params[0].pName = "envUniformBlock";
                    params[0].ppBuffers = &gEnv.pUniformBuffer[i];

                    params[1].pName = "objectsUniformBlock";
                    params[1].ppBuffers = &gSponza.pUniformBuffer[i];

                    updateDescriptorSet(pRenderer, i, gGBufferPass.pDescriptorSets_PerFrame, PARAMS_COUNT, params);
                }
            }
        }

        return true;
    }
    void unloadGBufferPass()
    {
        removePipeline(pRenderer, gGBufferPass.pPipeline);

        removeRenderTarget(pRenderer, gGBufferPass.pColorRT);
        removeRenderTarget(pRenderer, gGBufferPass.pNormRT);
        removeRenderTarget(pRenderer, gGBufferPass.pVelocityRT);
        removeRenderTarget(pRenderer, gGBufferPass.pDepthBuffer);
    }
    void destroyGBufferPass()
    { 
        for (uint32_t i = 0; i < gImageCount; ++i)
        {
            removeResource(gSponza.pUniformBuffer[i]);
        }

        for (uint32_t i = 0; i < Sponza::TOTAL_MODELS; ++i)
        {
            removeResource(gSponza.mModels[i]);
        }

        removeDescriptorSet(pRenderer, gGBufferPass.pDescriptorSets_NonFreq);
        removeDescriptorSet(pRenderer, gGBufferPass.pDescriptorSets_PerFrame);

        removeShader(pRenderer, gGBufferPass.pShader);
        removeRootSignature(pRenderer, gGBufferPass.pRootSignature);

        for (uint32_t i = 0; i < Sponza::TOTAL_IMAGES; ++i)
        {
            removeResource(gSponza.pMaterialTextures[i]);
        }

        gSponza.mTextureIndexforMaterial.set_capacity(0);
    }
    bool addGBuffers()
    {
        // Color RT
        {
            RenderTargetDesc colorRT = {};
            colorRT.mArraySize = 1;
            colorRT.mClearValue = { { 1.0f, 0.0f, 0.0f, 1.0f } };
            colorRT.mDepth = 1;
            colorRT.mDescriptors = DESCRIPTOR_TYPE_TEXTURE;
            colorRT.mWidth = mSettings.mWidth;
            colorRT.mHeight = mSettings.mHeight;
            colorRT.mSampleCount = SAMPLE_COUNT_1;
            colorRT.mFormat = TinyImageFormat_R32G32B32A32_SFLOAT;
            colorRT.mSampleQuality = 0;
            colorRT.mStartState = RESOURCE_STATE_SHADER_RESOURCE;
            colorRT.pName = "Color RT";

            addRenderTarget(pRenderer, &colorRT, &gGBufferPass.pColorRT);
        }

        // Normal RT
        {
            RenderTargetDesc normalRT = {};
            normalRT.mArraySize = 1;
            normalRT.mClearValue = { { 1.0f, 0.0f, 0.0f, 1.0f } };
            normalRT.mDepth = 1;
            normalRT.mDescriptors = DESCRIPTOR_TYPE_TEXTURE;
            normalRT.mWidth = mSettings.mWidth;
            normalRT.mHeight = mSettings.mHeight;
            normalRT.mSampleCount = SAMPLE_COUNT_1;
            normalRT.mFormat = TinyImageFormat_R16G16B16A16_SFLOAT;
            normalRT.mSampleQuality = 0;
            normalRT.mStartState = RESOURCE_STATE_SHADER_RESOURCE;
            normalRT.pName = "Normal RT";

            addRenderTarget(pRenderer, &normalRT, &gGBufferPass.pNormRT);
        }

        // Velocity vector RT
        {
            RenderTargetDesc velocityRT = {};
            velocityRT.mArraySize = 1;
            velocityRT.mClearValue = { { 1.0f, 0.0f, 0.0f, 1.0f } };
            velocityRT.mDepth = 1;
            velocityRT.mDescriptors = DESCRIPTOR_TYPE_TEXTURE;
            velocityRT.mWidth	= mSettings.mWidth;
            velocityRT.mHeight	= mSettings.mHeight;
            velocityRT.mSampleCount = SAMPLE_COUNT_1;
            velocityRT.mFormat = TinyImageFormat_R16G16_SFLOAT;
            velocityRT.mSampleQuality = 0;
            velocityRT.mStartState = RESOURCE_STATE_SHADER_RESOURCE;
            velocityRT.pName = "Velocity RT";

            addRenderTarget(pRenderer, &velocityRT, &gGBufferPass.pVelocityRT);
        }

        return NULL != gGBufferPass.pColorRT && NULL != gGBufferPass.pNormRT && NULL != gGBufferPass.pVelocityRT;
    }
    bool addDepthBuffer()
    {
        // Add depth buffer
        RenderTargetDesc depthRT = {};
        depthRT.mArraySize = 1;
        depthRT.mClearValue.depth = 1.0f;
        depthRT.mClearValue.stencil = 0;
        depthRT.mDepth = 1;
        depthRT.mFormat = TinyImageFormat_D32_SFLOAT;
        depthRT.mStartState = RESOURCE_STATE_DEPTH_WRITE;
        depthRT.mHeight = mSettings.mHeight;
        depthRT.mSampleCount = SAMPLE_COUNT_1;
        depthRT.mSampleQuality = 0;
        depthRT.mWidth = mSettings.mWidth;
        depthRT.mFlags = TEXTURE_CREATION_FLAG_ON_TILE;
        addRenderTarget(pRenderer, &depthRT, &gGBufferPass.pDepthBuffer);

        return gGBufferPass.pDepthBuffer != NULL;
    }
    void drawGBufferPass(Cmd * cmd)
    {   
        RenderTarget * colorBuffer	  = gGBufferPass.pColorRT;
        RenderTarget * normBuffer	  = gGBufferPass.pNormRT;
        RenderTarget * velocityBuffer = gGBufferPass.pVelocityRT;
        RenderTarget * depthBuffer	  = gGBufferPass.pDepthBuffer;

        RenderTargetBarrier barriers[] =
        {
            { colorBuffer,	  RESOURCE_STATE_SHADER_RESOURCE, RESOURCE_STATE_RENDER_TARGET },
            { normBuffer,	  RESOURCE_STATE_SHADER_RESOURCE, RESOURCE_STATE_RENDER_TARGET },
            { velocityBuffer, RESOURCE_STATE_SHADER_RESOURCE, RESOURCE_STATE_RENDER_TARGET },
        };
        cmdResourceBarrier(cmd, 0, NULL, 0, NULL, 3, barriers);
        
        // Clear
        {
            RenderTarget * renderTargets[] = { colorBuffer, normBuffer, velocityBuffer };
            LoadActionsDesc loadActions = {};
            loadActions.mLoadActionsColor[0] = LOAD_ACTION_CLEAR;
            loadActions.mLoadActionsColor[1] = LOAD_ACTION_CLEAR;
            loadActions.mLoadActionsColor[2] = LOAD_ACTION_CLEAR;
            loadActions.mLoadActionDepth	 = LOAD_ACTION_CLEAR;

            loadActions.mClearDepth = depthBuffer->mClearValue;
            cmdBeginGpuTimestampQuery(cmd, gGpuProfileToken, "GBuffer");
            cmdBindRenderTargets(cmd, 3, renderTargets, depthBuffer, &loadActions, NULL, NULL, -1, -1);
            cmdSetViewport(cmd, 0.0f, 0.0f, float(colorBuffer->mWidth), float(colorBuffer->mHeight), 0.0f, 1.0f);
            cmdSetScissor(cmd, 0, 0, colorBuffer->mWidth, colorBuffer->mHeight);
        }
        
        // Draw
        {
            cmdBindPipeline(cmd, gGBufferPass.pPipeline);
            cmdBindDescriptorSet(cmd, 0, gGBufferPass.pDescriptorSets_NonFreq);
            cmdBindDescriptorSet(cmd, gFrameIndex, gGBufferPass.pDescriptorSets_PerFrame);
            
            // Draw sponza building
            {                
                Geometry & sponzaMesh = *gSponza.mModels[0];
                uint32_t const drawCount = (uint32_t)sponzaMesh.mDrawArgCount;

                Buffer * pSponzaVertexBuffers[] = { sponzaMesh.pVertexBuffers[0] };
                cmdBindVertexBuffer(cmd, 1, pSponzaVertexBuffers, sponzaMesh.mVertexStrides, NULL);
                cmdBindIndexBuffer(cmd, sponzaMesh.pIndexBuffer, sponzaMesh.mIndexType, 0);

                for (uint32_t i = 0; i < drawCount; ++i)
                {
                    // Set the textureId
                    {
                        int materialID = gSponza.mMaterialIds[i];
                        materialID *= 5;    //because it uses 5 basic textures for redering BRDF

                        uint textureMaps = ((gSponza.mTextureIndexforMaterial[materialID + 0] & 0xFF) << 0)  |
                                           ((gSponza.mTextureIndexforMaterial[materialID + 1] & 0xFF) << 8)  |
                                           ((gSponza.mTextureIndexforMaterial[materialID + 2] & 0xFF) << 16) |
                                           ((gSponza.mTextureIndexforMaterial[materialID + 3] & 0xFF) << 24);
                        gGBufferPass.mPushConstant =
                        {
                            { float(mSettings.mWidth), float(mSettings.mHeight) },
                            gTileSize,
                            textureMaps,
                            0, // object index
                            gExposure,
                            gDeltaTime,
                        };
                        cmdBindPushConstants(cmd, gGBufferPass.pRootSignature, "cbRootConstants", &gGBufferPass.mPushConstant);
                    }

                    IndirectDrawIndexArguments & cmdData = sponzaMesh.pDrawArgs[i];
                    cmdDrawIndexed(cmd, cmdData.mIndexCount, cmdData.mStartIndex, cmdData.mVertexOffset);
                }
            }

            // Draw lion
            {
                // Set the textureId
                {
                    uint textureMaps = ((63 & 0xFF) << 0) | ((83 & 0xFF) << 8) | ((6 & 0xFF) << 16) | ((6 & 0xFF) << 24);
                    gGBufferPass.mPushConstant =
                    {
                        { float(mSettings.mWidth), float(mSettings.mHeight) },
                        gTileSize,
                        textureMaps,
                        1, // object index
                        gExposure,
                        gDeltaTime,
                    };
                    cmdBindPushConstants(cmd, gGBufferPass.pRootSignature, "cbRootConstants", &gGBufferPass.mPushConstant);
                }

                Geometry & lionMesh = *gSponza.mModels[1];
                Buffer * pLionVertexBuffers[] = { lionMesh.pVertexBuffers[0] };
                cmdBindVertexBuffer(cmd, 1, pLionVertexBuffers, lionMesh.mVertexStrides, NULL);
                cmdBindIndexBuffer(cmd, lionMesh.pIndexBuffer, lionMesh.mIndexType, 0);

                for (uint32_t i = 0; i < (uint32_t)lionMesh.mDrawArgCount; ++i)
                {
                    IndirectDrawIndexArguments& cmdData = lionMesh.pDrawArgs[i];
                    cmdDrawIndexed(cmd, cmdData.mIndexCount, cmdData.mStartIndex, cmdData.mVertexOffset);
                }
            }
        }

        cmdBindRenderTargets(cmd, 0, NULL, 0, NULL, NULL, NULL, -1, -1);
        cmdEndGpuTimestampQuery(cmd, gGpuProfileToken);
    }
    
    // Tile pass
    void createTilePass()
    {
        // Root Sig, sets and shaders
        {
            ShaderLoadDesc shader = {};
            shader.mStages[0] = {"tile.comp", NULL, 0};

            addShader(pRenderer, &shader, &gTilePass.pShader);
            Shader * shaders[] = { gTilePass.pShader };

            RootSignatureDesc rootDesc = {};
            rootDesc.mStaticSamplerCount = 0;
            rootDesc.ppStaticSamplerNames = NULL;
            rootDesc.ppStaticSamplers = NULL;
            rootDesc.mShaderCount = 1;
            rootDesc.ppShaders = shaders;
            addRootSignature(pRenderer, &rootDesc, &gTilePass.pRootSignature);

            DescriptorSetDesc desc = { gTilePass.pRootSignature, DESCRIPTOR_UPDATE_FREQ_PER_FRAME, 1 };
            addDescriptorSet(pRenderer, &desc, &gTilePass.pDescriptorSets_PerFrame);
        }
    }
    bool loadTilePass()
    {
        if (!addTileBuffer())
            return false;

        // Create the pipeline
        {		
            PipelineDesc pipelineDesc = {};
            pipelineDesc.pName = "Tile Pipeline";
            pipelineDesc.mType = PIPELINE_TYPE_COMPUTE;

            ComputePipelineDesc & computePipelineDesc = pipelineDesc.mComputeDesc;
            computePipelineDesc.pRootSignature = gTilePass.pRootSignature;
            computePipelineDesc.pShaderProgram = gTilePass.pShader;
            addPipeline(pRenderer, &pipelineDesc, &gTilePass.pPipeline);
        }

        // Prepare descriptor sets
        {	
            constexpr uint32_t paramsCount = 2;
            DescriptorData params[paramsCount] = {};
            params[0].pName = "outputTexture";
            params[0].ppTextures = &gTilePass.pTileTexture;
            params[1].pName = "velocityTexture";
            params[1].ppTextures = &gGBufferPass.pVelocityRT->pTexture;

            updateDescriptorSet(pRenderer, 0, gTilePass.pDescriptorSets_PerFrame, paramsCount, params);
        }

        return true;
    }
    void unloadTilePass()
    {
        removePipeline(pRenderer, gTilePass.pPipeline);
        removeResource(gTilePass.pTileTexture);
    }
    void destroyTilePass()
    {
        removeDescriptorSet(pRenderer, gTilePass.pDescriptorSets_PerFrame);
        removeShader(pRenderer, gTilePass.pShader);
        removeRootSignature(pRenderer, gTilePass.pRootSignature);
    }
    bool addTileBuffer()
    {
        TextureDesc tileRT = {};
        tileRT.mArraySize = 1;
        tileRT.mMipLevels = 1;
        tileRT.mDepth = 1;
        tileRT.mDescriptors = DESCRIPTOR_TYPE_TEXTURE | DESCRIPTOR_TYPE_RW_TEXTURE;
        tileRT.mWidth	= mSettings.mWidth / int32_t(gTileSize);
        tileRT.mHeight	= mSettings.mHeight / int32_t(gTileSize);
        tileRT.mSampleCount = SAMPLE_COUNT_1;
        tileRT.mHostVisible = false;
        tileRT.mFormat = TinyImageFormat_R16G16_SFLOAT;
        tileRT.mStartState = RESOURCE_STATE_SHADER_RESOURCE;
        tileRT.pName = "Tile RT";

        TextureLoadDesc textureDesc = {};
        textureDesc.pDesc = &tileRT;
        textureDesc.ppTexture = &gTilePass.pTileTexture;
        addResource(&textureDesc, NULL);

        return NULL != gTilePass.pTileTexture;
    }
    void drawTilePass(Cmd * cmd)
    {		
        RenderTarget * colorRT    = gGBufferPass.pColorRT;
        RenderTarget * velocityRT = gGBufferPass.pVelocityRT;

        // Resources barriers
        {
            RenderTargetBarrier barriers[] =
            {
                { colorRT,	  RESOURCE_STATE_RENDER_TARGET, RESOURCE_STATE_SHADER_RESOURCE },
                { velocityRT, RESOURCE_STATE_RENDER_TARGET, RESOURCE_STATE_SHADER_RESOURCE },
            };

            TextureBarrier textureBarriers[] =
            {
                { gTilePass.pTileTexture, RESOURCE_STATE_SHADER_RESOURCE, RESOURCE_STATE_UNORDERED_ACCESS },
            };

            cmdResourceBarrier(cmd, 0, NULL, 1, textureBarriers, 2, barriers);
        }

        // Draw
        {
            cmdBeginGpuTimestampQuery(cmd, gGpuProfileToken, "Tile Pass");
            cmdBindPipeline(cmd, gTilePass.pPipeline);

            gPushConstant =
            {
                { float(1.0f / velocityRT->pTexture->mWidth), float(1.0f / velocityRT->pTexture->mHeight) },
                gTileSize,
                gSampleCount,
            };
            cmdBindPushConstants(cmd, gTilePass.pRootSignature, "cbRootConstants", &gPushConstant);

            cmdBindDescriptorSet(cmd, 0, gTilePass.pDescriptorSets_PerFrame);

            auto threadGroupSize = gTilePass.pShader->pReflection->mStageReflections[0].mNumThreadsPerGroup;
            uint32_t groupCountX = velocityRT->pTexture->mWidth  /  uint32_t(gTileSize) / threadGroupSize[0] + 1;
            uint32_t groupCountY = velocityRT->pTexture->mHeight /  uint32_t(gTileSize) / threadGroupSize[1] + 1;
            cmdDispatch(cmd, groupCountX, groupCountY, 1);
        }

        cmdBindRenderTargets(cmd, 0, NULL, 0, NULL, NULL, NULL, -1, -1);
        cmdEndGpuTimestampQuery(cmd, gGpuProfileToken);
    }
    
    // Neighbor pass
    void createNeighborPass()
    {
        // Root Sig, sets and shaders
        {
            ShaderLoadDesc shader = {};
            shader.mStages[0] = {"neighbor.comp", NULL, 0};
            addShader(pRenderer, &shader, &gNeighborPass.pShader);
            Shader * shaders[] = { gNeighborPass.pShader };

            RootSignatureDesc rootDesc = {};
            rootDesc.mStaticSamplerCount = 0;
            rootDesc.ppStaticSamplerNames = NULL;
            rootDesc.ppStaticSamplers = NULL;
            rootDesc.mShaderCount = 1;
            rootDesc.ppShaders = shaders;
            addRootSignature(pRenderer, &rootDesc, &gNeighborPass.pRootSignature);

            DescriptorSetDesc desc = { gNeighborPass.pRootSignature, DESCRIPTOR_UPDATE_FREQ_PER_FRAME, 1 };
            addDescriptorSet(pRenderer, &desc, &gNeighborPass.pDescriptorSets_PerFrame);
        }
    }
    bool loadNeighborPass()
    {
        if (!addNeighborBuffer())
            return false;

        // Create the pipeline
        {		
            RasterizerStateDesc rasterizerStateDesc = {};
            rasterizerStateDesc.mCullMode = CULL_MODE_NONE;

            PipelineDesc pipelineDesc = {};
            pipelineDesc.pName = "Neighbor Pipeline";
            pipelineDesc.mType = PIPELINE_TYPE_COMPUTE;

            ComputePipelineDesc & computePipelineDesc = pipelineDesc.mComputeDesc;
            computePipelineDesc.pRootSignature = gNeighborPass.pRootSignature;
            computePipelineDesc.pShaderProgram = gNeighborPass.pShader;
            addPipeline(pRenderer, &pipelineDesc, &gNeighborPass.pPipeline);
        }

        // Prepare descriptor sets
        {
            constexpr uint32_t paramsCount = 2;
            DescriptorData params[paramsCount] = {};
            params[0].pName = "outputTexture";
            params[0].ppTextures = &gNeighborPass.pNeighborTexture;
            params[1].pName = "tileTexture";
            params[1].ppTextures = &gTilePass.pTileTexture;

            updateDescriptorSet(pRenderer, 0, gNeighborPass.pDescriptorSets_PerFrame, paramsCount, params);
        }

        return true;
    }
    void unloadNeighborPass()
    {
        removePipeline(pRenderer, gNeighborPass.pPipeline);
        removeResource(gNeighborPass.pNeighborTexture);
    }
    void destroyNeighborPass()
    {
        removeDescriptorSet(pRenderer, gNeighborPass.pDescriptorSets_PerFrame);
        removeShader(pRenderer, gNeighborPass.pShader);
        removeRootSignature(pRenderer, gNeighborPass.pRootSignature);
    }
    bool addNeighborBuffer()
    {       
        TextureDesc neighborRT = {};
        neighborRT.mArraySize = 1;
        neighborRT.mMipLevels = 1;
        neighborRT.mDepth = 1;
        neighborRT.mDescriptors = DESCRIPTOR_TYPE_TEXTURE | DESCRIPTOR_TYPE_RW_TEXTURE;
        neighborRT.mWidth	= mSettings.mWidth / int32_t(gTileSize);
        neighborRT.mHeight	= mSettings.mHeight / int32_t(gTileSize);
        neighborRT.mSampleCount = SAMPLE_COUNT_1;
        neighborRT.mHostVisible = false;
        neighborRT.mFormat = TinyImageFormat_R16G16_SFLOAT;
        neighborRT.mStartState = RESOURCE_STATE_SHADER_RESOURCE;
        neighborRT.pName = "Neighbor RT";

        TextureLoadDesc textureDesc = {};
        textureDesc.pDesc = &neighborRT;
        textureDesc.ppTexture = &gNeighborPass.pNeighborTexture;
        addResource(&textureDesc, NULL);

        return NULL != gNeighborPass.pNeighborTexture;
    }
    void drawNeighborPass(Cmd * cmd)
    {    
        Texture * tileTexture = gTilePass.pTileTexture;
        RenderTarget * velocityRT = gGBufferPass.pVelocityRT;

        // Resources barriers
        {
            TextureBarrier textureBarriers[] =
            {
                { tileTexture,                    RESOURCE_STATE_UNORDERED_ACCESS, RESOURCE_STATE_SHADER_RESOURCE  },
                { gNeighborPass.pNeighborTexture, RESOURCE_STATE_SHADER_RESOURCE,  RESOURCE_STATE_UNORDERED_ACCESS },
            };
            cmdResourceBarrier(cmd, 0, NULL, 2, textureBarriers, 0, NULL);
        }

        // Draw
        {
            cmdBeginGpuTimestampQuery(cmd, gGpuProfileToken, "Neighbor Pass");
            cmdBindPipeline(cmd, gNeighborPass.pPipeline);

            cmdBindDescriptorSet(cmd, 0, gNeighborPass.pDescriptorSets_PerFrame);
            
            auto threadGroupSize = gNeighborPass.pShader->pReflection->mStageReflections[0].mNumThreadsPerGroup;
            uint32_t groupCountX = tileTexture->mWidth  / threadGroupSize[0] + 1;
            uint32_t groupCountY = tileTexture->mHeight / threadGroupSize[1] + 1;
            cmdDispatch(cmd, groupCountX, groupCountY, 1);
        }

        cmdBindRenderTargets(cmd, 0, NULL, 0, NULL, NULL, NULL, -1, -1);
        cmdEndGpuTimestampQuery(cmd, gGpuProfileToken);
    }
    
    // Reconstruct pass
    void createReconstructPass()
    {
        // Root Sig, sets and shaders
        {
            ShaderLoadDesc shader = {};
            shader.mStages[0] = {"reconstruct.vert", NULL, 0};
            shader.mStages[1] = {"reconstruct.frag", NULL, 0};
            addShader(pRenderer, &shader, &gReconstructPass.pShader);
            Shader * shaders[] = { gReconstructPass.pShader };

            RootSignatureDesc rootDesc = {};
            rootDesc.mStaticSamplerCount = SAMPLERS_COUNT;
            rootDesc.ppStaticSamplerNames = pStaticSamplersNames;
            rootDesc.ppStaticSamplers = pStaticSamplers;
            rootDesc.mShaderCount = 1;
            rootDesc.ppShaders = shaders;
            addRootSignature(pRenderer, &rootDesc, &gReconstructPass.pRootSignature);

            DescriptorSetDesc desc = { gReconstructPass.pRootSignature, DESCRIPTOR_UPDATE_FREQ_PER_FRAME, gImageCount };
            addDescriptorSet(pRenderer, &desc, &gReconstructPass.pDescriptorSets);
        }
    }
    bool loadReconstructPass()
    {	
        // Create the pipeline
        {		
            RasterizerStateDesc rasterizerStateDesc = {};
            rasterizerStateDesc.mCullMode = CULL_MODE_NONE;

            PipelineDesc pipelineDesc = {};
            pipelineDesc.pName = "Reconstruct Pipeline";
            pipelineDesc.mType = PIPELINE_TYPE_GRAPHICS;

            GraphicsPipelineDesc & graphicsPipelineDesc = pipelineDesc.mGraphicsDesc;
            graphicsPipelineDesc.mPrimitiveTopo = PRIMITIVE_TOPO_TRI_LIST;
            graphicsPipelineDesc.mRenderTargetCount = 1;
            graphicsPipelineDesc.pColorFormats = &pSwapChain->ppRenderTargets[0]->mFormat;
            graphicsPipelineDesc.mSampleCount = pSwapChain->ppRenderTargets[0]->mSampleCount;
            graphicsPipelineDesc.mSampleQuality = pSwapChain->ppRenderTargets[0]->mSampleQuality;
            graphicsPipelineDesc.pRootSignature = gReconstructPass.pRootSignature;
            graphicsPipelineDesc.pShaderProgram = gReconstructPass.pShader;
            graphicsPipelineDesc.pVertexLayout = NULL;
            graphicsPipelineDesc.pRasterizerState = &rasterizerStateDesc;
            addPipeline(pRenderer, &pipelineDesc, &gReconstructPass.pPipeline);
        }

        // Prepare descriptor sets
        {
            for (uint32_t i = 0; i < gImageCount; ++i)
            {
                constexpr uint32_t paramsCount = 5;
                DescriptorData params[paramsCount] = {};

                params[0].pName = "envUniformBlock";
                params[0].ppBuffers = &gEnv.pUniformBuffer[i];

                params[1].pName = "colorTexture";
                params[1].ppTextures = &gGBufferPass.pColorRT->pTexture;

                params[2].pName = "normTexture";
                params[2].ppTextures = &gGBufferPass.pNormRT->pTexture;

                params[3].pName = "velocityTexture";
                params[3].ppTextures = &gGBufferPass.pVelocityRT->pTexture;

                params[4].pName = "neighborTexture";
                params[4].ppTextures = &gNeighborPass.pNeighborTexture;

                updateDescriptorSet(pRenderer, i, gReconstructPass.pDescriptorSets, paramsCount, params);
            }
        }
        return true;
    }
    void unloadReconstructPass()
    {
        removePipeline(pRenderer, gReconstructPass.pPipeline);
        removeSwapChain(pRenderer, pSwapChain);
    }
    void destroyReconstructPass()
    {
        removeDescriptorSet(pRenderer, gReconstructPass.pDescriptorSets);
        removeShader(pRenderer, gReconstructPass.pShader);
        removeRootSignature(pRenderer, gReconstructPass.pRootSignature);
    }
    bool addSwapChain()
    {
        SwapChainDesc swapChainDesc = {};
        swapChainDesc.mWindowHandle = pWindow->handle;
        swapChainDesc.mPresentQueueCount = 1;
        swapChainDesc.ppPresentQueues = &pGraphicsQueue;
        swapChainDesc.mWidth = mSettings.mWidth;
        swapChainDesc.mHeight = mSettings.mHeight;
        swapChainDesc.mImageCount = gImageCount;
        swapChainDesc.mColorFormat = getRecommendedSwapchainFormat(true);
        swapChainDesc.mEnableVsync = mSettings.mDefaultVSyncEnabled;
        ::addSwapChain(pRenderer, &swapChainDesc, &pSwapChain);

        return NULL != pSwapChain;
    }
    void drawReconstructPass(Cmd * cmd, uint32_t swapchainImageIndex)
    {   
        auto renderTarget = pSwapChain->ppRenderTargets[swapchainImageIndex];

        // Resources barriers
        {
            RenderTargetBarrier barriers[] =
            {
                { gGBufferPass.pNormRT, RESOURCE_STATE_RENDER_TARGET, RESOURCE_STATE_SHADER_RESOURCE },
                { renderTarget,         RESOURCE_STATE_PRESENT,       RESOURCE_STATE_RENDER_TARGET   },
            };

            TextureBarrier textureBarriers[] =
            {
                { gNeighborPass.pNeighborTexture, RESOURCE_STATE_UNORDERED_ACCESS, RESOURCE_STATE_SHADER_RESOURCE },
            };

            cmdResourceBarrier(cmd, 0, NULL, 1, textureBarriers, 2, barriers); // Other RTs are already in the correct state from previous passes
        }

        // Clear
        {
            LoadActionsDesc loadActions = {};
            loadActions.mLoadActionsColor[0] = LOAD_ACTION_CLEAR;
            cmdBeginGpuTimestampQuery(cmd, gGpuProfileToken, "Reconstruct Pass");
            cmdBindRenderTargets(cmd, 1, &renderTarget, NULL, &loadActions, NULL, NULL, -1, -1);
            cmdSetViewport(cmd, 0.0f, 0.0f, float(renderTarget->mWidth), float(renderTarget->mHeight), 0.0f, 1.0f);
            cmdSetScissor(cmd, 0, 0, renderTarget->mWidth, renderTarget->mHeight);
        }
        
        // Draw
        {
            cmdBindPipeline(cmd, gReconstructPass.pPipeline);

            gPushConstant =
            {
                { float(1.0f / gGBufferPass.pVelocityRT->mWidth), float(1.0f / gGBufferPass.pVelocityRT->mHeight) },
                gTileSize,
                gSampleCount,
            };
            cmdBindPushConstants(cmd, gReconstructPass.pRootSignature, "cbRootConstants", &gPushConstant);

            cmdBindDescriptorSet(cmd, gFrameIndex, gReconstructPass.pDescriptorSets);
            cmdDraw(cmd, 3, 0);
        }

        cmdBindRenderTargets(cmd, 0, NULL, 0, NULL, NULL, NULL, -1, -1);
        cmdEndGpuTimestampQuery(cmd, gGpuProfileToken);
    }

    void loadMesh(size_t index)
    {
        //Load Sponza
        GeometryLoadDesc loadDesc = {};
        loadDesc.pFileName = gSponza.mModelNames[index];
        loadDesc.ppGeometry = &gSponza.mModels[index];
        loadDesc.pVertexLayout = &gVertexLayout;
        addResource(&loadDesc, NULL);
    }

    void loadTexture(size_t index)
    {
        static char const * MATERIAL_IMAGE_FILE_NAMES[] =
        {
            "SponzaPBR_Textures/ao",
            "SponzaPBR_Textures/ao",
            "SponzaPBR_Textures/ao",
            "SponzaPBR_Textures/ao",
            "SponzaPBR_Textures/ao",

            // Common
            "SponzaPBR_Textures/ao",
            "SponzaPBR_Textures/Dielectric_metallic",
            "SponzaPBR_Textures/Metallic_metallic",
            "SponzaPBR_Textures/gi_flag",

            // Background
            "SponzaPBR_Textures/Background/Background_Albedo",
            "SponzaPBR_Textures/Background/Background_Normal",
            "SponzaPBR_Textures/Background/Background_Roughness",

            // ChainTexture
            "SponzaPBR_Textures/ChainTexture/ChainTexture_Albedo",
            "SponzaPBR_Textures/ChainTexture/ChainTexture_Metallic",
            "SponzaPBR_Textures/ChainTexture/ChainTexture_Normal",
            "SponzaPBR_Textures/ChainTexture/ChainTexture_Roughness",

            // Lion
            "SponzaPBR_Textures/Lion/Lion_Albedo",
            "SponzaPBR_Textures/Lion/Lion_Normal",
            "SponzaPBR_Textures/Lion/Lion_Roughness",

            // Sponza_Arch
            "SponzaPBR_Textures/Sponza_Arch/Sponza_Arch_diffuse",
            "SponzaPBR_Textures/Sponza_Arch/Sponza_Arch_normal",
            "SponzaPBR_Textures/Sponza_Arch/Sponza_Arch_roughness",

            // Sponza_Bricks
            "SponzaPBR_Textures/Sponza_Bricks/Sponza_Bricks_a_Albedo",
            "SponzaPBR_Textures/Sponza_Bricks/Sponza_Bricks_a_Normal",
            "SponzaPBR_Textures/Sponza_Bricks/Sponza_Bricks_a_Roughness",

            // Sponza_Ceiling
            "SponzaPBR_Textures/Sponza_Ceiling/Sponza_Ceiling_diffuse",
            "SponzaPBR_Textures/Sponza_Ceiling/Sponza_Ceiling_normal",
            "SponzaPBR_Textures/Sponza_Ceiling/Sponza_Ceiling_roughness",

            // Sponza_Column
            "SponzaPBR_Textures/Sponza_Column/Sponza_Column_a_diffuse",
            "SponzaPBR_Textures/Sponza_Column/Sponza_Column_a_normal",
            "SponzaPBR_Textures/Sponza_Column/Sponza_Column_a_roughness",

            "SponzaPBR_Textures/Sponza_Column/Sponza_Column_b_diffuse",
            "SponzaPBR_Textures/Sponza_Column/Sponza_Column_b_normal",
            "SponzaPBR_Textures/Sponza_Column/Sponza_Column_b_roughness",

            "SponzaPBR_Textures/Sponza_Column/Sponza_Column_c_diffuse",
            "SponzaPBR_Textures/Sponza_Column/Sponza_Column_c_normal",
            "SponzaPBR_Textures/Sponza_Column/Sponza_Column_c_roughness",

            // Sponza_Curtain
            "SponzaPBR_Textures/Sponza_Curtain/Sponza_Curtain_Blue_diffuse",
            "SponzaPBR_Textures/Sponza_Curtain/Sponza_Curtain_Blue_normal",

            "SponzaPBR_Textures/Sponza_Curtain/Sponza_Curtain_Green_diffuse",
            "SponzaPBR_Textures/Sponza_Curtain/Sponza_Curtain_Green_normal",

            "SponzaPBR_Textures/Sponza_Curtain/Sponza_Curtain_Red_diffuse",
            "SponzaPBR_Textures/Sponza_Curtain/Sponza_Curtain_Red_normal",

            "SponzaPBR_Textures/Sponza_Curtain/Sponza_Curtain_metallic",
            "SponzaPBR_Textures/Sponza_Curtain/Sponza_Curtain_roughness",

            // Sponza_Details
            "SponzaPBR_Textures/Sponza_Details/Sponza_Details_diffuse",
            "SponzaPBR_Textures/Sponza_Details/Sponza_Details_metallic",
            "SponzaPBR_Textures/Sponza_Details/Sponza_Details_normal",
            "SponzaPBR_Textures/Sponza_Details/Sponza_Details_roughness",

            // Sponza_Fabric
            "SponzaPBR_Textures/Sponza_Fabric/Sponza_Fabric_Blue_diffuse",
            "SponzaPBR_Textures/Sponza_Fabric/Sponza_Fabric_Blue_normal",

            "SponzaPBR_Textures/Sponza_Fabric/Sponza_Fabric_Green_diffuse",
            "SponzaPBR_Textures/Sponza_Fabric/Sponza_Fabric_Green_normal",

            "SponzaPBR_Textures/Sponza_Fabric/Sponza_Fabric_metallic",
            "SponzaPBR_Textures/Sponza_Fabric/Sponza_Fabric_roughness",

            "SponzaPBR_Textures/Sponza_Fabric/Sponza_Fabric_Red_diffuse",
            "SponzaPBR_Textures/Sponza_Fabric/Sponza_Fabric_Red_normal",

            // Sponza_FlagPole
            "SponzaPBR_Textures/Sponza_FlagPole/Sponza_FlagPole_diffuse",
            "SponzaPBR_Textures/Sponza_FlagPole/Sponza_FlagPole_normal",
            "SponzaPBR_Textures/Sponza_FlagPole/Sponza_FlagPole_roughness",

            // Sponza_Floor
            "SponzaPBR_Textures/Sponza_Floor/Sponza_Floor_diffuse",
            "SponzaPBR_Textures/Sponza_Floor/Sponza_Floor_normal",
            "SponzaPBR_Textures/Sponza_Floor/Sponza_Floor_roughness",

            // Sponza_Roof
            "SponzaPBR_Textures/Sponza_Roof/Sponza_Roof_diffuse",
            "SponzaPBR_Textures/Sponza_Roof/Sponza_Roof_normal",
            "SponzaPBR_Textures/Sponza_Roof/Sponza_Roof_roughness",

            // Sponza_Thorn
            "SponzaPBR_Textures/Sponza_Thorn/Sponza_Thorn_diffuse",
            "SponzaPBR_Textures/Sponza_Thorn/Sponza_Thorn_normal",
            "SponzaPBR_Textures/Sponza_Thorn/Sponza_Thorn_roughness",

            // Vase
            "SponzaPBR_Textures/Vase/Vase_diffuse",
            "SponzaPBR_Textures/Vase/Vase_normal",
            "SponzaPBR_Textures/Vase/Vase_roughness",

            // VaseHanging
            "SponzaPBR_Textures/VaseHanging/VaseHanging_diffuse",
            "SponzaPBR_Textures/VaseHanging/VaseHanging_normal",
            "SponzaPBR_Textures/VaseHanging/VaseHanging_roughness",

            // VasePlant
            "SponzaPBR_Textures/VasePlant/VasePlant_diffuse",
            "SponzaPBR_Textures/VasePlant/VasePlant_normal",
            "SponzaPBR_Textures/VasePlant/VasePlant_roughness",

            // VaseRound
            "SponzaPBR_Textures/VaseRound/VaseRound_diffuse",
            "SponzaPBR_Textures/VaseRound/VaseRound_normal",
            "SponzaPBR_Textures/VaseRound/VaseRound_roughness",

            "lion/lion_albedo",
            "lion/lion_specular",
            "lion/lion_normal",
        };

        TextureLoadDesc textureDesc = {};
        textureDesc.pFileName = MATERIAL_IMAGE_FILE_NAMES[index];
        textureDesc.ppTexture = &gSponza.pMaterialTextures[index];
        addResource(&textureDesc, NULL);
    }

    char const * GetName() { return "Motion Blur"; }
};

void Sponza::assignTextures()
{
    int AO = 5;
    int NoMetallic = 6;

    // 00 : leaf
    mTextureIndexforMaterial.push_back(66);
    mTextureIndexforMaterial.push_back(67);
    mTextureIndexforMaterial.push_back(NoMetallic);
    mTextureIndexforMaterial.push_back(68);
    mTextureIndexforMaterial.push_back(AO);

    // 01 : vase_round
    mTextureIndexforMaterial.push_back(78);
    mTextureIndexforMaterial.push_back(79);
    mTextureIndexforMaterial.push_back(NoMetallic);
    mTextureIndexforMaterial.push_back(80);
    mTextureIndexforMaterial.push_back(AO);

    // 02 : 16___Default (gi_flag)
    mTextureIndexforMaterial.push_back(8);
    mTextureIndexforMaterial.push_back(8);    // !!!!!!
    mTextureIndexforMaterial.push_back(NoMetallic);
    mTextureIndexforMaterial.push_back(8);    // !!!!!
    mTextureIndexforMaterial.push_back(AO);

    //03 : Material__57 (Plant)
    mTextureIndexforMaterial.push_back(75);
    mTextureIndexforMaterial.push_back(76);
    mTextureIndexforMaterial.push_back(NoMetallic);
    mTextureIndexforMaterial.push_back(77);
    mTextureIndexforMaterial.push_back(AO);

    // 04 : Material__298
    mTextureIndexforMaterial.push_back(9);
    mTextureIndexforMaterial.push_back(10);
    mTextureIndexforMaterial.push_back(NoMetallic);
    mTextureIndexforMaterial.push_back(11);
    mTextureIndexforMaterial.push_back(AO);

    // 05 : bricks
    mTextureIndexforMaterial.push_back(22);
    mTextureIndexforMaterial.push_back(23);
    mTextureIndexforMaterial.push_back(NoMetallic);
    mTextureIndexforMaterial.push_back(24);
    mTextureIndexforMaterial.push_back(AO);

    // 06 :  arch
    mTextureIndexforMaterial.push_back(19);
    mTextureIndexforMaterial.push_back(20);
    mTextureIndexforMaterial.push_back(NoMetallic);
    mTextureIndexforMaterial.push_back(21);
    mTextureIndexforMaterial.push_back(AO);

    // 07 : ceiling
    mTextureIndexforMaterial.push_back(25);
    mTextureIndexforMaterial.push_back(26);
    mTextureIndexforMaterial.push_back(NoMetallic);
    mTextureIndexforMaterial.push_back(27);
    mTextureIndexforMaterial.push_back(AO);

    // 08 : column_a
    mTextureIndexforMaterial.push_back(28);
    mTextureIndexforMaterial.push_back(29);
    mTextureIndexforMaterial.push_back(NoMetallic);
    mTextureIndexforMaterial.push_back(30);
    mTextureIndexforMaterial.push_back(AO);

    // 09 : Floor
    mTextureIndexforMaterial.push_back(60);
    mTextureIndexforMaterial.push_back(61);
    mTextureIndexforMaterial.push_back(NoMetallic);
    mTextureIndexforMaterial.push_back(6);
    mTextureIndexforMaterial.push_back(AO);

    // 10 : column_c
    mTextureIndexforMaterial.push_back(34);
    mTextureIndexforMaterial.push_back(35);
    mTextureIndexforMaterial.push_back(NoMetallic);
    mTextureIndexforMaterial.push_back(36);
    mTextureIndexforMaterial.push_back(AO);

    // 11 : details
    mTextureIndexforMaterial.push_back(45);
    mTextureIndexforMaterial.push_back(47);
    mTextureIndexforMaterial.push_back(46);
    mTextureIndexforMaterial.push_back(48);
    mTextureIndexforMaterial.push_back(AO);

    // 12 : column_b
    mTextureIndexforMaterial.push_back(31);
    mTextureIndexforMaterial.push_back(32);
    mTextureIndexforMaterial.push_back(NoMetallic);
    mTextureIndexforMaterial.push_back(33);
    mTextureIndexforMaterial.push_back(AO);

    // 13 : flagpole
    mTextureIndexforMaterial.push_back(57);
    mTextureIndexforMaterial.push_back(58);
    mTextureIndexforMaterial.push_back(NoMetallic);
    mTextureIndexforMaterial.push_back(59);
    mTextureIndexforMaterial.push_back(AO);

    // 14 : fabric_e (green)
    mTextureIndexforMaterial.push_back(51);
    mTextureIndexforMaterial.push_back(52);
    mTextureIndexforMaterial.push_back(53);
    mTextureIndexforMaterial.push_back(54);
    mTextureIndexforMaterial.push_back(AO);

    // 15 : fabric_d (blue)
    mTextureIndexforMaterial.push_back(49);
    mTextureIndexforMaterial.push_back(50);
    mTextureIndexforMaterial.push_back(53);
    mTextureIndexforMaterial.push_back(54);
    mTextureIndexforMaterial.push_back(AO);

    // 16 : fabric_a (red)
    mTextureIndexforMaterial.push_back(55);
    mTextureIndexforMaterial.push_back(56);
    mTextureIndexforMaterial.push_back(53);
    mTextureIndexforMaterial.push_back(54);
    mTextureIndexforMaterial.push_back(AO);

    // 17 : fabric_g (curtain_blue)
    mTextureIndexforMaterial.push_back(37);
    mTextureIndexforMaterial.push_back(38);
    mTextureIndexforMaterial.push_back(43);
    mTextureIndexforMaterial.push_back(44);
    mTextureIndexforMaterial.push_back(AO);

    // 18 : fabric_c (curtain_red)
    mTextureIndexforMaterial.push_back(41);
    mTextureIndexforMaterial.push_back(42);
    mTextureIndexforMaterial.push_back(43);
    mTextureIndexforMaterial.push_back(44);
    mTextureIndexforMaterial.push_back(AO);

    // 19 : fabric_f (curtain_green)
    mTextureIndexforMaterial.push_back(39);
    mTextureIndexforMaterial.push_back(40);
    mTextureIndexforMaterial.push_back(43);
    mTextureIndexforMaterial.push_back(44);
    mTextureIndexforMaterial.push_back(AO);

    // 20 : chain
    mTextureIndexforMaterial.push_back(12);
    mTextureIndexforMaterial.push_back(14);
    mTextureIndexforMaterial.push_back(13);
    mTextureIndexforMaterial.push_back(15);
    mTextureIndexforMaterial.push_back(AO);

    // 21 : vase_hanging
    mTextureIndexforMaterial.push_back(72);
    mTextureIndexforMaterial.push_back(73);
    mTextureIndexforMaterial.push_back(NoMetallic);
    mTextureIndexforMaterial.push_back(74);
    mTextureIndexforMaterial.push_back(AO);

    // 22 : vase
    mTextureIndexforMaterial.push_back(69);
    mTextureIndexforMaterial.push_back(70);
    mTextureIndexforMaterial.push_back(NoMetallic);
    mTextureIndexforMaterial.push_back(71);
    mTextureIndexforMaterial.push_back(AO);

    // 23 : Material__25 (lion)
    mTextureIndexforMaterial.push_back(16);
    mTextureIndexforMaterial.push_back(17);
    mTextureIndexforMaterial.push_back(NoMetallic);
    mTextureIndexforMaterial.push_back(18);
    mTextureIndexforMaterial.push_back(AO);

    // 24 : roof
    mTextureIndexforMaterial.push_back(63);
    mTextureIndexforMaterial.push_back(64);
    mTextureIndexforMaterial.push_back(NoMetallic);
    mTextureIndexforMaterial.push_back(65);
    mTextureIndexforMaterial.push_back(AO);

    // 25 : Material__47 - it seems missing
    mTextureIndexforMaterial.push_back(19);
    mTextureIndexforMaterial.push_back(20);
    mTextureIndexforMaterial.push_back(NoMetallic);
    mTextureIndexforMaterial.push_back(21);
    mTextureIndexforMaterial.push_back(AO);
}

DEFINE_APPLICATION_MAIN(MotionBlur)
