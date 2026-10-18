set(TARGET au3-automation-data)
set(SOURCES AutomationData.cpp AutomationData.h)
set(LIBRARIES PUBLIC au3-xml PRIVATE wxBase)
audacity_library(${TARGET} "${SOURCES}" "${LIBRARIES}" "" "")
