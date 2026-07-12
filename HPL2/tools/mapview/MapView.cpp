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

#include "hpl.h"
#include "graphics/DebugDraw.h"

#include "../../tests/Common/SimpleCamera.h"

using namespace hpl;

cSimpleCamera *gpSimpleCamera=NULL;

tString gsMapFile = "";

cConfigFile *gpConfig=NULL;

cPostEffectComposite *gpPostEffectComp=NULL;

float gfScatterLengthMinVal = 0.05f;
float gfScatterLengthMaxVal = 0.6f;

float gfScatterMinMinVal = 0.01f;
float gfScatterMinMaxVal = 0.08f;

float gfScatterMaxMinVal = 0.1f;
float gfScatterMaxMaxVal = 0.3f;

cAINodeContainer *gpNodeContainer=NULL;

//--------------------------------------------------------------------------------

bool gbDrawPhysicsDebug = false;
bool gbDrawOcclusionTextInfo = true;
bool gbDrawOcclusionGfxInfo = false;
bool gbDrawStaticOcclusionGfxInfo = true;
bool gbDrawSepperateObjects = false;
bool gbDrawContainerDebug = false;
bool gbDrawSoundDebug = true;
bool gbRenderSoundsPlaying= false;
bool gbRenderPathNodes=false;
bool gbDrawDynContainerDebug=false;

bool gbDrawParticles = true;
bool gbDrawBillboards = true;
bool gbDrawShadows = true;

tVector3fVec gvStartPostions;


//--------------------------------------------------------------------------------

tString gsNodeCont_Name="";
cVector3f gvNodeCont_BodySize = 0;
bool gbNodeCont_NodeAtCenter =false;
int glNodeCont_MinEdges = 0;
int glNodeCont_MaxEdges = 0;
float gfNodeCont_MaxEdgeDistance = 0;
float gfNodeCont_MaxHeight = 0;

//--------------------------------------------------------------------------------

class cSimpleObjectLoader : public cEntityLoader_Object
{
public:
	cSimpleObjectLoader(const tString &asName) : cEntityLoader_Object(asName)
	{
	}

	void BeforeLoad(cXmlElement *apRootElem, const cMatrixf &a_mtxTransform,cWorld *apWorld, cResourceVarsObject *apInstanceVars)
	{

	}
	void AfterLoad(cXmlElement *apRootElem, const cMatrixf &a_mtxTransform,cWorld *apWorld, cResourceVarsObject *apInstanceVars)
	{

	}
};

//--------------------------------------------------------------------------------

class cSimplePathNodeLoader : public iAreaLoader
{
public:
	cSimplePathNodeLoader(const tString& asName) : iAreaLoader(asName)
	{
		
	}

	void Load(const tString &asName, int alID, bool abActive, const cVector3f &avSize, const cMatrixf &a_mtxTransform,cWorld *apWorld)
	{
		apWorld->AddAINode(asName,alID, "Default", a_mtxTransform.GetTranslation()+cVector3f(0,0.05f,0));
	}
};


//--------------------------------------------------------------------------------

class cSimplePlayerStartArea : public iAreaLoader
{
public:
	cSimplePlayerStartArea(const tString& asName) : iAreaLoader(asName)
	{

	}

	void Load(const tString &asName, int alID, bool abActive, const cVector3f &avSize, const cMatrixf &a_mtxTransform,cWorld *apWorld)
	{
		gvStartPostions.push_back(a_mtxTransform.GetTranslation());
	}
};

//--------------------------------------------------------------------------------

bool BoxInsideBox(const cVector3f&avMinA, const cVector3f &avMaxA, const cVector3f&avMinB, const cVector3f &avMaxB)
{
	if(avMinA.x+0.001f < avMinB.x) return false;
	if(avMinA.y+0.001f < avMinB.y) return false;
	if(avMinA.z+0.001f < avMinB.z) return false;

	if(avMaxA.x-0.001f > avMaxB.x) return false;
	if(avMaxA.y-0.001f > avMaxB.y) return false;
	if(avMaxA.z-0.001f > avMaxB.z) return false;

	return true;
}

bool CheckEntityInsideBox(iEntity3D *apEntity, const cVector3f&avMin, const cVector3f &avMax)
{
	cBoundingVolume *pBV = apEntity->GetBoundingVolume();
	const cVector3f& vEntMin = pBV->GetMin();
	const cVector3f& vEntMax = pBV->GetMin();

	const float fOffset = 0.001f;

	if(vEntMin.x+fOffset < avMin.x) return false;
	if(vEntMin.y+fOffset < avMin.y) return false;
	if(vEntMin.z+fOffset< avMin.z) return false;

	if(vEntMax.x-fOffset > avMax.x) return false;
	if(vEntMax.y-fOffset > avMax.y) return false;
	if(vEntMax.z-fOffset > avMax.z) return false;

	return true;
}


//--------------------------------------------------------------------------------

class cBodyPicker : public iPhysicsRayCallback
{
public:
	cBodyPicker()
	{
		Clear();
	}

	void Clear()
	{
		mfLastT = 100000.0f;
		mpPickedBody = NULL;
	}

	bool OnIntersect(iPhysicsBody *pBody,cPhysicsRayParams *apParams)
	{
		//Log("Test body: '%s' mass: %f\n", pBody->GetName().c_str(), pBody->GetMass());

		if(pBody->GetMass()>0)
		{
			if(apParams->mfT < mfLastT)
			{
				mpPickedBody = pBody;
				mfLastT = apParams->mfT;
				mvPos = apParams->mvPoint;
				mfDist = apParams->mfDist;
			}
		}

		return true;
	}

	iPhysicsBody* mpPickedBody;
	cVector3f mvPos;
	cVector3f mvLocalPos;
	float mfLastT;
	float mfDist;
};

//--------------------------------------------------------------------------------


//--------------------------------------------------------------------------------

const int glObjectDebugColorNum = 21;
cColor gObjectDebugColor[glObjectDebugColorNum] = {	cColor(1,1,1),cColor(1,0,1),cColor(1,1,0),
													cColor(0,1,1),cColor(0,0,1),cColor(0,1,0),
													cColor(1,0,0),cColor(0.5f,0.5f,1),cColor(1,1,0.5f), 
													cColor(1,0.5f,0.5f), cColor(1,0.75f,0.25f),cColor(0.75f,1,0.25f),
													cColor(1,0.75f,1),cColor(1,0.25f,1),cColor(1,0.75f,0),
													cColor(0,1,0.75f),cColor(0.25f,0.25f,1),cColor(0.25f,1,0.25f),
													cColor(1,0.25f,0.25f),cColor(0.75f,0.75f,1),cColor(0.75f,1,0.25f) 
													};
// RI path: enqueue the overlay into the global DebugDraw batcher right
// before the renderer Draw — the Hybrid renderer never runs
// iRendererCallback messages. Legacy depth-test-off sections ride on
// DebugDrawOptions::m_depthTest = Always instead.
class cSimpleRenderCallback
{
public:
	cSimpleRenderCallback()
	{

	};

	cColor CalcDistColor(iRenderable *apObject)
	{
		cVector3f vPos = gpSimpleCamera->GetCamera()->GetPosition();

		float fDist = cMath::Vector3Dist(vPos, apObject->GetBoundingVolume()->GetWorldCenter());
		float fVal = 1.0f - (fDist / 30.0f);
		if(fVal <0.2f) fVal =0.2f;

		return cColor(fVal, 1);
	}

	void RenderDynamicSetDebug(DebugDraw *apDebugDraw, cRenderableSet *apSet)
	{
		for(iRenderable *pObject : apSet->GetObjects())
		{
			cBoundingVolume *pBV = pObject->GetBoundingVolume();
			apDebugDraw->DebugDrawBoxMinMax(pBV->GetMin(), pBV->GetMax(),cColor(0,1,0,1));
		}
	}

	void OnPreWorldDraw()
	{
		DebugDraw* pDebugDraw = Interface<cEngine>::Get()->GetGraphics()->GetDebugDraw();
		if(pDebugDraw==NULL) return;

		// Legacy drew most of this overlay depth-test-off (always on top).
		DebugDraw::DebugDrawOptions overlayOptions;
		overlayOptions.m_depthTest = DebugDraw::DebugDepthTest::Always;

		cRenderSettings *pSettings = gpSimpleCamera->GetViewport()->GetRenderSettings();
		cRenderList *pRenderList = pSettings->mpRenderList;
		
		/////////////////////////////////////
		//Draw outlines on objects, so you can see what mesh each belongs to (after having been combined)
		if(gbDrawSepperateObjects)
		{
			///////////////////////////////////
			// Iterate the static mesh any draw the
			int lCount = 0;
			cMeshEntityIterator meshIt = mpWorld->GetStaticMeshEntityIterator();
			while(meshIt.HasNext())
			{
				cMeshEntity *pEntity = meshIt.Next();

				for(int i=0; i<pEntity->GetSubMeshEntityNum(); ++i)
				{
					cSubMeshEntity *pSubEnt = pEntity->GetSubMeshEntity(i);
					//if(pSubEnt->GetName() != "CombinedObjects131_SubMesh") continue;

					int lColNum = lCount % glObjectDebugColorNum;
					const cColor &currentColor = gObjectDebugColor[lColNum];

					DebugDraw::DebugDrawOptions wireOptions = overlayOptions;
					cMatrixf* pModelMtx = pSubEnt->GetModelMatrix(NULL);
					if(pModelMtx) wireOptions.m_transform = *pModelMtx;
					pDebugDraw->DebugWireFrameFromVertexBuffer(pSubEnt->GetVertexBuffer(), currentColor, wireOptions);

					//cBoundingVolume *pBV = pObject->GetBoundingVolume();
					//pDebugDraw->DebugDrawBoxMinMax(pBV->GetMin(), pBV->GetMax(),...);
					lCount++;
				}
			}
		}

		/////////////////////////////////////////
		//Sounds
		if(gbRenderSoundsPlaying)
		{
			cSoundHandler *pSoundHandler = Interface<cEngine>::Get()->GetSound()->GetSoundHandler();

			//////////////////////////////
			//Sounds

			//Get names and entrys
			tSoundEntryList *pEntryList = pSoundHandler->GetEntryList();
			for(tSoundEntryListIt it = pEntryList->begin(); it != pEntryList->end();++it)
			{
				cSoundEntry *pEntry = *it;
				iSoundChannel *pSound = pEntry->GetChannel();

				if(pSound->Get3D() && pSound->GetPositionIsRelative() ==false)
				{
					pDebugDraw->DebugDrawSphere(pSound->GetPosition(), 0.1f, cColor(1,1,1,1), overlayOptions);

					pDebugDraw->DebugDrawSphere(pSound->GetPosition(), pSound->GetMinDistance(), cColor(0.75,1), overlayOptions);
					pDebugDraw->DebugDrawSphere(pSound->GetPosition(), pSound->GetMaxDistance(), cColor(0.5,1), overlayOptions);
				}
			}
		}

		/////////////////////////////////////////
		//Picked object
		if(mpBodyPicker->mpPickedBody)
		{
			//cBoundingVolume *pBV = mBodyPicker.mpPickedBody->GetBV();
			//pDebugDraw->DebugDrawBoxMinMax(pBV->GetMin(), pBV->GetMax(),cColor(1,0,1,1));

			pDebugDraw->DebugDrawSphere(mpBodyPicker->mvPos,0.1f, cColor(1,0,0,1));
			pDebugDraw->DebugDrawSphere(mvDragPos,0.1f, cColor(1,0,0,1));

			pDebugDraw->DebugDrawLine(mpBodyPicker->mvPos, mvDragPos, cColor(1,1,1,1));
		}

		/////////////////////////////////////////
		//Draws every dynamic object's AABB
		if(gbDrawDynContainerDebug)
		{
			RenderDynamicSetDebug(pDebugDraw, mpWorld->GetRenderableSet(eWorldContainerType_Dynamic));
		}

		/////////////////////////////////////////
		//Draws what is current rendered
		if(gbDrawOcclusionGfxInfo)
		{
			cFrustum *pFrustum = gpSimpleCamera->GetCamera()->GetFrustum();

			/////////////////////////
			//Solid objects (reverse order = back to front)
			int lNum = pRenderList->GetSolidObjectNum();
			for(int i=lNum-1; i>=0; --i)
			{
				iRenderable *pObject = pRenderList->GetSolidObject(i);
				if(gbDrawStaticOcclusionGfxInfo==false && pObject->IsStatic())continue;

				cColor col = CalcDistColor(pObject);//cColor(1,1);//gObjectDebugColor[(size_t)pObject % glObjectDebugColorNum];

				DebugDraw::DebugDrawOptions wireOptions = overlayOptions;
				cMatrixf* pModelMtx = pObject->GetModelMatrix(pFrustum);
				if(pModelMtx) wireOptions.m_transform = *pModelMtx;
				pDebugDraw->DebugWireFrameFromVertexBuffer(pObject->GetVertexBuffer(), col, wireOptions);
			}

			/////////////////////////
			//Trans (reverse order = back to front)
			lNum = pRenderList->GetTransObjectNum();
			for(int i=lNum-1; i>=0; --i)
			{
				iRenderable *pObject = pRenderList->GetTransObject(i);
				if(gbDrawStaticOcclusionGfxInfo==false && pObject->IsStatic())continue;

				cColor col = CalcDistColor(pObject);//cColor(1,1);//gObjectDebugColor[(size_t)pObject % glObjectDebugColorNum];

				DebugDraw::DebugDrawOptions wireOptions = overlayOptions;
				cMatrixf* pModelMtx = pObject->GetModelMatrix(pFrustum);
				if(pModelMtx) wireOptions.m_transform = *pModelMtx;
				pDebugDraw->DebugWireFrameFromVertexBuffer(pObject->GetVertexBuffer(), col, wireOptions);
			}
		}
		
		/////////////////////////////////////////
		//Draws physics debug
		if(gbDrawPhysicsDebug && mpWorld)
		{
			iPhysicsWorld *pPhysicsWorld = mpWorld->GetPhysicsWorld();

			cPhysicsBodyIterator bodyIt = pPhysicsWorld->GetBodyIterator();

			cCamera *pCam = gpSimpleCamera->GetCamera();
			
			while(bodyIt.HasNext())
			{
				iPhysicsBody *pBody = bodyIt.Next();
				if(pCam->GetFrustum()->CollideBoundingVolume(pBody->GetBoundingVolume())== eCollision_Outside) continue; //Frustum test for some extra speed!				

				cColor col = pBody->GetCollide() ? cColor(1,1) : cColor(1,0,1,1);

				pBody->RenderDebugGeometry(pDebugDraw,col);
			}
		}

		///////////////////////////////////////
		//Draw path nodes and connections
		if(gbRenderPathNodes && gpNodeContainer)
		{
			///////////////////////
			//Render all nodes
			for(int i=0; i<gpNodeContainer->GetNodeNum(); ++i)
			{
				cAINode *pNode = gpNodeContainer->GetNode(i);

				pDebugDraw->DebugDrawSphere(pNode->GetPosition(), 0.3f,cColor(0.4f,1));

				for(int j=0; j < pNode->GetEdgeNum(); ++j)
				{
					cAINodeEdge *pEdge = pNode->GetEdge(j);

					pDebugDraw->DebugDrawLine(pNode->GetPosition(),pEdge->mpNode->GetPosition(),cColor(0.4f,0.4f,0.4f,1));
				}
			}
		}

		/////////////////////////////////////////
		//Draws bounding boxes
   	}

	cWorld *mpWorld;
	cBodyPicker *mpBodyPicker;
	cVector3f mvDragPos;
};

//--------------------------------------------------------------------------------

class cSimpleUpdate : public iUpdateable
{
public:
	cSimpleUpdate() : iUpdateable("Simple3D")
	{
		////////////////////////////////
		// Rendering setup
		Interface<cEngine>::Get()->GetResources()->GetMaterialManager()->SetTextureFilter(eTextureFilter_Trilinear);
		Interface<cEngine>::Get()->GetResources()->GetMaterialManager()->SetTextureSizeDownScaleLevel(0);
		
		////////////////////////////////
		// Input setup.
		Interface<cEngine>::Get()->GetInput()->CreateAction("MouseMode")->AddKey(eKey_Space);
		Interface<cEngine>::Get()->GetInput()->CreateAction("ContainerBugCheck")->AddKey(eKey_C);
		Interface<cEngine>::Get()->GetInput()->CreateAction("RebuildDynamic")->AddKey(eKey_V);

		
		////////////////////////////////
		// Misc setup
		Interface<cEngine>::Get()->GetPhysics()->LoadSurfaceData("materials.cfg");

		////////////////////////////////
		// Setup load
		Interface<cEngine>::Get()->GetResources()->AddEntityLoader(hplNew(cSimpleObjectLoader,("Object")),true);
		Interface<cEngine>::Get()->GetResources()->AddAreaLoader(hplNew(cSimplePlayerStartArea,("PlayerStart")));
		Interface<cEngine>::Get()->GetResources()->AddAreaLoader(hplNew(cSimplePathNodeLoader,("PathNode")));


		////////////////////////////////
		// Create world
		// TODO: Load world here instead!
		mpWorld = NULL;
		if(gsMapFile != "")
		{
			LoadWorld(gsMapFile, false, false);
		}
		else
		{
			mpWorld = Interface<cEngine>::Get()->GetScene()->CreateWorld("Temp");
			mpPhysicsWorld = NULL;
			
			Image *pSkyBox = Interface<cEngine>::Get()->GetResources()->GetTextureManager()->CreateCubeMapImage("cubemap_evening.dds",false);
			if(pSkyBox)
			{
				mpWorld->SetSkyBox(pSkyBox,true);
				mpWorld->SetSkyBoxActive(true);
			}
		}
				
		////////////////////////////////
		// Render callback
		renderCallback.mpWorld = mpWorld;
		renderCallback.mpBodyPicker = &mBodyPicker;
			
		
		/////////////////////////////////
		// Init variables
		msCurrentFilePath = _W("");

		//Debug:
		//CheckStaticContainerBugs();
	}

	//--------------------------------------------------------------

	void SetupView()
	{
		/////////////////////////////////
		// Setup test window
		cVector2l vTestWindowSize(400,300);
		mpTestFrameBuffer = Interface<cEngine>::Get()->GetGraphics()->CreateFrameBuffer("");

		mpTestRenderTexture = Interface<cEngine>::Get()->GetGraphics()->CreateTexture("TestTarget",eTextureType_Rect,eTextureUsage_RenderTarget);
		mpTestRenderTexture->CreateFromRawData(cVector3l(vTestWindowSize.x, vTestWindowSize.y,1),ePixelFormat_RGBA, NULL);

		// cGui only builds elements from Image* now and there is no bridge from
		// the legacy iTexture render target (the only DrawGfx of this element is
		// commented out below anyway).
		mpTestRenderGfx = NULL;

		iDepthStencilBuffer *pDepthBuffer = Interface<cEngine>::Get()->GetGraphics()->CreateDepthStencilBuffer(vTestWindowSize,24,8,false);
		
		mpTestFrameBuffer->SetDepthStencilBuffer(pDepthBuffer);
		mpTestFrameBuffer->SetTexture2D(0,mpTestRenderTexture);

		mpTestWorld = Interface<cEngine>::Get()->GetScene()->CreateWorld("Test");

		mpTestViewPort = Interface<cEngine>::Get()->GetScene()->CreateViewport(gpSimpleCamera->GetCamera(),mpTestWorld);
		mpTestViewPort->SetRenderer(Interface<cEngine>::Get()->GetGraphics()->GetRenderer(eRenderer_WireFrame));
		

		//mpTestViewPort->SetVisible(gbDrawOcclusionGfxInfo);
		//mpTestViewPort->SetActive(gbDrawOcclusionGfxInfo);

		mpTestViewPort->SetVisible(false);
		mpTestViewPort->SetActive(false);

		mpTestViewPort->GetRenderSettings()->mClearColor = cColor(0.3f,1);

		/////////////////////////////////
		//Set up normal view port camera
		cRenderSettings *pSettings = gpSimpleCamera->GetViewport()->GetRenderSettings();

		mPreWorldDrawHandler = EventHandler<const WorldDrawCtx&>([this](const WorldDrawCtx&){ renderCallback.OnPreWorldDraw(); });
		mPreWorldDrawHandler.Connect(gpSimpleCamera->GetViewport()->OnPreWorldDraw());

		gpSimpleCamera->SetMouseMode(true);

		//pSettings->mbLog = true;

		if(gvStartPostions.size() > 0)
			gpSimpleCamera->GetCamera()->SetPosition(gvStartPostions[0]);

		//////////////////////////
		//Set up post effects
		cGraphics *pGraphics = Interface<cEngine>::Get()->GetGraphics();
		gpPostEffectComp = pGraphics->CreatePostEffectComposite();
		gpSimpleCamera->GetViewport()->SetPostEffectComposite(gpPostEffectComp);

		//Bloom
		cPostEffectParams_Bloom bloomParams;
		gpPostEffectComp->AddPostEffect(pGraphics->CreatePostEffect(&bloomParams), 20);

		//Image trail
		/*cPostEffectParams_ImageTrail imageTrailParams;
		iPostEffect *pImageTrail = pGraphics->CreatePostEffect(&imageTrailParams);
		gpPostEffectComp->AddPostEffect(pImageTrail);
		pImageTrail->SetActive(true);*/


		CreateGuiWindow();


		//Debug Cam:
		gpSimpleCamera->SetMouseMode(false);

		//Start of ch1 main chamber:
		//gpSimpleCamera->GetCamera()->SetPosition(cVector3f(-1.371983, 2.157333, 8.728578));
		//gpSimpleCamera->GetCamera()->SetPitch(-0.013281);
		//gpSimpleCamera->GetCamera()->SetYaw(-1.613672);

		//Level 01 overview screen.
		//gpSimpleCamera->GetCamera()->SetPosition(cVector3f(7.121649, 0.488335, 30.064392));
		//gpSimpleCamera->GetCamera()->SetPitch(-0.053125);
		//gpSimpleCamera->GetCamera()->SetYaw(-7.040731);
	
		//Shadow disappears
		//gpSimpleCamera->GetCamera()->SetPosition(cVector3f(-56.677223, -0.390131, -33.132439));
		//gpSimpleCamera->GetCamera()->SetPitch(-0.265625);
		//gpSimpleCamera->GetCamera()->SetYaw(0.011622);

		
		//Level02 view of chamber
		//gpSimpleCamera->GetCamera()->SetPosition(cVector3f(22.688457, 2.444717, 24.260900));
		//gpSimpleCamera->GetCamera()->SetPitch(0.132813);
		//gpSimpleCamera->GetCamera()->SetYaw(-7.054004);

		//Level03 inside room and over map (>1800 without culling!)
		//gpSimpleCamera->GetCamera()->SetPosition(cVector3f(38.557411, 2.562200, 0.296503));
		//gpSimpleCamera->GetCamera()->SetPitch(-0.126171);
		//gpSimpleCamera->GetCamera()->SetYaw(-8.463478);

		//Good view in plane test
		//gpSimpleCamera->GetCamera()->SetPosition(cVector3f(0.705097, 4.125532, 5.037270));
		//gpSimpleCamera->GetCamera()->SetPitch(-0.524610);
		//gpSimpleCamera->GetCamera()->SetYaw(-0.199219);

		//Show test in level 21
		//gpSimpleCamera->GetCamera()->SetPosition(cVector3f(3.017026, 2.640739, 25.054111));
		//gpSimpleCamera->GetCamera()->SetPitch(-0.374090);
		//gpSimpleCamera->GetCamera()->SetYaw(0.237402);

		//Level 02 perf test view
		//gpSimpleCamera->GetCamera()->SetPosition(cVector3f(23.083452, 3.477001, 23.002367));
		//gpSimpleCamera->GetCamera()->SetPitch(0.073047);
		//gpSimpleCamera->GetCamera()->SetYaw(-6.904590);

		//Level 22/26 perf test view
		//gpSimpleCamera->GetCamera()->SetPosition(cVector3f(0.123177, 4.492890, 29.673765));
		//gpSimpleCamera->GetCamera()->SetPitch(0.035417);
		//gpSimpleCamera->GetCamera()->SetYaw(-6.328499);

		//Level 18 perf test view
		//gpSimpleCamera->GetCamera()->SetPosition(cVector3f(5.184788, 4.735771, 28.755508));
		//gpSimpleCamera->GetCamera()->SetPitch(-0.245703);
		//gpSimpleCamera->GetCamera()->SetYaw(-4.110550);

		//Nemesis test
		//gpSimpleCamera->GetCamera()->SetPosition(cVector3f(3.148003, 1.418888, 2.518114));
		//gpSimpleCamera->GetCamera()->SetPitch(-0.035417);
		//gpSimpleCamera->GetCamera()->SetYaw(-1.542286);

		
		gpSimpleCamera->SetMouseMode(true);
		Interface<cEngine>::Get()->GetInput()->GetLowLevel()->LockInput(false);
		Interface<cEngine>::Get()->GetInput()->GetLowLevel()->RelativeMouse(false);
	}

	//--------------------------------------------------------------

	void OnExit()
	{
	
	}

	//--------------------------------------------------------------

	void LoadWorld(const tString& asFileName, bool abSetToViewport, bool abKeepPosition)
	{
		if(asFileName == "") return;

		if(mpWorld)
		{ 
			Interface<cEngine>::Get()->GetScene()->DestroyWorld(mpWorld);
			mpWorld = NULL;
			mpPhysicsWorld = NULL;
			gvStartPostions.clear();

			gpNodeContainer=NULL;
		}

		//////////////////////////////////
		//Load the world
		tString sExt = cString::ToLowerCase(cString::GetFileExt(asFileName));

		//Editor level
		if(sExt=="map")
		{
			Interface<cEngine>::Get()->GetResources()->GetMeshManager()->SetFastloadMaterial("material_floor.mat");

			tWorldLoadFlag lFlags = 0;
			//lFlags |= eWorldLoadFlag_FastPhysicsLoad;
			//lFlags |= eWorldLoadFlag_FastStaticLoad;
			//lFlags |= eWorldLoadFlag_FastEntityLoad;
			//lFlags |= eWorldLoadFlag_NoGameEntities;

			mpWorld = Interface<cEngine>::Get()->GetScene()->LoadWorld(asFileName, lFlags);
			if(mpWorld == NULL)FatalError("Could not load world '%s'\n", asFileName.c_str());
		}
		//Dae level (non proper)
		else if(sExt=="dae")
		{
			mpWorld = Interface<cEngine>::Get()->GetScene()->CreateWorld(asFileName);

			cMesh *pMesh = Interface<cEngine>::Get()->GetResources()->GetMeshManager()->CreateMesh(asFileName);
			if(pMesh==NULL)FatalError("Could not load world '%s'\n", asFileName.c_str());
			
			cMeshEntity* pEntity = mpWorld->CreateMeshEntity("Level",pMesh,true);

			pEntity->SetRenderFlagBit(eRenderableFlag_ShadowCaster, true);

			mpWorld->Compile(true);
		}
		else
		{
			FatalError("Invalid map format!\n");
		}
		
		mpPhysicsWorld = mpWorld->GetPhysicsWorld();
		
		//////////////////////////////////
		//Node container
       	gpNodeContainer = mpWorld->CreateAINodeContainer( gsNodeCont_Name, "Default", gvNodeCont_BodySize, gbNodeCont_NodeAtCenter,
																glNodeCont_MinEdges , glNodeCont_MaxEdges , 
																gfNodeCont_MaxEdgeDistance, gfNodeCont_MaxHeight);


		//////////////////////////////////
		//Set to viewport
		if(abSetToViewport)
		{
			gpSimpleCamera->GetViewport()->SetWorld(mpWorld);
			renderCallback.mpWorld = mpWorld;


			if(abKeepPosition==false && gvStartPostions.size() > 0)
				gpSimpleCamera->GetCamera()->SetPosition(gvStartPostions[0]);

			//Force  a clearing of render list
			cRenderList *pRenderList = gpSimpleCamera->GetViewport()->GetRenderSettings()->mpRenderList;
			pRenderList->Clear();
		}

		//////////////////////////////////
		//Add a skybox now for fun!
		Image *pSkyBox = Interface<cEngine>::Get()->GetResources()->GetTextureManager()->CreateCubeMapImage("cubemap_evening.dds",false);
		if(pSkyBox)
		{
			mpWorld->SetSkyBox(pSkyBox,true);
			mpWorld->SetSkyBoxActive(true);
		}
		
		//////////////////////////////////
		//Debug output
		return;
		Log("--- Static bodies -----\n");
		cPhysicsBodyIterator bodyIt = mpPhysicsWorld->GetBodyIterator();
		while(bodyIt.HasNext())
		{
			iPhysicsBody *pBody = bodyIt.Next();
			if(pBody->GetMass()!=0) continue;

			Log(" '%s' Material: '%s'\n", pBody->GetName().c_str(), pBody->GetMaterial()->GetName().c_str());
		}
		
		
		return;
	}

	//--------------------------------------------------------------

	void PrintDynamicSetContents()
	{
		Log("---------- BEGIN DYNAMIC SET OUTPUT ---------------\n");
		cRenderableSet* pSet = mpWorld->GetRenderableSet(eWorldContainerType_Dynamic);

		pSet->UpdateBeforeRendering();
		for(iRenderable *pObject : pSet->GetObjects())
		{
			cBoundingVolume *pBV = pObject->GetBoundingVolume();
			Log(" %s (%s) AABB: (%s)-(%s)\n", pObject->GetName().c_str(), pObject->GetEntityType().c_str(),
												pBV->GetMin().ToString().c_str(), pBV->GetMax().ToString().c_str());
		}

		Log("---------- STOP DYNAMIC SET OUTPUT ---------------\n");
	}

	//--------------------------------------------------------------

	void InitGuiAfterLoad()
	{	
	}

	//--------------------------------------------------------------

	void SetPhysicsActive(bool abX)
	{
		//TODO?
	}

	//--------------------------------------------------------------

	void CreateGuiWindow()
	{
		cWidgetCheckBox *pCheckBox=NULL;
		cWidgetButton *pButton = NULL;
		cWidgetComboBox *pComboBox = NULL;
		cWidgetLabel *pLabel = NULL;
		cWidgetGroup *pGroup = NULL;
		cWidgetSlider *pSlider = NULL;

		cGuiSet *pSet = gpSimpleCamera->GetSet();

		cRenderSettings *pSettings = gpSimpleCamera->GetViewport()->GetRenderSettings();
		

		cVector3f vGroupPos =0;
		cVector2f vGroupSize =0;

		///////////////////////////
		//Window
		cVector2f vSize = cVector2f(250, 740);
		vGroupSize.x = vSize.x - 20;
		cVector3f vPos = cVector3f(pSet->GetVirtualSize().x - vSize.x - 10, 10, 0);
		mpOptionWindow = pSet->CreateWidgetWindow(0,vPos,vSize,_W("ModelView Toolbar") );

		vSize = cVector2f(vSize.x-30, 18);
		vPos = cVector3f(10, 30, 0.1f);

		///////////////////////////
		// Loading
		{
			//Group
			vGroupPos = cVector3f(5,10,0.1f);
			pGroup = pSet->CreateWidgetGroup(vPos,100,_W("Loading"),mpOptionWindow);

			//Load File
			pButton = pSet->CreateWidgetButton(vGroupPos,vSize,_W("Load Scene"),pGroup);
			pButton->AddCallback(eGuiMessage_ButtonPressed,this, kGuiCallback(PressLoadWorld));
			vGroupPos.y += 22;

			//Reload File
			pButton = pSet->CreateWidgetButton(vGroupPos,vSize,_W("Reload"),pGroup);
			pButton->AddCallback(eGuiMessage_ButtonPressed,this, kGuiCallback(PressReload));
			vGroupPos.y += 22;

			//Group end
			vGroupSize.y = vGroupPos.y + 15;
			pGroup->SetSize(vGroupSize);
			vPos.y += vGroupSize.y + 15;
		}

		///////////////////////////
		// Debug
		{
			//Group
			vGroupPos = cVector3f(5,10,0.1f);
			pGroup = pSet->CreateWidgetGroup(vPos,100,_W("Debug"),mpOptionWindow);

			//Occlusion Text info
			pCheckBox = pSet->CreateWidgetCheckBox(vGroupPos,vSize,_W("Show occlusion debug text info"),pGroup);
			pCheckBox->SetChecked(gbDrawOcclusionTextInfo);
			pCheckBox->AddCallback(eGuiMessage_CheckChange,this, kGuiCallback(ChangeOcclusionTextInfo));
			vGroupPos.y += 22;

			//Occlusion Gfx info
			pCheckBox = pSet->CreateWidgetCheckBox(vGroupPos,vSize,_W("Draw occlusion debug info"),pGroup);
			pCheckBox->SetChecked(gbDrawOcclusionGfxInfo);
			pCheckBox->AddCallback(eGuiMessage_CheckChange,this, kGuiCallback(ChangeOcclusionGfxInfo));
			vGroupPos.y += 22;


			//Static Occlusion Gfx info
			pCheckBox = pSet->CreateWidgetCheckBox(vGroupPos,vSize,_W("Show static debug info"),pGroup);
			pCheckBox->SetChecked(gbDrawStaticOcclusionGfxInfo);
			pCheckBox->AddCallback(eGuiMessage_CheckChange,this, kGuiCallback(ChangeStaticOcclusionGfxInfo));
			vGroupPos.y += 22;

			//Static Occlusion Gfx info
			pCheckBox = pSet->CreateWidgetCheckBox(vGroupPos,vSize,_W("Draw dyn container errors"),pGroup);
			pCheckBox->SetChecked(gbDrawDynContainerDebug);
			pCheckBox->AddCallback(eGuiMessage_CheckChange,this, kGuiCallback(ChangeDynamicContainerDebug));
			vGroupPos.y += 22;
			


			
			//Container debug
			pCheckBox = pSet->CreateWidgetCheckBox(vGroupPos,vSize,_W("Draw container info"),pGroup);
			pCheckBox->SetChecked(gbDrawContainerDebug);
			pCheckBox->AddCallback(eGuiMessage_CheckChange,this, kGuiCallback(ChangeContainerInfo));
			vGroupPos.y += 22;

			//Combined objects debug
			pCheckBox = pSet->CreateWidgetCheckBox(vGroupPos,vSize,_W("Draw combined object outlines"),pGroup);
			pCheckBox->SetChecked(gbDrawSepperateObjects);
			pCheckBox->AddCallback(eGuiMessage_CheckChange,this, kGuiCallback(ChangeCombinedObjectInfo));
			vGroupPos.y += 22;

			//Draw physics debug
			pCheckBox = pSet->CreateWidgetCheckBox(vGroupPos,vSize,_W("Draw physics info"),pGroup);
			pCheckBox->SetChecked(gbDrawPhysicsDebug);
			pCheckBox->AddCallback(eGuiMessage_CheckChange,this, kGuiCallback(ChangePhysicsDebugInfo));
			vGroupPos.y += 22;

			//Draw sound debug
			pCheckBox = pSet->CreateWidgetCheckBox(vGroupPos,vSize,_W("Show sound debug"),pGroup);
			pCheckBox->SetChecked(gbDrawSoundDebug);
			pCheckBox->AddCallback(eGuiMessage_CheckChange,this, kGuiCallback(ChangeSoundDebugInfo));
			vGroupPos.y += 22;

			//Render sound debug
			pCheckBox = pSet->CreateWidgetCheckBox(vGroupPos,vSize,_W("Render sounds"),pGroup);
			pCheckBox->SetChecked(gbRenderSoundsPlaying);
			pCheckBox->AddCallback(eGuiMessage_CheckChange,this, kGuiCallback(ChangeSoundRender));
			vGroupPos.y += 22;

			//Render sound debug
			pCheckBox = pSet->CreateWidgetCheckBox(vGroupPos,vSize,_W("Render path nodes"),pGroup);
			pCheckBox->SetChecked(gbRenderPathNodes);
			pCheckBox->AddCallback(eGuiMessage_CheckChange,this, kGuiCallback(ChangeRenderPathNodes));
			vGroupPos.y += 22;
			

			//Group end
			vGroupSize.y = vGroupPos.y + 15;
			pGroup->SetSize(vGroupSize);
			vPos.y += vGroupSize.y + 15;
		}

		///////////////////////////
		// Graphics
		{
			//Group
			vGroupPos = cVector3f(5,10,0.1f);
			pGroup = pSet->CreateWidgetGroup(vPos,100,_W("Graphics"),mpOptionWindow);
			
			//SSAO
			pCheckBox = pSet->CreateWidgetCheckBox(vGroupPos,vSize,_W("SSAO Active"),pGroup);
			pCheckBox->SetChecked(pSettings->mbSSAOActive);
			pCheckBox->AddCallback(eGuiMessage_CheckChange,this, kGuiCallback(ChangeSSAOActive));
			vGroupPos.y += 22;

			//Bloom
			pCheckBox = pSet->CreateWidgetCheckBox(vGroupPos,vSize,_W("Bloom Active"),pGroup);
			pCheckBox->SetChecked(gpPostEffectComp->GetPostEffect(0)->IsActive());
			pCheckBox->AddCallback(eGuiMessage_CheckChange,this, kGuiCallback(ChangeBloomActive));
			vGroupPos.y += 22;

			//Ambient active
			pCheckBox = pSet->CreateWidgetCheckBox(vGroupPos,vSize,_W("Ambient Active"),pGroup);
			pCheckBox->AddCallback(eGuiMessage_CheckChange,this, kGuiCallback(ChangeAmbientActive));
			mpCheckAmbientActive = pCheckBox;
			vGroupPos.y += 22;

			//Ambient intensity
			cVector3f vAmbSlidePos = vGroupPos; vAmbSlidePos.x += 55;
			cVector2f vAmbSlideSize = vSize; vAmbSlideSize.x -= 55;

			pSlider = pSet->CreateWidgetSlider(eWidgetSliderOrientation_Horizontal,vAmbSlidePos,vAmbSlideSize,255,pGroup);
			pSlider->AddCallback(eGuiMessage_SliderMove,this, kGuiCallback(ChangeAmbientColor));
			pLabel = pSet->CreateWidgetLabel(vGroupPos, vSize, _W("Intensity:"), pGroup);
			mpSliderAmbientColor = pSlider;

			vGroupPos.y += 22;

			//Draw particles
			pCheckBox = pSet->CreateWidgetCheckBox(vGroupPos,vSize,_W("Draw Particles"),pGroup);
			pCheckBox->SetChecked(gbDrawParticles);
			pCheckBox->AddCallback(eGuiMessage_CheckChange,this, kGuiCallback(ChangeDrawParticles));
			vGroupPos.y += 22;

			//Draw billboards
			pCheckBox = pSet->CreateWidgetCheckBox(vGroupPos,vSize,_W("Draw Billboards"),pGroup);
			pCheckBox->SetChecked(gbDrawBillboards);
			pCheckBox->AddCallback(eGuiMessage_CheckChange,this, kGuiCallback(ChangeDrawBillboards));
			vGroupPos.y += 22;

			//Draw shadows
			pCheckBox = pSet->CreateWidgetCheckBox(vGroupPos,vSize,_W("Draw Shadows"),pGroup);
			pCheckBox->SetChecked(gbDrawShadows);
			pCheckBox->AddCallback(eGuiMessage_CheckChange,this, kGuiCallback(ChangeDrawShadows));
			vGroupPos.y += 22;

			
			/////////////////////////////
			//SSAO settings
			cVector2f vLabelSize = vSize; vLabelSize.x *=0.5f;

			//SSAO scatter length
			/*{
				pLabel = pSet->CreateWidgetLabel(vGroupPos, vLabelSize, _W("Scatter Length:"), pGroup);
				pLabel = pSet->CreateWidgetLabel(vGroupPos+cVector3f(vLabelSize.x,0,0), vLabelSize, 
												cString::ToStringW(fVal,4), pGroup);
				mpLabelScatterLength = pLabel;

				vGroupPos.y += 18;

				pSlider = pSet->CreateWidgetSlider(eWidgetSliderOrientation_Horisontal,vGroupPos,vSize,200,pGroup);
				pSlider->AddCallback(eGuiMessage_SliderMove,this, kGuiCallback(ChangeScatterLength));
				pSlider->SetValue(  (int)((fVal-gfScatterLengthMinVal) / (gfScatterLengthMaxVal-)*200.0f)  );
				
				vGroupPos.y += 22;
			}


			//SSAO scatter min
			{
				pLabel = pSet->CreateWidgetLabel(vGroupPos, vLabelSize, _W("Scatter Min:"), pGroup);
				pLabel = pSet->CreateWidgetLabel(vGroupPos+cVector3f(vLabelSize.x,0,0), vLabelSize, 
					cString::ToStringW(fVal,4), pGroup);
				mpLabelScatterMin = pLabel;

				vGroupPos.y += 18;

				pSlider = pSet->CreateWidgetSlider(eWidgetSliderOrientation_Horisontal,vGroupPos,vSize,200,pGroup);
				pSlider->AddCallback(eGuiMessage_SliderMove,this, kGuiCallback(ChangeScatterMin));
				pSlider->SetValue(  (int)((fVal-gfScatterMinMinVal) / (gfScatterMinMaxVal-gfScatterMinMinVal)*200.0f)  );

				vGroupPos.y += 22;
			}

			//SSAO scatter max
			{
				pLabel = pSet->CreateWidgetLabel(vGroupPos, vLabelSize, _W("Scatter Max:"), pGroup);
				pLabel = pSet->CreateWidgetLabel(vGroupPos+cVector3f(vLabelSize.x,0,0), vLabelSize, 
					cString::ToStringW(fVal,4), pGroup);
				mpLabelScatterMax = pLabel;

				vGroupPos.y += 18;

				pSlider = pSet->CreateWidgetSlider(eWidgetSliderOrientation_Horisontal,vGroupPos,vSize,200,pGroup);
				pSlider->AddCallback(eGuiMessage_SliderMove,this, kGuiCallback(ChangeScatterMax));
				pSlider->SetValue(  (int)((fVal-gfScatterMaxMinVal) / (gfScatterMaxMaxVal-gfScatterMaxMinVal)*200.0f)  );

				vGroupPos.y += 22;
			}*/


			//Group end
			vGroupSize.y = vGroupPos.y + 15;
			pGroup->SetSize(vGroupSize);
			vPos.y += vGroupSize.y + 15;
		}

		//////////////////////////
		//Graphics
		{
			//Group
			vGroupPos = cVector3f(5,10,0.1f);
			pGroup = pSet->CreateWidgetGroup(vPos,100,_W("Graphics"),mpOptionWindow);

			//Occlusion test
			pCheckBox = pSet->CreateWidgetCheckBox(vGroupPos,vSize,_W("Occlusion test"),pGroup);
			pCheckBox->SetChecked(true);
			pCheckBox->AddCallback(eGuiMessage_CheckChange,this, kGuiCallback(ChangeOcclusionTest));
			vGroupPos.y += 22;

			//Reflect world
			pCheckBox = pSet->CreateWidgetCheckBox(vGroupPos,vSize,_W("Reflect World"),pGroup);
			pCheckBox->SetChecked(true);
			pCheckBox->AddCallback(eGuiMessage_CheckChange,this, kGuiCallback(ChangeReflectWorld));
			vGroupPos.y += 22;

			//Large light test
			pCheckBox = pSet->CreateWidgetCheckBox(vGroupPos,vSize,_W("Large light test"),pGroup);
			pCheckBox->SetChecked(false);
			pCheckBox->AddCallback(eGuiMessage_CheckChange,this, kGuiCallback(ChangeLargeLightTest));
			vGroupPos.y += 22;

			
			//Group end
			vGroupSize.y = vGroupPos.y + 15;
			pGroup->SetSize(vGroupSize);
			vPos.y += vGroupSize.y + 15;
		}

		//////////////////////////
		//Physics
		{
			
		}




		InitGuiAfterLoad();
	}
	
	//--------------------------------------------------------------

	bool PressLoadWorld(iWidget* apWidget,const cGuiMessageData& aData)
	{
		mvPickedFiles.clear();
		cGuiSet *pSet = gpSimpleCamera->GetSet();
		cGuiPopUpFilePicker* pPicker = pSet->CreatePopUpLoadFilePicker(mvPickedFiles,false,msCurrentFilePath,false, this, kGuiCallback(LoadWorldFromFilePicker));
		pPicker->AddCategory(_W("Scenes"),_W("*.map"));
		pPicker->AddFilter(0, _W("*.dae"));
		
		//pPicker->SetSaveFileDest(_W("maps"));

		return true;
	}
	kGuiCallbackFuncEnd(cSimpleUpdate,PressLoadWorld)  

	bool PressReload(iWidget* apWidget,const cGuiMessageData& aData)
	{
		LoadWorld(gsMapFile, true, true);
		InitGuiAfterLoad();

		return true;
	}
	kGuiCallbackFuncEnd(cSimpleUpdate,PressReload)  

	bool LoadWorldFromFilePicker(iWidget* apWidget,const cGuiMessageData& aData)
	{
		if(mvPickedFiles.empty()) return true;

		tWString& sFilePath = mvPickedFiles[0];

		msCurrentFilePath = cString::GetFilePathW(sFilePath);
		Interface<cEngine>::Get()->GetResources()->AddResourceDir(msCurrentFilePath,false);

		gsMapFile = cString::To8Char(cString::GetFileNameW(sFilePath));
		LoadWorld(gsMapFile, true, false);  
		InitGuiAfterLoad();

		return true;		
	}
	kGuiCallbackFuncEnd(cSimpleUpdate,LoadWorldFromFilePicker)  

	//--------------------------------------------------------------

	bool ChangeOcclusionTextInfo(iWidget* apWidget,const cGuiMessageData& aData)
	{
		gbDrawOcclusionTextInfo = aData.mlVal == 1;	
		return true;
	}
	kGuiCallbackFuncEnd(cSimpleUpdate,ChangeOcclusionTextInfo)  

	bool ChangeOcclusionGfxInfo(iWidget* apWidget,const cGuiMessageData& aData)
	{
		gbDrawOcclusionGfxInfo = aData.mlVal == 1;	

		//mpTestViewPort->SetVisible(gbDrawOcclusionGfxInfo);
		//mpTestViewPort->SetActive(gbDrawOcclusionGfxInfo);

		return true;
	}
	kGuiCallbackFuncEnd(cSimpleUpdate,ChangeOcclusionGfxInfo)

	bool ChangeStaticOcclusionGfxInfo(iWidget* apWidget,const cGuiMessageData& aData)
	{
		gbDrawStaticOcclusionGfxInfo = aData.mlVal == 1;	
		return true;
	}
	kGuiCallbackFuncEnd(cSimpleUpdate,ChangeStaticOcclusionGfxInfo)

	bool ChangeDynamicContainerDebug(iWidget* apWidget,const cGuiMessageData& aData)
	{
		gbDrawDynContainerDebug = aData.mlVal == 1;	
		return true;
	}
	kGuiCallbackFuncEnd(cSimpleUpdate,ChangeDynamicContainerDebug)

	
	bool ChangeContainerInfo(iWidget* apWidget,const cGuiMessageData& aData)
	{
		gbDrawContainerDebug = aData.mlVal == 1;
		return true;
	}
	kGuiCallbackFuncEnd(cSimpleUpdate,ChangeContainerInfo)

	bool ChangeCombinedObjectInfo(iWidget* apWidget,const cGuiMessageData& aData)
	{
		gbDrawSepperateObjects = aData.mlVal == 1;
		return true;
	}
	kGuiCallbackFuncEnd(cSimpleUpdate,ChangeCombinedObjectInfo)

		
	bool ChangePhysicsDebugInfo(iWidget* apWidget,const cGuiMessageData& aData)
	{
		gbDrawPhysicsDebug = aData.mlVal == 1;
		return true;
	}
	kGuiCallbackFuncEnd(cSimpleUpdate,ChangePhysicsDebugInfo)

	bool ChangeSoundDebugInfo(iWidget* apWidget,const cGuiMessageData& aData)
	{
		gbDrawSoundDebug = aData.mlVal == 1;
		return true;
	}
	kGuiCallbackFuncEnd(cSimpleUpdate,ChangeSoundDebugInfo)

	bool ChangeSoundRender(iWidget* apWidget,const cGuiMessageData& aData)
	{
		gbRenderSoundsPlaying = aData.mlVal == 1;
		return true;
	}
	kGuiCallbackFuncEnd(cSimpleUpdate,ChangeSoundRender)

	bool ChangeRenderPathNodes(iWidget* apWidget,const cGuiMessageData& aData)
	{
		gbRenderPathNodes = aData.mlVal == 1;
		return true;
	}
	kGuiCallbackFuncEnd(cSimpleUpdate,ChangeRenderPathNodes)
	
	//--------------------------------------------------------------
	
	bool ChangeSSAOActive(iWidget* apWidget,const cGuiMessageData& aData)
	{
		cRenderSettings *pSettings = gpSimpleCamera->GetViewport()->GetRenderSettings();

        pSettings->mbSSAOActive = aData.mlVal == 1;

		return true;
	}
	kGuiCallbackFuncEnd(cSimpleUpdate,ChangeSSAOActive)

	bool ChangeBloomActive(iWidget* apWidget,const cGuiMessageData& aData)
	{
		gpPostEffectComp->GetPostEffect(0)->SetActive(aData.mlVal == 1);

		return true;
	}
	kGuiCallbackFuncEnd(cSimpleUpdate,ChangeBloomActive)
	

	bool ChangeAmbientActive(iWidget* apWidget,const cGuiMessageData& aData)
	{
		return true;
	}
	kGuiCallbackFuncEnd(cSimpleUpdate,ChangeAmbientActive)
		
	bool ChangeAmbientColor(iWidget* apWidget,const cGuiMessageData& aData)
	{
		return true;
	}
	kGuiCallbackFuncEnd(cSimpleUpdate,ChangeAmbientColor)  


	bool ChangeScatterLength(iWidget* apWidget,const cGuiMessageData& aData)
	{
		float fVal = ((float)aData.mlVal)/200.0f * gfScatterLengthMaxVal + gfScatterLengthMinVal;
		
		mpLabelScatterLength->SetText(cString::ToStringW(fVal,4));

		return true;
	}
	kGuiCallbackFuncEnd(cSimpleUpdate,ChangeScatterLength)  


	bool ChangeScatterMin(iWidget* apWidget,const cGuiMessageData& aData)
	{
		float fVal = ((float)aData.mlVal)/200.0f * gfScatterMinMaxVal + gfScatterMinMinVal;

		mpLabelScatterMin->SetText(cString::ToStringW(fVal,4));

		return true;
	}
	kGuiCallbackFuncEnd(cSimpleUpdate,ChangeScatterMin)  

	bool ChangeScatterMax(iWidget* apWidget,const cGuiMessageData& aData)
	{
		float fVal = ((float)aData.mlVal)/200.0f * gfScatterMaxMaxVal + gfScatterMaxMinVal;

		mpLabelScatterMax->SetText(cString::ToStringW(fVal,4));

		return true;
	}
	kGuiCallbackFuncEnd(cSimpleUpdate,ChangeScatterMax)  
  

	bool ChangeDrawParticles(iWidget* apWidget,const cGuiMessageData& aData)
	{
		gbDrawParticles = aData.mlVal == 1;

		cParticleSystemIterator it = mpWorld->GetParticleSystemIterator();
		while(it.HasNext())
		{
			cParticleSystem *pPS = it.Next();

            pPS->SetActive(gbDrawParticles);
			pPS->SetVisible(gbDrawParticles);
		}

		return true;
	}
	kGuiCallbackFuncEnd(cSimpleUpdate,ChangeDrawParticles)

	bool ChangeDrawBillboards(iWidget* apWidget,const cGuiMessageData& aData)
	{
		gbDrawBillboards = aData.mlVal == 1;

		cBillboardIterator it = mpWorld->GetBillboardIterator();
		while(it.HasNext())
		{
			cBillboard *pBB = it.Next();

			pBB->SetActive(gbDrawBillboards);
			pBB->SetVisible(gbDrawBillboards);
		}

		return true;
	}
	kGuiCallbackFuncEnd(cSimpleUpdate,ChangeDrawBillboards)


	bool ChangeDrawShadows(iWidget* apWidget,const cGuiMessageData& aData)
	{
		gbDrawShadows = aData.mlVal == 1;

		gpSimpleCamera->GetViewport()->GetRenderSettings()->mbRenderShadows = gbDrawShadows;

		return true;
	}
	kGuiCallbackFuncEnd(cSimpleUpdate,ChangeDrawShadows)

		

	//--------------------------------------------------------------

	bool ChangeOcclusionTest(iWidget* apWidget,const cGuiMessageData& aData)
	{
		gpSimpleCamera->GetViewport()->GetRenderSettings()->mbUseOcclusionCulling = aData.mlVal==1 ? true : false;
		return true;
	}
	kGuiCallbackFuncEnd(cSimpleUpdate,ChangeOcclusionTest)  

	bool ChangeReflectWorld(iWidget* apWidget,const cGuiMessageData& aData)
	{
		gpSimpleCamera->GetViewport()->GetRenderSettings()->mbRenderWorldReflection = aData.mlVal==1 ? true : false;
		return true;
	}
	kGuiCallbackFuncEnd(cSimpleUpdate,ChangeReflectWorld)  

	bool ChangeLargeLightTest(iWidget* apWidget,const cGuiMessageData& aData)
	{
		return true;
	}
	kGuiCallbackFuncEnd(cSimpleUpdate,ChangeLargeLightTest)  

	//--------------------------------------------------------------

	
	void CheckObjectsInFrustum(cRenderableSet *apSet, int *apLights, int *apObjects)
	{
		apSet->UpdateBeforeRendering();

		cFrustum *pCurrentFrustum = gpSimpleCamera->GetCamera()->GetFrustum();

		for(iRenderable *pObject : apSet->GetObjects())
		{
			if(pObject->IsVisible()==false) continue;

			///////////////////////////////////
			//Check if object is in frustum
			if(pCurrentFrustum->CollideBoundingVolume(pObject->GetBoundingVolume()) != eCollision_Outside)
			{
				if(pObject->GetRenderType() == eRenderableType_Light)
					(*apLights)++;
				else
					(*apObjects)++;
			}
		}
	}

	
	//--------------------------------------------------------------
	
	void OnDraw(float afFrameTime)
	{
		if(gpSimpleCamera->GetFont()==NULL) return;

		cGuiSet *pSet = gpSimpleCamera->GetSet();
		iFontData *pFont = gpSimpleCamera->GetFont();

		float fY =25;
		
		////////////////////////////////
		//Texture memory used:
		int lTexMem = Interface<cEngine>::Get()->GetResources()->GetTextureManager()->GetMemoryUsage()/1024;
		pSet->DrawFont(pFont,cVector3f(5,fY,0),14,cColor(1,1),_W("Texture memory usage: %d kb / %.1f Mb"),lTexMem, ((float)lTexMem) / 1024.0);
		fY+=16;

		//////////////////////
		//Occlusion rendering
		if(gbDrawOcclusionGfxInfo)
		{
			//float fY = gpSimpleCamera->GetSet()->GetVirtualSize().y - 300;
			//gpSimpleCamera->GetSet()->DrawGfx(mpTestRenderGfx,cVector3f(0,fY,0));
		}
			
		//////////////////////
		//Occlusion info
		if(gbDrawOcclusionTextInfo)
		{
			////////////////////////////////
			//Calculate how much would be seen using a normal frustum check
			int lLightsInFrustum=0;
			int lObjectsInFrustum=0;
			
            CheckObjectsInFrustum(mpWorld->GetRenderableSet(eWorldContainerType_Dynamic),
								&lLightsInFrustum,&lObjectsInFrustum);
			CheckObjectsInFrustum(mpWorld->GetRenderableSet(eWorldContainerType_Static),
								&lLightsInFrustum,&lObjectsInFrustum);

			////////////////////////////////
			//Number of objects visible
			cRenderSettings *pSettings = gpSimpleCamera->GetViewport()->GetRenderSettings();
			cRenderList *pRenderList = pSettings->mpRenderList;

			int lShadowCasters =0;
			for(int i=0; i<pRenderList->GetLightNum(); ++i)
			{
				iLight *pLight = pRenderList->GetLight(i);
				if(pLight->GetCastShadows() && pLight->GetLightType()==eLightType_Spot)
				{
					lShadowCasters++;
				}
			}

			gpSimpleCamera->GetSet()->DrawFont(gpSimpleCamera->GetFont(),cVector3f(5,fY,0),14,cColor(1,1),
																	_W("Num of lights: %d (%d) / %d ShadowCasters: %d"),
																	pSettings->mlNumberOfLightsRendered,
																	pRenderList->GetLightNum(),
																	lLightsInFrustum,
																	lShadowCasters);
			fY+=16;
			gpSimpleCamera->GetSet()->DrawFont(gpSimpleCamera->GetFont(),cVector3f(5,fY,0),14,cColor(1,1),
																	_W("Num of objects: %d / %d"),
																	pRenderList->GetSolidObjectNum() + pRenderList->GetTransObjectNum(),
																	lObjectsInFrustum);
			fY+=16;
			gpSimpleCamera->GetSet()->DrawFont(gpSimpleCamera->GetFont(),cVector3f(5,fY,0),14,cColor(1,1),
																	_W("Queries: %d"),
																		pSettings->mlNumberOfOcclusionQueries);
			fY+=16;
		}

		//////////////////////
		//Sound info
		if(gbDrawSoundDebug)
		{
			tStringVec vSoundNames;
			std::vector<cSoundEntry*> vEntries;

			cSoundHandler *pSoundHandler = Interface<cEngine>::Get()->GetSound()->GetSoundHandler();
			cMusicHandler *pMusicHandler = Interface<cEngine>::Get()->GetSound()->GetMusicHandler();

			//////////////////////////////
			//Sounds

			//Get names and entrys
			tSoundEntryList *pEntryList = pSoundHandler->GetEntryList();
			for(tSoundEntryListIt it = pEntryList->begin(); it != pEntryList->end();++it)
			{
				cSoundEntry *pEntry = *it;
				iSoundChannel *pSound = pEntry->GetChannel();
				vSoundNames.push_back(pSound->GetData()->GetName());
				vEntries.push_back(pEntry);
			}

			//Draw number of sounds
			gpSimpleCamera->GetSet()->DrawFont(gpSimpleCamera->GetFont(), cVector3f(5,fY,0),14,cColor(1,1),_W("Num of sounds: %d"),vSoundNames.size());
			fY+=16.0f;

			//Iterate sound entries and names
			int lRow=0, lCol=0;
			for(int i=0; i< (int)vSoundNames.size(); i++)
			{
				cSoundEntry *pEntry = vEntries[i];
				if(pEntry == NULL){
					lRow = 4;
					lCol =0;
					continue;
				}
				gpSimpleCamera->GetSet()->DrawFont(gpSimpleCamera->GetFont(),cVector3f(10 + (float)lCol*250,fY+(float)lRow*13,0),12,cColor(1,1),
					_W("%ls(%.2f)(%d) (%.2f/%.2f)"),
					cString::To16Char(vSoundNames[i]).c_str(),
					pEntry->GetChannel()->GetVolume(),
					pEntry->GetChannel()->GetPriority(),
					pEntry->GetChannel()->GetElapsedTime(),
					pEntry->GetChannel()->GetTotalTime()
					);

				lCol++;
				if(lCol == 3)
				{
					lCol =0;
					lRow++;
				}
			}
			if(vSoundNames.empty()==false) fY+=13.0f * lRow;

			//////////////////////////////
			//Music
			cMusicEntry *pMusic = pMusicHandler->GetCurrentSong();
			if(pMusic)
			{
				iSoundChannel *pChannel = pMusic->mpStream;
				gpSimpleCamera->GetSet()->DrawFont(gpSimpleCamera->GetFont(),cVector3f(5,fY,0),10,cColor(1,1),
					_W("Music: '%ls' vol: %.2f playing: %d prio: %d elapsed: %.2f total time: %.2f"),
					cString::To16Char(pChannel->GetData()->GetName()).c_str(),
					pChannel->GetVolume(),
					pChannel->IsPlaying(),
					pChannel->GetPriority(),
					pChannel->GetElapsedTime(),
					pChannel->GetTotalTime()
					);
				fY+=13.0f;
			}

		}
	}

	//--------------------------------------------------------------

	void Update(float afTimeStep)
	{
		//////////////////////////////////
		// Input
		if(Interface<cEngine>::Get()->GetInput()->BecameTriggerd("MouseMode"))
		{
			gpSimpleCamera->SetMouseMode(!gpSimpleCamera->GetMouseMode());
			Interface<cEngine>::Get()->GetInput()->GetLowLevel()->LockInput(!gpSimpleCamera->GetMouseMode());
			Interface<cEngine>::Get()->GetInput()->GetLowLevel()->RelativeMouse(!gpSimpleCamera->GetMouseMode());
		}

		//////////////////////////////////////////
		// Dynamic set dump
		if(mpWorld && Interface<cEngine>::Get()->GetInput()->BecameTriggerd("ContainerBugCheck"))
		{
			PrintDynamicSetContents();
		}

		//////////////////////////////////////////
		// Body picking
		if(mpPhysicsWorld && Interface<cEngine>::Get()->GetInput()->IsTriggerd("LeftMouse"))
		{
			cCamera *pCam = gpSimpleCamera->GetCamera();

			cVector2l vMousePosInt = Interface<cEngine>::Get()->GetInput()->GetMouse()->GetAbsPosition();
			cVector2f vMousePos = cVector2f((float)vMousePosInt.x, (float)vMousePosInt.y);

			if(mBodyPicker.mpPickedBody)
			{
				//Get pos of start.
				mBodyPicker.mvPos = cMath::MatrixMul(mBodyPicker.mpPickedBody->GetLocalMatrix(), 
					mBodyPicker.mvLocalPos);

				//Get Drag pos
				cVector3f vDir;
				pCam->UnProject(NULL,&vDir,vMousePos, Interface<cWindow>::Get()->GetSizeF());
				renderCallback.mvDragPos = pCam->GetPosition() + vDir*mBodyPicker.mfDist;

				//Spring testing:
				cVector3f vForce = (renderCallback.mvDragPos - mBodyPicker.mvPos)*20 - 
					(mBodyPicker.mpPickedBody->GetLinearVelocity()*0.4f);


				mBodyPicker.mpPickedBody->AddForceAtPosition(vForce *  mBodyPicker.mpPickedBody->GetMass(),
					mBodyPicker.mvPos);
			}
			else
			{
				cVector3f vDir;
				pCam->UnProject(NULL,&vDir,vMousePos, Interface<cWindow>::Get()->GetSizeF());
				cVector3f vOrigin = pCam->GetPosition();
				cVector3f vEnd = pCam->GetPosition() + vDir*100.0f;

				mBodyPicker.Clear();

				mpPhysicsWorld->CastRay(&mBodyPicker,vOrigin,vEnd,true,false,true);

				if(mBodyPicker.mpPickedBody)
				{
					mBodyPicker.mpPickedBody->SetAutoDisable(false);

					renderCallback.mvDragPos = mBodyPicker.mvPos;

					cMatrixf mtxInvWorld = cMath::MatrixInverse(mBodyPicker.mpPickedBody->GetLocalMatrix());
					mBodyPicker.mvLocalPos = cMath::MatrixMul(mtxInvWorld, mBodyPicker.mvPos);
				}
			}
		}
		else
		{
			if(mBodyPicker.mpPickedBody) mBodyPicker.mpPickedBody->SetAutoDisable(true);
			mBodyPicker.Clear();
		}		

	}

	//--------------------------------------------------------------


	~cSimpleUpdate()
	{
	
	}

public:
	cWorld* mpWorld;
	
	cSimpleRenderCallback renderCallback;
	EventHandler<const WorldDrawCtx&> mPreWorldDrawHandler;

	cWidgetWindow *mpOptionWindow;

	cWorld *mpTestWorld;
	iPhysicsWorld *mpPhysicsWorld;
	iTexture *mpTestRenderTexture;
	cViewport *mpTestViewPort;
	iFrameBuffer *mpTestFrameBuffer;
	cGuiGfxElement *mpTestRenderGfx;

	tWStringVec mvPickedFiles;
	tWString msCurrentFilePath;

	cBodyPicker mBodyPicker;

	cWidgetCheckBox *mpCheckAmbientActive;
	cWidgetSlider *mpSliderAmbientColor;
	cWidgetLabel *mpLabelScatterLength;
	cWidgetLabel *mpLabelScatterMin;
	cWidgetLabel *mpLabelScatterMax;
};

//-----------------------------------------------------------------------

#ifdef _WIN32
	#include <Windows.h>
#endif

#include "../LuxBasePersonal.h"

#ifdef __APPLE__
namespace hpl {
	extern tString FindGameResources();
}
#endif

int hplMain(const tString &asCommandline)
{
//To allow drag and drop:
/*#ifdef WIN32
	TCHAR buffer[MAX_PATH];
	HMODULE module = GetModuleHandle(NULL);
	GetModuleFileName(module, buffer,MAX_PATH);
	tString sDir = cString::GetFilePath(buffer);
	SetCurrentDirectory(sDir.c_str());
#endif*/
#if __APPLE__
	tWString sEditorDir = cPlatform::GetWorkingDir();
	sEditorDir = cString::AddSlashAtEndW(sEditorDir);

	tString gameDir = FindGameResources();
	if (gameDir.empty())
	{
		exit(1);
	}
#endif

	//iResourceBase::SetLogCreateAndDelete(true);
	//iGpuProgram::SetLogDebugInformation(true); 


	tWString sPersonalDir = cString::ReplaceCharToW(cPlatform::GetSystemSpecialPath(eSystemPath_Personal), _W("\\"), _W("/"));
#ifdef USERDIR_RESOURCES
	tWString sUserResourceDir = sPersonalDir + PERSONAL_RELATIVEROOT PERSONAL_RELATIVEGAME_PARENT PERSONAL_RESOURCES;
#endif

	// Create the base personal folders
	tWStringVec vDirs;
#ifdef USERDIR_RESOURCES
	SetupBaseDirs(vDirs, _W("HPL2"), _W(""),true);
#else
	SetupBaseDirs(vDirs, _W("HPL2"));
#endif
	CreateBaseDirs(vDirs, sPersonalDir);

	SetLogFile(sPersonalDir + PERSONAL_RELATIVEROOT _W("HPL2/mapview.log"));

	gpConfig = hplNew( cConfigFile, (sPersonalDir + PERSONAL_RELATIVEROOT _W("HPL2/mapview.cfg") ));

	gpConfig->Load();
	
	//Init the game engine
	cEngineInitVars vars;
	vars.mGraphics.mvScreenSize = {gpConfig->GetInt("Screen","Width",1024), gpConfig->GetInt("Screen","Height",768)};
	vars.mGraphics.mbFullscreen = gpConfig->GetBool("Screen","FullScreen", false);
	cEngine* gpEngine = CreateHPLEngine(eHplAPI_OpenGL, eHplSetup_All, &vars);
	Interface<cEngine>::Get()->SetLimitFPS(false);
	Interface<cEngine>::Get()->GetGraphics()->SetVsync(false);
	Interface<cEngine>::Get()->SetWaitIfAppOutOfFocus(true);

	gsNodeCont_Name = gpConfig->GetString("NodeCont","Name", "MapViewTest");
	gvNodeCont_BodySize = gpConfig->GetVector3f("NodeCont","BodySize", cVector3f(1, 1.5f, 1));
	gbNodeCont_NodeAtCenter = gpConfig->GetBool("NodeCont","NodeAtCenter", false);
	glNodeCont_MinEdges = gpConfig->GetInt("NodeCont","MinEdges", 2);
	glNodeCont_MaxEdges = gpConfig->GetInt("NodeCont","MaxEdges", 6);
	gfNodeCont_MaxEdgeDistance = gpConfig->GetFloat("NodeCont_","MaxEdgeDistance", 5.0f);
	gfNodeCont_MaxHeight = gpConfig->GetFloat("NodeCont","MaxHeight", 0.41f);

	if(asCommandline != "")
	{
		gsMapFile = asCommandline;
		gsMapFile = cString::ReplaceCharTo(gsMapFile,"\"","");

		tString sModelDir = cString::GetFilePath(gsMapFile);
		tWString sDir = cString::To16Char(sModelDir);
		if(sDir != _W(""))
			Interface<cEngine>::Get()->GetResources()->AddResourceDir(sDir,false);

		gsMapFile = cString::GetFileName(gsMapFile);
	}
	
	//Add resources
#ifdef USERDIR_RESOURCES
	Interface<cEngine>::Get()->GetResources()->LoadResourceDirsFile("resources.cfg", sUserResourceDir);
#else
	Interface<cEngine>::Get()->GetResources()->LoadResourceDirsFile("resources.cfg");
#endif
#ifdef __APPLE__
	Interface<cEngine>::Get()->GetResources()->AddResourceDir(sEditorDir + _W("viewer/"), true);
#endif

	//Add updates
	cSimpleUpdate Update;
	Interface<cEngine>::Get()->GetUpdater()->AddUpdate("Default", &Update);
	
	gpSimpleCamera = hplNew(cSimpleCamera, (Update.GetName(),Interface<cEngine>::Get(), Update.mpWorld, 10, cVector3f(0,0,9), true) );
	
	Interface<cEngine>::Get()->GetUpdater()->AddUpdate("Default", gpSimpleCamera);

	Update.SetupView();

	//Run the engine
	gpEngine->Run();


	hplDelete (gpSimpleCamera);

	//Delete the engine
	DestroyHPLEngine(gpEngine);

	gpConfig->GetString("NodeCont","Name", gsNodeCont_Name);
	gpConfig->SetVector3f("NodeCont","BodySize", gvNodeCont_BodySize);
	gpConfig->SetBool("NodeCont","NodeAtCenter", gbNodeCont_NodeAtCenter);
	gpConfig->SetInt("NodeCont","MinEdges", glNodeCont_MinEdges);
	gpConfig->SetInt("NodeCont","MaxEdges", glNodeCont_MaxEdges);
	gpConfig->SetFloat("NodeCont","MaxEdgeDistance", gfNodeCont_MaxEdgeDistance);
	gpConfig->SetFloat("NodeCont","MaxHeight",gfNodeCont_MaxHeight );

	gpConfig->SetInt("Screen","Width",vars.mGraphics.mvScreenSize.x);
	gpConfig->SetInt("Screen","Height",vars.mGraphics.mvScreenSize.y);
	gpConfig->SetBool("Screen","FullScreen", vars.mGraphics.mbFullscreen);

	gpConfig->Save();
	hplDelete(gpConfig);


	cMemoryManager::LogResults();

	return 0;
}
