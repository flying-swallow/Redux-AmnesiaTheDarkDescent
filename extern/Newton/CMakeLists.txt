cmake_minimum_required (VERSION 3.5)
project (Newton)

set(Newton_VERSION_MAJOR 2)
set(Newton_VERSION_MINOR 31)

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/core)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/physics)

if((CMAKE_GENERATOR_PLATFORM MATCHES "ARM64") OR (CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64"))
  set(NEWTON_IS_ARM64 TRUE)
else()
  set(NEWTON_IS_ARM64 FALSE)
endif()

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
elseif(APPLE)
	# dgThreads.cpp's ThreadExecute and dgAtomicAdd in dgTypes.h pick their
	# signature/intrinsic via _LINUX_VER / _MAC_VER. Without one of these
	# defined the function body is left without a signature and the file
	# fails to parse. The _*_VER_64 sibling gates 32-bit-only inline asm
	# (pushl %ebx around cpuid) in dgTypes.cpp.
	target_compile_definitions(Newton PRIVATE _MAC_VER)
	if(CMAKE_SIZEOF_VOID_P EQUAL 8)
		target_compile_definitions(Newton PRIVATE _MAC_VER_64)
	endif()
elseif(UNIX)
	target_compile_definitions(Newton PRIVATE _LINUX_VER)
	if(CMAKE_SIZEOF_VOID_P EQUAL 8)
		target_compile_definitions(Newton PRIVATE _LINUX_VER_64)
	endif()
endif()

if(NEWTON_IS_ARM64)
  target_compile_definitions(Newton PRIVATE _SCALAR_ARITHMETIC_ONLY)
  # dgTypes.cpp guards 32-bit x86 inline asm (cpuid) with #ifndef __x86_64__.
  # On arm64 Linux that block is reachable, so we fake __x86_64__ to skip it.
  # On Apple the asm is already excluded (its #if omits __APPLE__), and defining
  # __x86_64__ wrongly pulls libkern's i386 byte-order header alongside the arm
  # one (redefinition of _OSSwapInt*), so only apply the workaround off Apple.
  if(NOT APPLE)
    set_source_files_properties(core/dgTypes.cpp PROPERTIES COMPILE_DEFINITIONS "__x86_64__")
  endif()
endif()

target_include_directories(Newton PUBLIC include)
