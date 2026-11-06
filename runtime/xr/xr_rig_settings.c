// The VR settings tab's hooks into the rig (runtime/vr_settings.c declares them weak; XR builds only, so a flat build
// leaves them NULL and the tab's rows say "Applies After Restart"). The rig owns height, size and tilt (vr_rig.ini):
// each hook converts the tab's unit, keeps the other fields (the yaw too) and goes through xr_rig_set_user, which clamps.
// xr_rig_recentre (xr_rig.c) is the Recentre Table button's hook as it is. Docs: docs/af/VR_SETTINGS.md, RIG.md.
#include "xr_rig.h"

void xr_rig_set_height_cm(float cm) {  // centimetres above (+) / below (-) the default placement
    XrRigUser u;
    xr_rig_get_user(&u);
    u.height = cm / 100.0f;
    xr_rig_set_user(&u);
}

void xr_rig_set_size_pct(float pct) {  // percent of the default scale (100: 0.067 m per game unit)
    XrRigUser u;
    xr_rig_get_user(&u);
    u.size = pct / 100.0f;
    xr_rig_set_user(&u);
}

void xr_rig_set_tilt_deg(float deg) {  // degrees, positive lifts the table's far edge towards the player
    XrRigUser u;
    xr_rig_get_user(&u);
    u.tilt = deg;
    xr_rig_set_user(&u);
}
