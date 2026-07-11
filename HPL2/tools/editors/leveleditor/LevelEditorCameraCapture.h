/*
 * Off-screen "virtual camera" capture for the LevelEditor MCP server.
 *
 * Lets an external AI agent render the current map from an arbitrary camera
 * (eye position + look-at target + fov) and get back a PNG image, so it can
 * actually SEE the scene it is editing instead of only reading entity XML.
 *
 * Shape (mirrors cEditorThumbnailBuilder — async, GPU-resident):
 *   - The MCP tool handler only ENQUEUES a job (Enqueue) and returns a
 *     "deferred" result carrying the job id; the parked HTTP promise lives in
 *     the MCP server keyed by that id (see LevelEditorMCPServer::FulfillDeferred).
 *   - Pump() runs each frame from cLevelEditor::OnPostRender (AFTER cScene::Render,
 *     so the editor world's per-frame GPU data — TLAS / lights / decals — has
 *     already been published by PrepareFrame). It drives a small state machine:
 *       NEW       -> set the camera pose, point a persistent headless viewport
 *                    (eRenderer_Main + tonemap) at the editor world, allocate a
 *                    per-job RGBA8_SRGB target + a host-readback buffer, and
 *                    Evaluate once. The viewport's OnPostDelivery records a
 *                    barrier + vkCmdCopyImageToBuffer into the readback buffer.
 *       SUBMITTED -> after RI_NUMBER_FRAMES_FLIGHT frames the copy's fence has
 *                    signalled; map the buffer, encode a PNG, base64 it, build
 *                    the MCP image content block and push it to the completed
 *                    queue (drained by cLevelEditor and handed to the server).
 *   - At most ONE Evaluate per frame (the viewport's once-per-frame guard), so a
 *     NEW job starts only when no other started this frame; completions are
 *     unbounded (they record no commands).
 */

#ifndef LEVEL_EDITOR_CAMERA_CAPTURE_H
#define LEVEL_EDITOR_CAMERA_CAPTURE_H

#include "graphics/RITypes.h"
#include "math/MathTypes.h"
#include "system/Event.h"

#include "LevelEditorMCPServer.h" // cMCPToolResult

#include <list>
#include <string>
#include <utility>

using namespace hpl;

class iEditorBase;

namespace hpl {
	class cViewport;
	class cCamera;
	class cWorld;
	class cPostEffectComposite;
	class iPostEffect;
	struct WorldDrawCtx;
}

//--------------------------------------------------------------------

class cLevelEditorCameraCapture
{
public:
	cLevelEditorCameraCapture(iEditorBase* apEditor);
	~cLevelEditorCameraCapture();

	// Queue a capture from the given virtual camera. Returns a job id (>=0) to
	// pair the eventual result with the parked MCP promise, or -1 if capture is
	// unavailable (headless setup failed). Runs on the MAIN thread.
	int Enqueue(const cVector3f& avPos, const cVector3f& avTarget,
				float afFovRadians, float afNearClip, float afFarClip,
				int alWidth, int alHeight);

	// Per-frame pump, driven by cLevelEditor::OnPostRender (see class comment).
	void Pump(float afFrameTime);

	// Pop one finished capture (jobId + its MCP result). Returns false when the
	// completed queue is empty. cLevelEditor drains these into the MCP server.
	bool PopCompleted(int& alJobId, cMCPToolResult& aResult);

	bool IsAvailable() const { return mpViewport != NULL; }

private:
	enum eCaptureState { eCaptureState_New, eCaptureState_Submitted };

	struct cCaptureJob
	{
		int            mlId        = -1;
		cVector3f      mvPos       = 0;
		cVector3f      mvTarget    = 0;
		float          mfFov       = 0;
		float          mfNear      = 0;
		float          mfFar       = 0;
		int            mlWidth     = 0;
		int            mlHeight    = 0;
		eCaptureState  mState      = eCaptureState_New;
		uint32_t       mlFrameStamp = 0;

		// Per-job GPU resources: an RGBA8_SRGB color target the viewport delivers
		// into (TRANSFER_SRC backs the readback copy) + a host-readable buffer the
		// copy lands in.
		RISharedPointer<RITexture>     mTargetTexture;
		RISharedPointer<RITextureView> mTargetView;
		RIBuffer                       mReadback = {};
	};

	// OnPostDelivery handler body: barrier the delivered target to COPY_SRC and
	// record the vkCmdCopyImageToBuffer into the active job's readback buffer.
	void RecordReadbackCopy(const WorldDrawCtx& ctx);

	// SUBMITTED -> done: map the readback buffer, encode a PNG, base64 it and
	// build the MCP image content block. Frees the job's GPU resources.
	cMCPToolResult BuildResultFromReadback(cCaptureJob& aJob);

	// Free a job's per-job GPU resources (texture deferred to the freelist,
	// readback buffer disposed — safe once its fence has signalled).
	void FreeJobResources(cCaptureJob& aJob);

	/////////////////////////////////////
	// Data
	iEditorBase* mpEditor;

	cViewport*             mpViewport;
	cCamera*               mpCamera;
	cPostEffectComposite*  mpPostEffectComposite;
	iPostEffect*           mpPostEffectToneMap;

	std::list<cCaptureJob> mlstJobs;
	std::list<std::pair<int,cMCPToolResult>> mlstCompleted;

	int mlNextJobId;

	// The job Evaluated this frame — RecordReadbackCopy copies into its readback
	// buffer. Armed by Pump around Evaluate (cleared by the handler).
	cCaptureJob* mpActiveJob;

	EventHandler<const WorldDrawCtx&> mPostDeliveryHandler;
};

//--------------------------------------------------------------------

#endif // LEVEL_EDITOR_CAMERA_CAPTURE_H
