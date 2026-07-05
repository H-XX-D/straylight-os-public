# FindWaylandProtocols.cmake
# Minimal find module for wayland-protocols on systems that provide it via
# pkg-config but not via a CMake config package.
#
# Provides:
#   WaylandProtocols_FOUND    — always TRUE when wayland-protocols is present
#   WaylandProtocols_DATADIR  — path to the .xml protocol files

include(FindPackageHandleStandardArgs)

find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
    pkg_get_variable(WaylandProtocols_DATADIR wayland-protocols pkgdatadir)
endif()

if(NOT WaylandProtocols_DATADIR)
    # Fallback: common install paths
    foreach(dir
        /usr/share/wayland-protocols
        /usr/local/share/wayland-protocols)
        if(IS_DIRECTORY "${dir}")
            set(WaylandProtocols_DATADIR "${dir}")
            break()
        endif()
    endforeach()
endif()

find_package_handle_standard_args(WaylandProtocols
    REQUIRED_VARS WaylandProtocols_DATADIR
    VERSION_VAR   WaylandProtocols_DATADIR
)

mark_as_advanced(WaylandProtocols_DATADIR)
