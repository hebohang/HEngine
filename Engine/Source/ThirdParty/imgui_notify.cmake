set(ImguiNotifySourceDir ${ThirdPartyDir}/imgui-notify-v2)

set(imguiNotifySrc 
    "${ImguiNotifySourceDir}/fa_solid_900.h"
    "${ImguiNotifySourceDir}/font_awesome_5.h"
    "${ImguiNotifySourceDir}/imgui_notify.h"
)

add_library(imgui_notify INTERFACE ${imguiNotifySrc})
target_include_directories(imgui_notify INTERFACE "${ImguiNotifySourceDir}")