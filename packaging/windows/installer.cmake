# Shared by normal builds and packaging of already signed release binaries.
get_filename_component(_ta_package_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(CPACK_PACKAGE_NAME "ta-engine")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Clean-room engine recreation of Total Annihilation")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/pocketgeek/ta-engine")
set(CPACK_PACKAGE_CONTACT "Curtis Edge <pocket_geek@pgnet.us>")
set(CPACK_RESOURCE_FILE_LICENSE "${_ta_package_root}/LICENSE")
set(CPACK_PACKAGE_FILE_NAME "ta-engine-${PROJECT_VERSION}-windows-x64-setup")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "TA Engine")
set(CPACK_NSIS_MUI_ICON "${_ta_package_root}/res/icons/taclient.ico")
set(CPACK_NSIS_MUI_UNIICON "${_ta_package_root}/res/icons/taclient.ico")
set(CPACK_NSIS_DISPLAY_NAME "Total Annihilation")
set(CPACK_NSIS_PACKAGE_NAME "Total Annihilation")
set(CPACK_NSIS_URL_INFO_ABOUT "https://github.com/pocketgeek/ta-engine")
set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
set(CPACK_NSIS_EXECUTABLES_DIRECTORY ".")        # shortcuts point at the install root
# Windows shortcut filenames cannot contain a colon.
set(CPACK_PACKAGE_EXECUTABLES "taclient" "Total Annihilation")
if(TA_PACKAGE_CARTOGRAPHER)
  list(APPEND CPACK_PACKAGE_EXECUTABLES "cartographer" "TA Cartographer")
endif()
set(CPACK_NSIS_MUI_FINISHPAGE_RUN "taclient.exe")
