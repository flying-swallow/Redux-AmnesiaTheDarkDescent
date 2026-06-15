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

#ifndef HPL_XML_HELPER_H
#define HPL_XML_HELPER_H

#include "system/SystemTypes.h"
#include "graphics/GraphicsTypes.h"
#include "math/MathTypes.h"

namespace tinyxml2 {
	class XMLElement;
	class XMLDocument;
}

namespace hpl {

	//-------------------------------------
	// Typed attribute accessors for tinyxml2 elements.
	//
	// These replace the old cXmlElement::GetAttribute*/SetAttribute* methods and
	// preserve the exact same string parsing / formatting (cString::To* and
	// ToFileString()) so the on-disk format is unchanged.
	//-------------------------------------

	tString		GetAttributeString  (const tinyxml2::XMLElement* apElement, const tString& asName, const tString& asDefault="");
	float		GetAttributeFloat   (const tinyxml2::XMLElement* apElement, const tString& asName, float afDefault=0);
	int			GetAttributeInt     (const tinyxml2::XMLElement* apElement, const tString& asName, int alDefault=0);
	bool		GetAttributeBool    (const tinyxml2::XMLElement* apElement, const tString& asName, bool abDefault=false);
	cVector2f	GetAttributeVector2f(const tinyxml2::XMLElement* apElement, const tString& asName, const cVector2f& avDefault=0);
	cVector3f	GetAttributeVector3f(const tinyxml2::XMLElement* apElement, const tString& asName, const cVector3f& avDefault=0);
	cColor		GetAttributeColor   (const tinyxml2::XMLElement* apElement, const tString& asName, const cColor& aDefault=cColor(0,0));

	void SetAttributeString  (tinyxml2::XMLElement* apElement, const tString& asName, const tString& asVal);
	void SetAttributeFloat   (tinyxml2::XMLElement* apElement, const tString& asName, float afVal);
	void SetAttributeInt     (tinyxml2::XMLElement* apElement, const tString& asName, int alVal);
	void SetAttributeBool    (tinyxml2::XMLElement* apElement, const tString& asName, bool abVal);
	void SetAttributeVector2f(tinyxml2::XMLElement* apElement, const tString& asName, const cVector2f& avVal);
	void SetAttributeVector3f(tinyxml2::XMLElement* apElement, const tString& asName, const cVector3f& avVal);
	void SetAttributeColor   (tinyxml2::XMLElement* apElement, const tString& asName, const cColor& aVal);

	//-------------------------------------
	// Wide-path document I/O. tinyxml2's own LoadFile(const char*) cannot open
	// wide (unicode) paths on Windows, so route through cPlatform::OpenFile.
	//-------------------------------------

	// Logs parse errors and returns false when the document has no root element
	// (tinyxml2 itself accepts declaration/comment-only files), so on success
	// aDoc.RootElement() is guaranteed non-NULL.
	bool LoadXmlFile(tinyxml2::XMLDocument& aDoc, const tWString& asPath);
	bool SaveXmlFile(tinyxml2::XMLDocument& aDoc, const tWString& asPath);

	//-------------------------------------
}

#endif // HPL_XML_HELPER_H
