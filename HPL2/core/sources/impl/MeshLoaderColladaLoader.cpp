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

#include "impl/MeshLoaderCollada.h"

#include "system/LowLevelSystem.h"
#include "system/String.h"
#include "system/System.h"
#include "system/Platform.h"

#include "graphics/Mesh.h"
#include "graphics/SubMesh.h"
#include "graphics/Material.h"
#include "graphics/VertexBuffer.h"
#include "graphics/Bone.h"
#include "graphics/Skeleton.h"
#include "graphics/Animation.h"
#include "graphics/AnimationTrack.h"

#include "resources/MaterialManager.h"

#include <tinyxml2.h>

#include "math/Math.h"

namespace hpl {

	//------------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// FILL STRUCTURES
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	bool cMeshLoaderCollada::FillStructures(const tWString &asFile,
		tColladaImageVec *apColladaImageVec,
		tColladaTextureVec *apColladaTextureVec,
		tColladaMaterialVec *apColladaMaterialVec,
		tColladaLightVec *apColladaLightVec,
		tColladaGeometryVec *apColladaGeometryVec,
		tColladaControllerVec *apColladaControllerVec,
		tColladaAnimationVec *apColladaAnimVec,
		cColladaScene *apColladaScene,bool abCache)
	{
		bool bLoadCache =false;
		//abCache = false;
		//Log("Loading %s\n",asFile.c_str());

		tWString sCacheFile = cString::SetFileExtW(asFile,_W("dae_cache"));
		
		if(	abCache &&
			cPlatform::FileExists(sCacheFile) && 
			cPlatform::FileExists(asFile))
		{
			cDate colladaDate = cPlatform::FileModifiedDate(asFile);
			cDate cacheDate = cPlatform::FileModifiedDate(sCacheFile);
			
			//Check if cache is newer
			if(cacheDate > colladaDate) bLoadCache = true;
		}
		
		/////////////////////////////////////////////////
		// LOAD CACHE
		if(bLoadCache)
		{
			return LoadStructures(sCacheFile,
				apColladaImageVec,
				apColladaTextureVec,
				apColladaMaterialVec,
				apColladaLightVec,
				apColladaGeometryVec,
				apColladaControllerVec,
				apColladaAnimVec,
				apColladaScene);	
		}

		Log("Cache out of date! Reloading collada file '%s'\n",cString::To8Char(asFile).c_str());

		/////////////////////////////////////////////////
		// LOAD THE DOCUMENT
		//unsigned long lStartTime = mpSystem->GetLowLevel()->GetTime();
		
		FILE *pFile = cPlatform::OpenFile(asFile, _W("rb"));
		if(pFile==NULL) return false;

		tinyxml2::XMLDocument* pXmlDoc = hplNew(tinyxml2::XMLDocument, (true, tinyxml2::COLLAPSE_WHITESPACE) );
		if(pXmlDoc->LoadFile(pFile)!=tinyxml2::XML_SUCCESS)
		{
			Error("Couldn't load Collada XML file '%s'!\n",asFile.c_str());
			fclose(pFile);
			hplDelete(pXmlDoc);
			return false;
		}

		fclose(pFile);

		//unsigned long lTime = mpSystem->GetLowLevel()->GetTime() - lStartTime;
		//Log("Loading collada XML file for '%s' took: %d ms\n",asFile.c_str(),lTime);

		//Get the root.
		tinyxml2::XMLElement* pRootElem = pXmlDoc->RootElement();

		/////////////////////////////////////////////////
		// FIRST SETUP

		//Set up axis as Y.
		mbZToY = false;
		mfUnitScale = 1.0f;

		//Load assets element.
		tinyxml2::XMLElement *pAssetElem = pRootElem->FirstChildElement("asset");
		if(pAssetElem)
		{
			tinyxml2::XMLElement *pUpAxisElem = pAssetElem->FirstChildElement("up_axis");
			if(pUpAxisElem)
			{
				tinyxml2::XMLText *pAxisText = pUpAxisElem->FirstChild()->ToText();

				if(tString(pAxisText->Value())=="Z_UP"){
					mbZToY = true;
					//Log("!!!!!!!Z IS UP!!!!!!");
				}
			}


			bool bGetUnit = false;

			// Only get unit from 3ds max files!
			tinyxml2::XMLElement *pCont = pAssetElem->FirstChildElement("contributor");
			
			if(pCont)
			{
				pCont = pCont->FirstChildElement("authoring_tool");
			}
			else
			{
				pCont = pAssetElem->FirstChildElement("authoring_tool");
			}

			if(pCont)
			{
				tString sName = cString::ToString(pCont->FirstChild()->Value(), ""); 

				if(sName == "FBX COLLADA exporter")
				{
					bGetUnit = true;
				}
			}

			tinyxml2::XMLElement *pUnitElem = pAssetElem->FirstChildElement("unit");

			if(pUnitElem && bGetUnit && mbConvertUnit)
			{
				float fUnitTypeScale = 1.0f;
				float fUnitValue = 1.0f;

				const tinyxml2::XMLAttribute *pFirstAttri = pUnitElem->FirstAttribute();

				for(;pFirstAttri; pFirstAttri = pFirstAttri->Next())
				{
					tString sName = cString::ToString(pFirstAttri->Name(), "");

					if(sName == "meter")
					{
						//////////////
						// Read the scale
						fUnitValue = (float)pFirstAttri->DoubleValue();
					}					
				}

				mfUnitScale = fUnitValue;
			}

		}
		else
		{
			Error("Couldn't find asset element!\n");
		}
		
		//mbZToY = false;
		//mfUnitScale = 1.0f;


		/////////////////////////////////////////////////
		// ITERATE THE LIBRARIES BEFORE SCENE LIBRARIES
		tinyxml2::XMLElement* pLibraryElem = pRootElem->FirstChildElement();
		while(pLibraryElem)
		{
			tString sType = cString::ToString(pLibraryElem->Attribute("type"),"");
			tString sValue = cString::ToString(pLibraryElem->Value(),"");

			// Lights
			if((sType == "LIGHT" || sValue == "library_lights") && apColladaLightVec)
			{
				///Log("Loading type: %s\n",sType.c_str());
				LoadLights(pLibraryElem,*apColladaLightVec);
				//Log(" --- \n");
			}	

			pLibraryElem = pLibraryElem->NextSiblingElement();
		}

		//////////////////////////////////////////////
		// LOAD SCENE
		if(apColladaScene)
		{
			tinyxml2::XMLElement* pSceneElem = pRootElem->FirstChildElement("library_visual_scenes");
			if(pSceneElem){
				pSceneElem = pSceneElem->FirstChildElement("visual_scene");
				if(pSceneElem==NULL) Warning("No visual scene element found!\n");
			}

			if(pSceneElem==NULL) pSceneElem = pRootElem->FirstChildElement("scene");

			if(pSceneElem==NULL)
			{ 
				Warning("No scene element found!\n");
			}
			else
			{
				//Get start and end time (MAYA ONLY!)
				tinyxml2::XMLElement* pExtraElem = pSceneElem->FirstChildElement("extra");
				if(pExtraElem)
				{
					// Get techniques
					tinyxml2::XMLElement* pExtraTechElem = pExtraElem->FirstChildElement("technique");
					for(;pExtraTechElem; pExtraTechElem = pExtraTechElem->NextSiblingElement("technique"))
					{
						//Check for maya profile
						tString sProfile = cString::ToString(pExtraTechElem->Attribute("profile"),"");
						if(sProfile == "MAYA" || sProfile == "FCOLLADA")
						{
							//Iterate params
							tinyxml2::XMLElement *pParam = pExtraTechElem->FirstChildElement();
							for(;pParam; pParam = pParam->NextSiblingElement())
							{
								tString sName = cString::ToString(pParam->Attribute("name"),"");
								if(sName=="")
								{
									sName = cString::ToString(pParam->Value(),"");
								}

								sName = cString::ToLowerCase(sName);

								tinyxml2::XMLText *pText = pParam->FirstChild()->ToText();
								float fValue = cString::ToFloat(pText->Value(),0);

								if(sName == "start_time") apColladaScene->mfStartTime = fValue;
								else if(sName == "end_time") apColladaScene->mfEndTime = fValue;
							}
						}

						apColladaScene->mfDeltaTime = apColladaScene->mfEndTime - apColladaScene->mfStartTime;
					}

					//Log("Anim Start: %f End: %f\n",apColladaScene->mfStartTime, apColladaScene->mfEndTime);
				}
				else
				{
					apColladaScene->mfStartTime = 0;
					apColladaScene->mfEndTime = 0;
					apColladaScene->mfDeltaTime = -1;
					Warning("No 'extra scene' element found!\n");
				}

				//Load all nodes in the scene.
				tinyxml2::XMLElement* pNodeElem = pSceneElem->FirstChildElement("node");
				while(pNodeElem)
				{
					LoadColladaScene(pNodeElem, &apColladaScene->mRoot, apColladaScene, apColladaLightVec);

					pNodeElem = pNodeElem->NextSiblingElement("node");
				}
			}
		}

		/////////////////////////////////////////////////
		// ITERATE THE AFTER SCENE LIBRARIES

		pLibraryElem = pRootElem->FirstChildElement();
		while(pLibraryElem)
		{
			tString sValue = cString::ToString(pLibraryElem->Value(),"");
			tString sType = cString::ToString(pLibraryElem->Attribute("type"),"");

			// Image
			if((sType == "IMAGE" || sValue == "library_images") && apColladaImageVec)
			{
				//Loads all images files available
				LoadImages(pLibraryElem, *apColladaImageVec);
			}
			// Effects / Texture (texture is name in older versions)
			else if((sType == "TEXTURE" || sValue == "library_effects") && apColladaTextureVec)
			{
				//Loads all texture and their settings
				LoadTextures(pLibraryElem, *apColladaTextureVec);
			}
			// Material
			else if((sType == "MATERIAL" || sValue == "library_materials") && apColladaMaterialVec)
			{
				//All materials used are loaded
				LoadMaterials(pLibraryElem, *apColladaMaterialVec);
			}
			// Geometry
			else if((sType == "GEOMETRY" || sValue == "library_geometries") && apColladaGeometryVec)
			{
				//geometry for all objects in the scene are loaded.
				LoadGeometry(pLibraryElem, *apColladaGeometryVec);
			}
			// Animation
			else if((sType == "ANIMATION" || sValue == "library_animations") && apColladaScene && apColladaAnimVec)
			{
				LoadAnimations(pLibraryElem, *apColladaAnimVec, apColladaScene);
			}
			// Controller
			else if((sType == "CONTROLLER" || sValue == "library_controllers") && apColladaGeometryVec && apColladaControllerVec)
			{
				LoadControllers(pLibraryElem,*apColladaControllerVec,apColladaGeometryVec);
			}


			//Log(" ---- \n",sType.c_str());

			pLibraryElem = pLibraryElem->NextSiblingElement();
		}

		/*if(abCache && mbLoadAndSaveMSHFormat==false)
		{
			SaveStructures(sCacheFile,apColladaImageVec,
							apColladaTextureVec,
							apColladaMaterialVec,
							apColladaLightVec,
							apColladaGeometryVec,
							apColladaControllerVec,
							apColladaAnimVec,
							apColladaScene);
		}*/
		hplDelete(pXmlDoc);
		return true;
	}

	//--------------------------------------------------------------------------

	//////////////////////////////////////////////////////////////////////////
	// SAVE COLLADA DATA
	//////////////////////////////////////////////////////////////////////////

	#define CreateXMLChild(parent, name) static_cast<tinyxml2::XMLElement*>(parent->InsertEndChild(parent->GetDocument()->NewElement(name)));

	#define CreateXMLTextChild(parent, name) static_cast<tinyxml2::XMLText*>(parent->InsertEndChild(parent->GetDocument()->NewText(name)));

	//-----------------------------------------------------------------------

	static void SaveImageVec(tinyxml2::XMLElement *apRootElem, tColladaImageVec *apColladaImageVec);
	static void SaveTextureVec(tinyxml2::XMLElement *apRootElem, tColladaTextureVec *apColladaTextureVec);
	static void SaveMaterialVec(tinyxml2::XMLElement *apRootElem, tColladaMaterialVec *apColladaMaterialVec);
	static void SaveLightVec(tinyxml2::XMLElement *apRootElem, tColladaLightVec *apColladaLightVec);
	static void SaveAnimationVec(tinyxml2::XMLElement *apRootElem, tColladaAnimationVec *apColladaAnimationVec);
	static void SaveControllerVec(tinyxml2::XMLElement *apRootElem, tColladaControllerVec *apColladaControllerVec);
	static void SaveGeometryVec(tinyxml2::XMLElement *apRootElem, tColladaGeometryVec *apColladaGeometryVec);
	static void SaveScene(tinyxml2::XMLElement *apRootElem, cColladaScene *apColladaScene);
	
	bool cMeshLoaderCollada::SaveStructures(const tWString &asFile,
											tColladaImageVec *apColladaImageVec,
											tColladaTextureVec *apColladaTextureVec,
											tColladaMaterialVec *apColladaMaterialVec,
											tColladaLightVec *apColladaLightVec,
											tColladaGeometryVec *apColladaGeometryVec,
											tColladaControllerVec *apColladaControllerVec,
											tColladaAnimationVec *apColladaAnimVec,
											cColladaScene *apColladaScene)
	{
		tinyxml2::XMLDocument* pXmlDoc = hplNew(tinyxml2::XMLDocument, () );

		tinyxml2::XMLElement *pRootElem = CreateXMLChild(pXmlDoc,"ColladaCache");
		
		if(apColladaImageVec)		SaveImageVec(pRootElem,apColladaImageVec);
		if(apColladaTextureVec)		SaveTextureVec(pRootElem,apColladaTextureVec);
		if(apColladaMaterialVec)	SaveMaterialVec(pRootElem,apColladaMaterialVec);
		if(apColladaLightVec)		SaveLightVec(pRootElem,apColladaLightVec);
		if(apColladaAnimVec)		SaveAnimationVec(pRootElem,apColladaAnimVec);
		if(apColladaControllerVec)	SaveControllerVec(pRootElem,apColladaControllerVec);
		if(apColladaGeometryVec)	SaveGeometryVec(pRootElem,apColladaGeometryVec);
		if(apColladaScene)			SaveScene(pRootElem,apColladaScene);

		FILE *pFile = cPlatform::OpenFile(asFile, _W("w+"));
		if(pFile==NULL || pXmlDoc->SaveFile(pFile)!=tinyxml2::XML_SUCCESS)
		{
			Error("Couldn't save XML file %s\n",asFile.c_str());
			hplDelete(pXmlDoc);		
			return false;	
		}
		fclose(pFile);
		hplDelete(pXmlDoc);		
		return true;
	}

	//-----------------------------------------------------------------------

	static void SaveImageVec(tinyxml2::XMLElement *apRootElem, tColladaImageVec *apColladaImageVec)
	{
		tinyxml2::XMLElement *pImageRootElem =  CreateXMLChild(apRootElem,"ImageRoot");

		pImageRootElem->SetAttribute("Size",(int)apColladaImageVec->size());

		for(size_t i=0; i< apColladaImageVec->size(); ++i)
		{
			cColladaImage *pImage = &(*apColladaImageVec)[i];
            
			tinyxml2::XMLElement *pImageElem = CreateXMLChild(pImageRootElem,"Image");

            pImageElem->SetAttribute("Id",pImage->msId.c_str());
			pImageElem->SetAttribute("Name",pImage->msName.c_str());
			pImageElem->SetAttribute("Source",cString::GetFileName(pImage->msSource.c_str()).c_str());
		}
	}

	//-----------------------------------------------------------------------

	static void SaveTextureVec(tinyxml2::XMLElement *apRootElem, tColladaTextureVec *apColladaTextureVec)
	{
		tinyxml2::XMLElement *pTextureRootElem =  CreateXMLChild(apRootElem,"TextureRoot");

		pTextureRootElem->SetAttribute("Size",(int)apColladaTextureVec->size());

		for(size_t i=0; i< apColladaTextureVec->size(); ++i)
		{
			cColladaTexture *pTexture = &(*apColladaTextureVec)[i];

			tinyxml2::XMLElement *pTextureElem = CreateXMLChild(pTextureRootElem,"Texture");

			pTextureElem->SetAttribute("Id",pTexture->msId.c_str());
			pTextureElem->SetAttribute("Name",pTexture->msName.c_str());
			pTextureElem->SetAttribute("Image",pTexture->msImage.c_str());
		}
	}

	//-----------------------------------------------------------------------

	static void SaveMaterialVec(tinyxml2::XMLElement *apRootElem, tColladaMaterialVec *apColladaMaterialVec)
	{
		tinyxml2::XMLElement *pMaterialRootElem =  CreateXMLChild(apRootElem,"MaterialRoot");

		pMaterialRootElem->SetAttribute("Size",(int)apColladaMaterialVec->size());

		for(size_t i=0; i< apColladaMaterialVec->size(); ++i)
		{
			cColladaMaterial *pMaterial = &(*apColladaMaterialVec)[i];

			tinyxml2::XMLElement *pMaterialElem = CreateXMLChild(pMaterialRootElem,"Material");

			pMaterialElem->SetAttribute("Id",pMaterial->msId.c_str());
			pMaterialElem->SetAttribute("Name",pMaterial->msName.c_str());
			pMaterialElem->SetAttribute("Texture",pMaterial->msTexture.c_str());
			pMaterialElem->SetAttribute("Color",pMaterial->mDiffuseColor.ToFileString().c_str());
		}
	}

	//-----------------------------------------------------------------------

	static void SaveLightVec(tinyxml2::XMLElement *apRootElem, tColladaLightVec *apColladaLightVec)
	{
		tinyxml2::XMLElement *pLightRootElem =  CreateXMLChild(apRootElem,"LightRoot");

		pLightRootElem->SetAttribute("Size",(int)apColladaLightVec->size());

		for(size_t i=0; i< apColladaLightVec->size(); ++i)
		{
			cColladaLight *pLight = &(*apColladaLightVec)[i];

			tinyxml2::XMLElement *pLightElem = CreateXMLChild(pLightRootElem,"Light");

			//Log("Light %s\n",pLight->msName.c_str());

			pLightElem->SetAttribute("Id",pLight->msId.c_str());
			pLightElem->SetAttribute("Name",pLight->msName.c_str());
			pLightElem->SetAttribute("Type",pLight->msType.c_str());
			
			pLightElem->SetAttribute("Angle",cString::ToString(pLight->mfAngle).c_str());
			pLightElem->SetAttribute("Color",pLight->mDiffuseColor.ToFileString().c_str());
		}
	}

	//-----------------------------------------------------------------------

	static void SaveAnimationVec(tinyxml2::XMLElement *apRootElem, tColladaAnimationVec *apColladaAnimationVec)
	{
		tinyxml2::XMLElement *pAnimationRootElem =  CreateXMLChild(apRootElem,"AnimationRoot");

		pAnimationRootElem->SetAttribute("Size",(int)apColladaAnimationVec->size());

		for(size_t i=0; i< apColladaAnimationVec->size(); ++i)
		{
			cColladaAnimation *pAnimation = &(*apColladaAnimationVec)[i];

			tinyxml2::XMLElement *pAnimationElem = CreateXMLChild(pAnimationRootElem,"Animation");

			pAnimationElem->SetAttribute("Id",pAnimation->msId.c_str());
			pAnimationElem->SetAttribute("TargetNode",pAnimation->msTargetNode.c_str());
			
            /////////////////////////////////////
			// Channels
			{
				tinyxml2::XMLElement *pChannelVecElem =  CreateXMLChild(pAnimationElem,"ChannelVec");
				pChannelVecElem->SetAttribute("Size",(int)pAnimation->mvChannels.size());

				for(size_t idx =0; idx < pAnimation->mvChannels.size(); ++idx)
				{
					cColladaChannel *pChannel = &pAnimation->mvChannels[idx];
					tinyxml2::XMLElement *pChannelElem =  CreateXMLChild(pChannelVecElem,"Channel");
					
					pChannelElem->SetAttribute("Id",pChannel->msId.c_str());
					pChannelElem->SetAttribute("Target",pChannel->msTarget.c_str());
					pChannelElem->SetAttribute("Source",pChannel->msSource.c_str());
				}
			}
			/////////////////////////////////////
			// Samplers
			{
				tinyxml2::XMLElement *pSamplerVecElem =  CreateXMLChild(pAnimationElem,"SamplerVec");
				pSamplerVecElem->SetAttribute("Size",(int)pAnimation->mvSamplers.size());

				for(size_t idx =0; idx < pAnimation->mvSamplers.size(); ++idx)
				{
					cColladaSampler *pSampler = &pAnimation->mvSamplers[idx];
					tinyxml2::XMLElement *pSamplerElem =  CreateXMLChild(pSamplerVecElem,"Sampler");

					pSamplerElem->SetAttribute("Id",pSampler->msId.c_str());
					pSamplerElem->SetAttribute("TimeArray",pSampler->msTimeArray.c_str());
					pSamplerElem->SetAttribute("ValueArray",pSampler->msValueArray.c_str());
					pSamplerElem->SetAttribute("Target",pSampler->msTarget.c_str());
				}
			}
			/////////////////////////////////////
			// Sources
			{
				tinyxml2::XMLElement *pSourceVecElem =  CreateXMLChild(pAnimationElem,"SourceVec");
				pSourceVecElem->SetAttribute("Size",(int)pAnimation->mvSources.size());

				for(size_t idx =0; idx < pAnimation->mvSources.size(); ++idx)
				{
					cColladaAnimSource *pSource = &pAnimation->mvSources[idx];
					tinyxml2::XMLElement *pSourceElem =  CreateXMLChild(pSourceVecElem,"Source");

					pSourceElem->SetAttribute("Id",pSource->msId.c_str());
					tString sData ="";
					for(size_t j=0; j< pSource->mvValues.size();++j) 
						sData += cString::ToString(pSource->mvValues[j])+" ";
					
					pSourceElem->SetAttribute("Values",sData.c_str());
				}
			}
		}
	}

	//-----------------------------------------------------------------------

	static void SaveControllerVec(tinyxml2::XMLElement *apRootElem, tColladaControllerVec *apColladaControllerVec)
	{
		tinyxml2::XMLElement *pControllerRootElem =  CreateXMLChild(apRootElem,"ControllerRoot");

		pControllerRootElem->SetAttribute("Size",(int)apColladaControllerVec->size());

		for(size_t i=0; i< apColladaControllerVec->size(); ++i)
		{
			cColladaController *pController = &(*apColladaControllerVec)[i];

			tinyxml2::XMLElement *pControllerElem = CreateXMLChild(pControllerRootElem,"Controller");

			pControllerElem->SetAttribute("Id",pController->msId.c_str());
			pControllerElem->SetAttribute("Target",pController->msTarget.c_str());

			pControllerElem->SetAttribute("BindShapeMatrix",pController->m_mtxBindShapeMatrix.ToFileString().c_str());

			pControllerElem->SetAttribute("JointPairIdx",pController->mlJointPairIdx);
			pControllerElem->SetAttribute("WeightPairIdx",pController->mlWeightPairIdx);
			
			///////////////////////
			// Joints
			{
				tinyxml2::XMLElement *pJointsElem = CreateXMLChild(pControllerElem,"Joints");
				pJointsElem->SetAttribute("Size",(int)pController->mvJoints.size());
				
				tString sData=""; sData.reserve(pController->mvJoints.size()*10);
				for(size_t idx =0; idx < pController->mvJoints.size(); ++idx)
				{
					sData += pController->mvJoints[idx] + " ";
				}
				CreateXMLTextChild(pJointsElem, sData.c_str());
			}
			///////////////////////
			// Weights
			{
				tinyxml2::XMLElement *pWeightsElem = CreateXMLChild(pControllerElem,"Weights");
				pWeightsElem->SetAttribute("Size",(int)pController->mvWeights.size());

				tString sData=""; sData.reserve(pController->mvWeights.size()*5);
				for(size_t idx =0; idx < pController->mvWeights.size(); ++idx)
				{
					sData += cString::ToString(pController->mvWeights[idx]) + " ";
				}
				CreateXMLTextChild(pWeightsElem, sData.c_str());
			}
			///////////////////////
			// Matrices
			{
				tinyxml2::XMLElement *pMatricesElem = CreateXMLChild(pControllerElem,"Matrices");
				pMatricesElem->SetAttribute("Size",(int)pController->mvMatrices.size());

				tString sData=""; sData.reserve(pController->mvMatrices.size()*5);
				for(size_t idx =0; idx < pController->mvMatrices.size(); ++idx)
				{
					sData += pController->mvMatrices[idx].ToFileString() + " ";
				}
				CreateXMLTextChild(pMatricesElem, sData.c_str());
			}
			///////////////////////
			// Pairs
			{
				tinyxml2::XMLElement *pPairVecElem = CreateXMLChild(pControllerElem,"PairVec");
				pPairVecElem->SetAttribute("Size",(int)pController->mvPairs.size());

				tIntVec vPairNumVec; vPairNumVec.resize(pController->mvPairs.size(),0);

				////////////////////////////////
				//The pair connections
				int lCount =0;
                tinyxml2::XMLElement *pPairsElem = CreateXMLChild(pPairVecElem,"Pairs");
				tString sData=""; sData.reserve(pController->mvPairs.size()*4*2);
				for(size_t idx =0; idx < pController->mvPairs.size(); ++idx)
				{
					tColladaJointPairList *pList = &pController->mvPairs[idx];
					
					for(tColladaJointPairListIt it = pList->begin(); it != pList->end(); ++it)
					{
						cColladaJointPair &Pair = *it;
						sData += cString::ToString(Pair.mlJoint) +" " + cString::ToString(Pair.mlWeight) + " ";
						++lCount;
						++vPairNumVec[idx];
					}
				}
				CreateXMLTextChild(pPairsElem, sData.c_str());
				pPairsElem->SetAttribute("Size",lCount);

				////////////////////////////////
				//The pair numbers
				tinyxml2::XMLElement *pPairNumElem = CreateXMLChild(pPairVecElem,"PairNum");
				sData=""; sData.reserve(pController->mvPairs.size()*4);
				for(size_t idx =0; idx < pController->mvPairs.size(); ++idx)
				{
					sData += cString::ToString(vPairNumVec[idx]) +" ";
				}
				CreateXMLTextChild(pPairNumElem, sData.c_str());
				
			}
		}
	}

	//-----------------------------------------------------------------------

	static void SaveGeometryVec(tinyxml2::XMLElement *apRootElem, tColladaGeometryVec *apColladaGeometryVec)
	{
		tinyxml2::XMLElement *pGeometryRootElem =  CreateXMLChild(apRootElem,"GeometryRoot");

		pGeometryRootElem->SetAttribute("Size",(int)apColladaGeometryVec->size());

		for(size_t i=0; i< apColladaGeometryVec->size(); ++i)
		{
			cColladaGeometry *pGeometry = &(*apColladaGeometryVec)[i];

			tinyxml2::XMLElement *pGeometryElem = CreateXMLChild(pGeometryRootElem,"Geometry");

			/////////////////////////////////7
			//Main properties
            pGeometryElem->SetAttribute("Id",pGeometry->msId.c_str());
			pGeometryElem->SetAttribute("Name",pGeometry->msName.c_str());
			
			pGeometryElem->SetAttribute("Material",pGeometry->msMaterial.c_str());

			/////////////////////////////////
			// Vertex properties
			// Should not be needed.
			/*pGeometryElem->SetAttribute("PosIdxNum",pGeometry->mlPosIdxNum); 
			pGeometryElem->SetAttribute("NormIdxNum",pGeometry->mlNormIdxNum); 
			pGeometryElem->SetAttribute("TexIdxNum",pGeometry->mlTexIdxNum);

			pGeometryElem->SetAttribute("PosArrayIdx",pGeometry->mlPosArrayIdx); 
			pGeometryElem->SetAttribute("NormArrayIdx",pGeometry->mlNormArrayIdx); 
			pGeometryElem->SetAttribute("TexArrayIdx",pGeometry->mlTexArrayIdx); */

			/////////////////////////////////
			// Index Vec
			tinyxml2::XMLElement *pIndexVecElem = CreateXMLChild(pGeometryElem,"IndexVec");
            
			pIndexVecElem->SetAttribute("Size",(int)pGeometry->mvIndexVec.size());

			tinyxml2::XMLElement *pIndicesDataElem = CreateXMLChild(pIndexVecElem,"Indices");

			tString sIndexVec = "";
			sIndexVec.reserve(pGeometry->mvIndexVec.size() * 5);
			for(size_t j=0; j < pGeometry->mvIndexVec.size(); ++j)
			{
				sIndexVec += cString::ToString((int)pGeometry->mvIndexVec[j])+" ";
			}
			tinyxml2::XMLText *pIndecVecText = CreateXMLTextChild(pIndicesDataElem,"Data");
            pIndecVecText->SetValue(sIndexVec.c_str());

			/////////////////////////////////
			// Vertex vec
			tinyxml2::XMLElement *pVertexVecElem = CreateXMLChild(pGeometryElem,"VertexVec");
			pVertexVecElem->SetAttribute("Size",(int)pGeometry->mvVertexVec.size());
			{
				/////////////////////////////////
				// Positions
				tString sData = "";
				sData.reserve(pGeometry->mvVertexVec.size() * 10 * 3);
				for(size_t j=0; j < pGeometry->mvVertexVec.size(); ++j)
				{
					sData += pGeometry->mvVertexVec[j].pos.ToFileString()+" ";
				}
				tinyxml2::XMLElement *pDataElem = CreateXMLChild(pVertexVecElem,"Positions");
				tinyxml2::XMLText *pVecText = CreateXMLTextChild(pDataElem,"Data");
				pVecText->SetValue(sData.c_str());
				
				/////////////////////////////////
				// Normals
				sData = "";
				sData.reserve(pGeometry->mvVertexVec.size() * 10 * 3);
				for(size_t j=0; j < pGeometry->mvVertexVec.size(); ++j)
				{
					sData += pGeometry->mvVertexVec[j].norm.ToFileString()+" ";
				}
				pDataElem = CreateXMLChild(pVertexVecElem,"Normals");
				pVecText = CreateXMLTextChild(pDataElem,"Data");
				pVecText->SetValue(sData.c_str());

				/////////////////////////////////
				// UVs
				sData = "";
				sData.reserve(pGeometry->mvVertexVec.size() * 10 * 3);
				for(size_t j=0; j < pGeometry->mvVertexVec.size(); ++j)
				{
					sData += pGeometry->mvVertexVec[j].tex.ToFileString()+" ";
				}
				pDataElem = CreateXMLChild(pVertexVecElem,"UV");
				pVecText = CreateXMLTextChild(pDataElem,"Data");
				pVecText->SetValue(sData.c_str());

				/////////////////////////////////
				// Tangents
				sData = "";
				char sTemp[20];
				sData.reserve(pGeometry->mvVertexVec.size() * 10 * 4);
				for(size_t j=0; j < pGeometry->mvTangents.size(); ++j)
				{
				//	snprintf(sTemp,20,"%g ",pGeometry->mvTangents[j]);
					sData += sTemp;
				}
				pDataElem = CreateXMLChild(pVertexVecElem,"Tangents");
				CreateXMLTextChild(pDataElem,sData.c_str());
			}

			/////////////////////////////////
			// Extra vertex vec
			//if(false)
			{
				tinyxml2::XMLElement *pExtraVertexVecElem = CreateXMLChild(pGeometryElem,"ExtraVertexVec");
				pExtraVertexVecElem->SetAttribute("Size",(int)pGeometry->mvExtraVtxVec.size());
				
				tUIntVec vExtraNum; vExtraNum.resize(pGeometry->mvExtraVtxVec.size(),0);

				////////////////////////////
				//Extra vertices
				tString sData = ""; sData.reserve(pGeometry->mvExtraVtxVec.size()*4*4 *4);
				int lExtraCount =0;
				tinyxml2::XMLElement *pExtraElem = CreateXMLChild(pExtraVertexVecElem,"ExtraVertex");
				for(size_t idx=0; idx < pGeometry->mvExtraVtxVec.size(); ++idx)
				{
					tColladaExtraVtxList *pList = &pGeometry->mvExtraVtxVec[idx];
										
					tColladaExtraVtxListIt it = pList->begin();
					for(; it != pList->end(); ++it)
					{
						cColladaExtraVtx& extra = *it;
						sData +=	cString::ToString(extra.mlVtx) + " " +
									cString::ToString(extra.mlNorm) + " " +
									cString::ToString(extra.mlTex) + " " +
									cString::ToString(extra.mlNewVtx) + " ";
						++lExtraCount;
						++vExtraNum[idx];
					}
					
				}
				CreateXMLTextChild(pExtraElem,sData.c_str());
				pExtraElem->SetAttribute("Size",(int)lExtraCount);

				////////////////////////////
				//Extra num
				sData = ""; sData.reserve(pGeometry->mvExtraVtxVec.size()*4 *4);
				tinyxml2::XMLElement *pExtraNumElem = CreateXMLChild(pExtraVertexVecElem,"ExtraNum");
				for(size_t idx=0; idx < pGeometry->mvExtraVtxVec.size(); ++idx)
				{
					sData += cString::ToString((int)vExtraNum[idx]) + " ";
				}
				CreateXMLTextChild(pExtraNumElem,sData.c_str());
			}
		}

	}

	//-----------------------------------------------------------------------

	static void SaveIterativeNode(tinyxml2::XMLElement *apParentElem, cColladaNode *apParentNode)
	{
		tColladaNodeListIt it = apParentNode->mlstChildren.begin();
		for(; it != apParentNode->mlstChildren.end(); ++it)
		{
			cColladaNode *pNode = *it;
			tinyxml2::XMLElement *pNodeElem =  CreateXMLChild(apParentElem,"Node");

			pNodeElem->SetAttribute("Id",pNode->msId.c_str());
			pNodeElem->SetAttribute("Sid",pNode->msSid.c_str());
			pNodeElem->SetAttribute("Name",pNode->msName.c_str());
			pNodeElem->SetAttribute("Type",pNode->msType.c_str());

			pNodeElem->SetAttribute("InstanceMaterial",pNode->msInstanceMaterial.c_str());

			pNodeElem->SetAttribute("Source",pNode->msSource.c_str());
			pNodeElem->SetAttribute("SourceIsFile",pNode->mbSourceIsFile?"true":"false");

			pNodeElem->SetAttribute("Transform", pNode->m_mtxTransform.ToFileString().c_str());
			pNodeElem->SetAttribute("WorldTransform", pNode->m_mtxWorldTransform.ToFileString().c_str());
			
			pNodeElem->SetAttribute("Scale", pNode->mvScale.ToFileString().c_str());

			pNodeElem->SetAttribute("Count", pNode->mlCount);


			tinyxml2::XMLElement *pTransformRootElem =  CreateXMLChild(pNodeElem,"TransformRoot");
			//tColladaTransformList mlstTransforms;
			tColladaTransformListIt transIt = pNode->mlstTransforms.begin();
			for(; transIt != pNode->mlstTransforms.end(); ++transIt)
			{
				cColladaTransform & transform = *transIt;
				tinyxml2::XMLElement *pTransformElem =  CreateXMLChild(pTransformRootElem,"Transform");

				pTransformElem->SetAttribute("Sid",transform.msSid.c_str());
				pTransformElem->SetAttribute("Type",transform.msType.c_str());
				
				tString sValues = "";
				for(size_t i=0; i< transform.mvValues.size(); ++i) 
					sValues += cString::ToString(transform.mvValues[i]) + " ";
				
				pTransformElem->SetAttribute("Values",sValues.c_str());
			}
			
			SaveIterativeNode(pNodeElem,pNode);
		}
	}

	static void SaveScene(tinyxml2::XMLElement *apRootElem, cColladaScene *apColladaScene)
	{
		tinyxml2::XMLElement *pSceneElem =  CreateXMLChild(apRootElem,"Scene");

		pSceneElem->SetAttribute("StartTime",cString::ToString(apColladaScene->mfStartTime).c_str());
		pSceneElem->SetAttribute("EndTime",cString::ToString(apColladaScene->mfEndTime).c_str());
		pSceneElem->SetAttribute("DeltaTime",cString::ToString(apColladaScene->mfDeltaTime).c_str());
		
		tinyxml2::XMLElement *pSceneRootElem =  CreateXMLChild(pSceneElem,"Root");
		
		SaveIterativeNode(pSceneRootElem,&apColladaScene->mRoot);
	}

	//-----------------------------------------------------------------------
	
	//////////////////////////////////////////////////////////////////////////
	// LOAD COLLADA DATA
	//////////////////////////////////////////////////////////////////////////

	//-----------------------------------------------------------------------

	static void LoadImageVec(tinyxml2::XMLElement *apRootElem, tColladaImageVec *apColladaImageVec);
	static void LoadTextureVec(tinyxml2::XMLElement *apRootElem, tColladaTextureVec *apColladaTextureVec);
	static void LoadMaterialVec(tinyxml2::XMLElement *apRootElem, tColladaMaterialVec *apColladaMaterialVec);
	static void LoadLightVec(tinyxml2::XMLElement *apRootElem, tColladaLightVec *apColladaLightVec);
	static void LoadAnimationVec(tinyxml2::XMLElement *apRootElem, tColladaAnimationVec *apColladaAnimVec);
	static void LoadControllerVec(tinyxml2::XMLElement *apRootElem, tColladaControllerVec *apColladaControllerVec);
	static void LoadGeometryVec(tinyxml2::XMLElement *apRootElem, tColladaGeometryVec *apColladaGeometryVec);
	static void LoadScene(tinyxml2::XMLElement *apRootElem, cColladaScene *apColladaScene);

	bool cMeshLoaderCollada::LoadStructures(const tWString &asFile,
		tColladaImageVec *apColladaImageVec,
		tColladaTextureVec *apColladaTextureVec,
		tColladaMaterialVec *apColladaMaterialVec,
		tColladaLightVec *apColladaLightVec,
		tColladaGeometryVec *apColladaGeometryVec,
		tColladaControllerVec *apColladaControllerVec,
		tColladaAnimationVec *apColladaAnimVec,
		cColladaScene *apColladaScene)
	{
		//unsigned long lStartTime = mpSystem->GetLowLevel()->GetTime();

		FILE *pFile = cPlatform::OpenFile(asFile, _W("rb"));
		if(pFile==NULL) return false;

		tinyxml2::XMLDocument* pXmlDoc = hplNew(tinyxml2::XMLDocument, (true, tinyxml2::COLLAPSE_WHITESPACE) );
		if(pXmlDoc->LoadFile(pFile)!=tinyxml2::XML_SUCCESS)
		{
			Warning("Couldn't open XML file %s\n",cString::To8Char(asFile).c_str());
			fclose(pFile);
			hplDelete(pXmlDoc);
			return false;
		}
		fclose(pFile);

		//unsigned long lTime = mpSystem->GetLowLevel()->GetTime() - lStartTime;
		//Log("Loading cached collada XML file '%s' took: %d ms\n",asFile.c_str(),lTime);

		tinyxml2::XMLElement *pRootElem = pXmlDoc->RootElement();

		if(apColladaImageVec)		LoadImageVec(pRootElem,apColladaImageVec);
		if(apColladaTextureVec)		LoadTextureVec(pRootElem,apColladaTextureVec);
		if(apColladaMaterialVec)	LoadMaterialVec(pRootElem,apColladaMaterialVec);
		if(apColladaLightVec)		LoadLightVec(pRootElem,apColladaLightVec);
		if(apColladaAnimVec)		LoadAnimationVec(pRootElem,apColladaAnimVec);
		if(apColladaControllerVec)	LoadControllerVec(pRootElem,apColladaControllerVec);
		if(apColladaGeometryVec)	LoadGeometryVec(pRootElem,apColladaGeometryVec);
		if(apColladaScene)			LoadScene(pRootElem,apColladaScene);

		hplDelete(pXmlDoc);		
		return true;
	}

	//-----------------------------------------------------------------------

	static void LoadImageVec(tinyxml2::XMLElement *apRootElem, tColladaImageVec *apColladaImageVec)
	{
		tinyxml2::XMLElement *pImageRootElem = apRootElem->FirstChildElement("ImageRoot");
		
		int lSize = cString::ToInt(pImageRootElem->Attribute("Size"),0);

		apColladaImageVec->clear();
		apColladaImageVec->resize(lSize);
	
		int lCount =0;
		tinyxml2::XMLElement *pImageElem = pImageRootElem->FirstChildElement();
		for(; pImageElem != NULL; pImageElem = pImageElem->NextSiblingElement(),++lCount)
		{
			cColladaImage* pImage = &(*apColladaImageVec)[lCount];

			pImage->msId = pImageElem->Attribute("Id");
			pImage->msName = pImageElem->Attribute("Name");
			pImage->msSource = pImageElem->Attribute("Source");
		}
	}

	//-----------------------------------------------------------------------

	static void LoadTextureVec(tinyxml2::XMLElement *apRootElem, tColladaTextureVec *apColladaTextureVec)
	{
		tinyxml2::XMLElement *pTextureRootElem = apRootElem->FirstChildElement("TextureRoot");

		int lSize = cString::ToInt(pTextureRootElem->Attribute("Size"),0);

		apColladaTextureVec->clear();
		apColladaTextureVec->resize(lSize);

		int lCount =0;
		tinyxml2::XMLElement *pTextureElem = pTextureRootElem->FirstChildElement();
		for(; pTextureElem != NULL; pTextureElem = pTextureElem->NextSiblingElement(),++lCount)
		{
			cColladaTexture* pTexture = &(*apColladaTextureVec)[lCount];

			pTexture->msId = pTextureElem->Attribute("Id");
			pTexture->msName = pTextureElem->Attribute("Name");
			pTexture->msImage = pTextureElem->Attribute("Image");
		}
	}

	//-----------------------------------------------------------------------
	
	static void LoadMaterialVec(tinyxml2::XMLElement *apRootElem, tColladaMaterialVec *apColladaMaterialVec)
	{
		tinyxml2::XMLElement *pMaterialRootElem = apRootElem->FirstChildElement("MaterialRoot");

		int lSize = cString::ToInt(pMaterialRootElem->Attribute("Size"),0);

		apColladaMaterialVec->clear();
		apColladaMaterialVec->resize(lSize);

		int lCount =0;
		tinyxml2::XMLElement *pMaterialElem = pMaterialRootElem->FirstChildElement();
		for(; pMaterialElem != NULL; pMaterialElem = pMaterialElem->NextSiblingElement(),++lCount)
		{
			cColladaMaterial* pMaterial = &(*apColladaMaterialVec)[lCount];

			pMaterial->msId = pMaterialElem->Attribute("Id");
			pMaterial->msName = pMaterialElem->Attribute("Name");
			pMaterial->msTexture = pMaterialElem->Attribute("Texture");
			pMaterial->mDiffuseColor = cString::ToColor(pMaterialElem->Attribute("Color"),cColor(0,0));
		}
	}

	//-----------------------------------------------------------------------

	static void LoadControllerVec(tinyxml2::XMLElement *apRootElem, tColladaControllerVec *apColladaControllerVec)
	{
		tinyxml2::XMLElement *pControllerRootElem = apRootElem->FirstChildElement("ControllerRoot");

		int lSize = cString::ToInt(pControllerRootElem->Attribute("Size"),0);
		
		apColladaControllerVec->clear();
		apColladaControllerVec->resize(lSize);

		int lCount =0;
		tinyxml2::XMLElement *pControllerElem = pControllerRootElem->FirstChildElement();
		for(; pControllerElem != NULL; pControllerElem = pControllerElem->NextSiblingElement(),++lCount)
		{
			cColladaController* pController = &(*apColladaControllerVec)[lCount];

			pController->msId = pControllerElem->Attribute("Id");
			pController->msTarget = pControllerElem->Attribute("Target");

			pController->m_mtxBindShapeMatrix = cString::ToMatrixf(pControllerElem->Attribute("BindShapeMatrix"),cMatrixf::Identity);

			pController->mlJointPairIdx = cString::ToInt(pControllerElem->Attribute("JointPairIdx"),0);
			pController->mlWeightPairIdx = cString::ToInt(pControllerElem->Attribute("WeightPairIdx"),0);
			
			////////////////////////////
			//Joints
			{
				tinyxml2::XMLElement * pJointsElem = pControllerElem->FirstChildElement("Joints");
				int lSize = cString::ToInt(pJointsElem->Attribute("Size"),0);
				tinyxml2::XMLText *pJointsText = pJointsElem->FirstChild()->ToText();
                
				tString sData = pJointsText->Value();
                tString sSepp = " "; pController->mvJoints.reserve(lSize);
				cString::GetStringVec(sData,pController->mvJoints,&sSepp);
			}
			////////////////////////////
			//Weights
			{
				tinyxml2::XMLElement * pWeightsElem = pControllerElem->FirstChildElement("Weights");
				int lSize = cString::ToInt(pWeightsElem->Attribute("Size"),0);
				tinyxml2::XMLText *pWeightsText = pWeightsElem->FirstChild()->ToText();

				//tString sData = pWeightsText->Value();
				//tString sSepp = " "; pController->mvWeights.reserve(lSize);
				//cString::GetFloatVec(sData,pController->mvWeights,&sSepp);
				
				pController->mvWeights.resize(lSize);
				cString::FloatStringToArray(&pController->mvWeights[0],pWeightsText->Value(),lSize);
			}
			////////////////////////////
			//Matrices
			{
				tinyxml2::XMLElement * pMatricesElem = pControllerElem->FirstChildElement("Matrices");
				int lSize = cString::ToInt(pMatricesElem->Attribute("Size"),0);
				tinyxml2::XMLText *pMatricesText = pMatricesElem->FirstChild()->ToText();

				//tString sData = pMatricesText->Value();
				//tString sSepp = " "; 
				//cString::GetFloatVec(sData,vRawData,&sSepp);
				tFloatVec vRawData;vRawData.resize(lSize * 16);
				cString::FloatStringToArray(&vRawData[0],pMatricesText->Value(),lSize * 16);
				
				
				float *pData = &vRawData[0];
				pController->mvMatrices.reserve(lSize);
				for(int i=0; i< lSize; ++i)
				{
					pController->mvMatrices.push_back(
									cMatrixf(	pData[0],pData[1],pData[2],pData[3],
												pData[4],pData[5],pData[6],pData[7],
												pData[8],pData[9],pData[10],pData[11],
												pData[12],pData[13],pData[14],pData[15]));
					pData += 16;
				}
			}
			////////////////////////////
			//Pairs
			{
				tinyxml2::XMLElement * pPairVecElem = pControllerElem->FirstChildElement("PairVec");
				int lSize = cString::ToInt(pPairVecElem->Attribute("Size"),0);

				/////////////////////////////////////////
				// Get number of connections per pair
				tinyxml2::XMLElement* pPairNumElem = pPairVecElem->FirstChildElement("PairNum");
				tinyxml2::XMLText *pPairNumText = pPairNumElem->FirstChild()->ToText();

				tUIntVec vPairNum; vPairNum.resize(lSize);
				//tString sData = pPairNumText->Value();
				//tString sSepp = " ";
				//cString::GetIntVec(sData,vPairNum,&sSepp);
				cString::UIntStringToArray(&vPairNum[0],pPairNumText->Value(),lSize);

				/////////////////////////////////////////
				// Get pairs
				tinyxml2::XMLElement* pPairsElem = pPairVecElem->FirstChildElement("Pairs");
				int lPairSize = cString::ToInt(pPairsElem->Attribute("Size"),0);
				tinyxml2::XMLText *pPairsText = pPairsElem->FirstChild()->ToText();

				tUIntVec vPairs; vPairs.resize(lPairSize*2);
				//sData = pPairsText->Value();
				//cString::GetIntVec(sData,vPairs,&sSepp);
				cString::UIntStringToArray(&vPairs[0],pPairsText->Value(),lPairSize*2);


				pController->mvPairs.resize(lSize);

				int lPairCount =0;
				for(int i=0; i<lSize; ++i)
				{
					for(unsigned int j=0; j< vPairNum[i]; ++j)
					{
						pController->mvPairs[i].push_back(
										cColladaJointPair(vPairs[lPairCount],vPairs[lPairCount+1]));
						lPairCount+=2;
					}
				}
			}
		}
	}
	//-----------------------------------------------------------------------

	static void LoadLightVec(tinyxml2::XMLElement *apRootElem, tColladaLightVec *apColladaLightVec)
	{
		tinyxml2::XMLElement *pLightRootElem = apRootElem->FirstChildElement("LightRoot");

		int lSize = cString::ToInt(pLightRootElem->Attribute("Size"),0);

		apColladaLightVec->clear();
		apColladaLightVec->resize(lSize);

		int lCount =0;
		tinyxml2::XMLElement *pLightElem = pLightRootElem->FirstChildElement();
		for(; pLightElem != NULL; pLightElem = pLightElem->NextSiblingElement(),++lCount)
		{
			cColladaLight* pLight = &(*apColladaLightVec)[lCount];

			pLight->msId = pLightElem->Attribute("Id");
			pLight->msName = pLightElem->Attribute("Name");
			pLight->msType =pLightElem->Attribute("Type");
			pLight->mfAngle = cString::ToFloat(pLightElem->Attribute("Angle"),0);
			pLight->mDiffuseColor = cString::ToColor(pLightElem->Attribute("Color"),cColor(0,0));
		}
	}

	//-----------------------------------------------------------------------

	static void LoadAnimationVec(tinyxml2::XMLElement *apRootElem, tColladaAnimationVec *apColladaAnimationVec)
	{
		tinyxml2::XMLElement *pAnimationRootElem = apRootElem->FirstChildElement("AnimationRoot");

		int lSize = cString::ToInt(pAnimationRootElem->Attribute("Size"),0);

		apColladaAnimationVec->clear();
		apColladaAnimationVec->resize(lSize);

		int lCount =0;
		tinyxml2::XMLElement *pAnimationElem = pAnimationRootElem->FirstChildElement();
		for(; pAnimationElem != NULL; pAnimationElem = pAnimationElem->NextSiblingElement(),++lCount)
		{
			cColladaAnimation* pAnimation = &(*apColladaAnimationVec)[lCount];

			pAnimation->msId = pAnimationElem->Attribute("Id");
			pAnimation->msTargetNode = pAnimationElem->Attribute("TargetNode");
			
			/////////////////////////////////////
			//Channels
			{
				tinyxml2::XMLElement *pChannelVecElem = pAnimationElem->FirstChildElement("ChannelVec");
				int lSize = cString::ToInt(pChannelVecElem->Attribute("Size"),0);
				pAnimation->mvChannels.resize(lSize);

				int idx=0;
				tinyxml2::XMLElement *pChannelElem = pChannelVecElem->FirstChildElement("Channel");
				for(; pChannelElem != NULL; pChannelElem = pChannelElem->NextSiblingElement("Channel"),++idx)
				{
					cColladaChannel *pChannel = &pAnimation->mvChannels[idx];
					
					pChannel->msId = pChannelElem->Attribute("Id");
					pChannel->msTarget = pChannelElem->Attribute("Target");
					pChannel->msSource = pChannelElem->Attribute("Source");
				}
			}

			/////////////////////////////////////
			//Samplers
			{
				tinyxml2::XMLElement *pSamplerVecElem = pAnimationElem->FirstChildElement("SamplerVec");
				int lSize = cString::ToInt(pSamplerVecElem->Attribute("Size"),0);
				pAnimation->mvSamplers.resize(lSize);

				int idx=0;
				tinyxml2::XMLElement *pSamplerElem = pSamplerVecElem->FirstChildElement("Sampler");
				for(; pSamplerElem != NULL; pSamplerElem = pSamplerElem->NextSiblingElement("Sampler"),++idx)
				{
					cColladaSampler *pSampler = &pAnimation->mvSamplers[idx];

					pSampler->msId = pSamplerElem->Attribute("Id");
					pSampler->msTarget = pSamplerElem->Attribute("Target");
					pSampler->msTimeArray = pSamplerElem->Attribute("TimeArray");
					pSampler->msValueArray = pSamplerElem->Attribute("ValueArray");
				}
			}

			/////////////////////////////////////
			//Sources
			{
				tinyxml2::XMLElement *pSourceVecElem = pAnimationElem->FirstChildElement("SourceVec");
				int lSize = cString::ToInt(pSourceVecElem->Attribute("Size"),0);
				pAnimation->mvSources.resize(lSize);

				int idx=0;
				tinyxml2::XMLElement *pSourceElem = pSourceVecElem->FirstChildElement("Source");
				for(; pSourceElem != NULL; pSourceElem = pSourceElem->NextSiblingElement("Source"),++idx)
				{
					cColladaAnimSource *pSource = &pAnimation->mvSources[idx];

					pSource->msId = pSourceElem->Attribute("Id");
					tString sData = pSourceElem->Attribute("Values");
					tString sSepp = " ";
					pSource->mvValues.reserve(3);

					cString::GetFloatVec(sData,pSource->mvValues,&sSepp);
				}
			}
            
		}
	}

	//-----------------------------------------------------------------------

	static void LoadGeometryVec(tinyxml2::XMLElement *apRootElem, tColladaGeometryVec *apColladaGeometryVec)
	{
		tinyxml2::XMLElement *pGeometryRootElem = apRootElem->FirstChildElement("GeometryRoot");
		int lSize = cString::ToInt(pGeometryRootElem->Attribute("Size"),0);
		
		apColladaGeometryVec->clear();
		apColladaGeometryVec->resize(lSize);

		int lCount =0;
		tinyxml2::XMLElement *pGeometryElem = pGeometryRootElem->FirstChildElement();
		for(; pGeometryElem != NULL; pGeometryElem = pGeometryElem->NextSiblingElement(),++lCount)
		{
			cColladaGeometry* pGeometry = &(*apColladaGeometryVec)[lCount];

			pGeometry->msId = pGeometryElem->Attribute("Id");
			pGeometry->msName = pGeometryElem->Attribute("Name");
			pGeometry->msMaterial = pGeometryElem->Attribute("Material");

			// XXX
			if(pGeometry->msName.empty())
			{
				pGeometry->msName = pGeometry->msId;
				Warning("ColladaGeometry object with id '%s' has empty name! Setting id as name\n", pGeometry->msId.c_str());
			}

			/////////////////////////////////
			// Index Vec
			{
				tinyxml2::XMLElement *pIndexVecElem = pGeometryElem->FirstChildElement("IndexVec");
				int lSize = cString::ToInt(pIndexVecElem->Attribute("Size"),0);
				
				if(lSize>0)
				{
					tinyxml2::XMLElement *pIndicesDataElem = pIndexVecElem->FirstChildElement("Indices");
					tinyxml2::XMLText *pVecText = pIndicesDataElem->FirstChild()->ToText();
					
					pGeometry->mvIndexVec.resize(lSize);

					cString::UIntStringToArray(&pGeometry->mvIndexVec[0],pVecText->Value(),lSize);
					
					//tString sData = pVecText->Value();
					//tString sSepp = " ";
					//cString::GetUIntVec(sData,pGeometry->mvIndexVec,&sSepp);
				}
			}

			/////////////////////////////////
			// Vertex vec
			{
				tinyxml2::XMLElement *pVertexVecElem = pGeometryElem->FirstChildElement("VertexVec");
				int lSize = cString::ToInt(pVertexVecElem->Attribute("Size"),0);
				
				if(lSize>0)
				{
					pGeometry->mvVertexVec.resize(lSize);

					tFloatVec vRawData; 
					vRawData.resize(lSize * 3);
					
					/////////////////////////////////
					// Positions
					{
						tinyxml2::XMLElement *pDataElem = pVertexVecElem->FirstChildElement("Positions");
						tinyxml2::XMLText *pVecText = pDataElem->FirstChild()->ToText();

						//tString sData = pVecText->Value();
						//tString sSepp = " ";
						//tFloatVec vRawData; vRawData.reserve(lSize * 3);
						//cString::GetFloatVec(sData,vRawData,&sSepp);

						cString::FloatStringToArray(&vRawData[0],pVecText->Value(),lSize * 3);
						
						float *pData = &vRawData[0];
						for(int i=0; i<lSize; ++i)
						{
							cVector3f &vPos = pGeometry->mvVertexVec[i].pos;
							vPos.x = pData[0]; vPos.y = pData[1]; vPos.z = pData[2];
							pData += 3;
						}
					}

					/////////////////////////////////
					// Normals
					{
						tinyxml2::XMLElement *pDataElem = pVertexVecElem->FirstChildElement("Normals");
						tinyxml2::XMLText *pVecText = pDataElem->FirstChild()->ToText();

						//tString sData = pVecText->Value();
						//tString sSepp = " ";
						//tFloatVec vRawData; vRawData.reserve(lSize * 3);
						//cString::GetFloatVec(sData,vRawData,&sSepp);

						cString::FloatStringToArray(&vRawData[0],pVecText->Value(),lSize * 3);

						float *pData = &vRawData[0];
						for(int i=0; i<lSize; ++i)
						{
							cVector3f &vPos = pGeometry->mvVertexVec[i].norm;
							vPos.x = pData[0]; vPos.y = pData[1]; vPos.z = pData[2];
							pData += 3;
						}
					}

					/////////////////////////////////
					// UVs
					{
						tinyxml2::XMLElement *pDataElem = pVertexVecElem->FirstChildElement("UV");
						tinyxml2::XMLText *pVecText = pDataElem->FirstChild()->ToText();

						//tString sData = pVecText->Value();
						//tString sSepp = " ";
						//tFloatVec vRawData; vRawData.reserve(lSize * 3);
						//cString::GetFloatVec(sData,vRawData,&sSepp);

						cString::FloatStringToArray(&vRawData[0],pVecText->Value(),lSize * 3);

						float *pData = &vRawData[0];
						for(int i=0; i<lSize; ++i)
						{
							cVector3f &vPos = pGeometry->mvVertexVec[i].tex;
							vPos.x = pData[0]; vPos.y = pData[1]; vPos.z = pData[2];
							pData += 3;
						}
					}

					/////////////////////////////////
					// Tangents
					{
						tinyxml2::XMLElement *pDataElem = pVertexVecElem->FirstChildElement("Tangents");
						tinyxml2::XMLText *pVecText = pDataElem->FirstChild()->ToText();

						//tString sData = pVecText->Value();
						//tString sSepp = " ";
						//tFloatVec vRawData; vRawData.reserve(lSize * 3);
						//cString::GetFloatVec(sData,vRawData,&sSepp);
						
						pGeometry->mvTangents.resize(lSize*4);
						cString::FloatStringToArray(&pGeometry->mvTangents[0],pVecText->Value(),lSize * 4);
					}
				}

			}

			/////////////////////////////////
			// Extra vertex vec
			{
				tinyxml2::XMLElement *pVertexVecElem = pGeometryElem->FirstChildElement("ExtraVertexVec");
				int lSize = cString::ToInt(pVertexVecElem->Attribute("Size"),0);

				if(lSize>0)
				{
					pGeometry->mvExtraVtxVec.resize(lSize);
					tUIntVec mvExtraNum; mvExtraNum.resize(lSize);
					tUIntVec mvExtraVtx;

					//////////////////////////////////////////////
					// Extra num
					{
						tinyxml2::XMLElement *pExtraNumElem = pVertexVecElem->FirstChildElement("ExtraNum");
						tinyxml2::XMLText* pExtraNumText = pExtraNumElem->FirstChild()->ToText(); 
                    
						cString::UIntStringToArray(&mvExtraNum[0],pExtraNumText->Value(),lSize);
					}
					
					//////////////////////////////////////////////
					// Extra vertex
					{
						tinyxml2::XMLElement *pExtraVertexElem = pVertexVecElem->FirstChildElement("ExtraVertex");
						tinyxml2::XMLText* pExtraVertexText = pExtraVertexElem->FirstChild()->ToText();
						int lExtraVtxSize = cString::ToInt(pExtraVertexElem->Attribute("Size"),0);
						 mvExtraVtx.resize(lExtraVtxSize*4);

						cString::UIntStringToArray(&mvExtraVtx[0],pExtraVertexText->Value(),lExtraVtxSize*4);
					}

					unsigned int *pData = &mvExtraVtx[0];
					for(int idx=0; idx< lSize; ++idx)
					{
						tColladaExtraVtxList *pList = &pGeometry->mvExtraVtxVec[idx];
						
						for(unsigned int j=0; j < mvExtraNum[idx]; ++j)
						{
							pList->push_back(cColladaExtraVtx(pData[0],pData[1],pData[2],pData[3]));
							pData += 4;
						}
					}
				}
			}
		}
	}
	
	//-----------------------------------------------------------------------

	static void LoadIterativeNode(tinyxml2::XMLElement *apNodeElem,cColladaNode *apParentNode, cColladaScene *apColladaScene)
	{
		cColladaNode *pNode = apParentNode->CreateChild();
		apColladaScene->mlstNodes.push_back(pNode);

		pNode->msId = apNodeElem->Attribute("Id");
		pNode->msSid = cString::ToString(apNodeElem->Attribute("Sid"),pNode->msId);
		pNode->msName = cString::ToString(apNodeElem->Attribute("Name"), pNode->msId);
		pNode->msType = apNodeElem->Attribute("Type");
		
		pNode->msInstanceMaterial = cString::ToString(apNodeElem->Attribute("InstanceMaterial"),"");

		pNode->msSource  = apNodeElem->Attribute("Source");
		pNode->mbSourceIsFile = cString::ToBool(apNodeElem->Attribute("SourceIsFile"),false);

		pNode->m_mtxTransform = cString::ToMatrixf(apNodeElem->Attribute("Transform"),cMatrixf::Identity);
		pNode->m_mtxWorldTransform = cString::ToMatrixf(apNodeElem->Attribute("WorldTransform"),cMatrixf::Identity);

		pNode->mvScale = cString::ToVector3f(apNodeElem->Attribute("Scale"),1);
		pNode->mlCount = cString::ToInt(apNodeElem->Attribute("Count"),1);

		tinyxml2::XMLElement *pTransformRootElem = apNodeElem->FirstChildElement("TransformRoot");
		tinyxml2::XMLElement *pTransformElem = pTransformRootElem->FirstChildElement("Transform");
		for(; pTransformElem != NULL; pTransformElem = pTransformElem->NextSiblingElement("Transform"))
		{
			pNode->mlstTransforms.push_back(cColladaTransform());
			cColladaTransform &transform = pNode->mlstTransforms.back();
			
			transform.msSid =  pTransformElem->Attribute("Sid");
			transform.msType =  pTransformElem->Attribute("Type");
			
			tString sData = pTransformElem->Attribute("Values");
			tString sSepp =" ";
			cString::GetFloatVec(sData,transform.mvValues,&sSepp);
		}

		tinyxml2::XMLElement *pChildElem = apNodeElem->FirstChildElement("Node");
		for(; pChildElem != NULL; pChildElem = pChildElem->NextSiblingElement("Node"))
		{
			LoadIterativeNode(pChildElem,pNode,apColladaScene);
		}
	}

	//-----------------------------------------------------------------------

	static void LoadScene(tinyxml2::XMLElement *apRootElem, cColladaScene *apColladaScene)
	{
		tinyxml2::XMLElement *pSceneElem = apRootElem->FirstChildElement("Scene");
		
		//Delete all nodes.
		apColladaScene->ResetNodes();
        
		apColladaScene->mfStartTime = cString::ToFloat(pSceneElem->Attribute("StartTime"),0);
		apColladaScene->mfEndTime = cString::ToFloat(pSceneElem->Attribute("EndTime"),0);
		apColladaScene->mfDeltaTime = cString::ToFloat(pSceneElem->Attribute("DeltaTime"),0);

        tinyxml2::XMLElement *pSceneRootElem = pSceneElem->FirstChildElement("Root");
		tinyxml2::XMLElement *pNodeElem = pSceneRootElem->FirstChildElement("Node");
		for(; pNodeElem != NULL; pNodeElem = pNodeElem->NextSiblingElement("Node"))
		{
			LoadIterativeNode(pNodeElem,&apColladaScene->mRoot,apColladaScene);
		}
	}



	//-----------------------------------------------------------------------
}
