/*
 * Copyright © 2009-2020 Frictional Games
 *
 * This file is part of Amnesia: The Dark Descent.
 *
 * Amnesia: The Dark Descent is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.

 * Amnesia: The Dark Descent is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Amnesia: The Dark Descent.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "EditorThumbnailBuilder.h"

#include "EditorBaseClasses.h"

#include "graphics/Texture.h"
#include "graphics/Image.h"
#include "graphics/RIBootstrap.h"
#include "graphics/RIRenderer.h"
#include "scene/Viewport.h"

#if (DEVICE_IMPL_VULKAN)
#include <vk_mem_alloc.h>
#endif

//-------------------------------------------------------------------

static constexpr uint32_t kThumbnailSize = 128;

//-------------------------------------------------------------------

// Creates one cache texture a finished thumbnail is copied into: a plain
// sampled RGBA8_SRGB image (TRANSFER_DST for the copy; the GUI samples it
// like any other Image — the editor-pane pattern). The cTexture deleter
// defers the GPU frees onto the frame freelist, so dropping cache entries
// mid-flight is safe.
static bool CreateThumbnailCacheTexture(std::shared_ptr<cTexture> &outTexture)
{
	std::shared_ptr<cTexture> texture(new cTexture{},
										cTexture::cTexture_Delete);

	RITextureDesc cacheDesc = {};
	cacheDesc.type = RI_TEXTURE_2D;
	cacheDesc.format = RI_FORMAT_RGBA8_SRGB;
	cacheDesc.width = kThumbnailSize;
	cacheDesc.height = kThumbnailSize;
	cacheDesc.depth = 1;
	cacheDesc.mipNum = 1;
	cacheDesc.layerNum = 1;
	cacheDesc.usage = RI_USAGE_SHADER_RESOURCE | RI_USAGE_TRANSFER_DST;
	texture->handle = RITexture::create(&RI.device, cacheDesc);
	if(texture->handle.isEmpty(&RI.device))
	{
		Error("ThumbnailBuilder: failed to create cache image\n");
		return false;
	}

	RITextureViewDesc cacheView = {};
	cacheView.viewType = RI_VIEWTYPE_SHADER_RESOURCE_2D;
	cacheView.format = RI_FORMAT_RGBA8_SRGB;
	cacheView.mipNum = 1;
	cacheView.layerNum = 1;
	texture->view = RITextureView::create(&RI.device, &texture->handle, cacheView);

	texture->binding =
		RIDescriptor::sampledImage(&RI.device, &texture->view, hash_random());

	texture->width = (uint16_t)kThumbnailSize;
	texture->height = (uint16_t)kThumbnailSize;
	texture->depth = 1;
	texture->mipNum = 1;
	texture->format = RI_FORMAT_RGBA8_SRGB;

	outTexture = texture;
	return true;
}

//-------------------------------------------------------------------

/////////////////////////////////////////////////////////////////////
// CONSTRUCTORS
/////////////////////////////////////////////////////////////////////

//-------------------------------------------------------------------

cEditorThumbnailBuilder::cEditorThumbnailBuilder(iEditorBase* apEditor)
{
	mpEditor = apEditor;
	mpViewport = NULL;
	mpWorld = NULL;
	mTargetTexture = RITexture{};
	mTargetView = RITextureView{};
	mTargetDescriptor = RIDescriptor{};
	mpActiveCopyDst = NULL;

	cEngine* pEngine = mpEditor->GetEngine();
	cScene* pScene = pEngine->GetScene();

	cCamera* pCamera = pScene->CreateCamera(eCameraMoveMode_Fly);
	pCamera->SetPitchLimits(0,0);

	//////////////////////////////////////////
	// 128x128 RGBA8_SRGB render target: the sRGB attachment write encodes
	// linear->display for free; TRANSFER_SRC backs the per-thumbnail cache
	// copy after delivery.
	if(!CreateViewportColorTexture(&RI.device, kThumbnailSize, kThumbnailSize,
								   RI_FORMAT_RGBA8_SRGB,
								   RI_USAGE_COLOR_ATTACHMENT |
								   RI_USAGE_SHADER_RESOURCE |
								   RI_USAGE_TRANSFER_SRC,
								   &mTargetTexture, &mTargetView, &mTargetDescriptor,
								   "EditorThumbnail"))
	{
		return; // builder stays a safe no-op
	}

	//////////////////////////////////////////
	// The builder's own world: job entities live here, fully isolated from
	// the editor temp world (nothing else can leak into a thumbnail render).
	mpWorld = pScene->CreateWorld("ThumbnailWorld");
	mpWorld->SetActive(false); // never logic-updated; entities are static props

	//////////////////////////////////////////
	// One persistent HEADLESS viewport — never visible to cScene's loop;
	// Pump Evaluates it directly. cRendererSimple: unlit textured forward,
	// no BLAS/surfel cost per render (and no use for the legacy key/fill/
	// back light rig the old GL path set up).
	mpViewport = pScene->CreateViewport(pCamera, mpWorld);
	mpViewport->SetRenderer(pEngine->GetGraphics()->GetRenderer(eRenderer_Simple));
	mpViewport->SetActive(false);
	mpViewport->SetVisible(false);
	mpViewport->GetRenderSettings()->mClearColor = cColor(0.2f,1);

	cViewport::TargetView target = {};
	target.width = kThumbnailSize;
	target.height = kThumbnailSize;
	target.texture = mTargetTexture;
	target.view    = mTargetView;
	target.format  = RI_FORMAT_RGBA8_SRGB;
	mpViewport->SetTarget(target);

	//////////////////////////////////////////
	// Record the cache copy from the viewport's delivery stage — the handler
	// runs inside Evaluate, with the target in a known layout.
	mPostDeliveryHandler = EventHandler<const WorldDrawCtx&>(
		[this](const WorldDrawCtx& ctx){ RecordCacheCopy(ctx); });
	mPostDeliveryHandler.Connect(mpViewport->OnPostDelivery());
}

//-------------------------------------------------------------------

cEditorThumbnailBuilder::~cEditorThumbnailBuilder()
{
	////////////////////////////////////////
	// Drop queued jobs — we own their entities (they live in our world).
	for(cThumbnailJob& job : mlstJobs)
	{
		if(mpWorld && job.mpEntity)
			mpWorld->DestroyMeshEntity(job.mpEntity);
	}
	mlstJobs.clear();

	////////////////////////////////////////
	// GPU handles go to the frame freelist — freed once the in-flight
	// pipeline is done with them (or in RIBootstrap::Dispose at shutdown).
	RIBootstrap::FrameContext* cntx = RI.GetActiveSet();
	ReleaseViewportColorTexture(cntx->freelist, &mTargetTexture, &mTargetView, &mTargetDescriptor);

	if(mpViewport)
		mpEditor->GetEngine()->GetScene()->DestroyViewport(mpViewport);
	if(mpWorld)
		mpEditor->GetEngine()->GetScene()->DestroyWorld(mpWorld);
}

//-------------------------------------------------------------------

/////////////////////////////////////////////////////////////////////
// PUBLIC METHODS
/////////////////////////////////////////////////////////////////////

//-------------------------------------------------------------------

void cEditorThumbnailBuilder::BuildThumbnailFromMeshEntity(cMeshEntity* apEntity, tThumbnailReadyCallback aOnReady)
{
	if(apEntity==NULL) return;

	// Target setup failed (no viewport), or the entity was created in the
	// wrong world (must be GetWorld() — the builder's own): safe no-op, but
	// we own the entity, so clean it up.
	if(mpWorld==NULL || apEntity->GetWorld()!=mpWorld || !aOnReady)
	{
		if(apEntity->GetWorld()) apEntity->GetWorld()->DestroyMeshEntity(apEntity);
		return;
	}

	cThumbnailJob job;
	job.mpEntity = apEntity;
	job.mOnReady = std::move(aOnReady);

	apEntity->SetActive(false);
	apEntity->SetVisible(false);

	mlstJobs.push_back(std::move(job));
}

//-------------------------------------------------------------------

void cEditorThumbnailBuilder::BuildThumbnailFromMesh(const tWString& asMeshFilename, tThumbnailReadyCallback aOnReady)
{
	if(mpWorld==NULL) return;

	tString sMeshFilename = cString::To8Char(asMeshFilename);
	tString sName = "ThumbnailObject_" + sMeshFilename;

	// Already queued for this mesh.
	if(mpWorld->GetDynamicMeshEntity(sName) != NULL)
		return;

	cMesh* pMesh = mpEditor->GetEngine()->GetResources()->GetMeshManager()->CreateMesh(sMeshFilename);
	if(pMesh==NULL)
		return;

	cMeshEntity* pEnt = mpWorld->CreateMeshEntity(sName, pMesh, false);
	pEnt->SetSourceFile(sMeshFilename);

	BuildThumbnailFromMeshEntity(pEnt, std::move(aOnReady));
}

//-------------------------------------------------------------------

void cEditorThumbnailBuilder::BuildThumbnailFromImage(const tWString& asImageFilename, const tWString& asDestName)
{
	/////////////////////////////////
	// Pure CPU path (legacy texture-browser flow, still disk-based): load,
	// point-sample down to 128x128 RGBA, save.
	cResources* pRes = mpEditor->GetEngine()->GetResources();

	cBitmap* pSrc = pRes->GetBitmapLoaderHandler()->LoadBitmap(asImageFilename, 0);
	if(pSrc==NULL)
		return;

	const ePixelFormat srcFormat = pSrc->GetPixelFormat();
	const bool bSupported =
		pSrc->IsCompressed()==false &&
		(srcFormat==ePixelFormat_RGB || srcFormat==ePixelFormat_RGBA ||
		 srcFormat==ePixelFormat_BGR || srcFormat==ePixelFormat_BGRA);

	// TODO: block-compressed sources (DDS) need a decompress pass.
	if(bSupported==false || pSrc->GetWidth()<=0 || pSrc->GetHeight()<=0)
	{
		hplDelete(pSrc);
		return;
	}

	const int lChannels = GetChannelsInPixelFormat(srcFormat);
	const bool bSwapRB = (srcFormat==ePixelFormat_BGR || srcFormat==ePixelFormat_BGRA);
	const unsigned char* pSrcData = pSrc->GetData(0,0)->mpData;
	const int lSrcW = pSrc->GetWidth();
	const int lSrcH = pSrc->GetHeight();

	cBitmap bmp;
	bmp.CreateData(cVector3l(kThumbnailSize,kThumbnailSize,1), ePixelFormat_RGBA, 0, 0);
	unsigned char* pDst = bmp.GetData(0,0)->mpData;

	for(int y=0; y<(int)kThumbnailSize; ++y)
	{
		const int lSrcY = (int)(((int64_t)y * lSrcH) / kThumbnailSize);
		for(int x=0; x<(int)kThumbnailSize; ++x)
		{
			const int lSrcX = (int)(((int64_t)x * lSrcW) / kThumbnailSize);
			const unsigned char* pPix = pSrcData + (lSrcY*lSrcW + lSrcX)*lChannels;
			unsigned char* pOut = pDst + (y*kThumbnailSize + x)*4;
			pOut[0] = bSwapRB ? pPix[2] : pPix[0];
			pOut[1] = pPix[1];
			pOut[2] = bSwapRB ? pPix[0] : pPix[2];
			pOut[3] = lChannels==4 ? pPix[3] : 255;
		}
	}

	tWString sDestinationFile = asDestName.empty()
		? GetThumbnailNameFromFileW(asImageFilename) : asDestName;
	pRes->GetBitmapLoaderHandler()->SaveBitmap(&bmp, sDestinationFile, 0);

	hplDelete(pSrc);
}

//-------------------------------------------------------------------

/////////////////////////////////////////////////////////////////////
// JOB PUMP (driven by iEditorBase::OnDraw, see header)
/////////////////////////////////////////////////////////////////////

//-------------------------------------------------------------------

void cEditorThumbnailBuilder::Pump(float afFrameTime)
{
	if(mpViewport==NULL || mlstJobs.empty()) return;

	cThumbnailJob job = mlstJobs.front();
	if(job.mpEntity==NULL)
	{
		mlstJobs.pop_front();
		return;
	}

	////////////////////////////////////////
	// The job's cache texture — the OnPostDelivery handler copies the
	// delivered target into it on the main command buffer.
	std::shared_ptr<cTexture> pCacheTexture;
	if(CreateThumbnailCacheTexture(pCacheTexture)==false)
		return; // retry next frame

	mlstJobs.pop_front();

	////////////////////////////////////////
	// Place + evaluate headless: the entity is visible for exactly this draw.
	cMeshEntity* pEntity = job.mpEntity;
	pEntity->SetPosition(0);
	pEntity->SetActive(true);
	pEntity->SetVisible(true);
	pEntity->SetRenderFlagBit(eRenderableFlag_VisibleInNonReflection, true);
	pEntity->SetRenderFlagBit(eRenderableFlag_VisibleInReflection, true);

	FocusCameraOnEntity(pEntity);

	mpActiveCopyDst = pCacheTexture.get();
	mpViewport->Evaluate(RI.GetActiveSet(), afFrameTime, tSceneRenderFlag_World);

	pEntity->SetVisible(false);
	pEntity->SetActive(false);

	////////////////////////////////////////
	// Delivery skipped (handler never fired — shouldn't happen with a valid
	// world/camera): put the job back and retry next frame.
	if(mpActiveCopyDst != NULL)
	{
		mpActiveCopyDst = NULL;
		mlstJobs.push_front(job);
		return;
	}

	////////////////////////////////////////
	// Hand the finished Image to the requester — OWNERSHIP TRANSFERS (the
	// builder retains nothing; the caller releases the resource by dropping
	// the shared_ptr). The copy is ordered before any GUI sampling by its
	// barriers, so the image is usable immediately.
	Image::SingleImage singleImage = {};
	singleImage.image = pCacheTexture;
	std::shared_ptr<Image> pImage = std::make_shared<Image>(std::move(singleImage));

	mpWorld->DestroyMeshEntity(pEntity);

	job.mOnReady(std::move(pImage));
}

//-------------------------------------------------------------------

void cEditorThumbnailBuilder::RecordCacheCopy(const WorldDrawCtx& ctx)
{
	// Fired by the viewport's OnPostDelivery for EVERY delivery — only act
	// when this Pump placed a job.
	if(mpActiveCopyDst == NULL) return;
	cTexture* pDst = mpActiveCopyDst;
	mpActiveCopyDst = NULL;

	RICmd* pCmd = ctx.cmd;

	////////////////////////////////////////
	// The delivery left the target SHADER_RESOURCE; copy it into the cache
	// texture and transition both for fragment sampling. The barrier back on
	// the target also keeps the chain the next delivery's UNDEFINED discard
	// hangs off intact.
	const RITextureBarrier pre[2] = {
		RITextureBarrier(&mTargetTexture,
			RI_RESOURCE_STATE_SHADER_RESOURCE, RI_RESOURCE_STATE_COPY_SRC,
			RI_STAGE_FRAGMENT, RI_STAGE_COPY),
		RITextureBarrier(&pDst->handle,
			RI_RESOURCE_STATE_UNDEFINED, RI_RESOURCE_STATE_COPY_DST,
			RI_STAGE_NONE, RI_STAGE_COPY),
	};
	pCmd->vk_d3d12_textureBarriers<2>(2, pre);

	RIImageCopyDesc region = {};
	region.width  = kThumbnailSize;
	region.height = kThumbnailSize;
	region.depth  = 1;
	pCmd->copyImage(&RI.renderer, &mTargetTexture, &pDst->handle, region);

	const RITextureBarrier post[2] = {
		RITextureBarrier(&mTargetTexture,
			RI_RESOURCE_STATE_COPY_SRC, RI_RESOURCE_STATE_SHADER_RESOURCE,
			RI_STAGE_COPY, RI_STAGE_FRAGMENT),
		RITextureBarrier(&pDst->handle,
			RI_RESOURCE_STATE_COPY_DST, RI_RESOURCE_STATE_SHADER_RESOURCE,
			RI_STAGE_COPY, RI_STAGE_FRAGMENT),
	};
	pCmd->vk_d3d12_textureBarriers<2>(2, post);
}

//-------------------------------------------------------------------

void cEditorThumbnailBuilder::PreBuild()
{
	////////////////////////////////////////
	// Cancellation point: iEditorObjectIndex::Refresh calls this FIRST, then
	// deletes/recreates index entries — whose pointers queued callbacks
	// capture. Flush the queue (callbacks never fire) before that churn;
	// Refresh re-enqueues whatever is still missing right after.
	for(cThumbnailJob& job : mlstJobs)
	{
		if(mpWorld && job.mpEntity)
			mpWorld->DestroyMeshEntity(job.mpEntity);
	}
	mlstJobs.clear();
}

//-------------------------------------------------------------------

void cEditorThumbnailBuilder::PostBuild()
{
	// Legacy hook (framebuffer unbind) — nothing to do.
}

//-------------------------------------------------------------------

void cEditorThumbnailBuilder::CleanUp()
{
	// Legacy hook — job entities are destroyed when their job completes.
}

//-------------------------------------------------------------------

tString cEditorThumbnailBuilder::GetThumbnailNameFromFile(const tWString& asFile)
{
	return cString::To8Char(GetThumbnailNameFromFileW(asFile));
}

tWString cEditorThumbnailBuilder::GetThumbnailNameFromFileW(const tWString& asFile)
{
	tWString sNormalizedFileString;
	mSha << asFile >> sNormalizedFileString << SHA1::reset;

	return mpEditor->GetThumbnailDir() + sNormalizedFileString + _W("_tmb.jpg");
}

//-------------------------------------------------------------------

/////////////////////////////////////////////////////////////////////
// PROTECTED METHODS
/////////////////////////////////////////////////////////////////////

//-------------------------------------------------------------------

void cEditorThumbnailBuilder::FocusCameraOnEntity(cMeshEntity *apEntity)
{
	cCamera *pCam = mpViewport->GetCamera();
	cMesh *pMesh = apEntity->GetMesh();

	cBoundingVolume meshBV;
	if(pMesh->GetSkeleton())
	{
		for(int i=0; i<pMesh->GetSubMeshNum(); ++i)
		{
			cSubMesh *pSubMesh = pMesh->GetSubMesh(i);
			cVertexBuffer *pVtxBuffer = pSubMesh->GetVertexBuffer();

			meshBV.AddArrayPoints(pVtxBuffer->GetFloatArray(eVertexBufferElement_Position), pVtxBuffer->GetVertexNum());
		}
		meshBV.CreateFromPoints(4);
		meshBV.SetTransform(apEntity->GetWorldMatrix());
	}
	else
	{
		meshBV = *apEntity->GetBoundingVolume();
	}


	/////////////////////////
	// Calculate direction most tris face
	cVector3f vVecSum =0;
	float fVecCount=0;
	for(int i=0; i<pMesh->GetSubMeshNum(); ++i)
	{
		cSubMesh *pSubMesh = pMesh->GetSubMesh(i);
		cSubMeshEntity *pSubEnt = apEntity->GetSubMeshEntity(i);

		cMatrixf mtxTransform = pSubEnt->GetWorldMatrix();
		cVertexBuffer *pVtxBuffer = pSubMesh->GetVertexBuffer();

		VtxBufferAddNormals(mtxTransform, pVtxBuffer,vVecSum,fVecCount);
	}

	vVecSum.Normalize();

	/////////////////////////
	// Calculate position
	cVector3f vDir=1;
	if(vVecSum.x >= 0)	vDir.x = 1;
	else				vDir.x = -1;
	if(vVecSum.y >= 0)	vDir.y = 1;
	else				vDir.y = -1;
	if(vVecSum.z >= 0)	vDir.z = 1;
	else				vDir.z = -1;
	vDir.Normalize();

	float fDist = meshBV.GetSize().x;
	if(fDist < meshBV.GetSize().y) fDist = meshBV.GetSize().y;
	if(fDist < meshBV.GetSize().z) fDist = meshBV.GetSize().z;

	//Making the 2.0 larger might reduce some of the perspective distortion if needed
	fDist = sqrt(fDist*fDist)*2.0f;

    cVector3f vCamPos = meshBV.GetWorldCenter() + vDir * fDist;// + cVector3f(0,fYDir*fDist, 0);
	pCam->SetPosition(vCamPos);

	/////////////////////////
	// Calculate Direction
	cVector3f vAngels = cMath::GetAngleFromPoints3D(vCamPos, meshBV.GetWorldCenter());
	pCam->SetYaw(vAngels.y);
	if(vAngels.x > kPif) vAngels.x = vAngels.x - k2Pif;
	pCam->SetPitch(vAngels.x);


	/////////////////////////
	// Calculate FOV

	//Corners of the bounding box
	cVector3f vMin = meshBV.GetMin();
	cVector3f vMax = meshBV.GetMax();
	cVector3f vCorners[8] =
	{
			cVector3f(vMax.x,vMax.y,vMax.z),
			cVector3f(vMax.x,vMax.y,vMin.z),
			cVector3f(vMax.x,vMin.y,vMax.z),
			cVector3f(vMax.x,vMin.y,vMin.z),

			cVector3f(vMin.x,vMax.y,vMax.z),
			cVector3f(vMin.x,vMax.y,vMin.z),
			cVector3f(vMin.x,vMin.y,vMax.z),
			cVector3f(vMin.x,vMin.y,vMin.z),
	};

	// For each corner, check angle between it and the direction to the center
	// Use largest angle x 2 as FOV
	float fLargestAngle =0;
	cVector3f vDirToCenter = cMath::Vector3Normalize(meshBV.GetWorldCenter() - vCamPos);
	for(int i=0;i<8; ++i)
	{
		cVector3f vToCorner =  cMath::Vector3Normalize(vCorners[i] - vCamPos);

		float fAngle = cMath::Vector3Angle(vToCorner, vDirToCenter);
		if(fAngle > fLargestAngle) fLargestAngle = fAngle;
	}

	pCam->SetFOV(fLargestAngle*2);
	pCam->SetAspect(1); //<- this makes it looks stretch but since u render to a square it should be like this for you!
}

//-------------------------------------------------------------------

void cEditorThumbnailBuilder::VtxBufferAddNormals(const cMatrixf a_mtxTransform, cVertexBuffer *apVtxBuffer, cVector3f &avVecSum, float& afCount)
{
	float *pPosVec = apVtxBuffer->GetFloatArray(eVertexBufferElement_Position);
	unsigned int *pIdxVec = apVtxBuffer->GetIndices();
	int lIndexNum = apVtxBuffer->GetIndexNum();

	/////////////////////////////
	// Iterate triangles
	for(int tri=0; tri<lIndexNum; tri+=3)
	{
		///////////////////////
		//Get positions
		cVector3f vPos[3];
		cVector3f vCenterPos=0;
		for(int i=0; i<3; ++i)
		{
			unsigned int lPosIdx = pIdxVec[tri+i];
			vPos[i].x = pPosVec[lPosIdx*4 + 0];
			vPos[i].y = pPosVec[lPosIdx*4 + 1];
			vPos[i].z = pPosVec[lPosIdx*4 + 2];

			vPos[i] = cMath::MatrixMul(a_mtxTransform,vPos[i]);
			vCenterPos += vPos[i];
		}
		vCenterPos = vCenterPos / 3.0f;

		///////////////////////
		//Calculate normal
		cVector3f vNormal = cMath::Vector3Cross(vPos[2] - vPos[0], vPos[1] - vPos[0]);
		vNormal.Normalize();

		//Use the area of poly to increase normal mul a bit.
		float fTriSide[3] = { cMath::Vector3Dist(vPos[0],vPos[1]),cMath::Vector3Dist(vPos[1],vPos[2]), cMath::Vector3Dist(vPos[2],vPos[0]) };
		float fS = (fTriSide[0]+fTriSide[1]+fTriSide[2]) / 2.0f;
		float fArea = sqrt(fS * (fS - fTriSide[0]) * (fS - fTriSide[1]) * (fS - fTriSide[2]));

		vNormal = vNormal * fArea;

		//Make sure that 2 times normals need to point down in order to view it from below.
		if(vNormal.y < 0) vNormal = vNormal * 0.45f;

		//In case both sides have equal amount of polygons
		if(vNormal.x < 0) vNormal = vNormal * 0.85f;
		else if(vNormal.z < 0) vNormal = vNormal * 0.8f;

		avVecSum += vNormal;
		afCount+=1;
	}
}

//-------------------------------------------------------------------
