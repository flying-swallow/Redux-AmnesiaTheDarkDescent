/*
 * See LevelEditorCameraCapture.h for the design + threading contract.
 *
 * This TU deliberately includes NO rapidjson — it only builds the
 * MCP *image* content block, whose payload is base64 (a JSON-safe alphabet), so
 * it is assembled by plain string concatenation. That keeps hpl.h's X11 headers
 * (which #define Bool) away from rapidjson entirely.
 */

#include "hpl.h"
using namespace hpl;

#include "LevelEditorCameraCapture.h"

#include "../common/EditorBaseClasses.h"
#include "../common/EditorWorld.h"

#include "graphics/Graphics.h"
#include "graphics/PostEffect_ToneMap.h"
#include "graphics/RIRenderer.h"
#include "graphics/RIVK.h"
#include "scene/Camera.h"
#include "scene/Scene.h"
#include "scene/Viewport.h"
#include "scene/World.h"

#include <IL/il.h>
#include <vk_mem_alloc.h>

#include <cstring>
#include <vector>

//--------------------------------------------------------------------

// Default / clamp bounds for the requested image size (kept modest so a single
// readback buffer + PNG encode stays cheap and the base64 payload reasonable).
static constexpr int kCaptureDefaultW = 1024;
static constexpr int kCaptureDefaultH = 576;
static constexpr int kCaptureMaxDim   = 2048;
static constexpr int kCaptureMinDim   = 16;

// Frames to wait after recording the readback copy before mapping the buffer.
// The copy recorded in OnPostRender is submitted at the next frame boundary and
// its fence has certainly signalled once the command ring has cycled — a small
// margin over RI_NUMBER_FRAMES_FLIGHT is comfortably safe (and the latency is
// invisible: the HTTP worker is parked on a 15s future).
static constexpr uint32_t kReadbackWaitFrames = RI_NUMBER_FRAMES_FLIGHT + 2;

static int ClampInt(int alX, int alMin, int alMax)
{
	return alX < alMin ? alMin : (alX > alMax ? alMax : alX);
}

//--------------------------------------------------------------------

// Minimal RFC 4648 base64 (kept local so httplib.h stays out of this hpl TU).
static std::string Base64Encode(const unsigned char* apData, size_t alSize)
{
	static const char* kTable =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string out;
	out.reserve(((alSize + 2) / 3) * 4);
	size_t i = 0;
	for(; i + 3 <= alSize; i += 3)
	{
		uint32_t n = (apData[i] << 16) | (apData[i+1] << 8) | apData[i+2];
		out.push_back(kTable[(n >> 18) & 0x3F]);
		out.push_back(kTable[(n >> 12) & 0x3F]);
		out.push_back(kTable[(n >>  6) & 0x3F]);
		out.push_back(kTable[ n        & 0x3F]);
	}
	if(alSize - i == 1)
	{
		uint32_t n = apData[i] << 16;
		out.push_back(kTable[(n >> 18) & 0x3F]);
		out.push_back(kTable[(n >> 12) & 0x3F]);
		out.push_back('=');
		out.push_back('=');
	}
	else if(alSize - i == 2)
	{
		uint32_t n = (apData[i] << 16) | (apData[i+1] << 8);
		out.push_back(kTable[(n >> 18) & 0x3F]);
		out.push_back(kTable[(n >> 12) & 0x3F]);
		out.push_back(kTable[(n >>  6) & 0x3F]);
		out.push_back('=');
	}
	return out;
}

// Encode tightly-packed RGBA8 pixels (top-row-first) to an in-memory PNG.
static bool EncodePngRGBA(const unsigned char* apData, int alWidth, int alHeight, std::string& asOut)
{
	// DevIL is initialised by the resource bitmap loader at startup; guard a
	// redundant init in case capture runs before any texture load.
	static bool s_bIlInit = false;
	if(s_bIlInit == false) { ilInit(); s_bIlInit = true; }

	// Our readback rows are top-to-bottom (Vulkan framebuffer origin), but DevIL's
	// PNG writer emits its in-memory image bottom-up — i.e. it flips vertically on
	// save (IL_ORIGIN_SET / IL_IMAGE_ORIGIN only affect loading, verified). Hand
	// it bottom-up rows so the flip lands us back at a correctly-oriented PNG.
	const size_t rowBytes = (size_t)alWidth * 4;
	std::vector<unsigned char> flipped((size_t)alHeight * rowBytes);
	for(int y = 0; y < alHeight; ++y)
		memcpy(&flipped[(size_t)(alHeight - 1 - y) * rowBytes],
			   apData + (size_t)y * rowBytes, rowBytes);

	ILuint lImg = 0;
	ilGenImages(1, &lImg);
	ilBindImage(lImg);

	if(ilTexImage(alWidth, alHeight, 1, 4, IL_RGBA, IL_UNSIGNED_BYTE, flipped.data()) == 0)
	{
		ilDeleteImages(1, &lImg);
		return false;
	}

	// First call sizes the buffer, second fills it (DevIL in-memory save).
	ILuint lSize = ilSaveL(IL_PNG, NULL, 0);
	if(lSize == 0)
	{
		ilDeleteImages(1, &lImg);
		return false;
	}
	asOut.resize(lSize);
	ILuint lWritten = ilSaveL(IL_PNG, &asOut[0], lSize);
	ilDeleteImages(1, &lImg);
	if(lWritten == 0)
		return false;
	asOut.resize(lWritten);
	return true;
}

//--------------------------------------------------------------------

cLevelEditorCameraCapture::cLevelEditorCameraCapture(iEditorBase* apEditor)
{
	mpEditor              = apEditor;
	mpViewport            = NULL;
	mpCamera              = NULL;
	mpPostEffectComposite = NULL;
	mpPostEffectToneMap   = NULL;
	mlNextJobId           = 0;
	mpActiveJob           = NULL;

	cEngine*   pEngine = mpEditor->GetEngine();
	cScene*    pScene  = pEngine->GetScene();
	cGraphics* pGfx    = pEngine->GetGraphics();

	//////////////////////////////////////////
	// Virtual camera: Fly move mode + Euler rotation so an arbitrary look-at
	// pose (yaw/pitch) applies directly; no pitch clamp.
	mpCamera = pScene->CreateCamera(eCameraMoveMode_Fly);
	mpCamera->SetPitchLimits(0, 0);

	//////////////////////////////////////////
	// One persistent HEADLESS viewport — never in cScene's visible loop; Pump
	// Evaluates it directly. eRenderer_Main (the full lit hybrid renderer) so the
	// capture matches the editor's Main viewport; the world + per-job target are
	// set in Pump.
	mpViewport = pScene->CreateViewport(mpCamera, NULL);
	mpViewport->SetRenderer(pGfx->GetRenderer(eRenderer_Main));
	mpViewport->SetActive(false);
	mpViewport->SetVisible(false);

	//////////////////////////////////////////
	// Post chain: the hybrid renderer outputs linear HDR; the tonemap effect is
	// the mandatory exposure + ACES + sRGB display encode (same as the editor
	// pane's Main viewport). Without it the readback pixels are wrong.
	mpPostEffectComposite = pGfx->CreatePostEffectComposite();
	mpViewport->SetPostEffectComposite(mpPostEffectComposite);

	cPostEffectParams_ToneMap tonemapParams;
	tonemapParams.mfExposure   = 1.0f;
	tonemapParams.mfShadowLift = 1.0f;
	mpPostEffectToneMap = pGfx->CreatePostEffect(&tonemapParams);
	mpPostEffectComposite->AddPostEffect(mpPostEffectToneMap, 0);

	//////////////////////////////////////////
	// Record the readback copy from the viewport's delivery stage — the handler
	// runs inside Evaluate with the delivered target in a known layout.
	mPostDeliveryHandler = EventHandler<const WorldDrawCtx&>(
		[this](const WorldDrawCtx& ctx){ RecordReadbackCopy(ctx); });
	mPostDeliveryHandler.Connect(mpViewport->OnPostDelivery());
}

//--------------------------------------------------------------------

cLevelEditorCameraCapture::~cLevelEditorCameraCapture()
{
	// Drop any queued/in-flight jobs — free their GPU resources. Parked HTTP
	// promises live in the MCP server and are error-fulfilled by its Stop().
	for(cCaptureJob& job : mlstJobs)
		FreeJobResources(job);
	mlstJobs.clear();
	mlstCompleted.clear();

	cScene* pScene = mpEditor->GetEngine()->GetScene();
	cGraphics* pGfx = mpEditor->GetEngine()->GetGraphics();

	if(mpPostEffectToneMap)   pGfx->DestroyPostEffect(mpPostEffectToneMap);
	if(mpPostEffectComposite) pGfx->DestroyPostEffectComposite(mpPostEffectComposite);
	if(mpViewport)            pScene->DestroyViewport(mpViewport);
	if(mpCamera)              pScene->DestroyCamera(mpCamera);
}

//--------------------------------------------------------------------

int cLevelEditorCameraCapture::Enqueue(const cVector3f& avPos, const cVector3f& avTarget,
									   float afFovRadians, float afNearClip, float afFarClip,
									   int alWidth, int alHeight)
{
	if(mpViewport == NULL) return -1; // headless setup failed

	cCaptureJob job;
	job.mlId     = mlNextJobId++;
	job.mvPos    = avPos;
	job.mvTarget = avTarget;
	job.mfFov    = afFovRadians;
	job.mfNear   = afNearClip;
	job.mfFar    = afFarClip;
	job.mlWidth  = ClampInt(alWidth,  kCaptureMinDim, kCaptureMaxDim);
	job.mlHeight = ClampInt(alHeight, kCaptureMinDim, kCaptureMaxDim);
	job.mState   = eCaptureState_New;

	mlstJobs.push_back(std::move(job));
	return mlstJobs.back().mlId;
}

//--------------------------------------------------------------------

void cLevelEditorCameraCapture::Pump(float afFrameTime)
{
	if(mpViewport == NULL || mlstJobs.empty()) return;

	cGraphics* pGfx = Interface<cGraphics>::Get();
	const uint32_t lFrame = pGfx->frameIndex;

	//////////////////////////////////////////
	// 1) Complete any SUBMITTED jobs whose readback fence has signalled. These
	//    record no commands, so any number can finish in one frame.
	for(std::list<cCaptureJob>::iterator it = mlstJobs.begin(); it != mlstJobs.end(); )
	{
		if(it->mState == eCaptureState_Submitted &&
		   (lFrame - it->mlFrameStamp) >= kReadbackWaitFrames)
		{
			cMCPToolResult result = BuildResultFromReadback(*it);
			FreeJobResources(*it);
			mlstCompleted.push_back(std::make_pair(it->mlId, result));
			it = mlstJobs.erase(it);
		}
		else
			++it;
	}

	//////////////////////////////////////////
	// 2) Start ONE new job (at most one Evaluate per frame — the viewport's
	//    once-per-frame guard). Point at the CURRENT editor world so a map
	//    reload never leaves us on a stale world.
	cCaptureJob* pNew = NULL;
	for(cCaptureJob& job : mlstJobs)
		if(job.mState == eCaptureState_New) { pNew = &job; break; }
	if(pNew == NULL) return;

	iEditorWorld* pEdWorld = mpEditor->GetEditorWorld();
	cWorld* pWorld = pEdWorld ? pEdWorld->GetWorld() : NULL;
	if(pWorld == NULL) return; // no world yet; retry next frame

	//////////////////////////////////////////
	// Per-job GPU resources: an RGBA8_SRGB color target (sRGB attachment write =
	// free linear->display encode; TRANSFER_SRC backs the readback copy) + a
	// host-readable buffer the copy lands in.
	if(!CreateViewportColorTexture(&pGfx->device, (uint32_t)pNew->mlWidth, (uint32_t)pNew->mlHeight,
								   RI_FORMAT_RGBA8_SRGB,
								   RI_USAGE_COLOR_ATTACHMENT | RI_USAGE_SHADER_RESOURCE | RI_USAGE_TRANSFER_SRC,
								   &pNew->mTargetTexture, &pNew->mTargetView,
								   "MCPCameraCapture"))
	{
		// Give up on this job — report an error rather than retry forever.
		cMCPToolResult err;
		err.mbIsError = true;
		err.msContentJson = "[{\"type\":\"text\",\"text\":\"capture failed: could not allocate render target\"}]";
		mlstCompleted.push_back(std::make_pair(pNew->mlId, err));
		FreeJobResources(*pNew);
		for(std::list<cCaptureJob>::iterator it = mlstJobs.begin(); it != mlstJobs.end(); ++it)
			if(&(*it) == pNew) { mlstJobs.erase(it); break; }
		return;
	}

	RIBufferDesc bd = {};
	bd.size     = (uint64_t)pNew->mlWidth * (uint64_t)pNew->mlHeight * 4ull;
	bd.usage    = RI_BUFFER_USAGE_TRANSFER_DST;
	bd.location = RI_MEMORY_HOST_READBACK;
	pNew->mReadback = RIBuffer::create(&pGfx->device, bd);
	if(pNew->mReadback.isEmpty())
	{
		// Without a readback buffer the OnPostDelivery copy would target null —
		// fail the job cleanly instead of evaluating.
		cMCPToolResult err;
		err.mbIsError = true;
		err.msContentJson = "[{\"type\":\"text\",\"text\":\"capture failed: could not allocate readback buffer\"}]";
		mlstCompleted.push_back(std::make_pair(pNew->mlId, err));
		FreeJobResources(*pNew);
		for(std::list<cCaptureJob>::iterator it = mlstJobs.begin(); it != mlstJobs.end(); ++it)
			if(&(*it) == pNew) { mlstJobs.erase(it); break; }
		return;
	}

	//////////////////////////////////////////
	// Camera pose: eye at position, look at target (yaw/pitch from the two
	// points, matching the thumbnail builder's look-at math), aspect from the
	// requested image size.
	mpCamera->SetPosition(pNew->mvPos);
	cVector3f vAngles = cMath::GetAngleFromPoints3D(pNew->mvPos, pNew->mvTarget);
	mpCamera->SetYaw(vAngles.y);
	if(vAngles.x > kPif) vAngles.x = vAngles.x - k2Pif;
	mpCamera->SetPitch(vAngles.x);
	mpCamera->SetFOV(pNew->mfFov);
	mpCamera->SetAspect((float)pNew->mlWidth / (float)pNew->mlHeight);
	mpCamera->SetNearClipPlane(pNew->mfNear);
	mpCamera->SetFarClipPlane(pNew->mfFar);

	//////////////////////////////////////////
	// Point the viewport at the current world + this job's target, clear to the
	// editor's background colour, and Evaluate (lit world + post = tonemap).
	mpViewport->SetWorld(pWorld);
	if(pEdWorld) mpViewport->GetRenderSettings()->mClearColor = pEdWorld->GetBGDefaultColor();

	cViewport::TargetView target = {};
	target.width  = (uint32_t)pNew->mlWidth;
	target.height = (uint32_t)pNew->mlHeight;
	target.texture      = *pNew->mTargetTexture;
	target.view.vk.image = pNew->mTargetView->vk.image;
	target.format = RI_FORMAT_RGBA8_SRGB;
	mpViewport->SetTarget(target);

	pNew->mlFrameStamp = lFrame;
	pNew->mState       = eCaptureState_Submitted;
	mpActiveJob        = pNew;
	mpViewport->Evaluate(pGfx->GetActiveSet(), afFrameTime,
						 tSceneRenderFlag_World | tSceneRenderFlag_PostEffects);
	mpActiveJob = NULL;
}

//--------------------------------------------------------------------

void cLevelEditorCameraCapture::RecordReadbackCopy(const WorldDrawCtx& ctx)
{
	// Fired for EVERY delivery — act only when this Pump armed a job.
	if(mpActiveJob == NULL) return;
	cCaptureJob* pJob = mpActiveJob;

	RICmd* pCmd = ctx.cmd;

	// Delivery left the target SHADER_RESOURCE; move it to COPY_SRC and copy the
	// whole image into the host-readable buffer (tightly packed rows).
	RITextureBarrier toCopy(pJob->mTargetTexture.Get(),
		RI_RESOURCE_STATE_SHADER_RESOURCE, RI_RESOURCE_STATE_COPY_SRC,
		RI_STAGE_FRAGMENT, RI_STAGE_COPY);
	pCmd->vk_d3d12_textureBarrier(toCopy);

	VkBufferImageCopy region = {};
	region.bufferOffset      = 0;
	region.bufferRowLength   = 0; // tightly packed
	region.bufferImageHeight = 0;
	region.imageSubresource  = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
	region.imageOffset       = { 0, 0, 0 };
	region.imageExtent       = { (uint32_t)pJob->mlWidth, (uint32_t)pJob->mlHeight, 1 };
	vkCmdCopyImageToBuffer(pCmd->vk.cmd,
						   pJob->mTargetTexture->vk.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
						   pJob->mReadback.vk.buffer, 1, &region);
	// No restore barrier: the target is a per-job image freed once the copy's
	// fence signals; layout is irrelevant to its destruction.
}

//--------------------------------------------------------------------

cMCPToolResult cLevelEditorCameraCapture::BuildResultFromReadback(cCaptureJob& aJob)
{
	cMCPToolResult r;

	cGraphics* pGfx = Interface<cGraphics>::Get();

	if(aJob.mReadback.mappedAddress == NULL)
	{
		r.mbIsError = true;
		r.msContentJson = "[{\"type\":\"text\",\"text\":\"capture failed: readback buffer not mapped\"}]";
		return r;
	}

	// The copy's fence has signalled (waited kReadbackWaitFrames); invalidate in
	// case the readback landed in non-coherent HOST_CACHED memory, then read.
	vmaInvalidateAllocation(pGfx->device.vk.vmaAllocator, aJob.mReadback.vk.allocation, 0, VK_WHOLE_SIZE);

	const unsigned char* pPixels = (const unsigned char*)aJob.mReadback.mappedAddress;

	std::string sPng;
	if(EncodePngRGBA(pPixels, aJob.mlWidth, aJob.mlHeight, sPng) == false)
	{
		r.mbIsError = true;
		r.msContentJson = "[{\"type\":\"text\",\"text\":\"capture failed: PNG encode error\"}]";
		return r;
	}

	// base64 uses only [A-Za-z0-9+/=] — all JSON-safe, so the content block is
	// assembled without a JSON library.
	std::string sB64 = Base64Encode((const unsigned char*)sPng.data(), sPng.size());
	r.mbIsError = false;
	r.msContentJson = "[{\"type\":\"image\",\"data\":\"" + sB64 + "\",\"mimeType\":\"image/png\"}]";
	return r;
}

//--------------------------------------------------------------------

void cLevelEditorCameraCapture::FreeJobResources(cCaptureJob& aJob)
{
	// Texture handles go to the graphics deferral queue (freed once the pipeline
	// is done with them). The readback buffer's fence has signalled by the time
	// we free it, so an immediate dispose is safe.
	ReleaseViewportAttachmentTexture(&aJob.mTargetTexture, &aJob.mTargetView);
	if(aJob.mReadback.isEmpty() == false)
	{
		aJob.mReadback.dispose(&Interface<cGraphics>::Get()->device);
		aJob.mReadback = {};
	}
}

//--------------------------------------------------------------------

bool cLevelEditorCameraCapture::PopCompleted(int& alJobId, cMCPToolResult& aResult)
{
	if(mlstCompleted.empty()) return false;
	alJobId = mlstCompleted.front().first;
	aResult = mlstCompleted.front().second;
	mlstCompleted.pop_front();
	return true;
}
