/* ------------------------------------------------------------------------ */
/* Radial menus                                                             */
/*                                                                          */
/* In flight, holding a grip shows that hand's star of eight commands above */
/* the controller (src/tie_vr/vr_radial_menu.c). The same hand's stick      */
/* picks an entry, releasing the grip runs it (stick centred = cancel). A   */
/* stick that is choosing doesn't fly. Commands are the game's own input    */
/* actions, dispatched directly (same thread as the game).                  */
/* Same feature as the X-Wing Alliance VR port (shared VR code base).     */
/* (Inserted into tie_vr.c by tools/quest/gen_tie_vr.py.)                   */

static void vr_radial_haptic(VrHand hand) {
	XrHapticActionInfo info      = { XR_TYPE_HAPTIC_ACTION_INFO };
	XrHapticVibration  vibration = { XR_TYPE_HAPTIC_VIBRATION };

	info.action         = g_vr.act_haptic;
	info.subactionPath  = g_vr.hand_path[hand];
	vibration.amplitude = 0.4f;
	vibration.duration  = 15000000; /* 15 ms */
	vibration.frequency = XR_FREQUENCY_UNSPECIFIED;
	p_xrApplyHapticFeedback(g_vr.session, &info, (const XrHapticBaseHeader*)&vibration);
}

/* Panel pose for a radial menu: above the controller, turned to the head. */
static int vr_radial_pose(VrHand hand, XrPosef* pose) {
	XrSpaceLocation location = { XR_TYPE_SPACE_LOCATION };
	XrVector3f      to_head;
	float           len, yaw, pitch;

	if (XR_FAILED(p_xrLocateSpace(g_vr.aim_space[hand], g_vr.local_space, g_vr.frame_state.predictedDisplayTime,
								  &location)) ||
		!(location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) {
		return 0;
	}
	pose->position = location.pose.position;
	pose->position.y += VR_RADIAL_LIFT_M;
	to_head.x = g_vr.head_pos.x - pose->position.x;
	to_head.y = g_vr.head_pos.y - pose->position.y;
	to_head.z = g_vr.head_pos.z - pose->position.z;
	len       = sqrtf(to_head.x * to_head.x + to_head.y * to_head.y + to_head.z * to_head.z);
	if (len < 1e-3f) {
		pose->orientation = (XrQuaternionf) { 0.0f, 0.0f, 0.0f, 1.0f };
		return 1;
	}
	/* Quad normal (+z) towards the head: yaw about y, then pitch about x. */
	yaw   = atan2f(to_head.x, to_head.z);
	pitch = -asinf(to_head.y / len);
	{
		const float sy = sinf(yaw * 0.5f), cy = cosf(yaw * 0.5f);
		const float sp = sinf(pitch * 0.5f), cp = cosf(pitch * 0.5f);
		pose->orientation = (XrQuaternionf) { cy * sp, sy * cp, -sy * sp, cy * cp };
	}
	return 1;
}

static void vr_run_command(const VrCommand* command) {
	TieInputActions_DispatchController((TieInputAction)command->action, true);
	TieInputActions_DispatchController((TieInputAction)command->action, false);
	Aeron_LogInfo(VR_LOG, "radial menu: %s", command->label);
}

/* Radial menus in flight: left grip = weapons/power, right grip =
 * targets/orders. Runs before vr_update_gamepad, which mutes a stick while
 * its menu is open. */
static void vr_update_radials(int flying) {
	const uint64_t now_ms = SDL_GetTicks();

	for (int hand = 0; hand < VR_HAND_COUNT; ++hand) {
		VrRadialState*   radial = &g_vr.radial[hand];
		const int        held   = flying && vr_hysteresis(g_vr.grip_down[hand], g_vr.grip[hand]);
		const XrVector2f stick  = g_vr.stick[hand];
		const float      mag    = sqrtf(stick.x * stick.x + stick.y * stick.y);
		const int        open   = held && radial->panel.swapchain;

		if (open && !radial->visible) {
			radial->selected    = VR_RADIAL_NONE;
			radial->panel.dirty = 1;
		}
		if (open) {
			const int item = mag > (radial->selected != VR_RADIAL_NONE ? VR_RADIAL_KEEP : VR_RADIAL_PICK)
								 ? VrRadialMenu_ItemForDirection(stick.x, stick.y)
								 : VR_RADIAL_NONE;
			if (item != VR_RADIAL_NONE) {
				if (item != radial->selected) {
					radial->selected = item;
					vr_radial_haptic((VrHand)hand);
				}
				radial->selected_ms = now_ms;
			} else if (radial->selected != VR_RADIAL_NONE && now_ms - radial->selected_ms > VR_RADIAL_LINGER_MS) {
				radial->selected = VR_RADIAL_NONE;
			}
			if (radial->selected != radial->drawn_selected) {
				radial->panel.dirty = 1;
			}
			radial->pose_valid = vr_radial_pose((VrHand)hand, &radial->pose);
		} else if (radial->visible && radial->selected != VR_RADIAL_NONE && flying) {
			/* Grip released with an entry chosen: run it. */
			vr_run_command(VrRadialMenu_Item((VrRadialSide)hand, radial->selected));
			vr_radial_haptic((VrHand)hand);
		}
		radial->visible = open;
	}
}

/* Uploads a CPU-drawn panel image into its swapchain. */
static void vr_panel_upload(VrPanel* panel) {
	XrSwapchainImageAcquireInfo acquire_info = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
	XrSwapchainImageWaitInfo    wait_info    = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
	XrSwapchainImageReleaseInfo release_info = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
	uint32_t                    index        = 0;
	SDL_GPUCommandBuffer*       command_buffer;
	void*                       mapped;

	mapped = SDL_MapGPUTransferBuffer(g_vr.device, panel->transfer, true);
	if (!mapped) {
		return;
	}
	memcpy(mapped, panel->pixels, (size_t)panel->width * panel->height * 4);
	SDL_UnmapGPUTransferBuffer(g_vr.device, panel->transfer);
	if (!vr_ok(p_xrAcquireSwapchainImage(panel->swapchain, &acquire_info, &index), "xrAcquireSwapchainImage(panel)")) {
		return;
	}
	wait_info.timeout = XR_INFINITE_DURATION;
	if (!vr_ok(p_xrWaitSwapchainImage(panel->swapchain, &wait_info), "xrWaitSwapchainImage(panel)")) {
		p_xrReleaseSwapchainImage(panel->swapchain, &release_info);
		return;
	}
	command_buffer = SDL_AcquireGPUCommandBuffer(g_vr.device);
	if (command_buffer) {
		SDL_GPUTextureTransferInfo source;
		SDL_GPUTextureRegion       region;
		SDL_GPUCopyPass*           copy_pass;
		SDL_zero(source);
		SDL_zero(region);
		source.transfer_buffer = panel->transfer;
		source.pixels_per_row  = (Uint32)panel->width;
		source.rows_per_layer  = (Uint32)panel->height;
		region.texture         = panel->images[index];
		region.w               = (Uint32)panel->width;
		region.h               = (Uint32)panel->height;
		region.d               = 1;
		copy_pass              = SDL_BeginGPUCopyPass(command_buffer);
		SDL_UploadToGPUTexture(copy_pass, &source, &region, false);
		SDL_EndGPUCopyPass(copy_pass);
		if (SDL_SubmitGPUCommandBuffer(command_buffer)) {
			panel->dirty = 0;
		} else {
			Aeron_LogError(VR_LOG, "panel upload failed: %s", SDL_GetError());
		}
	}
	p_xrReleaseSwapchainImage(panel->swapchain, &release_info);
	panel->ever_released = 1;
}

static void vr_upload_radials(void) {
	if (!g_vr.frame_begun) {
		return;
	}
	for (int hand = 0; hand < VR_HAND_COUNT; ++hand) {
		VrRadialState* radial = &g_vr.radial[hand];
		if (!radial->visible || !radial->panel.dirty) {
			continue;
		}
		VrRadialMenu_Render((VrRadialSide)hand, radial->selected, radial->panel.pixels, g_vr.panel_bgra);
		radial->drawn_selected = radial->selected;
		vr_panel_upload(&radial->panel);
	}
}

static int vr_panel_create(VrPanel* panel, SDL_GPUTextureFormat format, const XrSwapchainCreateInfo* base, int width,
						   int height) {
	XrSwapchainCreateInfo           info     = *base;
	SDL_GPUTransferBufferCreateInfo transfer = { 0 };

	SDL_zero(*panel);
	info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
					  XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
	info.width  = (uint32_t)width;
	info.height = (uint32_t)height;
	if (!vr_ok(SDL_CreateGPUXRSwapchain(g_vr.device, g_vr.session, &info, format, &panel->swapchain, &panel->images),
			   "SDL_CreateGPUXRSwapchain(panel)")) {
		SDL_zero(*panel);
		return 0;
	}
	transfer.usage  = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
	transfer.size   = (Uint32)width * (Uint32)height * 4;
	panel->transfer = SDL_CreateGPUTransferBuffer(g_vr.device, &transfer);
	panel->pixels   = SDL_malloc(transfer.size);
	panel->width    = width;
	panel->height   = height;
	if (!panel->transfer || !panel->pixels) {
		Aeron_LogError(VR_LOG, "panel unavailable: %s", SDL_GetError());
		SDL_DestroyGPUXRSwapchain(g_vr.device, panel->swapchain, panel->images);
		if (panel->transfer) {
			SDL_ReleaseGPUTransferBuffer(g_vr.device, panel->transfer);
		}
		SDL_free(panel->pixels);
		SDL_zero(*panel);
		return 0;
	}
	return 1;
}

static void vr_panel_destroy(SDL_GPUDevice* device, VrPanel* panel) {
	if (panel->swapchain) {
		SDL_DestroyGPUXRSwapchain(device, panel->swapchain, panel->images);
	}
	if (panel->transfer) {
		SDL_ReleaseGPUTransferBuffer(device, panel->transfer);
	}
	SDL_free(panel->pixels);
	SDL_zero(*panel);
}

static void vr_create_radials(SDL_GPUTextureFormat format, const XrSwapchainCreateInfo* base) {
	g_vr.panel_bgra = format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB || format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
	for (int hand = 0; hand < VR_HAND_COUNT; ++hand) {
		g_vr.radial[hand].selected       = VR_RADIAL_NONE;
		g_vr.radial[hand].drawn_selected = VR_RADIAL_NONE;
		if (!vr_panel_create(&g_vr.radial[hand].panel, format, base, VR_RADIAL_SIZE_PX, VR_RADIAL_SIZE_PX)) {
			Aeron_LogError(VR_LOG, "radial menu %d unavailable", hand);
		}
	}
	Aeron_LogInfo(VR_LOG, "radial menus ready (%dx%d)", VR_RADIAL_SIZE_PX, VR_RADIAL_SIZE_PX);
}

/* Adds the visible radial menus to the frame's layers. */
static uint32_t vr_add_radials(const XrCompositionLayerBaseHeader** layers, uint32_t count, uint32_t capacity,
							   XrCompositionLayerQuad* quads) {
	for (int hand = 0; hand < VR_HAND_COUNT && count < capacity; ++hand) {
		const VrRadialState*    radial = &g_vr.radial[hand];
		XrCompositionLayerQuad* quad   = &quads[hand];
		if (!radial->visible || !radial->pose_valid || !radial->panel.ever_released) {
			continue;
		}
		SDL_zero(*quad);
		/* Premultiplied alpha, drawn over everything. */
		quad->type                             = XR_TYPE_COMPOSITION_LAYER_QUAD;
		quad->layerFlags                       = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
		quad->space                            = g_vr.local_space;
		quad->eyeVisibility                    = XR_EYE_VISIBILITY_BOTH;
		quad->subImage.swapchain               = radial->panel.swapchain;
		quad->subImage.imageRect.extent.width  = radial->panel.width;
		quad->subImage.imageRect.extent.height = radial->panel.height;
		quad->pose                             = radial->pose;
		quad->size.width                       = VR_RADIAL_SIZE_M;
		quad->size.height                      = VR_RADIAL_SIZE_M;
		layers[count++]                        = (const XrCompositionLayerBaseHeader*)quad;
	}
	return count;
}

