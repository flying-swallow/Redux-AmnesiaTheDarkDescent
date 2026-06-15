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

#include "resources/XmlHelper.h"

#include "system/LowLevelSystem.h"
#include "system/Platform.h"
#include "system/String.h"

#include <tinyxml2.h>
#include <stdio.h>

namespace hpl {

	//-----------------------------------------------------------------------

	tString GetAttributeString(const tinyxml2::XMLElement* apElement, const tString& asName, const tString& asDefault)
	{
		const char* pString = apElement ? apElement->Attribute(asName.c_str()) : NULL;
		if(pString)	return pString;
		else		return asDefault;
	}

	float GetAttributeFloat(const tinyxml2::XMLElement* apElement, const tString& asName, float afDefault)
	{
		const char* pString = apElement ? apElement->Attribute(asName.c_str()) : NULL;
		return cString::ToFloat(pString, afDefault);
	}

	int GetAttributeInt(const tinyxml2::XMLElement* apElement, const tString& asName, int alDefault)
	{
		const char* pString = apElement ? apElement->Attribute(asName.c_str()) : NULL;
		return cString::ToInt(pString, alDefault);
	}

	bool GetAttributeBool(const tinyxml2::XMLElement* apElement, const tString& asName, bool abDefault)
	{
		const char* pString = apElement ? apElement->Attribute(asName.c_str()) : NULL;
		return cString::ToBool(pString, abDefault);
	}

	cVector2f GetAttributeVector2f(const tinyxml2::XMLElement* apElement, const tString& asName, const cVector2f& avDefault)
	{
		const char* pString = apElement ? apElement->Attribute(asName.c_str()) : NULL;
		return cString::ToVector2f(pString, avDefault);
	}

	cVector3f GetAttributeVector3f(const tinyxml2::XMLElement* apElement, const tString& asName, const cVector3f& avDefault)
	{
		const char* pString = apElement ? apElement->Attribute(asName.c_str()) : NULL;
		return cString::ToVector3f(pString, avDefault);
	}

	cColor GetAttributeColor(const tinyxml2::XMLElement* apElement, const tString& asName, const cColor& aDefault)
	{
		const char* pString = apElement ? apElement->Attribute(asName.c_str()) : NULL;
		return cString::ToColor(pString, aDefault);
	}

	//-----------------------------------------------------------------------

	void SetAttributeString(tinyxml2::XMLElement* apElement, const tString& asName, const tString& asVal)
	{
		apElement->SetAttribute(asName.c_str(), asVal.c_str());
	}

	void SetAttributeFloat(tinyxml2::XMLElement* apElement, const tString& asName, float afVal)
	{
		char buf[64];
		sprintf(buf, "%g", afVal);
		apElement->SetAttribute(asName.c_str(), buf);
	}

	void SetAttributeInt(tinyxml2::XMLElement* apElement, const tString& asName, int alVal)
	{
		char buf[64];
		sprintf(buf, "%d", alVal);
		apElement->SetAttribute(asName.c_str(), buf);
	}

	void SetAttributeBool(tinyxml2::XMLElement* apElement, const tString& asName, bool abVal)
	{
		apElement->SetAttribute(asName.c_str(), abVal ? "true" : "false");
	}

	void SetAttributeVector2f(tinyxml2::XMLElement* apElement, const tString& asName, const cVector2f& avVal)
	{
		apElement->SetAttribute(asName.c_str(), avVal.ToFileString().c_str());
	}

	void SetAttributeVector3f(tinyxml2::XMLElement* apElement, const tString& asName, const cVector3f& avVal)
	{
		apElement->SetAttribute(asName.c_str(), avVal.ToFileString().c_str());
	}

	void SetAttributeColor(tinyxml2::XMLElement* apElement, const tString& asName, const cColor& aVal)
	{
		apElement->SetAttribute(asName.c_str(), aVal.ToFileString().c_str());
	}

	//-----------------------------------------------------------------------

	bool LoadXmlFile(tinyxml2::XMLDocument& aDoc, const tWString& asPath)
	{
		FILE *pFile = cPlatform::OpenFile(asPath, _W("rb"));
		if(pFile==NULL) return false;

		bool bRet = aDoc.LoadFile(pFile) == tinyxml2::XML_SUCCESS;

		fclose(pFile);

		if(bRet==false)
		{
			Warning("Failed parsing of XML document '%s' at line %d: %s\n",
					cString::To8Char(asPath).c_str(), aDoc.ErrorLineNum(), aDoc.ErrorStr());
			return false;
		}

		// tinyxml2 accepts declaration/comment-only files; callers deref the root.
		if(aDoc.RootElement()==NULL)
		{
			Warning("XML document '%s' has no root element\n", cString::To8Char(asPath).c_str());
			return false;
		}

		return true;
	}

	bool SaveXmlFile(tinyxml2::XMLDocument& aDoc, const tWString& asPath)
	{
		if(asPath == _W("")) return false;

		FILE *pFile = cPlatform::OpenFile(asPath, _W("w+"));
		if(pFile==NULL) return false;

		bool bRet = aDoc.SaveFile(pFile) == tinyxml2::XML_SUCCESS;

		fclose(pFile);

		return bRet;
	}

	//-----------------------------------------------------------------------
}
