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

//////////////////////////////////////////////////////////////////////////
// MapDelta -- author and inspect .map_delta patch files.
//
//   mapdelta diff  <base.map> <modified.map> -o <out.map_delta>
//                  [--name NAME] [--priority N] [--target PATH]
//   mapdelta apply <base.map> <a.map_delta> [b.map_delta ...] -o <out.map>
//   mapdelta show  <x.map_delta>
//
// 'diff' compares two maps and writes the operations needed to turn the first
// into the second. 'apply' runs those operations through the exact same code
// the engine uses at load time, so it doubles as a check of the runtime path.
//
// A delta holds only the author's changes -- op keys, new values and new objects
// -- never base map content, which is what makes it the redistributable artifact.
// The full map that 'apply' writes is derived from the base and is not.
//
// Note: HPL2's entry wrapper chdirs to the data directory unless -cwd is passed,
// so either pass -cwd or use absolute paths. Paths may not contain spaces (the
// command line arrives pre-joined).
//////////////////////////////////////////////////////////////////////////

#include "resources/XmlDelta.h"
#include "resources/XmlHelper.h"
#include "system/LowLevelSystem.h"
#include "system/Platform.h"
#include "system/String.h"

#include <tinyxml2.h>

#include <map>
#include <set>
#include <string>
#include <vector>

using namespace hpl;

//------------------------------------------

// Object categories a delta can express, in the order they are emitted.
// "Misc" (<Compound>) is skipped: cWorldLoaderHplMap never reads it.
// "StaticObjectCombos" is skipped: it is derived data keyed on static object IDs.
static const char* gvCategories[] = { "StaticObjects", "Primitives", "Decals", "Entities", NULL };

// Attributes that carry no meaning for the game loader, or that are indices into
// per-file tables the delta must not depend on. Excluded from comparison, and
// stripped from added objects.
static const char* gvIgnoredAttributes[] = { "Group", "GUID", "FileIndex", "MaterialIndex", NULL };

// The engine writes floats with "%g" -- 6 significant digits -- so the
// representation error grows with magnitude: ~5e-4 at |v|=100, ~5e-3 at
// |v|=1000. A purely absolute epsilon therefore reports phantom edits on the
// large world coordinates real maps use. Scale it, keeping an absolute floor
// for values near zero.
static const float gfFloatEpsilon = 1e-4f;
static const float gfFloatEpsilonRelative = 1e-5f;

// When false, Remove/Modify operations carry no Name witness. The witness is
// the only base-derived text a delta contains; dropping it makes the delta
// purely a record of the author's own changes, at the cost of the check that
// catches a base map having moved under the delta.
static bool gbWriteWitness = true;

//------------------------------------------

typedef std::map<tString, tString> tAttrMap;
typedef std::map<int, tinyxml2::XMLElement*> tObjectMap;

//------------------------------------------

static bool IsIgnoredAttribute(const tString& asName)
{
	for(int i=0; gvIgnoredAttributes[i]; ++i)
		if(asName == gvIgnoredAttributes[i]) return true;
	return false;
}

//------------------------------------------

// Collapse an asset path to its root-relative form.
//
// The editor sometimes writes a path relative to the wrong root, producing long
// "../../.." prefixes (66 levels deep in real maps). The engine resolves assets
// by bare filename through cFileSearcher, so these still load and the
// corruption goes unnoticed -- but it is the same asset, and a delta must
// neither report it as a change nor propagate it.
static tString NormalizeAssetPath(const tString& asValue)
{
	if(asValue.find('/')==tString::npos && asValue.find('\\')==tString::npos) return asValue;

	tString sSepp = "/\\";
	tStringVec vParts;
	cString::GetStringVec(asValue, vParts, &sSepp);

	tString sOut = "";
	for(size_t i=0; i<vParts.size(); ++i)
	{
		if(vParts[i]=="" || vParts[i]=="." || vParts[i]=="..") continue;
		if(sOut!="") sOut += "/";
		sOut += vParts[i];
	}

	return sOut;
}

//------------------------------------------

static bool IsNumeric(const tString& asToken, float& afOut)
{
	if(asToken=="") return false;

	char* pEnd = NULL;
	afOut = strtof(asToken.c_str(), &pEnd);
	return pEnd!=NULL && *pEnd=='\0';
}

// Attribute values round-trip through the editor's float formatting, so "0" and
// "1e-011" and "1.00001" must not read as edits. Compare numerically when both
// sides are numeric tuples of the same arity, textually otherwise.
static bool ValuesEqual(const tString& asA, const tString& asB)
{
	if(asA==asB) return true;

	//////////////////////////////////////
	// Asset paths compare by their root-relative form.
	if(	asA.find('/')!=tString::npos || asA.find('\\')!=tString::npos ||
		asB.find('/')!=tString::npos || asB.find('\\')!=tString::npos)
	{
		return NormalizeAssetPath(asA) == NormalizeAssetPath(asB);
	}

	tStringVec vA, vB;
	cString::GetStringVec(asA, vA);
	cString::GetStringVec(asB, vB);

	if(vA.empty() || vA.size()!=vB.size()) return false;

	for(size_t i=0; i<vA.size(); ++i)
	{
		float fA, fB;
		if(IsNumeric(vA[i], fA)==false || IsNumeric(vB[i], fB)==false) return false;

		float fMagnitude = fabs(fA) > fabs(fB) ? fabs(fA) : fabs(fB);
		float fTolerance = gfFloatEpsilonRelative * fMagnitude;
		if(fTolerance < gfFloatEpsilon) fTolerance = gfFloatEpsilon;

		if(fabs(fA-fB) > fTolerance) return false;
	}

	return true;
}

//------------------------------------------

//////////////////////////////////////////////////////////////////////////
// MAP READING
//////////////////////////////////////////////////////////////////////////

//------------------------------------------

// One side of a diff: the parsed map plus the lookups the comparison needs.
class cMapSide
{
public:
	bool Load(const tWString& asPath)
	{
		msPath = asPath;

		if(LoadXmlFile(mDoc, asPath)==false)
		{
			printf("ERROR: could not parse '%s'\n", cString::To8Char(asPath).c_str());
			return false;
		}

		mpMapData = mDoc.RootElement() ? mDoc.RootElement()->FirstChildElement("MapData") : NULL;
		if(mpMapData==NULL)
		{
			printf("ERROR: '%s' has no <MapData> element\n", cString::To8Char(asPath).c_str());
			return false;
		}

		mpContents = mpMapData->FirstChildElement("MapContents");
		if(mpContents==NULL)
		{
			printf("ERROR: '%s' has no <MapContents> element\n", cString::To8Char(asPath).c_str());
			return false;
		}

		LoadFileIndex("FileIndex_StaticObjects", mvStaticObjectFiles);
		LoadFileIndex("FileIndex_Entities", mvEntityFiles);
		LoadFileIndex("FileIndex_Decals", mvDecalFiles);

		return true;
	}

	//////////////////////////////////////
	// All objects of one category, by ID.
	void GetObjects(const tString& asCategory, tObjectMap& aOut)
	{
		tinyxml2::XMLElement* pCat = mpContents->FirstChildElement(asCategory.c_str());
		if(pCat==NULL) return;

		for(tinyxml2::XMLElement* pObj = pCat->FirstChildElement(); pObj; pObj = pObj->NextSiblingElement())
		{
			int lID = GetAttributeInt(pObj, "ID", -1);
			if(lID<0)
			{
				printf("WARNING: %s object '%s' in '%s' has no ID, skipping\n", asCategory.c_str(),
						GetAttributeString(pObj, "Name", "?").c_str(), cString::To8Char(msPath).c_str());
				continue;
			}
			aOut[lID] = pObj;
		}
	}

	//////////////////////////////////////
	// Comparable attribute set: index attributes replaced by the literal path
	// they resolve to, editor bookkeeping dropped.
	void GetNormalizedAttributes(tinyxml2::XMLElement* apObj, const tString& asCategory, tAttrMap& aOut)
	{
		for(const tinyxml2::XMLAttribute* pAttr = apObj->FirstAttribute(); pAttr; pAttr = pAttr->Next())
		{
			if(IsIgnoredAttribute(pAttr->Name())) continue;
			aOut[pAttr->Name()] = pAttr->Value();
		}

		tString sPathAttr, sPath;
		if(ResolveFilePath(apObj, asCategory, sPathAttr, sPath)) aOut[sPathAttr] = sPath;
	}

	//////////////////////////////////////
	// The literal-path attribute an object's file index stands for, matching what
	// each loader in cWorldLoaderHplMap falls back to when the index is absent.
	bool ResolveFilePath(tinyxml2::XMLElement* apObj, const tString& asCategory, tString& asAttrName, tString& asPath)
	{
		const tStringVec* pTable = NULL;
		tString sIndexAttr;

		if(asCategory=="StaticObjects")	{ pTable = &mvStaticObjectFiles; sIndexAttr = "FileIndex";     asAttrName = "MeshFilename"; }
		else if(asCategory=="Entities")	{ pTable = &mvEntityFiles;       sIndexAttr = "FileIndex";     asAttrName = "Filename"; }
		else if(asCategory=="Decals")	{ pTable = &mvDecalFiles;        sIndexAttr = "MaterialIndex"; asAttrName = "Material"; }
		else return false;

		int lIndex = GetAttributeInt(apObj, sIndexAttr.c_str(), -1);
		if(lIndex<0) return false;

		if(lIndex >= (int)pTable->size())
		{
			printf("WARNING: '%s' has %s %d, out of range of the file index table\n",
					GetAttributeString(apObj, "Name", "?").c_str(), sIndexAttr.c_str(), lIndex);
			return false;
		}

		asPath = (*pTable)[lIndex];
		return true;
	}

	tinyxml2::XMLElement* GetMapData() { return mpMapData; }
	const tWString& GetPath() const { return msPath; }

private:
	void LoadFileIndex(const char* asElementName, tStringVec& avOut)
	{
		tinyxml2::XMLElement* pIndex = mpContents->FirstChildElement(asElementName);
		if(pIndex==NULL) return;

		for(tinyxml2::XMLElement* pFile = pIndex->FirstChildElement(); pFile; pFile = pFile->NextSiblingElement())
		{
			int lID = GetAttributeInt(pFile, "Id", -1);
			if(lID<0) continue;

			if((int)avOut.size() <= lID) avOut.resize(lID+1);
			avOut[lID] = GetAttributeString(pFile, "Path", "");
		}
	}

	tWString msPath;
	tinyxml2::XMLDocument mDoc;

	tinyxml2::XMLElement* mpMapData;
	tinyxml2::XMLElement* mpContents;

	tStringVec mvStaticObjectFiles;
	tStringVec mvEntityFiles;
	tStringVec mvDecalFiles;
};

//------------------------------------------

static void GetUserVariables(tinyxml2::XMLElement* apObj, tAttrMap& aOut)
{
	tinyxml2::XMLElement* pVars = apObj->FirstChildElement("UserVariables");
	if(pVars==NULL) pVars = apObj->FirstChildElement("UserDefinedVariables");
	if(pVars==NULL) return;

	for(tinyxml2::XMLElement* pVar = pVars->FirstChildElement("Var"); pVar; pVar = pVar->NextSiblingElement("Var"))
	{
		tString sName = GetAttributeString(pVar, "Name", "");
		if(sName!="") aOut[sName] = GetAttributeString(pVar, "Value", "");
	}
}

//------------------------------------------

//////////////////////////////////////////////////////////////////////////
// DIFF
//////////////////////////////////////////////////////////////////////////

//------------------------------------------

// Copies an object from the modified map into an <Add> block: strips the map's
// own bookkeeping and swaps file indices for the literal paths the loader
// accepts, so the added object does not depend on the base map's index tables.
static void AppendAddedObject(	tinyxml2::XMLElement* apAddElem, tinyxml2::XMLElement* apObj,
								const tString& asCategory, cMapSide& aModified)
{
	tinyxml2::XMLDocument* pDoc = apAddElem->GetDocument();
	tinyxml2::XMLElement* pCopy = pDoc->NewElement(apObj->Value());
	apAddElem->InsertEndChild(pCopy);

	for(const tinyxml2::XMLAttribute* pAttr = apObj->FirstAttribute(); pAttr; pAttr = pAttr->Next())
	{
		// ID is assigned by the applier, from a range that cannot collide with
		// the base map's own IDs.
		if(tString(pAttr->Name())=="ID" || IsIgnoredAttribute(pAttr->Name())) continue;
		pCopy->SetAttribute(pAttr->Name(), NormalizeAssetPath(pAttr->Value()).c_str());
	}

	tString sPathAttr, sPath;
	if(aModified.ResolveFilePath(apObj, asCategory, sPathAttr, sPath))
		SetAttributeString(pCopy, sPathAttr, sPath);

	//////////////////////////////////////
	// Carry the variable block across verbatim. <DecalMesh> is deliberately not
	// copied: it is baked geometry the loader rebuilds from the transform.
	tinyxml2::XMLElement* pVars = apObj->FirstChildElement("UserVariables");
	if(pVars) pCopy->InsertEndChild(pVars->DeepClone(pDoc));
}

//------------------------------------------

static int DiffCategory(	const tString& asCategory, cMapSide& aBase, cMapSide& aModified,
							tinyxml2::XMLElement* apDeltaRoot)
{
	tObjectMap mapBase, mapModified;
	aBase.GetObjects(asCategory, mapBase);
	aModified.GetObjects(asCategory, mapModified);

	tinyxml2::XMLDocument* pDoc = apDeltaRoot->GetDocument();
	int lOps =0;

	////////////////////////////////////
	// Objects the modification deleted
	for(tObjectMap::iterator it = mapBase.begin(); it != mapBase.end(); ++it)
	{
		if(mapModified.count(it->first)) continue;

		tinyxml2::XMLElement* pOp = pDoc->NewElement("Remove");
		SetAttributeString(pOp, "Category", asCategory);
		SetAttributeInt(pOp, "ID", it->first);
		if(gbWriteWitness) SetAttributeString(pOp, "Name", GetAttributeString(it->second, "Name", ""));
		apDeltaRoot->InsertEndChild(pOp);
		++lOps;
	}

	////////////////////////////////////
	// Objects that survived, with changed attributes or variables
	for(tObjectMap::iterator it = mapBase.begin(); it != mapBase.end(); ++it)
	{
		tObjectMap::iterator itMod = mapModified.find(it->first);
		if(itMod == mapModified.end()) continue;

		tAttrMap mapBaseAttrs, mapModAttrs;
		aBase.GetNormalizedAttributes(it->second, asCategory, mapBaseAttrs);
		aModified.GetNormalizedAttributes(itMod->second, asCategory, mapModAttrs);

		tAttrMap mapChanged;
		for(tAttrMap::iterator itA = mapModAttrs.begin(); itA != mapModAttrs.end(); ++itA)
		{
			tAttrMap::iterator itB = mapBaseAttrs.find(itA->first);
			if(itB == mapBaseAttrs.end() || ValuesEqual(itB->second, itA->second)==false)
				mapChanged[itA->first] = itA->second;
		}

		// Attributes the modified file no longer has. ID is the match key and is
		// never removed; ignored attributes never reach either map, so an editor
		// dropping a GUID or a FileIndex cannot produce a RemoveAttr.
		tStringVec vRemovedAttrs;
		for(tAttrMap::iterator itA = mapBaseAttrs.begin(); itA != mapBaseAttrs.end(); ++itA)
		{
			if(itA->first!="ID" && mapModAttrs.count(itA->first)==0)
				vRemovedAttrs.push_back(itA->first);
		}

		tAttrMap mapBaseVars, mapModVars;
		GetUserVariables(it->second, mapBaseVars);
		GetUserVariables(itMod->second, mapModVars);

		tAttrMap mapChangedVars;
		tStringVec vRemovedVars;
		for(tAttrMap::iterator itV = mapModVars.begin(); itV != mapModVars.end(); ++itV)
		{
			tAttrMap::iterator itB = mapBaseVars.find(itV->first);
			if(itB == mapBaseVars.end() || ValuesEqual(itB->second, itV->second)==false)
				mapChangedVars[itV->first] = itV->second;
		}
		for(tAttrMap::iterator itV = mapBaseVars.begin(); itV != mapBaseVars.end(); ++itV)
		{
			if(mapModVars.count(itV->first)==0) vRemovedVars.push_back(itV->first);
		}

		if(mapChanged.empty() && vRemovedAttrs.empty() && mapChangedVars.empty() && vRemovedVars.empty())
			continue;

		tinyxml2::XMLElement* pOp = pDoc->NewElement("Modify");
		SetAttributeString(pOp, "Category", asCategory);
		SetAttributeInt(pOp, "ID", it->first);
		if(gbWriteWitness) SetAttributeString(pOp, "Name", GetAttributeString(it->second, "Name", ""));
		apDeltaRoot->InsertEndChild(pOp);

		if(mapChanged.empty()==false)
		{
			tinyxml2::XMLElement* pSet = pDoc->NewElement("SetAttr");
			pOp->InsertEndChild(pSet);
			for(tAttrMap::iterator itA = mapChanged.begin(); itA != mapChanged.end(); ++itA)
				SetAttributeString(pSet, itA->first, NormalizeAssetPath(itA->second));
		}

		for(size_t i=0; i<vRemovedAttrs.size(); ++i)
		{
			tinyxml2::XMLElement* pRemove = pDoc->NewElement("RemoveAttr");
			SetAttributeString(pRemove, "Name", vRemovedAttrs[i]);
			pOp->InsertEndChild(pRemove);
		}

		for(tAttrMap::iterator itV = mapChangedVars.begin(); itV != mapChangedVars.end(); ++itV)
		{
			tinyxml2::XMLElement* pVar = pDoc->NewElement("SetVar");
			SetAttributeString(pVar, "Name", itV->first);
			SetAttributeString(pVar, "Value", itV->second);
			pOp->InsertEndChild(pVar);
		}

		for(size_t i=0; i<vRemovedVars.size(); ++i)
		{
			tinyxml2::XMLElement* pVar = pDoc->NewElement("RemoveVar");
			SetAttributeString(pVar, "Name", vRemovedVars[i]);
			pOp->InsertEndChild(pVar);
		}

		++lOps;
	}

	////////////////////////////////////
	// Objects the modification introduced
	tinyxml2::XMLElement* pAdd = NULL;
	for(tObjectMap::iterator it = mapModified.begin(); it != mapModified.end(); ++it)
	{
		if(mapBase.count(it->first)) continue;

		if(pAdd==NULL)
		{
			pAdd = pDoc->NewElement("Add");
			SetAttributeString(pAdd, "Category", asCategory);
			apDeltaRoot->InsertEndChild(pAdd);
		}

		AppendAddedObject(pAdd, it->second, asCategory, aModified);
		++lOps;
	}

	return lOps;
}

//------------------------------------------

static int DiffMapData(cMapSide& aBase, cMapSide& aModified, tinyxml2::XMLElement* apDeltaRoot)
{
	tinyxml2::XMLElement* pBase = aBase.GetMapData();
	tinyxml2::XMLElement* pMod = aModified.GetMapData();

	tinyxml2::XMLElement* pOp = NULL;

	for(const tinyxml2::XMLAttribute* pAttr = pMod->FirstAttribute(); pAttr; pAttr = pAttr->Next())
	{
		const char* pBaseVal = pBase->Attribute(pAttr->Name());
		if(pBaseVal && ValuesEqual(pBaseVal, pAttr->Value())) continue;

		if(pOp==NULL)
		{
			pOp = apDeltaRoot->GetDocument()->NewElement("SetMapData");
			apDeltaRoot->InsertEndChild(pOp);
		}
		pOp->SetAttribute(pAttr->Name(), pAttr->Value());
	}

	return pOp ? 1 : 0;
}

//------------------------------------------

static bool CommandDiff(	const tWString& asBase, const tWString& asModified, const tWString& asOut,
							const tString& asName, int alPriority, const tString& asTarget)
{
	cMapSide base, modified;
	if(base.Load(asBase)==false || modified.Load(asModified)==false) return false;

	tinyxml2::XMLDocument deltaDoc;
	tinyxml2::XMLElement* pRoot = deltaDoc.NewElement("MapDelta");
	deltaDoc.InsertEndChild(pRoot);

	SetAttributeInt(pRoot, "Version", 1);
	SetAttributeString(pRoot, "Target", asTarget!="" ? asTarget : cString::To8Char(cString::GetFileNameW(asBase)));
	if(asName!="") SetAttributeString(pRoot, "Name", asName);
	if(alPriority!=0) SetAttributeInt(pRoot, "Priority", alPriority);

	int lOps = DiffMapData(base, modified, pRoot);
	for(int i=0; gvCategories[i]; ++i)
		lOps += DiffCategory(gvCategories[i], base, modified, pRoot);

	if(SaveXmlFile(deltaDoc, asOut)==false)
	{
		printf("ERROR: could not write '%s'\n", cString::To8Char(asOut).c_str());
		return false;
	}

	printf("Wrote '%s': %d operation(s)\n", cString::To8Char(asOut).c_str(), lOps);
	return true;
}

//------------------------------------------

//////////////////////////////////////////////////////////////////////////
// APPLY / SHOW
//////////////////////////////////////////////////////////////////////////

//------------------------------------------

static bool CommandApply(const tWString& asBase, const tWStringVec& avDeltas, const tWString& asOut)
{
	tinyxml2::XMLDocument mapDoc;
	if(LoadXmlFile(mapDoc, asBase)==false)
	{
		printf("ERROR: could not parse '%s'\n", cString::To8Char(asBase).c_str());
		return false;
	}

	tinyxml2::XMLElement* pMapData = mapDoc.RootElement() ? mapDoc.RootElement()->FirstChildElement("MapData") : NULL;
	if(pMapData==NULL)
	{
		printf("ERROR: '%s' has no <MapData> element\n", cString::To8Char(asBase).c_str());
		return false;
	}

	int lNextAddID = HPL_XML_DELTA_FIRST_ADD_ID;
	cXmlDeltaStats totalStats;

	for(size_t i=0; i<avDeltas.size(); ++i)
	{
		tinyxml2::XMLDocument deltaDoc;
		if(LoadXmlFile(deltaDoc, avDeltas[i])==false)
		{
			printf("ERROR: could not parse '%s'\n", cString::To8Char(avDeltas[i]).c_str());
			return false;
		}

		cXmlDeltaStats stats;
		if(ApplyXmlDelta(pMapData, deltaDoc.RootElement(), lNextAddID, stats)==false)
		{
			printf("ERROR: could not apply '%s'\n", cString::To8Char(avDeltas[i]).c_str());
			return false;
		}

		printf("  %s: %d added, %d modified, %d removed, %d skipped\n",
				cString::To8Char(cString::GetFileNameW(avDeltas[i])).c_str(),
				stats.mlAdded, stats.mlModified, stats.mlRemoved, stats.mlSkipped);

		totalStats.Add(stats);
	}

	if(SaveXmlFile(mapDoc, asOut)==false)
	{
		printf("ERROR: could not write '%s'\n", cString::To8Char(asOut).c_str());
		return false;
	}

	printf("Wrote '%s': %d added, %d modified, %d removed, %d skipped\n",
			cString::To8Char(asOut).c_str(),
			totalStats.mlAdded, totalStats.mlModified, totalStats.mlRemoved, totalStats.mlSkipped);

	return totalStats.mlSkipped==0;
}

//------------------------------------------

static bool CommandShow(const tWString& asDelta)
{
	tinyxml2::XMLDocument deltaDoc;
	if(LoadXmlFile(deltaDoc, asDelta)==false)
	{
		printf("ERROR: could not parse '%s'\n", cString::To8Char(asDelta).c_str());
		return false;
	}

	tinyxml2::XMLElement* pRoot = deltaDoc.RootElement();
	printf("%s  Version=%d  Target='%s'  Name='%s'  Priority=%d\n",
			pRoot->Value(),
			GetAttributeInt(pRoot, "Version", 1),
			GetAttributeString(pRoot, "Target", "").c_str(),
			GetAttributeString(pRoot, "Name", "").c_str(),
			GetXmlDeltaPriority(pRoot));

	std::map<tString, int> mapCounts;
	for(tinyxml2::XMLElement* pOp = pRoot->FirstChildElement(); pOp; pOp = pOp->NextSiblingElement())
	{
		tString sOp = pOp->Value();

		if(sOp=="Add")
		{
			tString sCategory = GetAttributeString(pOp, "Category", "?");
			for(tinyxml2::XMLElement* pNew = pOp->FirstChildElement(); pNew; pNew = pNew->NextSiblingElement())
			{
				printf("  Add     %-14s %s\n", sCategory.c_str(), GetAttributeString(pNew, "Name", "?").c_str());
				++mapCounts["Add"];
			}
			continue;
		}

		if(sOp=="Remove" || sOp=="Modify")
		{
			printf("  %-7s %-14s ID %-7d %s\n", sOp.c_str(),
					GetAttributeString(pOp, "Category", "?").c_str(),
					GetAttributeInt(pOp, "ID", -1),
					GetAttributeString(pOp, "Name", "").c_str());
		}
		else
		{
			printf("  %s\n", sOp.c_str());
		}
		++mapCounts[sOp];
	}

	printf("---\n");
	for(std::map<tString, int>::iterator it = mapCounts.begin(); it != mapCounts.end(); ++it)
		printf("  %d x %s\n", it->second, it->first.c_str());

	return true;
}

//------------------------------------------

//////////////////////////////////////////////////////////////////////////
// ENTRY
//////////////////////////////////////////////////////////////////////////

//------------------------------------------

static void PrintUsage()
{
	printf(
		"MapDelta -- author and inspect .map_delta patch files.\n"
		"\n"
		"  mapdelta diff  <base.map> <modified.map> -o <out.map_delta>\n"
		"                 [--name NAME] [--priority N] [--target PATH] [--no-witness]\n"
		"  mapdelta apply <base.map> <a.map_delta> [b.map_delta ...] -o <out.map>\n"
		"  mapdelta show  <x.map_delta>\n"
		"\n"
		"A delta records only your changes -- it contains no base map data, so it is\n"
		"the file to redistribute. --no-witness also drops the Name attributes copied\n"
		"from the base map onto Remove/Modify (they exist to detect a changed base).\n"
		"The output of 'apply' is a full derived map: for testing, not for shipping.\n"
		"\n"
		"Pass -cwd to keep the current working directory (paths are otherwise\n"
		"resolved against the game data directory). Paths may not contain spaces.\n");
}

//------------------------------------------

// The applier reports bad operations through the engine log, which for a CLI
// tool belongs on the console.
static void LogToConsole(eLogOutputType aType, const char* asMessage)
{
	if(aType==eLogOutputType_Normal || aType==eLogOutputType_Update) return;
	printf("%s", asMessage);
}

//------------------------------------------

int hplMain(const tString& asCommandLine)
{
	SetLogMessageCallback(LogToConsole);

	tStringVec vArgs;
	cString::GetStringVec(asCommandLine, vArgs);

	if(vArgs.empty())
	{
		PrintUsage();
		return 1;
	}

	////////////////////////////////////
	// Split off the named options first, leaving the positional arguments
	tString sCommand = vArgs[0];
	tString sOut, sName, sTarget;
	int lPriority =0;
	tStringVec vPositional;

	for(size_t i=1; i<vArgs.size(); ++i)
	{
		const tString& sArg = vArgs[i];
		bool bNeedsValue = (sArg=="-o" || sArg=="--out" || sArg=="--name" || sArg=="--priority" || sArg=="--target");

		if(bNeedsValue && i+1 >= vArgs.size())
		{
			printf("ERROR: %s needs a value\n", sArg.c_str());
			return 1;
		}

		if(sArg=="-o" || sArg=="--out")		sOut = vArgs[++i];
		else if(sArg=="--name")				sName = vArgs[++i];
		else if(sArg=="--target")			sTarget = vArgs[++i];
		else if(sArg=="--priority")			lPriority = cString::ToInt(vArgs[++i].c_str(), 0);
		else if(sArg=="--no-witness")		gbWriteWitness = false;
		else if(sArg=="-h" || sArg=="--help") { PrintUsage(); return 0; }
		else if(sArg.size()>1 && sArg[0]=='-')
		{
			printf("ERROR: unknown option '%s'\n", sArg.c_str());
			return 1;
		}
		else vPositional.push_back(sArg);
	}

	////////////////////////////////////
	// Dispatch
	if(sCommand=="diff")
	{
		if(vPositional.size()!=2 || sOut=="")
		{
			PrintUsage();
			return 1;
		}
		return CommandDiff(	cString::To16Char(vPositional[0]), cString::To16Char(vPositional[1]),
							cString::To16Char(sOut), sName, lPriority, sTarget) ? 0 : 1;
	}

	if(sCommand=="apply")
	{
		if(vPositional.size()<2 || sOut=="")
		{
			PrintUsage();
			return 1;
		}

		tWStringVec vDeltas;
		for(size_t i=1; i<vPositional.size(); ++i) vDeltas.push_back(cString::To16Char(vPositional[i]));

		return CommandApply(cString::To16Char(vPositional[0]), vDeltas, cString::To16Char(sOut)) ? 0 : 1;
	}

	if(sCommand=="show")
	{
		if(vPositional.size()!=1)
		{
			PrintUsage();
			return 1;
		}
		return CommandShow(cString::To16Char(vPositional[0])) ? 0 : 1;
	}

	printf("ERROR: unknown command '%s'\n", sCommand.c_str());
	PrintUsage();
	return 1;
}

//------------------------------------------

#ifdef WIN32
int main(int argc, const char* argv[])
{
	tString sCommandLine;
	for(int i=1; i<argc; ++i)
	{
		sCommandLine += argv[i];
		if(i!=argc-1) sCommandLine += " ";
	}
	return hplMain(sCommandLine);
}
#endif

#ifdef __APPLE__
extern "C" int SDL_main(int argc, char *argv[]);
int main(int argc, char * argv[]) { return SDL_main(argc, argv); }
#endif
