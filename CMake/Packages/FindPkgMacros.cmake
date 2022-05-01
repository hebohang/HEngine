#-------------------------------------------------------------------
# This file is part of the CMake build system for OGRE
#     (Object-oriented Graphics Rendering Engine)
# For the latest info, see http://www.ogre3d.org/
#
# The contents of this file are placed in the public domain. Feel
# free to make use of it in any way you like.
#-------------------------------------------------------------------

##################################################################
# Provides some common functionality for the FindPackage modules
##################################################################

# Begin processing of package
macro(findpkg_begin PREFIX)
  if (NOT ${PREFIX}_FIND_QUIETLY)
    message(STATUS "Looking for ${PREFIX}...")
  endif ()
endmacro(findpkg_begin)

# Do the final processing for the package find.
macro(findpkg_finish PREFIX)
  # skip if already processed during this run
  if (NOT ${PREFIX}_FOUND)
    if (${PREFIX}_INCLUDE_DIR AND ${PREFIX}_LIBRARY)
      set(${PREFIX}_FOUND TRUE)
      set(${PREFIX}_INCLUDE_DIRS ${${PREFIX}_INCLUDE_DIR})
      set(${PREFIX}_LIBRARIES ${${PREFIX}_LIBRARY})
      if (NOT ${PREFIX}_FIND_QUIETLY)
        message(STATUS "Found ${PREFIX}: ${${PREFIX}_LIBRARIES}")
      endif ()
    else ()
      if (NOT ${PREFIX}_FIND_QUIETLY)
        message(STATUS "Could not locate ${PREFIX}")
      endif ()
      if (${PREFIX}_FIND_REQUIRED)
        message(FATAL_ERROR "Required library ${PREFIX} not found! Install the library (including dev packages) and try again. If the library is already installed, set the missing variables manually in cmake.")
      endif ()
    endif ()

    mark_as_advanced(${PREFIX}_INCLUDE_DIR ${PREFIX}_LIBRARY ${PREFIX}_LIBRARY_REL ${PREFIX}_LIBRARY_DBG ${PREFIX}_LIBRARY_FWK)
  endif ()
endmacro(findpkg_finish)