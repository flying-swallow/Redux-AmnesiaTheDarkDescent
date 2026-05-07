cmake_minimum_required (VERSION 3.0)
project (Newton)

set(Newton_VERSION_MAJOR 2)
set(Newton_VERSION_MINOR 31)

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/core)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/physics)

if(UNIX)
	set(CMAKE_CXX_FLAGS "-Wno-extern-c-compat")
endif()

add_library(Newton STATIC
  core/dg.cpp
  core/dgAABBPolygonSoup.cpp
  core/dgConvexHull3d.cpp
  core/dgConvexHull4d.cpp
  core/dgCRC.cpp
  core/dgDebug.cpp
  core/dgDelaunayTetrahedralization.cpp
  core/dgGeneralMatrix.cpp
  core/dgGeneralVector.cpp
  core/dgGoogol.cpp
  core/dgIntersections.cpp
  core/dgMatrix.cpp
  core/dgMemory.cpp
  core/dgNode.cpp
  core/dgPolygonSoupBuilder.cpp
  core/dgPolyhedra.cpp
  core/dgPolyhedraMassProperties.cpp
  core/dgQuaternion.cpp
  core/dgRandom.cpp
  core/dgRef.cpp
  core/dgRefCounter.cpp
  core/dgSmallDeterminant.cpp
  core/dgSPDMatrix.cpp
  core/dgSphere.cpp
  core/dgThreads.cpp
  core/dgTree.cpp
  core/dgTypes.cpp

  physics/dgBallConstraint.cpp
  physics/dgBilateralConstraint.cpp
  physics/dgBody.cpp
  physics/dgBodyMasterList.cpp
  physics/dgBroadPhaseCollision.cpp
  physics/dgCollision.cpp
  physics/dgCollisionBox.cpp
  physics/dgCollisionBVH.cpp
  physics/dgCollisionCapsule.cpp
  physics/dgCollisionChamferCylinder.cpp
  physics/dgCollisionCompound.cpp
  physics/dgCollisionCompoundBreakable.cpp
  physics/dgCollisionCone.cpp
  physics/dgCollisionConvex.cpp
  physics/dgCollisionConvexHull.cpp
  physics/dgCollisionConvexModifier.cpp
  physics/dgCollisionCylinder.cpp
  physics/dgCollisionEllipse.cpp
  physics/dgCollisionHeightField.cpp
  physics/dgCollisionMesh.cpp
  physics/dgCollisionNull.cpp
  physics/dgCollisionScene.cpp
  physics/dgCollisionSphere.cpp
  physics/dgCollisionUserMesh.cpp
  physics/dgConnectorConstraint.cpp
  physics/dgConstraint.cpp
  physics/dgContact.cpp
  physics/dgCorkscrewConstraint.cpp
  physics/dgHingeConstraint.cpp
  physics/dgMeshEffect.cpp
  physics/dgMeshEffect2.cpp
  physics/dgMinkowskiConv.cpp
  physics/dgNarrowPhaseCollision.cpp
  physics/dgPointToCurveConstraint.cpp
  physics/dgSlidingConstraint.cpp
  physics/dgUniversalConstraint.cpp
  physics/dgUpVectorConstraint.cpp
  physics/dgUserConstraint.cpp
#  physics/dgVehicleConstraint.cpp
  physics/dgWorld.cpp
  physics/dgWorldDynamicUpdate.cpp
  
  newton/Newton.cpp
  newton/NewtonClass.cpp
)

if(WIN32)
	# PUBLIC so consumers (HPL2) also get _NEWTON_USE_LIB; otherwise Newton.h
	# decorates functions with __declspec(dllimport) and the link fails with
	# __imp_Newton* symbol errors on the static lib.
	target_compile_definitions(Newton PUBLIC "_NEWTON_USE_LIB=1")
	# Newton's dgTypes.h gates __declspec(align) vs __attribute__((aligned))
	# on _WIN_32_VER / _WIN_64_VER. Without the right one, it picks the GCC
	# branch on MSVC and the build fails to parse __attribute__.
	if(CMAKE_SIZEOF_VOID_P EQUAL 8)
		target_compile_definitions(Newton PRIVATE _WIN_64_VER)
	else()
		target_compile_definitions(Newton PRIVATE _WIN_32_VER)
	endif()
endif()

target_include_directories(Newton PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/core ${CMAKE_CURRENT_SOURCE_DIR}/physics ${CMAKE_SOURCE_DIR}/HPL2/dependencies/include)
