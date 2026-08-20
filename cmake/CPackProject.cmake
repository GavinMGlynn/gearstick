# CPackProject.cmake - read once per generator, while a package is being made.
#
# **The only place a setting can differ between the zip and the MSI.** Everything
# in CMakeLists.txt is decided before CPack knows which generator it is running,
# so a value that has to be one thing for one package and another for the next
# has to be decided here.
#
# There is exactly one such value, and it is not cosmetic. Windows Installer
# requires a product version of four integers and refuses `0.1.0-beta1`; it also
# uses that number, rather than the file name, to decide whether installing this
# is an upgrade of what is already there. So the MSI carries a numeric version
# while the file keeps the name a person can read.
if(CPACK_GENERATOR STREQUAL "WIX")
    if(CPACK_GS_MSI_VERSION)
        set(CPACK_PACKAGE_VERSION "${CPACK_GS_MSI_VERSION}")
    endif()
endif()
