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

#ifndef HPL_WORLD_H
#define HPL_WORLD_H

#include "resources/ResourceBase.h"
#include "system/SystemTypes.h"
#include "graphics/GraphicsTypes.h"
#include "math/MathTypes.h"
#include "engine/EngineTypes.h"
#include "scene/SceneTypes.h"
#include "graphics/RITypes.h" // RISharedPointer<RIBuffer> decal buffer members
#include "graphics/IndexPool.h" // stable per-type GPU light slots

#include <cstdint>

namespace tinyxml2 { class XMLElement; }

namespace hpl {

	class cGraphics;
	class cResources;
	class cDecal;
	class cMaterial;
	class cSound;
	class cPhysics;
	class cScene;
	class cSystem;
	class cAI;
	class cHaptic;

	class iCamera;
	class cCamera;
	class cNode3D;
	class iEntity3D;
	class iLight;
	class cLightSpot;
	class cLightPoint;
	class cLightBox;
	class cLightArea;
	class cImageEntity;
	class cParticleManager;
	class cParticleSystem;
	class iScript;
	class cPortalContainer;
	class iRenderableContainer;
	class cMeshEntity;
	class cMesh;
	class cBillboard;
	class cBeam;
	class cGuiSetEntity;
	class iPhysicsWorld;
	class iPhysicsBody;
	class cSoundEntity;
	class cAINodeContainer;
	class cAStarHandler;
	class cAINodeGeneratorParams;
	class cVertexBuffer;
	class Image;
	class cGuiSet;
	class cRopeEntity;
	class iPhysicsRope;
	class cResourceVarsObject;
	class cFogArea;
	class cEntFile;
	class cDummyRenderable;
	struct SceneConstants; // per-frame UBO (SceneTypes.slang); SubmitToGpu fills the *Count fields

	
	typedef std::list<cEntFile*> tEntFileList;
	typedef tEntFileList::iterator tEntFileListIt;

	class cTempAiNode
	{
	public:
		cTempAiNode(const cVector3f& avPos, const tString& asName, int alID) : mvPos(avPos),msName(asName), mlID(alID) {}
		cVector3f mvPos;
		tString msName;
		int mlID;
	};
	
	typedef std::list<cTempAiNode> tTempAiNodeList;
	typedef std::list<cTempAiNode>::iterator tTempAiNodeListIt;

	class cTempNodeContainer
	{
	public:
		tString msName;
        tTempAiNodeList mlstNodes;		
	};

	typedef std::map<tString,cTempNodeContainer*> tTempNodeContainerMap;
	typedef std::map<tString,cTempNodeContainer*>::iterator tTempNodeContainerMapIt;

	//-------------------------------------------------------------------

	class cAreaEntity :public iSerializable
	{
		kSerializableClassInit(cAreaEntity)
	public:
		tString msName;
		tString msType;
		cMatrixf m_mtxTransform;
		cVector3f mvSize;
	};
	
	typedef std::map<tString, cAreaEntity*> tAreaEntityMap;
	typedef tAreaEntityMap::iterator tAreaEntityMapIt;

	//-------------------------------------------------------------------

	class cStartPosEntity : public iSerializable
	{
		kSerializableClassInit(cStartPosEntity)
	public:
		cStartPosEntity() {}
		cStartPosEntity(const tString& asName) : msName(asName){}

		cMatrixf& GetWorldMatrix(){ return m_mtxTransform;}
		cMatrixf& GetLocalMatrix(){ return m_mtxTransform;}
		void SetMatrix(const cMatrixf& a_mtxTrans){ m_mtxTransform = a_mtxTrans;}
		
		tString& GetName(){ return msName;}

		cMatrixf m_mtxTransform;
		tString msName;
	};
	
	typedef std::list<cStartPosEntity*> tStartPosEntityList;
	typedef std::list<cStartPosEntity*>::iterator tStartPosEntityListIt;

	//-------------------------------------------------------------------


	class cWorld
	{
	public:
		cWorld(tString asName,cGraphics *apGraphics,cResources *apResources,cSound* apSound,
					cPhysics *apPhysics, cScene *apScene,cSystem *apSystem, cAI *apAI,
					cHaptic *apHaptic);
		~cWorld();

		void DestroyAllEntities(tWorldDestroyAllFlag aFlags);

		tString GetName(){ return msName;}

		bool CreateFromFile(tString asFile);

		void SetFilePath(const tWString& asFile){ msFilePath = asFile;}
		const tWString& GetFilePath(){ return msFilePath;}

		void SetActive(bool abX) {mbActive = abX;}
		inline bool IsActive()  const { return mbActive;}

		void Update(float afTimeStep);

		void PreUpdate(float afTotalTime, float afTimeStep);

		cVector3f GetWorldSize(){ return mvWorldSize;}

		void SetIsSoundEmitter(bool abX){ mbIsSoundEmitter = abX;}
		bool IsSoundEmitter(){ return mbIsSoundEmitter;}

		iRenderableContainer* GetRenderableContainer(eWorldContainerType aType);
		
		cPhysics* GetPhysics(){ return mpPhysics;}
		cResources* GetResources(){ return mpResources;}
		cSound* GetSound(){ return mpSound;}
		cSystem* GetSystem(){ return mpSystem;}
		cHaptic* GetHaptic(){ return mpHaptic;}

		iEntity3D* CreateEntity(const tString& asName, const cMatrixf &a_mtxTransform, 
								const tString& asFile, int alID = -1, bool abActive=true,
								const cVector3f &avScale=cVector3f(1),
								cResourceVarsObject *apInstanceVars=NULL,
								bool abSkipNonStaticEntity=false);
								
		/**
		 * Call this when all things have been added to set up things like physics world size.
		 **/
		void Compile(bool abCalcPhysicsWorldSize);


		///// PHYSICS ////////////////////////////////

		void SetPhysicsWorld(iPhysicsWorld *apWorld, bool abAutoDelete=true);
		iPhysicsWorld* GetPhysicsWorld();

		///// SKYBOX ////////////////////////////////

		void SetSkyBox(Image *apImage, bool abAutoDestroy);
		void SetSkyBoxActive(bool abX);
		void SetSkyBoxColor(const cColor& aColor);

		Image* GetSkyBoxImage() const { return mpSkyBoxImage; }
		cVertexBuffer *GetSkyBoxVertexBuffer(){ return mpSkyBoxVtxBuffer;}
		bool GetSkyBoxActive(){ return mbSkyBoxActive;}
		cColor GetSkyBoxColor(){ return mSkyBoxColor;}
        
		///// FOG ////////////////////////////////

		void SetFogActive(bool abX){ mbFogActive = abX;}
		void SetFogStart(float afX){ mfFogStart = afX;}
		void SetFogEnd(float afX){ mfFogEnd = afX;}
		void SetFogFalloffExp(float afX){ mfFogFalloffExp = afX;}
		void SetFogColor(const cColor& aCol){ mFogColor = aCol; }
		void SetFogCulling(bool abX) {mbFogCulling=abX;}

		bool GetFogActive(){ return mbFogActive;}
		float GetFogStart(){ return mfFogStart;}
		float GetFogEnd(){ return mfFogEnd;}
		float GetFogFalloffExp(){ return mfFogFalloffExp;}
		const cColor& GetFogColor(){ return mFogColor; }
		bool GetFogCulling() { return mbFogCulling;}

		///// AREA ////////////////////////////////
		
		cAreaEntity* CreateAreaEntity(const tString &asName);
		cAreaEntity* GetAreaEntity(const tString &asName);
		tAreaEntityMap* GetAreaEntityMap(){return &m_mapAreaEntities;}

		///// MESH ENTITY METHODS ////////////////////
		
		cMeshEntity* CreateMeshEntity(const tString &asName,cMesh *apMesh, bool abStatic=false);
		void DestroyMeshEntity(cMeshEntity* apMesh);

		///// DECAL METHODS //////////////////////////
		// Creates an oriented-box decal (no geometry). The world owns it and the
		// material; both are freed in DestroyAllEntities. Caller sets the box
		// transform via SetWorldMatrix/SetPosition.
		cDecal* CreateDecal(const tString& asName, const tString& asMaterial,
							const cColor& aColor, const cVector2l& avSubDiv);

		// All decals, in stable order (== gDecals[] upload order + the index space
		// of GetDecalObjectIndices()).
		const std::vector<cDecal*>& GetDecals() const { return mvDecals; }
		// Flat pool of stable decal indices; each renderable's [offset,count)
		// (iRenderable::GetDecalList*) addresses a run here. Built by Compile().
		const std::vector<uint32_t>& GetDecalObjectIndices() const { return mvDecalObjectIndices; }

		// Per-world decal GPU buffers, baked once by SubmitToGpu (geometry from
		// mvDecals with the bindless slot baked in + a pin per Image; object-index
		// pool from mvDecalObjectIndices). Re-baked on membership change. Null until
		// the first bake. The renderer binds these on the composite pass; cWorld
		// owns them.
		RIBuffer* GetDecalBuffer() const { return mpDecalBuffer.Get(); }
		RIBuffer* GetDecalObjectIndexBuffer() const { return mpDecalObjectIndexBuffer.Get(); }
		uint32_t GetDecalCount() const { return (uint32_t)mvDecals.size(); }

		// Single per-frame world→GPU submission, called once by the renderer before
		// any pass: rebuilds + uploads the light/fog/decal buffers and fills the
		// perFrame *Count fields. It binds no descriptor set — the light/fog buffers
		// ride the per-world set kWorldSet, bound by each consuming pass; the set-2
		// decal buffers are bound at the composite pass.
		void SubmitToGpu(SceneConstants& perFrame);

		// Per-world fog-area buffer (FogAreaParams[], bound on the per-world set
		// kWorldSet by each consuming pass). Rebuilt + uploaded every frame by
		// SubmitToGpu over ALL fog areas (no frustum cull — the per-pixel Fog.slang
		// loop is bounded by the count) and grown on demand. Null until the first
		// SubmitToGpu.
		RIBuffer* GetFogAreaBuffer() const { return mpFogAreaBuffer.Get(); }
		uint32_t GetFogAreaCount() const { return mFogAreaCount; }

		// Per-world light buffers (PointLight[]/SpotLight[]/RectLight[], bound on the
		// per-world set kWorldSet). Each light keeps a STABLE per-type slot from the
		// matching light-slot pool (mPointLightPool/…) for its whole lifetime, so its
		// packed GPU id (packLightId(type, slot)) is identical every frame — the
		// identity ReSTIR DI reservoirs persist. SubmitToGpu scatters each light into
		// buffer[slot] (sparse) and uploads up to the high-water slot; grown on demand.
		// "Counts" are now per-type SLOT CAPACITIES (high-water), which the GPU light
		// grid + Scene.slang decode consume directly. Unused/destroyed-but-not-reused
		// slots and invisible lights are zeroed holes (radius 0) the grid skips. Null
		// until the first SubmitToGpu.
		RIBuffer* GetPointLightBuffer() const { return mpPointLightBuffer.Get(); }
		uint32_t  GetPointLightCount()  const { return mPointLightCount; }
		RIBuffer* GetSpotLightBuffer()  const { return mpSpotLightBuffer.Get(); }
		uint32_t  GetSpotLightCount()   const { return mSpotLightCount; }
		RIBuffer* GetAreaLightBuffer()  const { return mpAreaLightBuffer.Get(); }
		uint32_t  GetAreaLightCount()   const { return mAreaLightCount; }

		// No-ops: the light / fog / decal buffers are all rebuilt from the live
		// lists every frame in SubmitToGpu, so membership changes need no flag.
		// Kept so the Create*/Destroy* call sites (incl. the editor) stay unchanged.
		void MarkLightBuffersDirty() {}
		void MarkFogBufferDirty()    {}
		void MarkDecalBuffersDirty() { mbDecalBuffersDirty = true; }
		cMeshEntity* GetDynamicMeshEntity(const tString& asName);
		
		cMeshEntityIterator GetDynamicMeshEntityIterator();
		cMeshEntityIterator GetStaticMeshEntityIterator();
		
		void DrawMeshBoundingBoxes(const cColor& aColor, bool abStatic) {} // STUB
		
		///// LIGHT METHODS ////////////////////

		cLightPoint* CreateLightPoint(const tString &asName="",const tString &asGobo="", bool abStatic=false);
		cLightSpot* CreateLightSpot(const tString &asName="", const tString &asGobo="", bool abStatic=false);
		cLightBox* CreateLightBox(const tString &asName="", bool abStatic=false);
		cLightArea* CreateLightArea(const tString &asName="", bool abStatic=false);
		void DestroyLight(iLight* apLight);
		iLight* GetLight(const tString& asName);
		iLight* GetLightFromUniqueID(int alID);

		tLightList * GetLightList(){ return &mlstLights;}

		cLightListIterator GetLightIterator(){ return cLightListIterator(&mlstLights);}

		///// BILLBOARD METHODS ////////////////////

		cBillboard* CreateBillboard(const tString& asName, const cVector2f& avSize,eBillboardType aType,const tString& asMaterial="",bool abStatic=false);
		void DestroyBillboard(cBillboard* apObject);
		cBillboard* GetBillboard(const tString& asName);
		cBillboard* GetBillboardFromUniqueID(int alID);
		cBillboardIterator GetBillboardIterator();

		///// BEAM METHODS ////////////////////

		cBeam* CreateBeam(const tString& asName, bool abStatic=false);
		void DestroyBeam(cBeam* apObject);
		cBeam* GetBeam(const tString& asName);
		cBeam* GetBeamFromUniqueID(int alID);
		cBeamIterator GetBeamIterator();

		///// PARTICLE METHODS ////////////////////

		cParticleSystem* CreateParticleSystem(	const tString& asName,const tString& asType, const cVector3f& avSize, bool abRemoveWhenDead=true);
		cParticleSystem* CreateParticleSystem(	const tString& asName,const tString& asDataName, tinyxml2::XMLElement* apElement, const cVector3f& avSize);
		void DestroyParticleSystem(cParticleSystem* apPS);
		cParticleSystem* GetParticleSystem(const tString& asName);
		cParticleSystem* GetParticleSystemFromUniqueID(int alID);
		bool ParticleSystemExists(cParticleSystem* apPS);

		void DestroyAllParticleSystems();

		cParticleSystemIterator GetParticleSystemIterator(){ return cParticleSystemIterator(&mlstParticleSystems);}

		///// GUISET ENTITY METHODS ////////////////////
		
		cGuiSetEntity* CreateGuiSetEntity(const tString& asName, cGuiSet *apSet, bool abStatic=false);
		void DestroyGuiSetEntity(cGuiSetEntity* apObject);
		cGuiSetEntity* GetGuiSetEntity(const tString& asName);
		cGuiSetEntity* GetGuiSetEntityFromUniqueID(int alID);
		cGuiSetEntityIterator GetGuiSetEntityIterator();

		///// ROPE ENTITY METHODS ////////////////////
		
		cRopeEntity* CreateRopeEntity(const tString& asName, iPhysicsRope *apRope, int alMaxSegments);
		void DestroyRopeEntity(cRopeEntity* apRope);
		cRopeEntity* GetRopeEntity(const tString& asName);
		cRopeEntity* GetRopeEntityFromUniqueID(int alID);
		cRopeEntityIterator GetRopeEntityIterator();

		///// FOG AREA METHODS ////////////////////
		cFogArea* CreateFogArea(const tString& asName, bool abStatic=false);
		void DestroyFogArea(cFogArea* apRope);
		cFogArea* GetFogArea(const tString& asName);
		cFogArea* GetFogAreaFromUniqueID(int alID);
		cFogAreaIterator GetFogAreaIterator();

		///// SOUND ENTITY METHODS ////////////////////

		cSoundEntity* CreateSoundEntity(const tString &asName,const tString &asSoundEntity, 
										bool abRemoveWhenOver);
		void DestroySoundEntity(cSoundEntity* apEntity);
		cSoundEntity* GetSoundEntity(const tString& asName);
		cSoundEntity* GetSoundEntityFromUniqueID(int alID);
		void DestroyAllSoundEntities();
		bool SoundEntityExists(cSoundEntity* apEntity, int alCreationID);

		cSoundEntityIterator GetSoundEntityIterator(){ return cSoundEntityIterator(&mlstSoundEntities);}

		///// START POS ENTITY METHODS ////////////////
		
		cStartPosEntity* CreateStartPos(const tString &asName);
		cStartPosEntity* GetStartPosEntity(const tString &asName);
		cStartPosEntity* GetFirstStartPosEntity();

		///// AI NODE METHODS ////////////////

		void GenerateAINodes(cAINodeGeneratorParams *apParams);

		cAINodeContainer* CreateAINodeContainer(const tString &asName, 
											const tString &asNodeName, 
											const cVector3f &avSize,
											bool abNodeIsAtCenter, 
											int alMinEdges, int alMaxEdges, float afMaxEdgeDistance,
											float afMaxHeight);

		cAStarHandler* CreateAStarHandler(cAINodeContainer* apContainer);
		void DestroyAStarHandler(cAStarHandler* apHandler);
        
		void AddAINode(const tString &asName, int alID, const tString &asType, const cVector3f &avPosition);
		tTempAiNodeList* GetAINodeList(const tString &asType);


		/// NODE METHODS //////////////////////
		// Remove this for the time being, not need it seems.
		//cNode3D* GetRootNode(){ return mpRootNode; }

		///// DUMMY RENDERABLE METHODS ////////////////////
		cDummyRenderable* CreateDummyRenderable(const tString& asName, bool abStatic=false);
		void DestroyDummyRenderable(cDummyRenderable* apDummy);
		cDummyRenderable* GetDummyRenderable(const tString& asName);
		cDummyRenderable* GetDummyRenderableFromUniqueID(int alID);
		cDummyRenderableIterator GetDummyRenderableIterator();


	private:
		void AddRenderableToContainer(iRenderable *apObject);
		void RemoveRenderableFromContainer(iRenderable *apObject);

		void UpdateEntities(float afTimeStep);
		void UpdateParticles(float afTimeStep);
		void UpdateLights(float afTimeStep);
		void UpdateSoundEntities(float afTimeStep);

		tString msName;
		tWString msFilePath;
		bool mbActive;

		cGraphics *mpGraphics;
		cSound* mpSound;
		cResources *mpResources;
		cPhysics *mpPhysics;
		cScene *mpScene;
		cSystem *mpSystem;
		cAI *mpAI;
		cHaptic *mpHaptic;

		iPhysicsWorld *mpPhysicsWorld;
		bool mbAutoDeletePhysicsWorld;

		bool mbIsSoundEmitter;
		
		cVector3f mvWorldSize;

		iRenderableContainer* mpRenderableContainer[2];

		cVertexBuffer* mpSkyBoxVtxBuffer;
		Image* mpSkyBoxImage = nullptr;
		bool mbAutoDestroySkybox;
		bool mbSkyBoxActive;
		cColor mSkyBoxColor;

		bool mbFogActive;
		bool mbFogCulling;
		float mfFogStart;
		float mfFogEnd;
		float mfFogFalloffExp;
		cColor mFogColor;

		tLightList mlstLights;
		tMeshEntityList mlstDynamicMeshEntities;
		tMeshEntityList mlstStaticMeshEntities;
		std::vector<cDecal*> mvDecals;
		std::vector<uint32_t> mvDecalObjectIndices;   // flat per-object decal-index pool (Compile())

		// Decals are static set-dressing, so their GPU buffers are baked ONCE (on
		// membership change / first submit), not rebuilt per frame. The lifetime-
		// stable bindless diffuse slot is baked into each GpuDecal; a SharedResourcePin
		// per diffuse Image (mvDecalImagePins) keeps that slot valid for the buffer's
		// lifetime, independent of the decal objects. The object-index pool rides
		// mvDecalObjectIndices (built by Compile() — empty in an uncompiled world).
		RISharedPointer<RIBuffer> mpDecalBuffer;
		RISharedPointer<RIBuffer> mpDecalObjectIndexBuffer;
		std::vector<SharedResourcePin> mvDecalImagePins;
		bool mbDecalBuffersDirty = true;
		void BakeDecalBuffers();      // build/upload the per-world decal buffers + pins
		void DisposeDecalBuffers();   // defer-dispose the decal buffers + release pins

		// Fog + light buffers are dynamic: rebuilt + uploaded every frame by
		// SubmitToGpu and grown on demand (reserved = doubling capacity; count =
		// elements the last submit wrote, slot = index). Each is pinned to the defer
		// queue every frame, so a grow — or world teardown — drops the old buffer
		// safely once the GPU passes the frame; hence no Bake/Refresh/Dispose/dirty.
		RISharedPointer<RIBuffer> mpFogAreaBuffer;
		size_t fogAreaReserved = 0;
		uint32_t mFogAreaCount = 0;

		// Lifetime-stable per-type GPU light slot allocators (graphics/IndexPool,
		// bottom-up: lowest free slot first). A light requestIdLow()s a slot at
		// creation and keeps it for life (returnId() on destroy), so its packed GPU
		// id (packLightId(type, slot)) is identical every frame — the stable identity
		// ReSTIR DI temporal/spatial reuse needs. SubmitToGpu scatters each light into
		// buffer[slot]; the device buffer grows on demand to fit the high-water slot
		// (SyncStorageBuffer realloc). Because slots pack from 0, the high-water — and
		// so the per-cell grid-build loop — tracks the live light count, not the
		// reserve. The reserve is just the initial id space; AcquireLightSlot grow()s
		// the pool (and so the buffer) on exhaustion, so a light is never dropped.
		IndexPool mPointLightPool{256};
		IndexPool mSpotLightPool{256};
		IndexPool mAreaLightPool{64};

		// Per-type slot pool for `apLight`'s type, or nullptr for light types not
		// uploaded to the GPU (box lights). Centralizes the type→pool switch used by
		// Create*/Destroy/SubmitToGpu.
		IndexPool* GpuLightPoolFor(iLight* apLight);

		size_t pointLightReserved = 0;
		uint32_t mPointLightCount = 0;
		RISharedPointer<RIBuffer> mpPointLightBuffer;
		size_t spotLightReserved = 0;
		uint32_t mSpotLightCount = 0;
		RISharedPointer<RIBuffer> mpSpotLightBuffer;
		size_t areaLightReserved = 0;
		uint32_t mAreaLightCount = 0;
		RISharedPointer<RIBuffer> mpAreaLightBuffer;

		tBillboardList mlstBillboards;
		tBeamList mlstBeams;
		tParticleSystemList mlstParticleSystems;
		tGuiSetEntityList mlstGuiSetEntities;
		tRopeEntityList mlstRopeEntities;
		tSoundEntityList mlstSoundEntities;
		tStartPosEntityList mlstStartPosEntities;
		tAreaEntityMap m_mapAreaEntities;
		tFogAreaList mlstFogAreas;
		tDummyRenderableList mlstDummyRenderables;

		int mlSoundCreationIDCount;
		std::vector<SharedResourceHandle<cEntFile>> entityCache;

		tAINodeContainerList mlstAINodeContainers;
		tAStarHandlerList mlstAStarHandlers;
		tTempNodeContainerMap m_mapTempNodes;

		cNode3D* mpRootNode;

		tString msMapName;
		cColor mAmbientColor;
	};

};
#endif // HPL_WOLRD_H
