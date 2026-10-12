/* ------------------------------------------------------------------------ */
/* Recenter + left Menu button                                              */
/*                                                                          */
/* The system recenter (hold the Meta button) moves the OpenXR LOCAL space; */
/* as a fallback that also works while Horizon is in stationary/tracking-   */
/* lost mode, holding the left Menu button for 0.8 s recenters inside the   */
/* app: LOCAL is re-created with the current head yaw/position as origin.   */
/* A short press of Menu still means Escape (menus) / Start (flight).       */
/* (Inserted into tie_vr.c by tools/quest/gen_tie_vr.py.)                   */

static void vr_set_local_space(const XrPosef* pose) {
	XrReferenceSpaceCreateInfo info  = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
	XrSpace                    space = XR_NULL_HANDLE;

	info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	if (pose) {
		info.poseInReferenceSpace = *pose;
	} else {
		info.poseInReferenceSpace.orientation.w = 1.0f;
	}
	if (!vr_ok(p_xrCreateReferenceSpace(g_vr.session, &info, &space), "xrCreateReferenceSpace(recenter)")) {
		return;
	}
	if (g_vr.local_space) {
		p_xrDestroySpace(g_vr.local_space);
	}
	g_vr.local_space = space;
}

static void vr_recenter(void) {
	XrReferenceSpaceCreateInfo info     = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
	XrSpace                    base     = XR_NULL_HANDLE;
	XrSpace                    view     = XR_NULL_HANDLE;
	XrSpaceLocation            location = { XR_TYPE_SPACE_LOCATION };
	XrPosef                    pose;
	XrVector3f                 forward;
	float                      yaw;

	info.poseInReferenceSpace.orientation.w = 1.0f;
	info.referenceSpaceType                 = XR_REFERENCE_SPACE_TYPE_LOCAL;
	if (!vr_ok(p_xrCreateReferenceSpace(g_vr.session, &info, &base), "xrCreateReferenceSpace(base)")) {
		return;
	}
	info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
	if (!vr_ok(p_xrCreateReferenceSpace(g_vr.session, &info, &view), "xrCreateReferenceSpace(view)")) {
		p_xrDestroySpace(base);
		return;
	}
	if (XR_SUCCEEDED(p_xrLocateSpace(view, base, g_vr.frame_state.predictedDisplayTime, &location)) &&
		(location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
		forward = vr_rotate(location.pose.orientation, (XrVector3f) { 0.0f, 0.0f, -1.0f });
		yaw     = atan2f(-forward.x, -forward.z);
		SDL_zero(pose);
		pose.orientation.y = sinf(yaw * 0.5f);
		pose.orientation.w = cosf(yaw * 0.5f);
		if (location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) {
			pose.position = location.pose.position;
		}
		vr_set_local_space(&pose);
		vr_text_pulse();
		Aeron_LogInfo(VR_LOG, "recentered (yaw %.0f deg)", (double)(yaw * 57.29578f));
	}
	p_xrDestroySpace(view);
	p_xrDestroySpace(base);
}

static void vr_update_menu_button(float dt) {
	if (g_vr.menu_tap_frames > 0) {
		--g_vr.menu_tap_frames;
	}
	if (g_vr.button_menu) {
		g_vr.menu_held_s += dt;
		if (!g_vr.menu_long_done && g_vr.menu_held_s >= 0.8f) {
			g_vr.menu_long_done = 1;
			vr_recenter();
		}
		return;
	}
	if (g_vr.menu_held_s > 0.0f && !g_vr.menu_long_done) {
		/* Short press: hold Escape / Start for a few frames. */
		g_vr.menu_tap_frames = 4;
	}
	g_vr.menu_held_s    = 0.0f;
	g_vr.menu_long_done = 0;
}

