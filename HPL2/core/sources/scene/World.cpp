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

#include "scene/World.h"

#include <tinyxml2.h>

#include "system/Script.h"
#include "system/String.h"
#include "system/LowLevelSystem.h"

#include "math/Math.h"
#include "math/MathTypes.h"

#include "engine/Engine.h"

#include "graphics/Image.h"
#include "graphics/Mesh.h"
#include "graphics/SubMesh.h"
#include "graphics/Graphics.h"
#include "graphics/Renderer.h"
#include "graphics/LowLevelGraphics.h"
#include "graphics/MeshCreator.h"
#include "graphics/VertexBuffer.h"
#include "graphics/Material.h"
#include "graphics/HybridRenderer.h"
#include "graphics/GlobalManagedSets.h"
#include "graphics/RIBootstrap.h"
#include "graphics/RIResourceUploader.h"


#include "resources/ParticleManager.h"
#include "resources/ScriptManager.h"
#include "resources/MaterialManager.h"
#include "resources/TextureManager.h"
#include "resources/EntFileManager.h"
#include "resources/FileSearcher.h"
#include "resources/Resources.h"
#include "resources/SoundEntityManager.h"
#include "resources/LowLevelResources.h"
#include "resources/XmlHelper.h"

#include "scene/Scene.h"
#include "scene/Node3D.h"
#include "scene/LightPoint.h"
#include "scene/LightSpot.h"
#include "scene/LightBox.h"
#include "scene/LightArea.h"
#include "scene/MeshEntity.h"
#include "scene/Decal.h"
#include "scene/SoundEntity.h"
#include "scene/ParticleEmitter.h"
#include "scene/ParticleSystem.h"
#include "scene/BillBoard.h"
#include "scene/Beam.h"
#include "scene/GuiSetEntity.h"
#include "scene/RopeEntity.h"
#include "scene/FogArea.h"
#include "scene/RenderableContainer_List.h"
#include "scene/RenderableContainer_BoxTree.h"
#include "scene/RenderableContainer_DynBoxTree.h"
#include "scene/DummyRenderable.h"

#include "system/System.h"
#include "system/Platform.h"

#include "sound/SoundEntityData.h"
#include "sound/Sound.h"
#include "sound/SoundHandler.h"

#include "physics/Physics.h"
#include "physics/PhysicsWorld.h"
#include "physics/PhysicsBody.h"
#include "physics/PhysicsJoint.h"

#include "ai/AI.h"
#include "ai/AINodeContainer.h"
#include "ai/AINodeGenerator.h"
#include "ai/AStar.h"

#include "haptic/Haptic.h"
#include "haptic/LowLevelHaptic.h"

#include <algorithm>
#include <cstdint>
#include <utility>

namespace hpl {

	namespace {

		//-----------------------------------------------------------------------
		// Morton (Z-order) helpers for decal locality. Spread the low 10 bits of
		// each axis so a 30-bit interleaved code keeps spatially-near points near
		// in linear order. See cWorld::Compile for why decals are sorted this way.
		//-----------------------------------------------------------------------

		inline uint32_t Part1By2(uint32_t v)
		{
			v &= 0x000003ffu;                       // keep 10 bits
			v = (v ^ (v << 16)) & 0xff0000ffu;
			v = (v ^ (v <<  8)) & 0x0300f00fu;
			v = (v ^ (v <<  4)) & 0x030c30c3u;
			v = (v ^ (v <<  2)) & 0x09249249u;
			return v;
		}

		inline uint32_t MortonCode3D(uint32_t x, uint32_t y, uint32_t z)
		{
			return (Part1By2(z) << 2) | (Part1By2(y) << 1) | Part1By2(x);
		}

	} // namespace

	//////////////////////////////////////////////////////////////////////////
	// CONSTRUCTORS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	cWorld::cWorld(tString asName,cGraphics *apGraphics,cResources *apResources,cSound* apSound,
						cPhysics *apPhysics, cScene *apScene,cSystem *apSystem, cAI *apAI,
						cHaptic *apHaptic)
	{
		mpGraphics = apGraphics;
		mpResources = apResources;
		mpSound = apSound;
		mpPhysics = apPhysics;
		mpScene = apScene;
		mpSystem =apSystem;
		mpAI = apAI;
		mpHaptic = apHaptic;

		mpRootNode = hplNew( cNode3D, () );

		msName=asName;

		mbActive = true;

		mAmbientColor=cColor(0,0);

		mbIsSoundEmitter = false;

		mlSoundCreationIDCount =0;

		//TODO: Have the container type as param and create.
		mpRenderableContainer[eWorldContainerType_Static] = hplNew( cRenderableContainer_BoxTree, () );
		mpRenderableContainer[eWorldContainerType_Dynamic] = hplNew( cRenderableContainer_DynBoxTree, () );

		mpPhysicsWorld = NULL;
		mbAutoDeletePhysicsWorld = false;

		//////////////////////////////
		//Sky box
		mpSkyBoxVtxBuffer = mpGraphics->GetMeshCreator()->CreateSkyBoxVertexBuffer(1);
		mbAutoDestroySkybox = false;
		mbSkyBoxActive = false;
		mSkyBoxColor = cColor(1,1);

		//////////////////////////////
		//Fog
		mbFogActive = false;
		mfFogStart =0;
		mfFogEnd = 10;
		mfFogFalloffExp = 1;
		mFogColor = cColor(1,1);
		mbFogCulling = true;


		msFilePath = _W("");
	}

	//-----------------------------------------------------------------------

	cWorld::~cWorld()
	{
		// Release the baked decal GPU buffers + their persistent texture pins.
		// Independent of the decal objects (unpin keys off the stored slots), so
		// it is safe before DestroyAllEntities tears the decals down.
		DisposeDecalBuffers();
		DisposeFogBuffers();
		DisposeLightBuffers();

		if(mpSkyBoxVtxBuffer) hplDelete(mpSkyBoxVtxBuffer);
		if(mpSkyBoxImage && mbAutoDestroySkybox)
		{
			mpResources->GetTextureManager()->Destroy(mpSkyBoxImage);
		}

		DestroyAllEntities(0);
		
		for(int i=0; i<2; ++i)
		{
			if(mpRenderableContainer[i]) hplDelete(mpRenderableContainer[i]);
		}

		hplDelete(mpRootNode);
	}

	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// PUBLIC METHODS
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	void cWorld::DestroyAllEntities(tWorldDestroyAllFlag aFlags)
	{
		if( (aFlags & eWorldDestroyAllFlag_SkipStaticEntities)==0)
		{
			STLDeleteAll(mlstStaticMeshEntities);

			// Decals are static. Each cDecal owns its material via a
			// SharedResourceHandle, released automatically when the decal is deleted.
			STLDeleteAll(mvDecals);
		}

		STLDeleteAll(mlstDynamicMeshEntities);
		STLDeleteAll(mlstLights);
		STLDeleteAll(mlstBillboards);
		STLDeleteAll(mlstBeams);
		STLDeleteAll(mlstParticleSystems);
		STLDeleteAll(mlstGuiSetEntities);
		STLDeleteAll(mlstRopeEntities);
		STLDeleteAll(mlstFogAreas);
		STLDeleteAll(mlstStartPosEntities);
		STLMapDeleteAll(m_mapAreaEntities);


		STLDeleteAll(mlstAINodeContainers);
		STLDeleteAll(mlstAStarHandlers);
		STLMapDeleteAll(m_mapTempNodes);

		if( (aFlags & eWorldDestroyAllFlag_SkipPhysics)==0)
		{
			if(mpPhysicsWorld && mbAutoDeletePhysicsWorld)
				mpPhysics->DestroyWorld(mpPhysicsWorld);
		}

		//So that bodies can stop sound entities on destruction.
		STLDeleteAll(mlstSoundEntities);

	}

	//-----------------------------------------------------------------------

	void cWorld::Update(float afTimeStep)
	{
		START_TIMING(Physics);
		if(mpPhysicsWorld) mpPhysicsWorld->Update(afTimeStep);
		STOP_TIMING(Physics);


		START_TIMING(Entities);
		UpdateEntities(afTimeStep);
		STOP_TIMING(Entities);
		
		START_TIMING(Particles);
		UpdateParticles(afTimeStep);
		STOP_TIMING(Particles);

		START_TIMING(Lights);
		UpdateLights(afTimeStep);
		STOP_TIMING(Lights);

		START_TIMING(SoundEntities);
		UpdateSoundEntities(afTimeStep);
		STOP_TIMING(SoundEntities);
	}

	//-----------------------------------------------------------------------

	void cWorld::PreUpdate(float afTotalTime, float afTimeStep)
	{
		mpSound->GetSoundHandler()->SetSilent(true);

		while(afTotalTime>0)
		{
			if(mpPhysicsWorld) mpPhysicsWorld->Update(afTimeStep);
			UpdateParticles(afTimeStep);

			afTotalTime -= afTimeStep;
		}

		mpSound->GetSoundHandler()->SetSilent(false);
	}

	//-----------------------------------------------------------------------

	iRenderableContainer* cWorld::GetRenderableContainer(eWorldContainerType aType)
	{
		return mpRenderableContainer[aType];
	}

	//-----------------------------------------------------------------------

	void cWorld::SetPhysicsWorld(iPhysicsWorld *apWorld, bool abAutoDelete)
	{
		mpPhysicsWorld = apWorld;
		mbAutoDeletePhysicsWorld = abAutoDelete;
		if(mpPhysicsWorld)
			mpPhysicsWorld->SetWorld(this);
	}

	iPhysicsWorld* cWorld::GetPhysicsWorld()
	{
		return mpPhysicsWorld;
	}

	//-----------------------------------------------------------------------

	static void CheckMinMaxUpdate(cVector3f &avMin,cVector3f &avMax,
							const cVector3f &avLocalMin,const cVector3f &avLocalMax)
	{
		if(avMin.x > avLocalMin.x) avMin.x = avLocalMin.x;
		if(avMax.x < avLocalMax.x) avMax.x = avLocalMax.x;

		if(avMin.y > avLocalMin.y) avMin.y = avLocalMin.y;
		if(avMax.y < avLocalMax.y) avMax.y = avLocalMax.y;

		if(avMin.z > avLocalMin.z) avMin.z = avLocalMin.z;
		if(avMax.z < avLocalMax.z) avMax.z = avLocalMax.z;
	}

	//-----------------------------------------------------------------------

	void cWorld::Compile(bool abCalcPhysicsWorldSize)
	{
		for(int i=0; i<2; ++i)
			if(mpRenderableContainer[i]) mpRenderableContainer[i]->Compile();

		if(mvDecals.size() > 1)
		{
			cVector3f vBoundsMin(0.0f), vBoundsMax(0.0f);
			bool bFirst = true;
			for(cDecal* pDecal : mvDecals)
			{
				cBoundingVolume* pBV = pDecal ? pDecal->GetBoundingVolume() : NULL;
				if(pBV == NULL) continue;
				const cVector3f vC = pBV->GetWorldCenter();
				if(bFirst) { vBoundsMin = vBoundsMax = vC; bFirst = false; continue; }
				vBoundsMin.x = std::min(vBoundsMin.x, vC.x);
				vBoundsMin.y = std::min(vBoundsMin.y, vC.y);
				vBoundsMin.z = std::min(vBoundsMin.z, vC.z);
				vBoundsMax.x = std::max(vBoundsMax.x, vC.x);
				vBoundsMax.y = std::max(vBoundsMax.y, vC.y);
				vBoundsMax.z = std::max(vBoundsMax.z, vC.z);
			}

			const cVector3f vExtent = vBoundsMax - vBoundsMin;
			auto quant = [](float v, float lo, float ext) -> uint32_t {
				if(ext <= 0.0f) return 0u;          // degenerate axis -> single bucket
				float t = (v - lo) / ext;            // -> [0,1]
				t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
				return (uint32_t)(t * 1023.0f + 0.5f);
			};

			// Precompute (code, decal) once so the sort doesn't re-fetch the BV.
			std::vector<std::pair<uint32_t, cDecal*>> lvOrder;
			lvOrder.reserve(mvDecals.size());
			for(cDecal* pDecal : mvDecals)
			{
				cBoundingVolume* pBV = pDecal ? pDecal->GetBoundingVolume() : NULL;
				const cVector3f vC = pBV ? pBV->GetWorldCenter() : cVector3f(0.0f);
				const uint32_t lCode = MortonCode3D(quant(vC.x, vBoundsMin.x, vExtent.x),
													quant(vC.y, vBoundsMin.y, vExtent.y),
													quant(vC.z, vBoundsMin.z, vExtent.z));
				lvOrder.emplace_back(lCode, pDecal);
			}
			std::stable_sort(lvOrder.begin(), lvOrder.end(),
				[](const std::pair<uint32_t, cDecal*>& a, const std::pair<uint32_t, cDecal*>& b)
				{ return a.first < b.first; });
			for(size_t i=0; i<lvOrder.size(); ++i)
				mvDecals[i] = lvOrder[i].second;
		}

		// NOTE: the OOB decal box is world-fixed at this position, so a decal on an
		// object that later MOVES won't follow it (the old baked mesh did) — fine
		// for static-placed set-dressing.
		mvDecalObjectIndices.clear();
		if(mvDecals.empty() == false)
		{
			auto associateContainer = [&](iRenderableContainer* apContainer, int alCategoryBits)
			{
				if(apContainer == NULL) return;
				std::vector<iRenderableContainerNode*> lstStack;
				lstStack.push_back(apContainer->GetRoot());
				while(lstStack.empty() == false)
				{
					iRenderableContainerNode* pNode = lstStack.back();
					lstStack.pop_back();
					if(pNode == NULL) continue;

					for(iRenderableContainerNode* pChild : pNode->GetChildNodes())
						lstStack.push_back(pChild);

					for(iRenderable* pObj : pNode->GetObjects())
					{
						cBoundingVolume* pObjBV = pObj->GetBoundingVolume();
						if(pObjBV == NULL) continue;

						const int lOffset = (int)mvDecalObjectIndices.size();
						int lCount = 0;
						for(size_t i=0; i<mvDecals.size(); ++i)
						{
							if((mvDecals[i]->GetReceiverMask() & alCategoryBits) == 0) continue;
							cBoundingVolume* pDecalBV = mvDecals[i]->GetBoundingVolume();
							if(pDecalBV == NULL) continue;
							if(cMath::CheckBVIntersection(*pObjBV, *pDecalBV) == false) continue;
							if(lCount >= 255)   // 8-bit count in UniformObject.decalList
							{
								Warning("cWorld::Compile: object exceeded 255 overlapping decals; clipping\n");
								break;
							}
							mvDecalObjectIndices.push_back((uint32_t)i);
							++lCount;
						}
						pObj->SetDecalList(lOffset, lCount);
					}
				}
			};

			associateContainer(mpRenderableContainer[eWorldContainerType_Static],
							   eDecalReceiver_Static);
			associateContainer(mpRenderableContainer[eWorldContainerType_Dynamic],
							   eDecalReceiver_Entity | eDecalReceiver_Primitive);
		}

		// Decal set is now final and stable — bake it into the per-world GPU
		// buffers (gDecals[] + gObjectDecalIndices[]) once. Static for the
		// world's lifetime; the renderer just binds them.
		BakeDecalBuffers();

		// Fog areas → a persistent per-world buffer (bound on the per-world set
		// kWorldSet by each consuming pass); refreshed per-frame on change.
		BakeFogBuffer();

		// Lights → persistent per-world per-type buffers holding ALL world lights
		// at stable slots (counts become world totals); refreshed per-frame.
		BakeLightBuffers();

		if(mpPhysicsWorld && abCalcPhysicsWorldSize)
		{
			iRenderableContainerNode *pStaticRoot = mpRenderableContainer[eWorldContainerType_Static]->GetRoot();

			//Create a 10 m border around the world too
			cVector3f vMin = pStaticRoot->GetMin() - cVector3f(10,10,10);
			cVector3f vMax = pStaticRoot->GetMax() + cVector3f(10,10,10);

			mpPhysicsWorld->SetWorldSize(vMin, vMax);
		}
	}

	//-----------------------------------------------------------------------

	void cWorld::BakeDecalBuffers()
	{
		mbDecalBuffersDirty = false; // consumed (cleared even if the bake early-outs)
		// Re-Compile drops any previous bake first.
		DisposeDecalBuffers();

		// Decal projection only runs on the hybrid (main) renderer. Tools using
		// the wireframe/simple renderers don't composite decals, so skip the GPU
		// bake there (and avoid touching an uninitialised managed set).
		cHybridRenderer* pHybrid =
			static_cast<cHybridRenderer*>(mpGraphics->GetRenderer(eRenderer_Main));
		if(pHybrid == nullptr)
			return;

		// Build the static GpuDecal[] geometry in stable (Morton-sorted) order.
		// diffuseTexIndex is left invalid here and filled by RefreshDecalTextures,
		// which reads each diffuse Image's lifetime-stable bindless slot
		// (cTextureManager-assigned).
		std::vector<GpuDecal> decals(mvDecals.size());
		mvDecalTexSlots.assign(mvDecals.size(), kInvalidTextureIndex);
		for(size_t i=0; i<mvDecals.size(); ++i)
		{
			cDecal* pDecal = mvDecals[i];
			cMaterial* pMat = pDecal ? pDecal->GetMaterial() : nullptr;

			GpuDecal d{};
			cMatrixf* pMtx = pDecal ? pDecal->GetModelMatrix(nullptr) : nullptr; // frustum-independent
			const cMatrixf wm = pMtx ? *pMtx : cMatrixf::Identity;
			ml::float4x4 invF4 = cMath::ToFloatTranspose4x4(wm);
			invF4.Invert();
			std::memcpy(d.invModelMat, invF4.a, sizeof(d.invModelMat));

			// World-space projection axis = the decal box's local +Y in world
			// space; normalize out the box scale.
			cVector3f up = wm.GetUp();
			const float upLen = up.Length();
			up = upLen > 1e-6f ? up / upLen : cVector3f(0,1,0);
			d.projAxisWS = float3{up.x, up.y, up.z};
			const cColor c = pDecal ? pDecal->GetDecalColor() : cColor(1,1);
			d.color = float4{c.r, c.g, c.b, c.a};

			// Diffuse texture slot is resolved per frame (RefreshDecalTextures);
			// leave it invalid here — the first refresh fills it before composite.
			d.diffuseTexIndex = kInvalidTextureIndex;
			d.receiverMask = (uint32_t)(pDecal ? pDecal->GetReceiverMask() : 0);
			d.blendMode = pMat ? (uint32_t)pMat->GetBlendMode() : 0u;
			const cVector2l sd = pDecal ? pDecal->GetSubDiv() : cVector2l(1,1);
			d.subDivX = (uint32_t)sd.x;
			d.subDivY = (uint32_t)sd.y;
			d.subDivIndex = (uint32_t)(pDecal ? pDecal->GetCurrentSubDiv() : 0);
			decals[i] = d;
		}

		const uint32_t kSsboUsage =
			RI_BUFFER_USAGE_SHADER_RESOURCE_STORAGE | RI_BUFFER_USAGE_TRANSFER_DST;

		auto uploadStatic = [&](RIBuffer* buf, const void* src, size_t bytes) {
			RIResourceBufferTransaction trans = {};
			trans.target = *buf;
			trans.size = bytes;
			trans.offset = 0;
			// Read-only on set 2 (StructuredBuffer), sampled by the composite
			// compute pass; FRAGMENT included for safety against future readers.
			trans.currentState = RI_RESOURCE_STATE_SHADER_RESOURCE;
			trans.currentStages = RI_STAGE_COMPUTE | RI_STAGE_FRAGMENT;
			trans.postState = RI_RESOURCE_STATE_SHADER_RESOURCE;
			trans.postStages = RI_STAGE_COMPUTE | RI_STAGE_FRAGMENT;
			RI_ResourceBeginCopyBuffer(&RI.device, &RI.uploader, &trans);
			std::memcpy(trans.mapped.data, src, bytes);
			RI_ResourceEndCopyBuffer(&RI.device, &RI.uploader, &trans);
		};

		// gDecals[] — one GpuDecal per world decal. Allocate at least one element
		// so the set-2 binding stays valid even for a decal-less world (the shader
		// never reads it then — every object's decalList count is 0).
		{
			const size_t count = decals.empty() ? (size_t)1 : decals.size();
			const size_t bytes = count * sizeof(GpuDecal);
			mpDecalBuffer = RISharedPointer<RIBuffer>(&RI.device, RIBuffer::create(
				&RI.device, {(uint64_t)bytes, kSsboUsage, RI_MEMORY_DEVICE, 0}));
			if(decals.empty() == false)
				uploadStatic(mpDecalBuffer.Get(), decals.data(),
							 decals.size() * sizeof(GpuDecal));
		}

		// gObjectDecalIndices[] — flat per-object pool. Allocate at least one
		// element so the set-2 binding stays valid even with no receivers.
		{
			const size_t count = mvDecalObjectIndices.empty()
				? (size_t)1 : mvDecalObjectIndices.size();
			const size_t bytes = count * sizeof(uint32_t);
			mpDecalObjectIndexBuffer = RISharedPointer<RIBuffer>(&RI.device, RIBuffer::create(
				&RI.device, {(uint64_t)bytes, kSsboUsage, RI_MEMORY_DEVICE, 0}));
			if(mvDecalObjectIndices.empty() == false)
				uploadStatic(mpDecalObjectIndexBuffer.Get(), mvDecalObjectIndices.data(),
							 mvDecalObjectIndices.size() * sizeof(uint32_t));
		}
	}

	//-----------------------------------------------------------------------

	void cWorld::RefreshDecalTextures()
	{
		// Decals added (or init-dirty for an uncompiled world) → (re)bake. Ensures
		// the decal buffer is always allocated (≥1-element placeholder) so the
		// composite's set-2 gDecals/gObjectDecalIndices binding is never left
		// unbound — e.g. the level editor, which never Compiles.
		if(mbDecalBuffersDirty)
			BakeDecalBuffers();

		if(mvDecals.empty() || mpDecalBuffer.isEmpty())
			return;
		// Each decal's diffuse texture now carries a lifetime-stable bindless slot
		// assigned by cTextureManager, so just read it. The GPU diffuseTexIndex is
		// patched only when the slot actually changes (≈ first sight only); after
		// that this loop is a cheap no-op every call.
		for(size_t i=0; i<mvDecals.size(); ++i)
		{
			cDecal* pDecal = mvDecals[i];
			cMaterial* pMat = pDecal ? pDecal->GetMaterial() : nullptr;
			Image* img = pMat ? pMat->GetImage(eMaterialTexture_Diffuse) : nullptr;
			uint32_t slot = img ? img->GetBindlessSlot() : kInvalidTextureIndex;
			if(mvDecalTexSlots[i] == slot)
				continue;
			mvDecalTexSlots[i] = slot;

			// Patch just this decal's diffuseTexIndex field in the device buffer.
			RIResourceBufferTransaction trans = {};
			trans.target = *mpDecalBuffer;
			trans.size = sizeof(uint32_t);
			trans.offset = i * sizeof(GpuDecal) + offsetof(GpuDecal, diffuseTexIndex);
			trans.currentState = RI_RESOURCE_STATE_SHADER_RESOURCE;
			trans.currentStages = RI_STAGE_COMPUTE | RI_STAGE_FRAGMENT;
			trans.postState = RI_RESOURCE_STATE_SHADER_RESOURCE;
			trans.postStages = RI_STAGE_COMPUTE | RI_STAGE_FRAGMENT;
			RI_ResourceBeginCopyBuffer(&RI.device, &RI.uploader, &trans);
			std::memcpy(trans.mapped.data, &slot, sizeof(uint32_t));
			RI_ResourceEndCopyBuffer(&RI.device, &RI.uploader, &trans);
		}
	}

	//-----------------------------------------------------------------------

	void cWorld::DisposeDecalBuffers()
	{
		mvDecalTexSlots.clear();
		// Defer GPU disposal to the graphics freelist: an in-flight frame may still
		// be reading the old buffer (notably on a runtime re-bake). Pushing the
		// shared owner hands a counted reference to the defer queue, which disposes
		// it once the GPU passes this frame's timeline value; resetting the member
		// drops our reference now.
		if(!mpDecalBuffer.isEmpty())
			RI.graphicsDefer.push(mpDecalBuffer);
		mpDecalBuffer = {};
		if(!mpDecalObjectIndexBuffer.isEmpty())
			RI.graphicsDefer.push(mpDecalObjectIndexBuffer);
		mpDecalObjectIndexBuffer = {};
	}

	//-----------------------------------------------------------------------

	// Build the GPU struct for one fog area (mirrors the former per-frame fill in
	// HybridRenderer). invModelMat maps world → the fog box's unit cube; colour is
	// sRGB→linear like the box-light path; flags pack the backside-visibility bits.
	static FogAreaParams BuildFogParams(cFogArea* apFogArea)
	{
		FogAreaParams fa{};
		// Use the computed model matrix (world × size-scale), not GetModelMatrixPtr()
		// — the cached raw pointer is null at Compile/bake time (it's only wired up
		// during the per-frame render walk). Mirrors the decal bake.
		cMatrixf* pMtx = apFogArea->GetModelMatrix(nullptr);
		const cMatrixf inv = cMath::MatrixInverse(pMtx ? *pMtx : cMatrixf::Identity);
		const ml::float4x4 invF4 = cMath::ToFloatTranspose4x4(inv);
		std::memcpy(fa.invModelMat, invF4.a, sizeof(fa.invModelMat));
		const cColor c = apFogArea->GetColor();
		fa.color = float3{sRGBToLinear(c.r), sRGBToLinear(c.g), sRGBToLinear(c.b)};
		fa.colorA = c.a;
		fa.start = apFogArea->GetStart();
		fa.end = apFogArea->GetEnd();
		fa.falloffExp = apFogArea->GetFalloffExp();
		fa.flags = (apFogArea->GetShowBacksideWhenInside() ? 1u : 0u) |
				   (apFogArea->GetShowBacksideWhenOutside() ? 2u : 0u);
		return fa;
	}

	// Build all fog params and upload the whole buffer in one transaction.
	static void UploadFogBuffer(const std::vector<cFogArea*>& areas, RIBuffer* buf)
	{
		if(areas.empty()) return; // buffer is the 1-element placeholder
		std::vector<FogAreaParams> params(areas.size());
		for(size_t i=0; i<areas.size(); ++i) params[i] = BuildFogParams(areas[i]);
		RIResourceBufferTransaction trans = {};
		trans.target = *buf;
		trans.size = areas.size() * sizeof(FogAreaParams);
		trans.offset = 0;
		trans.currentState = RI_RESOURCE_STATE_UNORDERED_ACCESS;
		trans.currentStages = RI_STAGE_FRAGMENT | RI_STAGE_COMPUTE;
		trans.postState = RI_RESOURCE_STATE_UNORDERED_ACCESS;
		trans.postStages = RI_STAGE_FRAGMENT | RI_STAGE_COMPUTE;
		RI_ResourceBeginCopyBuffer(&RI.device, &RI.uploader, &trans);
		std::memcpy(trans.mapped.data, params.data(), trans.size);
		RI_ResourceEndCopyBuffer(&RI.device, &RI.uploader, &trans);
	}

	void cWorld::BakeFogBuffer()
	{
		mbFogBufferDirty = false; // consumed (cleared even if the bake early-outs)
		DisposeFogBuffers();

		// Fog is only composited by the hybrid (main) renderer; tools on the
		// wireframe/simple renderers don't read gFogAreas, so skip the GPU bake.
		cHybridRenderer* pHybrid =
			static_cast<cHybridRenderer*>(mpGraphics->GetRenderer(eRenderer_Main));
		if(pHybrid == nullptr)
			return;

		// Stable bake-order snapshot of ALL fog areas (slot = index).
		mvFogAreas.assign(mlstFogAreas.begin(), mlstFogAreas.end());

		const uint32_t kSsboUsage =
			RI_BUFFER_USAGE_SHADER_RESOURCE_STORAGE | RI_BUFFER_USAGE_TRANSFER_DST;
		// At least one element so the binding stays valid for a fog-less world
		// (GetFogAreaCount()==0 → the shader never reads it).
		const size_t count = mvFogAreas.empty() ? (size_t)1 : mvFogAreas.size();
		mpFogAreaBuffer = RISharedPointer<RIBuffer>(&RI.device, RIBuffer::create(
			&RI.device, {(uint64_t)(count * sizeof(FogAreaParams)), kSsboUsage,
						 RI_MEMORY_DEVICE, 0}));
		UploadFogBuffer(mvFogAreas, mpFogAreaBuffer.Get());
	}

	void cWorld::RefreshFogAreas()
	{
		// Membership changed (fog area added/removed; init-dirty for an uncompiled
		// world) → re-bake. Set by Create/DestroyFogArea, so editor edits work too.
		if(mbFogBufferDirty)
			BakeFogBuffer();

		if(mpFogAreaBuffer.isEmpty())
			return; // bake skipped (no hybrid renderer)

		// Per-frame content (move / colour): re-upload the whole buffer (small:
		// ≤32 × 100 B) in one transaction.
		UploadFogBuffer(mvFogAreas, mpFogAreaBuffer.Get());
	}

	void cWorld::DisposeFogBuffers()
	{
		mvFogAreas.clear();
		if(!mpFogAreaBuffer.isEmpty())
			RI.graphicsDefer.push(mpFogAreaBuffer);
		mpFogAreaBuffer = {};
	}

	//-----------------------------------------------------------------------
	// Persistent per-world light buffers (point/spot/area). All world lights of a
	// type live at stable slots; counts = world totals (the GPU light grid +
	// Scene.slang decode use them directly). Invisible lights get radius 0 so
	// LightGridBuildPass skips them (no shader change). The per-type GPU struct
	// build mirrors the former per-frame fill in HybridRenderer.
	//-----------------------------------------------------------------------

	static PointLight BuildPointLight(iLight* pLight)
	{
		PointLight pl{};
		const cVector3f pos = pLight->GetWorldPosition();
		pl.position[0] = pos.x; pl.position[1] = pos.y; pl.position[2] = pos.z;
		const cColor c = pLight->GetDiffuseColor();
		pl.color[0] = sRGBToLinear(c.r); pl.color[1] = sRGBToLinear(c.g); pl.color[2] = sRGBToLinear(c.b);
		pl.intensity = pLight->GetIntensity();
		pl.radius = pLight->GetRadius();
		pl.sourceRadius = pLight->GetSourceRadius();
		Image* gobo = pLight->GetGoboImage();
		pl.goboTextureIndex = gobo ? gobo->GetBindlessSlot() : kInvalidTextureIndex;
		const cMatrixf& world = pLight->GetWorldMatrix();
		pl.worldToLightX[0] = world.m[0][0]; pl.worldToLightX[1] = world.m[0][1]; pl.worldToLightX[2] = world.m[0][2];
		pl.worldToLightY[0] = world.m[1][0]; pl.worldToLightY[1] = world.m[1][1]; pl.worldToLightY[2] = world.m[1][2];
		pl.worldToLightZ[0] = world.m[2][0]; pl.worldToLightZ[1] = world.m[2][1]; pl.worldToLightZ[2] = world.m[2][2];
		if(!pLight->IsVisible()) pl.radius = 0.0f; // grid skips radius <= 0
		return pl;
	}

	static SpotLight BuildSpotLight(iLight* pLight)
	{
		cLightSpot* pSpot = static_cast<cLightSpot*>(pLight);
		SpotLight sl{};
		const cVector3f pos = pLight->GetWorldPosition();
		sl.position[0] = pos.x; sl.position[1] = pos.y; sl.position[2] = pos.z;
		const cMatrixf& world = pLight->GetWorldMatrix();
		sl.direction[0] = -world.m[0][2]; sl.direction[1] = -world.m[1][2]; sl.direction[2] = -world.m[2][2];
		{
			float len = std::sqrt(sl.direction[0]*sl.direction[0] + sl.direction[1]*sl.direction[1] + sl.direction[2]*sl.direction[2]);
			if(len > 1e-6f) { sl.direction[0] /= len; sl.direction[1] /= len; sl.direction[2] /= len; }
		}
		sl.cosOuterAngle = std::cos(pSpot->GetFOV() * 0.5f);
		const cColor c = pLight->GetDiffuseColor();
		sl.color[0] = sRGBToLinear(c.r); sl.color[1] = sRGBToLinear(c.g); sl.color[2] = sRGBToLinear(c.b);
		sl.intensity = pLight->GetIntensity();
		sl.radius = pLight->GetRadius();
		sl.sourceRadius = pLight->GetSourceRadius();
		Image* gobo = pLight->GetGoboImage();
		sl.goboTextureIndex = gobo ? gobo->GetBindlessSlot() : kInvalidTextureIndex;
		sl.shadowEnabled = pLight->GetCastShadows() ? 1u : 0u;
		const ml::float4x4 vpF4 = cMath::ToFloatTranspose4x4(pSpot->GetViewProjMatrix());
		std::memcpy(sl.viewProjection, vpF4.a, sizeof(sl.viewProjection));
		if(!pLight->IsVisible()) sl.radius = 0.0f;
		return sl;
	}

	static RectLight BuildRectLight(iLight* pLight)
	{
		cLightArea* pArea = static_cast<cLightArea*>(pLight);
		RectLight al{};
		const cVector3f pos = pLight->GetWorldPosition();
		al.position[0] = pos.x; al.position[1] = pos.y; al.position[2] = pos.z;
		const cColor c = pLight->GetDiffuseColor();
		al.color[0] = sRGBToLinear(c.r); al.color[1] = sRGBToLinear(c.g); al.color[2] = sRGBToLinear(c.b);
		al.intensity = pLight->GetIntensity();
		al.radius = pLight->GetRadius();
		al.width = pArea->GetWidth();
		al.height = pArea->GetHeight();
		al.barnDoorAngle = pArea->GetBarnDoorAngle();
		al.barnDoorLength = pArea->GetBarnDoorLength();
		Image* gobo = pLight->GetGoboImage();
		al.sourceTextureIndex = gobo ? gobo->GetBindlessSlot() : kInvalidTextureIndex;
		// UE Rect Light basis: width = local +Y (world col 1), height = local +Z
		// (col 2), emission normal = local +X (col 0).
		const cMatrixf& world = pLight->GetWorldMatrix();
		al.right[0] = world.m[0][1]; al.right[1] = world.m[1][1]; al.right[2] = world.m[2][1];
		al.up[0] = world.m[0][2]; al.up[1] = world.m[1][2]; al.up[2] = world.m[2][2];
		al.normal[0] = world.m[0][0]; al.normal[1] = world.m[1][0]; al.normal[2] = world.m[2][0];
		if(!pLight->IsVisible()) al.radius = 0.0f;
		return al;
	}

	// Create + upload a full per-type light buffer (≥1 element so the binding stays
	// valid for an empty type).
	// Build all structs for a type and upload the whole buffer in one transaction.
	// Used by both the bake (after allocating the buffer) and the per-frame refresh
	// (into the existing buffer). The data is small (≤ tens of KB per type), so a
	// bulk copy is simpler and cheaper than per-entry compare-and-patch.
	template<typename T, typename BuildFn>
	static void UploadLightType(const std::vector<iLight*>& slots, RIBuffer* buf, BuildFn build)
	{
		if(slots.empty()) return; // buffer is the 1-element placeholder
		std::vector<T> params(slots.size());
		for(size_t i=0; i<slots.size(); ++i) params[i] = build(slots[i]);
		RIResourceBufferTransaction trans = {};
		trans.target = *buf; trans.size = slots.size() * sizeof(T); trans.offset = 0;
		trans.currentState = RI_RESOURCE_STATE_UNORDERED_ACCESS;
		trans.currentStages = RI_STAGE_FRAGMENT | RI_STAGE_COMPUTE;
		trans.postState = RI_RESOURCE_STATE_UNORDERED_ACCESS;
		trans.postStages = RI_STAGE_FRAGMENT | RI_STAGE_COMPUTE;
		RI_ResourceBeginCopyBuffer(&RI.device, &RI.uploader, &trans);
		std::memcpy(trans.mapped.data, params.data(), trans.size);
		RI_ResourceEndCopyBuffer(&RI.device, &RI.uploader, &trans);
	}

	// Allocate a per-type light buffer (≥1 element so the binding stays valid for
	// an empty type) and upload all its lights.
	template<typename T, typename BuildFn>
	static void BakeLightType(const std::vector<iLight*>& slots,
							  RISharedPointer<RIBuffer>& buf, BuildFn build)
	{
		const uint32_t kSsboUsage = RI_BUFFER_USAGE_SHADER_RESOURCE_STORAGE | RI_BUFFER_USAGE_TRANSFER_DST;
		const size_t count = slots.empty() ? (size_t)1 : slots.size();
		buf = RISharedPointer<RIBuffer>(&RI.device, RIBuffer::create(
			&RI.device, {(uint64_t)(count * sizeof(T)), kSsboUsage, RI_MEMORY_DEVICE, 0}));
		UploadLightType<T>(slots, buf.Get(), build);
	}

	// Partition mlstLights by type into the three stable-order slot vectors.
	static void PartitionLightsByType(const tLightList& lights,
									  std::vector<iLight*>& pts, std::vector<iLight*>& sps, std::vector<iLight*>& ars)
	{
		pts.clear(); sps.clear(); ars.clear();
		for(iLight* l : lights)
		{
			switch(l->GetLightType())
			{
				case eLightType_Point: pts.push_back(l); break;
				case eLightType_Spot:  sps.push_back(l); break;
				case eLightType_Area:  ars.push_back(l); break;
				default: break; // cLightBox is not GPU-lit
			}
		}
	}

	void cWorld::BakeLightBuffers()
	{
		mbLightBuffersDirty = false; // consumed (cleared even if the bake early-outs)
		DisposeLightBuffers();

		// Lights are only consumed by the hybrid (main) renderer's GPU lighting.
		cHybridRenderer* pHybrid =
			static_cast<cHybridRenderer*>(mpGraphics->GetRenderer(eRenderer_Main));
		if(pHybrid == nullptr)
			return;

		PartitionLightsByType(mlstLights, mvPointLights, mvSpotLights, mvAreaLights);
		BakeLightType<PointLight>(mvPointLights, mpPointLightBuffer, BuildPointLight);
		BakeLightType<SpotLight>(mvSpotLights, mpSpotLightBuffer, BuildSpotLight);
		BakeLightType<RectLight>(mvAreaLights, mpAreaLightBuffer, BuildRectLight);
	}

	void cWorld::RefreshLights()
	{
		// Membership changed (light added/removed; init-dirty for an uncompiled
		// world) → re-bake. Set by Create*/DestroyLight, so editor edits work too.
		if(mbLightBuffersDirty)
			BakeLightBuffers();

		if(mpPointLightBuffer.isEmpty() && mpSpotLightBuffer.isEmpty() && mpAreaLightBuffer.isEmpty())
			return; // bake skipped (no hybrid renderer)

		// Per-frame content (move / colour / flicker): re-upload each whole per-type
		// buffer (small) in one transaction.
		UploadLightType<PointLight>(mvPointLights, mpPointLightBuffer.Get(), BuildPointLight);
		UploadLightType<SpotLight>(mvSpotLights, mpSpotLightBuffer.Get(), BuildSpotLight);
		UploadLightType<RectLight>(mvAreaLights, mpAreaLightBuffer.Get(), BuildRectLight);
	}

	void cWorld::SubmitToGpu(SceneConstants& perFrame)
	{
		// Single per-frame world→GPU submission, called once before any pass.
		// 1) Bake-if-dirty + upload the persistent light/fog/decal buffers.
		RefreshLights();
		RefreshFogAreas();
		RefreshDecalTextures();

		// 2) Publish world-total counts — the light grid build + Scene.slang
		//    unified-index decode read these directly.
		perFrame.pointLightCount = GetPointLightCount();
		perFrame.spotLightCount  = GetSpotLightCount();
		perFrame.areaLightCount  = GetAreaLightCount();
		perFrame.fogAreaCount    = GetFogAreaCount();
		perFrame.decalCount      = GetDecalCount();

		// No descriptor binding here. The light/fog buffers ride the dedicated
		// per-world set kWorldSet, bound by each consuming pass via
		// RIProgram::bindDescriptors (cached) — see appendWorldLightFog in
		// HybridRenderer.cpp. cWorld only owns + uploads its buffers; it never
		// touches a descriptor set. Invisible lights carry radius 0 so the grid
		// skips them; the set-2 decal buffers are bound at the composite pass.
	}

	void cWorld::DisposeLightBuffers()
	{
		mvPointLights.clear(); mvSpotLights.clear(); mvAreaLights.clear();
		if(!mpPointLightBuffer.isEmpty()) RI.graphicsDefer.push(mpPointLightBuffer);
		mpPointLightBuffer = {};
		if(!mpSpotLightBuffer.isEmpty()) RI.graphicsDefer.push(mpSpotLightBuffer);
		mpSpotLightBuffer = {};
		if(!mpAreaLightBuffer.isEmpty()) RI.graphicsDefer.push(mpAreaLightBuffer);
		mpAreaLightBuffer = {};
	}

	//-----------------------------------------------------------------------

	void cWorld::SetSkyBox(Image *apImage, bool abAutoDestroy)
	{
		if(mpSkyBoxImage && mbAutoDestroySkybox)
		{
			mpResources->GetTextureManager()->Destroy(mpSkyBoxImage);
		}
		mbAutoDestroySkybox = abAutoDestroy;
		mpSkyBoxImage = apImage;
	}

	void cWorld::SetSkyBoxActive(bool abX)
	{
		mbSkyBoxActive = abX;
	}

	void cWorld::SetSkyBoxColor(const cColor& aColor)
	{
		if(mSkyBoxColor == aColor) return;

		mSkyBoxColor = aColor;

		float *pColors = mpSkyBoxVtxBuffer->GetFloatArray(eVertexBufferElement_Color0);
		int lNum = mpSkyBoxVtxBuffer->GetVertexNum();
		for(int i=0; i<lNum;++i)
		{
			pColors[0] = mSkyBoxColor.r;
			pColors[1] = mSkyBoxColor.g;
			pColors[2] = mSkyBoxColor.b;
			pColors[3] = mSkyBoxColor.a;
			pColors+=4;
		}

		mpSkyBoxVtxBuffer->UpdateData(eVertexElementFlag_Color0,false);
	}
	//-----------------------------------------------------------------------

	cAreaEntity* cWorld::CreateAreaEntity(const tString &asName)
	{
		cAreaEntity *pArea = hplNew( cAreaEntity, () );
		pArea->msName = asName;
		m_mapAreaEntities.insert(tAreaEntityMap::value_type(asName, pArea));
		return pArea;
	}

	cAreaEntity* cWorld::GetAreaEntity(const tString &asName)
	{
		tAreaEntityMapIt it = m_mapAreaEntities.find(asName);
		if(it== m_mapAreaEntities.end()) return NULL;

		return it->second;
	}

	//-----------------------------------------------------------------------

	iEntity3D* cWorld::CreateEntity(const tString& asName, const cMatrixf &a_mtxTransform,const tString& asFile, int alID, bool abActive,
									const cVector3f &avScale,
									cResourceVarsObject *apInstanceVars, bool abSkipNonStaticEntity)
	{
		iEntity3D *pEntity = NULL;
		
		// World owns the cEntFile via mlstEntFileCache (raw list, Destroyed in ~cWorld);
		// take the reference out of the handle as a raw pointer.
		SharedResourceHandle<cEntFile> pEntFile = mpResources->GetEntFileManager()->CreateEntFile(asFile);
		if(!pEntFile.IsValid()) {
			return NULL;
		}
		entityCache.push_back(pEntFile);
		
		tString sEntityType = "";
		tinyxml2::XMLElement *pDoc = pEntFile->GetXmlDoc();

		// Get Root element
		tinyxml2::XMLElement *pVarRootElem = pDoc->FirstChildElement("UserDefinedVariables");
		if(pVarRootElem==NULL){
			Warning("Can not find a UserDefinedVariables element in '%s'. Using default entity type\n", asFile.c_str());
		}
		else
		{
			sEntityType = GetAttributeString(pVarRootElem, "EntityType");
		}

		
		//////////////////////////////////
		// Get Loader and load data
		iEntityLoader *pLoader = mpResources->GetEntityLoader(sEntityType);
		if(pLoader)
		{
			if(abSkipNonStaticEntity==false || pLoader->GetCreatesStaticEntity())
			{
				pEntity = pLoader->Load(asName,alID, abActive, pDoc,a_mtxTransform, avScale, this,pEntFile->GetName(),pEntFile->GetFullPath(), apInstanceVars);
				if(pEntity) pEntity->SetSourceFile(pEntFile->GetName());
			}
		}
		else
		{
			Error("Couldn't find loader for type '%s' in file '%s'\n",sEntityType.c_str(),pEntFile->GetName().c_str());
		}
		
		return pEntity;
	}

	//-----------------------------------------------------------------------

	cMeshEntity* cWorld::CreateMeshEntity(const tString &asName,cMesh *apMesh, bool abStatic)
	{
		cMeshEntity* pMeshEntity = hplNew( cMeshEntity, (asName,apMesh,mpResources->GetMaterialManager(),
														mpResources->GetMeshManager(), 
														mpResources->GetAnimationManager()) );

		//////////////////////////////
		//Put in entity list
		if(abStatic)
			mlstStaticMeshEntities.push_back(pMeshEntity);
		else
			mlstDynamicMeshEntities.push_back(pMeshEntity);


		//////////////////////////////
		// Add submeshes to renderable container
		for(int i=0; i<pMeshEntity->GetSubMeshEntityNum(); ++i) 
		{
			cSubMeshEntity *pSubEntity = pMeshEntity->GetSubMeshEntity(i);
			if(pSubEntity->GetSubMesh()->IsCollideShape()) continue; //Collide shapes are never rendered!
            
			pSubEntity->SetStatic(abStatic);
			AddRenderableToContainer(pSubEntity);
		}
		
		pMeshEntity->SetWorld(this);

		return pMeshEntity;
	}

	//-----------------------------------------------------------------------

	cDecal* cWorld::CreateDecal(const tString& asName, const tString& asMaterial,
								const cColor& aColor, const cVector2l& avSubDiv)
	{
		SharedResourceHandle<cMaterial> pMaterial = mpResources->GetMaterialManager()->CreateMaterial(asMaterial);
		if(!pMaterial)
			return NULL;

		cDecal* pDecal = hplNew( cDecal, (asName, mpGraphics, std::move(pMaterial), aColor, avSubDiv) );
		pDecal->SetStatic(true);

		mvDecals.push_back(pDecal);

		// Decals are static -> goes into the static renderable container, collected
		// into eRenderListType_Decal each frame by its decal material.
		AddRenderableToContainer(pDecal);

		MarkDecalBuffersDirty(); // membership changed → re-bake the decal buffers
		return pDecal;
	}

	//-----------------------------------------------------------------------

	void cWorld::DestroyMeshEntity(cMeshEntity* apMesh)
	{
		if(apMesh==NULL) return;

		for(int i=0; i<apMesh->GetSubMeshEntityNum(); ++i) 
		{
			RemoveRenderableFromContainer(apMesh->GetSubMeshEntity(i));
		}

		if(apMesh->IsStatic())
			STLFindAndDelete(mlstStaticMeshEntities,apMesh);
		else
			STLFindAndDelete(mlstDynamicMeshEntities,apMesh);
	}

	//-----------------------------------------------------------------------

	cMeshEntity* cWorld::GetDynamicMeshEntity(const tString& asName)
	{
		 return (cMeshEntity*)STLFindByName(mlstDynamicMeshEntities, asName);
	}

	//-----------------------------------------------------------------------

	cMeshEntityIterator cWorld::GetDynamicMeshEntityIterator()
	{
		return cMeshEntityIterator(&mlstDynamicMeshEntities);
	}

	cMeshEntityIterator cWorld::GetStaticMeshEntityIterator()
	{
		return cMeshEntityIterator(&mlstStaticMeshEntities);
	}

	//-----------------------------------------------------------------------


	cLightPoint* cWorld::CreateLightPoint(const tString &asName,const tString &asGobo,bool abStatic)
	{
		cLightPoint* pLight = hplNew( cLightPoint, (asName,mpResources) );
		mlstLights.push_back(pLight);

		if(asGobo != "")
		{
			Image *pImage = mpResources->GetTextureManager()->CreateCubeMapImage(asGobo,true).Release();
			if(pImage!=NULL)
				pLight->SetGoboTexture(pImage);
			else
				Warning("Couldn't load gobo texture '%s' for light '%s'",asGobo.c_str(), asName.c_str());
		}

		pLight->SetStatic(abStatic);
		AddRenderableToContainer(pLight);

		pLight->SetWorld(this);

		MarkLightBuffersDirty(); // membership changed → re-bake the light buffers
		return pLight;
	}

	cLightSpot* cWorld::CreateLightSpot(const tString &asName, const tString &asGobo,
										bool abStatic)
	{
		cLightSpot* pLight = hplNew( cLightSpot, (asName,mpResources) );
		mlstLights.push_back(pLight);

		if(asGobo != "")
		{
			Image *pImage = mpResources->GetTextureManager()->Create2DImage(asGobo,true).Release();
			if(pImage!=NULL)
				pLight->SetGoboTexture(pImage);
			else
				Warning("Couldn't load gobo texture '%s' for light '%s'",asGobo.c_str(), asName.c_str());
		}

		pLight->SetStatic(abStatic);
		AddRenderableToContainer(pLight);

		pLight->SetWorld(this);

		MarkLightBuffersDirty(); // membership changed → re-bake the light buffers
		return pLight;
	}

	cLightBox* cWorld::CreateLightBox(const tString &asName,bool abStatic)
	{
		cLightBox* pLight = hplNew( cLightBox, (asName,mpResources) );
		mlstLights.push_back(pLight);

		pLight->SetStatic(abStatic);
		AddRenderableToContainer(pLight);

		pLight->SetWorld(this);

		MarkLightBuffersDirty(); // membership changed → re-bake the light buffers
		return pLight;
	}

	cLightArea* cWorld::CreateLightArea(const tString &asName,bool abStatic)
	{
		cLightArea* pLight = hplNew( cLightArea, (asName,mpResources) );
		mlstLights.push_back(pLight);

		pLight->SetStatic(abStatic);
		AddRenderableToContainer(pLight);

		pLight->SetWorld(this);

		MarkLightBuffersDirty(); // membership changed → re-bake the light buffers
		return pLight;
	}

	//-----------------------------------------------------------------------

	void cWorld::DestroyLight(iLight* apLight)
	{
		RemoveRenderableFromContainer(apLight);

		STLFindAndDelete(mlstLights, apLight);
		MarkLightBuffersDirty(); // membership changed → re-bake the light buffers
	}

	//-----------------------------------------------------------------------

	iLight* cWorld::GetLight(const tString& asName)
	{
		tLightListIt LightIt=mlstLights.begin();
		for(;LightIt !=mlstLights.end();++LightIt)
		{
			if((*LightIt)->GetName() == asName){
				return *LightIt;
			}
		}
		return NULL;
	}

	iLight* cWorld::GetLightFromUniqueID(int alID)
	{
		tLightListIt LightIt=mlstLights.begin();
		for(;LightIt !=mlstLights.end();++LightIt)
		{
			if((*LightIt)->GetUniqueID() == alID){
				return *LightIt;
			}
		}
		return NULL;
	}

	//-----------------------------------------------------------------------

	cBillboard* cWorld::CreateBillboard(const tString& asName, const cVector2f& avSize,eBillboardType aType,
										const tString& asMaterial, bool abStatic)
	{
		cBillboard* pBillboard = hplNew( cBillboard, (asName, avSize,aType,mpResources,mpGraphics) );
		mlstBillboards.push_back(pBillboard);

		if(asMaterial!="")
		{
			pBillboard->SetMaterial(mpResources->GetMaterialManager()->CreateMaterial(asMaterial));
		}

		pBillboard->SetStatic(abStatic);
		AddRenderableToContainer(pBillboard);
		
		return pBillboard;
	}
	//-----------------------------------------------------------------------

	void cWorld::DestroyBillboard(cBillboard* apObject)
	{
		RemoveRenderableFromContainer(apObject);

		STLFindAndDelete(mlstBillboards, apObject);
	}

	//-----------------------------------------------------------------------

	cBillboard* cWorld::GetBillboard(const tString& asName)
	{
		return (cBillboard*)STLFindByName(mlstBillboards,asName);
	}

	cBillboard* cWorld::GetBillboardFromUniqueID(int alID)
	{
		tBillboardListIt BillboardIt=mlstBillboards.begin();
		for(;BillboardIt !=mlstBillboards.end();++BillboardIt)
		{
			if((*BillboardIt)->GetUniqueID() == alID){
				return *BillboardIt;
			}
		}
		return NULL;
	}

	//-----------------------------------------------------------------------

	cBillboardIterator cWorld::GetBillboardIterator()
	{
		return cBillboardIterator(&mlstBillboards);
	}

	//-----------------------------------------------------------------------


	cBeam* cWorld::CreateBeam(const tString& asName, bool abStatic)
	{
		cBeam* pBeam = hplNew( cBeam, (asName,mpResources,mpGraphics) );
		mlstBeams.push_back(pBeam);

		pBeam->SetStatic(abStatic);
		AddRenderableToContainer(pBeam);
			
		
		return pBeam;
	}
	//-----------------------------------------------------------------------

	void cWorld::DestroyBeam(cBeam* apObject)
	{
		RemoveRenderableFromContainer(apObject);

		STLFindAndDelete(mlstBeams, apObject);
	}

	//-----------------------------------------------------------------------

	cBeam* cWorld::GetBeam(const tString& asName)
	{
		return (cBeam*)STLFindByName(mlstBeams,asName);
	}

	cBeam* cWorld::GetBeamFromUniqueID(int alID)
	{
		for(tBeamListIt BeamIt=mlstBeams.begin();BeamIt !=mlstBeams.end();++BeamIt)
		{
			if((*BeamIt)->GetUniqueID() == alID) return *BeamIt;
		}
		return NULL;
	}

	//-----------------------------------------------------------------------

	cBeamIterator cWorld::GetBeamIterator()
	{
		return cBeamIterator(&mlstBeams);
	}


	//-----------------------------------------------------------------------

	cParticleSystem* cWorld::CreateParticleSystem(const tString& asName,const tString& asType,const cVector3f& avSize, bool abRemoveWhenDead)
	{
		cParticleSystem* pPS = mpResources->GetParticleManager()->CreatePS(asName,asType, avSize);
		if(pPS == NULL){
			Error("Couldn't create particle system '%s' of type '%s'\n",asName.c_str(), asType.c_str());
			return NULL;
		}

		//Log("Created particle system '%s' of type '%s'\n",asName.c_str(), asType.c_str());
		if(false)//asName== "candlestick02_1_ParticleSystem_1")
		{
			for(int i=0; i< pPS->GetEmitterNum();++i)
			{
				iParticleEmitter *pPE = pPS->GetEmitter(i);
				pPE->SetRenderFlagBit(eRenderableFlag_ContainerDebug, true);
			}
		}

		pPS->SetRemoveWhenDead(abRemoveWhenDead);

		//Add the emitters contained in the system.
		//Do not add the system itself.
		for(int i=0; i< pPS->GetEmitterNum();++i)
		{
			iParticleEmitter *pPE = pPS->GetEmitter(i);

			AddRenderableToContainer(pPE);

			pPE->SetWorld(this);
		}

		mlstParticleSystems.push_back(pPS);

		//Log("Created particle system '%s'\n",asType.c_str());

		return pPS;
	}

	//-----------------------------------------------------------------------

	cParticleSystem* cWorld::CreateParticleSystem(const tString& asName, const tString& asDataName, tinyxml2::XMLElement* apElement, const cVector3f& avSize)
	{
		cParticleSystem* pPS = mpResources->GetParticleManager()->CreatePS(asName,asDataName, apElement, avSize);
		if(pPS == NULL){
			Error("Couldn't create particle system '%s' of type '%s'\n",asName.c_str(), asDataName.c_str());
			return NULL;
		}

		//Log("Created particle system '%s' of type '%s'\n",asName.c_str(), asType.c_str());

		//Add the emitters contained in the system.
		//Do not add the system itself.
		for(int i=0; i< pPS->GetEmitterNum();++i)
		{
			iParticleEmitter *pPE = pPS->GetEmitter(i);

			AddRenderableToContainer(pPE);

			pPE->SetWorld(this);
		}

		mlstParticleSystems.push_back(pPS);

		//Log("Created particle system '%s'\n",asType.c_str());

		return pPS;
	}

	//-----------------------------------------------------------------------

	void cWorld::DestroyParticleSystem(cParticleSystem* apPS)
	{
		if(apPS==NULL)return;

		for(int i=0; i< apPS->GetEmitterNum();++i)
		{
			iParticleEmitter *pPE = apPS->GetEmitter(i);

			RemoveRenderableFromContainer(pPE);
		}

		STLFindAndDelete(mlstParticleSystems, apPS);
	}

	//-----------------------------------------------------------------------

	cParticleSystem* cWorld::GetParticleSystem(const tString& asName)
	{
		return (cParticleSystem*)STLFindByName(mlstParticleSystems,asName);
	}

	cParticleSystem* cWorld::GetParticleSystemFromUniqueID(int alID)
	{
		for(tParticleSystemListIt PSIt=mlstParticleSystems.begin();PSIt !=mlstParticleSystems.end();++PSIt)
		{
			if((*PSIt)->GetUniqueID() == alID) return *PSIt;
		}
		return NULL;
	}

	//-----------------------------------------------------------------------

	bool cWorld::ParticleSystemExists(cParticleSystem* apPS)
	{
		tParticleSystemListIt it = mlstParticleSystems.begin();
		for(; it != mlstParticleSystems.end(); ++it)
		{
			if(apPS == *it) return true;
		}
		return false;
	}

	void cWorld::DestroyAllParticleSystems()
	{
		tParticleSystemListIt it = mlstParticleSystems.begin();
		for(; it != mlstParticleSystems.end(); ++it)
		{
			cParticleSystem *pPS = *it;
			
			for(int i=0; i< pPS->GetEmitterNum();++i)
			{
				iParticleEmitter *pPE = pPS->GetEmitter(i);

				RemoveRenderableFromContainer(pPE);
			}
			hplDelete(pPS);
		}
		mlstParticleSystems.clear();
	}

	//-----------------------------------------------------------------------

	cGuiSetEntity* cWorld::CreateGuiSetEntity(const tString& asName, cGuiSet *apSet, bool abStatic)
	{
		cGuiSetEntity *pSetEntity = hplNew( cGuiSetEntity, (asName, apSet) );
		mlstGuiSetEntities.push_back(pSetEntity);

		pSetEntity->SetStatic(abStatic);
		AddRenderableToContainer(pSetEntity);
		
		return pSetEntity;
	}
	
	void cWorld::DestroyGuiSetEntity(cGuiSetEntity* apObject)
	{
		//TODO...RemoveRenderableFromContainer(...), etc

		STLFindAndDelete(mlstGuiSetEntities, apObject);
	}
	
	cGuiSetEntity* cWorld::GetGuiSetEntity(const tString& asName)
	{
		return static_cast<cGuiSetEntity*>(STLFindByName(mlstGuiSetEntities, asName));
	}

	cGuiSetEntity* cWorld::GetGuiSetEntityFromUniqueID(int alID)
	{
		for(tGuiSetEntityListIt it=mlstGuiSetEntities.begin();it !=mlstGuiSetEntities.end();++it)
		{
			if((*it)->GetUniqueID() == alID) return *it;
		}
		return NULL;
	}
	
	cGuiSetEntityIterator cWorld::GetGuiSetEntityIterator()
	{
		return cGuiSetEntityIterator(&mlstGuiSetEntities);
	}
	
	//-----------------------------------------------------------------------

	cRopeEntity* cWorld::CreateRopeEntity(const tString& asName, iPhysicsRope *apRope, int alMaxSegments)
	{
		cRopeEntity *pRope = hplNew( cRopeEntity, (asName, mpResources, mpGraphics, apRope, alMaxSegments));
		mlstRopeEntities.push_back(pRope);

		AddRenderableToContainer(pRope);
		
		return pRope;
	}

	void cWorld::DestroyRopeEntity(cRopeEntity* apRope)
	{
		RemoveRenderableFromContainer(apRope);

		STLFindAndDelete(mlstRopeEntities, apRope);
	}

	cRopeEntity* cWorld::GetRopeEntity(const tString& asName)
	{
		return static_cast<cRopeEntity*>(STLFindByName(mlstRopeEntities, asName));
	}

	cRopeEntity* cWorld::GetRopeEntityFromUniqueID(int alID)
	{
		for(tRopeEntityListIt it=mlstRopeEntities.begin();it !=mlstRopeEntities.end();++it)
		{
			if((*it)->GetUniqueID() == alID) return *it;
		}
		return NULL;
	}

	cRopeEntityIterator cWorld::GetRopeEntityIterator()
	{
		return cRopeEntityIterator(&mlstRopeEntities);
	}

	//-----------------------------------------------------------------------

	cFogArea* cWorld::CreateFogArea(const tString& asName, bool abStatic)
	{
		cFogArea *pFog = hplNew( cFogArea, (asName, mpResources));
		mlstFogAreas.push_back(pFog);
		pFog->SetStatic(abStatic);


		AddRenderableToContainer(pFog);

		MarkFogBufferDirty(); // membership changed → re-bake the fog buffer
		return pFog;
	}

	void cWorld::DestroyFogArea(cFogArea* apRope)
	{
		RemoveRenderableFromContainer(apRope);

		STLFindAndDelete(mlstFogAreas, apRope);
		MarkFogBufferDirty(); // membership changed → re-bake the fog buffer
	}

	cFogArea* cWorld::GetFogArea(const tString& asName)
	{
		return static_cast<cFogArea*>(STLFindByName(mlstFogAreas, asName));
	}

	cFogArea* cWorld::GetFogAreaFromUniqueID(int alID)
	{
		for(tFogAreaListIt it=mlstFogAreas.begin();it !=mlstFogAreas.end();++it)
		{
			if((*it)->GetUniqueID() == alID) return *it;
		}
		return NULL;
	}

	cFogAreaIterator cWorld::GetFogAreaIterator()
	{
		return cFogAreaIterator(&mlstFogAreas);
	}

	//-----------------------------------------------------------------------

	cSoundEntity* cWorld::CreateSoundEntity(const tString &asName,const tString &asSoundEntity,
											bool abRemoveWhenOver)
	{
		cSoundEntityData *pData = mpResources->GetSoundEntityManager()->CreateSoundEntity(asSoundEntity);
		if(pData==NULL){
			Error("Cannot find sound entity '%s'\n",asSoundEntity.c_str());
			return NULL;
		}

		cSoundEntity *pSound = hplNew( cSoundEntity, (asName,pData, mpResources->GetSoundEntityManager(), this, mpSound->GetSoundHandler(),
														abRemoveWhenOver,mlSoundCreationIDCount++));
		/*cSoundEntity *pSound = NULL;
		if(mlstSoundEntityPool.empty())
		{
			pSound = hplNew( cSoundEntity, (asName,pData,
											mpResources->GetSoundEntityManager(),
											this,
											mpSound->GetSoundHandler(),abRemoveWhenOver,mlSoundCreationIDCount++));
		}
		else
		{
			pSound = mlstSoundEntityPool.back();
			pSound->Setup(asName, pData, abRemoveWhenOver);
			mlstSoundEntityPool.pop_back();
		}*/

		mlstSoundEntities.push_back(pSound);

		return pSound;
	}

	void cWorld::DestroySoundEntity(cSoundEntity* apEntity)
	{
		//STLFindAndDelete(mlstSoundEntities,apEntity);
		
		//Only delete if found! (as it is possible that the sound entity is not longer a valid pointer!
		tSoundEntityListIt it= mlstSoundEntities.begin();
		for(; it != mlstSoundEntities.end(); ++it)
		{
			cSoundEntity *pSound = *it;
			if(pSound == apEntity)
			{
				mlstSoundEntities.erase(it);
				hplDelete(pSound);
				//mlstSoundEntityPool.push_back(apEntity);
				break;
			}
		}
	}

	void cWorld::DestroyAllSoundEntities()
	{
		//Make sure no body has any sound entity
		if(mpPhysicsWorld)
		{
			cPhysicsBodyIterator bodyIt = mpPhysicsWorld->GetBodyIterator();
			while(bodyIt.HasNext())
			{
				iPhysicsBody *pBody = bodyIt.Next();
				pBody->SetScrapeSoundEntity(NULL);
				pBody->SetRollSoundEntity(NULL);
			}

			cPhysicsJointIterator jointIt = mpPhysicsWorld->GetJointIterator();
			while(jointIt.HasNext())
			{
				iPhysicsJoint *pJoint = jointIt.Next();
				pJoint->SetSound(NULL);
			}
		}
		
		//Destroy all sound entities
		STLDeleteAll(mlstSoundEntities);
		mlstSoundEntities.clear();
	}


	cSoundEntity* cWorld::GetSoundEntity(const tString& asName)
	{
		return (cSoundEntity*)STLFindByName(mlstSoundEntities,asName);
	}

	cSoundEntity* cWorld::GetSoundEntityFromUniqueID(int alID)
	{
		for(tSoundEntityListIt it=mlstSoundEntities.begin();it !=mlstSoundEntities.end();++it)
		{
			if((*it)->GetUniqueID() == alID) return *it;
		}
		return NULL;
	}

	bool cWorld::SoundEntityExists(cSoundEntity* apEntity, int alCreationID)
	{
		tSoundEntityListIt it= mlstSoundEntities.begin();
		tSoundEntityListIt end = mlstSoundEntities.end();
		for(; it != end; ++it)
		{
			cSoundEntity *pTestSound = *it;
			if(*it == apEntity)
			{
				if(alCreationID==pTestSound->GetCreationID())	return true;
				else											return false;
			}
		}

		return false;
	}

	//-----------------------------------------------------------------------

	cStartPosEntity* cWorld::CreateStartPos(const tString &asName)
	{
		cStartPosEntity *pStartPos = hplNew( cStartPosEntity, (asName) );

		mlstStartPosEntities.push_back(pStartPos);

		return pStartPos;
	}

	cStartPosEntity* cWorld::GetStartPosEntity(const tString &asName)
	{
		return (cStartPosEntity*)STLFindByName(mlstStartPosEntities,asName);
	}

	cStartPosEntity* cWorld::GetFirstStartPosEntity()
	{
		if(mlstStartPosEntities.empty()) return NULL;

		return mlstStartPosEntities.front();
	}


	//-----------------------------------------------------------------------

	void  cWorld::GenerateAINodes(cAINodeGeneratorParams *apParams)
	{
		mpAI->GetNodeGenerator()->Generate(this, apParams);
	}

	//-----------------------------------------------------------------------

	cAINodeContainer* cWorld::CreateAINodeContainer(const tString &asName, 
								const tString &asNodeName,
								const cVector3f &avSize,
								bool abNodeIsAtCenter,
								int alMinEdges, int alMaxEdges, float afMaxEdgeDistance,float afMaxHeight)
	{
		cAINodeContainer* pContainer=NULL;

		//unsigned long lStartTime = mpSystem->GetLowLevel()->GetTime();

		//////////////////////////////////
		//See if the container is allready loaded.
		tAINodeContainerListIt it = mlstAINodeContainers.begin();
		for(; it != mlstAINodeContainers.end(); ++it)
		{
			cAINodeContainer* pCont = *it;
			if(pCont->GetName() == asName)
			{
				pContainer = pCont;
			}
		}
		

		//////////////////////////////////
		//Get file name
		cFileSearcher *pFileSearcher = mpResources->GetFileSearcher();
		tWString sMapPath = GetFilePath();

		tWString sAiFileName = cString::SetFileExtW(sMapPath,_W(""));
		sAiFileName += _W("_")+cString::To16Char(asName);
		sAiFileName = cString::SetFileExtW(sAiFileName,_W("nodes"));

		//////////////////////////////////
		//If there is no container created, create it.
		if(pContainer == NULL)
		{
			tTempNodeContainerMapIt ContIt = m_mapTempNodes.find(asNodeName);
			if(ContIt == m_mapTempNodes.end()){
				Warning("AI node type '%s' does not exist!\n",asNodeName.c_str());
				return NULL;
			}
			cTempNodeContainer *pTempCont = ContIt->second;

			pContainer = hplNew( cAINodeContainer, (asName,asNodeName,this,avSize) );
			mlstAINodeContainers.push_back(pContainer);

			//Set properties
			pContainer->SetMinEdges(alMinEdges);
			pContainer->SetMaxEdges(alMaxEdges);
			pContainer->SetMaxEdgeDistance(afMaxEdgeDistance);
			pContainer->SetMaxHeight(afMaxHeight);
			pContainer->SetNodeIsAtCenter(abNodeIsAtCenter);

			//Reserve space for the incoming nodes.
			pContainer->ReserveSpace(pTempCont->mlstNodes.size());

            //Add nodes to container
			tTempAiNodeListIt NodeIt = pTempCont->mlstNodes.begin();
			for(; NodeIt != pTempCont->mlstNodes.end(); ++NodeIt)
			{
				cTempAiNode& pNode = *NodeIt;
				pContainer->AddNode(pNode.msName,pNode.mlID,pNode.mvPos,NULL);
			}
			
			bool bLoadedFromFile=false;
			if(cPlatform::FileExists(sAiFileName))
			{
				cDate dateMapFile = cPlatform::FileModifiedDate(sMapPath);
				cDate dateAIFile = cPlatform::FileModifiedDate(sAiFileName);

				if(dateAIFile > dateMapFile || cResources::GetForceCacheLoadingAndSkipSaving())
				{
					bLoadedFromFile = true;
					pContainer->LoadFromFile(sAiFileName);
				}
			}
			
			if(bLoadedFromFile==false)
			{
				Log("Rebuilding node connections and saving to '%s'\n",cString::To8Char(sAiFileName).c_str());

				//Compile
				pContainer->Compile();

				//Save to disk
				if(cResources::GetForceCacheLoadingAndSkipSaving()==false)
				{
					pContainer->SaveToFile(sAiFileName);
				}
			}
		}
		
		//unsigned long lTime = mpSystem->GetLowLevel()->GetTime() - lStartTime;
		//Log("Creating ai nodes took: %d\n",lTime);


		return pContainer;
	}

	//-----------------------------------------------------------------------

	cAStarHandler* cWorld::CreateAStarHandler(cAINodeContainer* apContainer)
	{
		cAStarHandler *pAStar = hplNew( cAStarHandler, (apContainer) );

		mlstAStarHandlers.push_back(pAStar);

		return pAStar;
	}

	void cWorld::DestroyAStarHandler(cAStarHandler* apHandler)
	{
		STLFindAndDelete(mlstAStarHandlers, apHandler);
	}

	//-----------------------------------------------------------------------

	void cWorld::AddAINode(const tString &asName, int alID, const tString &asType, const cVector3f &avPosition)
	{
		cTempNodeContainer *pContainer = NULL;
		tTempNodeContainerMapIt it = m_mapTempNodes.find(asType);
		if(it != m_mapTempNodes.end()){
			pContainer = it->second;
		}

		if(pContainer==NULL){
			pContainer = hplNew( cTempNodeContainer, () );
			m_mapTempNodes.insert(tTempNodeContainerMap::value_type(asType,pContainer));
		}

		pContainer->mlstNodes.push_back(cTempAiNode(avPosition,asName, alID));
	}

	//-----------------------------------------------------------------------

	tTempAiNodeList* cWorld::GetAINodeList(const tString &asType)
	{
		cTempNodeContainer *pContainer = NULL;
		tTempNodeContainerMapIt it = m_mapTempNodes.find(asType);
		if(it != m_mapTempNodes.end()){
			pContainer = it->second;
		}

		if(pContainer==NULL){
			pContainer = hplNew( cTempNodeContainer, () );
			m_mapTempNodes.insert(tTempNodeContainerMap::value_type(asType,pContainer));
		}

		return &pContainer->mlstNodes;
	}

	//-----------------------------------------------------------------------

	bool cWorld::CreateFromFile(tString asFile)
	{
		return false;
	}

	//-----------------------------------------------------------------------

	cDummyRenderable* cWorld::CreateDummyRenderable(const tString& asName, bool abStatic)
	{
		cDummyRenderable *pDummy = hplNew( cDummyRenderable, (asName));
		mlstDummyRenderables.push_back(pDummy);
		pDummy->SetStatic(abStatic);

		AddRenderableToContainer(pDummy);

		return pDummy;
	}

	void cWorld::DestroyDummyRenderable(cDummyRenderable* apDummy)
	{
		RemoveRenderableFromContainer(apDummy);

		STLFindAndDelete(mlstDummyRenderables, apDummy);
	}

	cDummyRenderable* cWorld::GetDummyRenderable(const tString& asName)
	{
		return static_cast<cDummyRenderable*>(STLFindByName(mlstDummyRenderables, asName));
	}

	cDummyRenderable* cWorld::GetDummyRenderableFromUniqueID(int alID)
	{
		for(tDummyRenderableListIt it=mlstDummyRenderables.begin();it !=mlstDummyRenderables.end();++it)
		{
			if((*it)->GetUniqueID() == alID) return *it;
		}
		return NULL;
	}

	cDummyRenderableIterator cWorld::GetDummyRenderableIterator()
	{
		return cDummyRenderableIterator(&mlstDummyRenderables);
	}

	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// PRIVATE METHODS
	//////////////////////////////////////////////////////////////////////////
	
	//-----------------------------------------------------------------------

	void cWorld::AddRenderableToContainer(iRenderable *apObject)
	{
		if(apObject->IsStatic())
			mpRenderableContainer[eWorldContainerType_Static]->Add(apObject);
		else
			mpRenderableContainer[eWorldContainerType_Dynamic]->Add(apObject);
	}
	
	//-----------------------------------------------------------------------

	void cWorld::RemoveRenderableFromContainer(iRenderable *apObject)
	{
		if(apObject->IsStatic())	mpRenderableContainer[eWorldContainerType_Static]->Remove(apObject);
		else						mpRenderableContainer[eWorldContainerType_Dynamic]->Remove(apObject);
	}

	//-----------------------------------------------------------------------

	void cWorld::UpdateParticles(float afTimeStep)
	{
		tParticleSystemListIt it = mlstParticleSystems.begin();

		while(it != mlstParticleSystems.end())
		{
			cParticleSystem *pPS = *it;

			pPS->UpdateLogic(afTimeStep);

			//Check if the system is alive, else destroy
			if(pPS->GetRemoveWhenDead() && pPS->IsDead())
			{
                it = mlstParticleSystems.erase(it);
				for(int i=0; i< pPS->GetEmitterNum();++i)
				{
					RemoveRenderableFromContainer(pPS->GetEmitter(i));
				}
				hplDelete(pPS);
			}
			else
			{
				it++;
			}
		}
	}
	//-----------------------------------------------------------------------


	void cWorld::UpdateEntities(float afTimeStep)
	{
		//static size_t lLastSize = 0;
		//bool bRenderDebug = lLastSize != mlstDynamicMeshEntities.size() && mlstDynamicMeshEntities.size()>=2;
		//if(mlstDynamicMeshEntities.size()>=2) lLastSize = mlstDynamicMeshEntities.size();
        	
		tMeshEntityListIt MeshIt = mlstDynamicMeshEntities.begin();
		tMeshEntityListIt endIt =mlstDynamicMeshEntities.end();
		//if(bRenderDebug)Log("----\n");
		for(;MeshIt != endIt;MeshIt++)
		{
			cMeshEntity *pEntity = *MeshIt;

			if(pEntity->IsActive()){
				//if(pEntity->IsStatic()==false) START_TIMING_EX(pEntity->GetName().c_str(),entity);
				pEntity->UpdateLogic(afTimeStep);
				//if(bRenderDebug) Log("Enitity '%s'. Pos: (%s), Matrix: (%s)\n", pEntity->GetName().c_str(), pEntity->GetWorldPosition().ToString().c_str(),
																			//pEntity->GetWorldMatrix().ToString().c_str());
				//if(pEntity->IsStatic()==false) STOP_TIMING(entity);
			}
		}
		//if(bRenderDebug)Log("----\n");
	}

	//-----------------------------------------------------------------------

	void cWorld::UpdateLights(float afTimeStep)
	{
		tLightListIt it = mlstLights.begin();

		while(it != mlstLights.end())
		{
			iLight *pLight = *it;

			if(pLight->IsActive()) pLight->UpdateLogic(afTimeStep);

			++it;
		}
	}

	//-----------------------------------------------------------------------

	void cWorld::UpdateSoundEntities(float afTimeStep)
	{
		tSoundEntityListIt it = mlstSoundEntities.begin();

		while(it != mlstSoundEntities.end())
		{
			cSoundEntity *pSound = *it;

			if(pSound->IsActive()) pSound->UpdateLogic(afTimeStep);

			//Check if the system is stopped, else destroy
			if(pSound->IsStopped() && pSound->GetRemoveWhenOver())
			{
				it =  mlstSoundEntities.erase(it);
				//mlstSoundEntityPool.push_back(pSound);
				hplDelete(pSound);
			}
			else
			{
				it++;
			}
		}
	}

	//-----------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// SAVE OBJECT STUFF
	//////////////////////////////////////////////////////////////////////////

	//-------------------------------------------------------------------

	kBeginSerializeBase(cAreaEntity)
	kSerializeVar(msName, eSerializeType_String)
	kSerializeVar(msType, eSerializeType_String)
	kSerializeVar(m_mtxTransform, eSerializeType_Matrixf)
	kSerializeVar(mvSize, eSerializeType_Vector3f)
	kEndSerialize()

	//-------------------------------------------------------------------

	kBeginSerializeBase(cStartPosEntity)
	kSerializeVar(msName, eSerializeType_String)
	kSerializeVar(m_mtxTransform, eSerializeType_Matrixf)
	kEndSerialize()

	
	//-----------------------------------------------------------------------


}
