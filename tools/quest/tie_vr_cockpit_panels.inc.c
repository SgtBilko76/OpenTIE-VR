/* ------------------------------------------------------------------------ */
/* 3D cockpit instrument panels                                             */
/*                                                                          */
/* With the 3D TIE cockpit on, the flat frame holds only instruments and    */
/* HUD on a transparent background. The window region stays world-aligned   */
/* on the HUD quad (crosshair, target box); each instrument is cut out of   */
/* the same image as its own quad, mounted on the 3D model's housing        */
/* (positions from tools/quest/gen_vr_cockpit.py). Returns the new count.   */
/* (Inserted into tie_vr.c by tools/quest/gen_tie_vr.py.)                   */

static uint32_t vr_add_cockpit_panels(XrCompositionLayerQuad* hud, const float hud_tan[4],
									  const XrCompositionLayerBaseHeader** layers, uint32_t count,
									  uint32_t capacity, XrCompositionLayerQuad* quads) {
	float       map[4];
	float       frame[2];
	const float tan_w = hud_tan[0] + hud_tan[1];
	const float tan_h = hud_tan[2] + hud_tan[3];

	if (!TieRemasterVr_Cockpit3d(map, frame) || frame[0] <= 0.0f || frame[1] <= 0.0f) {
		return count;
	}
	const float sx = (float)VR_SCREEN_WIDTH_PX / frame[0];
	const float sy = (float)VR_SCREEN_HEIGHT_PX / frame[1];

	/* HUD: only the window region, still on the game camera's frustum. */
	{
		const float* r   = TIE_VR_COCKPIT_HUD_RECT;
		const float  fx0 = map[0] + r[0] * map[2];
		const float  fy0 = map[1] + r[1] * map[3];
		const float  fx1 = map[0] + r[2] * map[2];
		const float  fy1 = map[1] + r[3] * map[3];
		const float  tl  = -hud_tan[0] + fx0 / frame[0] * tan_w;
		const float  tr  = -hud_tan[0] + fx1 / frame[0] * tan_w;
		const float  tu  = hud_tan[2] - fy0 / frame[1] * tan_h;
		const float  td  = hud_tan[2] - fy1 / frame[1] * tan_h;

		hud->subImage.imageRect.offset.x      = (int32_t)(fx0 * sx);
		hud->subImage.imageRect.offset.y      = (int32_t)(fy0 * sy);
		hud->subImage.imageRect.extent.width  = (int32_t)((fx1 - fx0) * sx);
		hud->subImage.imageRect.extent.height = (int32_t)((fy1 - fy0) * sy);
		hud->pose.position.x                  = VR_HUD_DISTANCE * (tl + tr) * 0.5f;
		hud->pose.position.y                  = VR_HUD_DISTANCE * (tu + td) * 0.5f;
		hud->pose.position.z                  = -VR_HUD_DISTANCE;
		hud->size.width                       = VR_HUD_DISTANCE * (tr - tl);
		hud->size.height                      = VR_HUD_DISTANCE * (tu - td);
	}

	for (size_t i = 0; i < SDL_arraysize(TIE_VR_COCKPIT_PANELS) && count < capacity; ++i) {
		const TieVrCockpitPanel* panel = &TIE_VR_COCKPIT_PANELS[i];
		XrCompositionLayerQuad*  quad  = &quads[i];
		const float              fx0   = map[0] + panel->src[0] * map[2];
		const float              fy0   = map[1] + panel->src[1] * map[3];
		const float              fx1   = map[0] + panel->src[2] * map[2];
		const float              fy1   = map[1] + panel->src[3] * map[3];

		SDL_zerop(quad);
		quad->type                            = XR_TYPE_COMPOSITION_LAYER_QUAD;
		quad->layerFlags                      = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
		quad->space                           = g_vr.local_space;
		quad->eyeVisibility                   = XR_EYE_VISIBILITY_BOTH;
		quad->subImage.swapchain              = g_vr.swapchain;
		quad->subImage.imageRect.offset.x     = (int32_t)(fx0 * sx);
		quad->subImage.imageRect.offset.y     = (int32_t)(fy0 * sy);
		quad->subImage.imageRect.extent.width  = (int32_t)((fx1 - fx0) * sx);
		quad->subImage.imageRect.extent.height = (int32_t)((fy1 - fy0) * sy);
		quad->pose.position.x                 = panel->position[0];
		quad->pose.position.y                 = panel->position[1];
		quad->pose.position.z                 = panel->position[2];
		quad->pose.orientation.x              = panel->orientation[0];
		quad->pose.orientation.y              = panel->orientation[1];
		quad->pose.orientation.z              = panel->orientation[2];
		quad->pose.orientation.w              = panel->orientation[3];
		quad->size.width                      = panel->size[0];
		quad->size.height                     = panel->size[1];
		layers[count++]                       = (const XrCompositionLayerBaseHeader*)quad;
	}
	return count;
}

