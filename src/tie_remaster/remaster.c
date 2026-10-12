#include "tie_remaster/remaster.h"
#include "tie_remaster/vr_stereo.h"
#include "tie_vr/tie_vr_cockpit_panels.h"

#include "aeron/external_present.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "aeron/compat/host.h"
#include "aeron/scene/blend_ramp.h"
#include "aeron/scene/present.h"
#include "tie_remaster/flight/cockpit/renderer.h"
#include "tie_remaster/flight/render_math.h"
#include "tie_remaster/flight/renderer.h"
#include "tie_remaster/integration/snapshot_adapter.h"
#include "tie_remaster/scene2d/cursor.h"
#include "tie_remaster/scene2d/cutscene.h"
#include "tie_remaster/scene2d/map.h"
#include "tie_remaster/scene2d/snapshot_merge.h"
#include "tie_remaster/scene2d/srgb_math.h"
#include "tie_remaster/scene2d/text.h"
#include "tie_remaster/scene2d/viewport.h"
#include "tie_runtime/audio/config.h"
#include "tie_runtime/diagnostics/diagnostics.h"
#include "tie_runtime/display/classic_display.h"
#include "tie_runtime/display/classic_framebuffer.h"
#include "tie_runtime/flight_assets/assets.h"
#include "tie_runtime/flight_assets/model_types.h"
#include "tie_runtime/flight_assets/source.h"
#include "tie_runtime/input/input.h"
#include "tie_runtime/presentation/presentation.h"
#include "tie_runtime/runtime/exports.h"
#include "tie_runtime/runtime/profile.h"
#include "tie_runtime/snapshot/snapshot.h"
#include "tie_runtime/storage/storage.h"

#define TIE_REMASTER_2D_RT_FORMAT AERON_TEXTURE_FORMAT_RGBA8_SRGB

#define TIE_FLIGHT_RT_FORMAT AERON_TEXTURE_FORMAT_R11G11B10_UFLOAT
#define TIE_FLIGHT_PRESENT_RT_FORMAT AERON_TEXTURE_FORMAT_RGBA16_FLOAT

/* Large enough for the scaled 32-pixel Landru cursor and hotspot headroom. */
#define CURSOR_RT_DIM 512

typedef enum TieRemasterViewMode {
	TIE_REMASTER_VIEW_CLASSIC = 0,
	TIE_REMASTER_VIEW_SPLIT,
	TIE_REMASTER_VIEW_HD,
} TieRemasterViewMode;

/* Split view is a debug-only comparison aid; production toggles the two full-screen modes. */
static TieRemasterViewMode TieRemaster_NextViewMode(TieRemasterViewMode selected) {
#if !defined(NDEBUG)
	return (TieRemasterViewMode)((selected + 1) % 3);
#else
	return selected == TIE_REMASTER_VIEW_CLASSIC ? TIE_REMASTER_VIEW_HD : TIE_REMASTER_VIEW_CLASSIC;
#endif
}

static bool TieRemaster_KeyActiveThisFrame(const AeronInputSnapshot* input, AeronKey key) {
	return input->key_down[key] || input->key_pressed[key] || input->key_released[key];
}

static bool TieRemaster_ModifierActiveThisFrame(const AeronInputSnapshot* input) {
	static const AeronKey modifiers[] = {
		AERON_KEY_LSHIFT, AERON_KEY_RSHIFT, AERON_KEY_LCTRL, AERON_KEY_RCTRL,
		AERON_KEY_LALT,   AERON_KEY_RALT,   AERON_KEY_LGUI,  AERON_KEY_RGUI,
	};
	for (size_t i = 0; i < sizeof modifiers / sizeof modifiers[0]; ++i) {
		if (TieRemaster_KeyActiveThisFrame(input, modifiers[i]))
			return true;
	}
	return false;
}

typedef struct TieRemaster2D {
	TieScene2dCutscene* cutscene;
	TieScene2dTextRenderer* text;
	TieScene2dMapRenderer* map;
	AeronRenderTarget* target;
	AeronRenderTarget* retired_target;
	AeronDrawList2D* draw_list;
	int target_width;
	int target_height;
	uint32_t render_generation;
} TieRemaster2D;

typedef struct TiePresentationCursor {
	TieScene2dCursorRenderer* renderer;
	AeronRenderTarget* target;
	AeronDrawList2D* draw_list;
	uint32_t palette[256];
	float scale_x;
	float scale_y;
	int16_t hot_x;
	int16_t hot_y;
	int16_t width;
	int16_t height;
	uint8_t kind;
	bool rendered;
} TiePresentationCursor;

typedef struct TieRemasterFlight {
	TieFlightRenderer* renderer;
	TieCockpitRenderer* cockpit;
	AeronRenderTarget* present_target;
	int target_width;
	int target_height;
	uint32_t render_generation;
	uint32_t last_flight_frame;
	uint32_t last_mission_generation;
	int32_t last_training_score;
	int32_t last_training_bonus;
	uint8_t last_camera_valid;
	uint8_t last_training_bonus_active;
	uint8_t last_training_timer_min;
	uint8_t last_training_timer_sec;
	bool rendered;
	bool force_render;
	bool render_suspended;
	/* VR stereo (Quest): per-eye tonemapped image, cockpit/HUD overlay with
	 * its own RGBA cockpit renderer (the main one targets an alpha-less HDR
	 * format), all at the presentation render size. */
	TieCockpitRenderer* vr_cockpit;
	AeronRenderTarget* vr_eye_target;
	AeronRenderTarget* vr_overlay_target;
	AeronDepthTarget* vr_overlay_depth;
	int vr_width;
	int vr_height;
} TieRemasterFlight;

typedef struct TieRemasterState {
	bool initialized;
	uint64_t host_elapsed_us;
	bool aspect_correct_legacy_scenes;
	TieRemasterViewMode view_mode;
	TieRemasterViewMode pending_view_mode;
	uint64_t pending_classic_frame_serial;
	bool pending_view_mode_valid;
	AeronBlendRamp blend;
	bool overlay_was_frozen;
	char last_scene_tag[64];
	TieVideoOptions video_options;
	bool video_options_pending;
	TieRemasterConfig config;
	const TieFlightAssetSource* assets;
	bool flight_rebuild_pending;
	uint32_t loading_generation_seen;
	uint32_t loading_generation_prepared;
	TieRemaster2D scene2d;
	TieRemasterFlight flight;
	TiePresentationCursor cursor;
} TieRemasterState;

static TieRemasterState g_remaster;

static void TieRemaster_SnapshotLandruSource(const TieSnapshot* snapshot, int* source_w, int* source_h,
											 uint8_t* source_pixel_aspect) {
	*source_w = snapshot && snapshot->landru_coord_w ? snapshot->landru_coord_w : CLASSIC_FB_W;
	*source_h = snapshot && snapshot->landru_coord_h ? snapshot->landru_coord_h : CLASSIC_FB_H;
	*source_pixel_aspect =
		snapshot ? snapshot->landru_pixel_aspect : TIE_SCENE2D_VIEWPORT_PIXEL_ASPECT_VGA_4_3;
}

static TieScene2dTextSpace TieRemaster_LandruTextSpace(int source_w, int source_h) {
	return (TieScene2dTextSpace) {
		.classic_w = source_w,
		.classic_h = source_h,
		.atlas_scale_x = 2880.0f / (float)source_w,
		.atlas_scale_y = 2160.0f / (float)source_h,
		.space_between_classic = 1.0f,
		.fit = TIE_SCENE2D_TEXT_FIT_LETTERBOX_4_3,
	};
}

#if !defined(__APPLE__)
static float TieRemaster_SdrContentGammaDecode(TieSdrContentGamma gamma) {
	switch (gamma) {
		case TIE_SDR_CONTENT_GAMMA_2_2:
			return 2.2f;
		case TIE_SDR_CONTENT_GAMMA_2_4:
			return 2.4f;
		case TIE_SDR_CONTENT_GAMMA_SRGB:
			return 0.0f;
	}
	return 0.0f;
}
#endif

static bool TieRemaster_ApplyPendingVideoOptions(void) {
	if (!g_remaster.video_options_pending)
		return true;

	g_remaster.video_options_pending = false;
	const TieVideoOptions* options = &g_remaster.video_options;
	if (!Aeron_SetOutputHdr(options->hdr ? 1 : 0)) {
		Aeron_RequestFatalRendererError("HDR output configuration");
		return false;
	}
#if !defined(__APPLE__)
	Aeron_SetOutputSdrContentGamma(TieRemaster_SdrContentGammaDecode(options->sdr_content_gamma));
	Aeron_SetOutputPaperWhiteNits(options->paper_white_auto ? 0.0f : options->paper_white_nits);
#endif
	if (g_remaster.flight.renderer &&
		!TieFlightRenderer_ApplyQuality(
			g_remaster.flight.renderer, options->ssao_quality, options->shadows_enabled,
			options->shadow_atlas_size, options->fsr_mode, options->fsr_sharpness,
			options->motion_blur_quality, options->motion_blur_shutter, options->msaa_samples))
		return false;
	TieFlightRenderer_SetStarfieldStyle(g_remaster.flight.renderer, options->starfield_style);
	return true;
}

static bool TieRemaster_LoadSharedText(AeronCommandBuffer* cmd, const char* remaster_dir) {
	char atlas_path[1024];
	snprintf(atlas_path, sizeof atlas_path, "%s/assets/fonts/subtitle/font8", remaster_dir);
	g_remaster.scene2d.text = TieScene2dTextRenderer_Init(cmd, TIE_REMASTER_2D_RT_FORMAT, atlas_path);
	if (!g_remaster.scene2d.text) {
		if (!Aeron_FatalErrorRequested())
			Aeron_LogWarn("tie.assets", "subtitle atlas unavailable at %s", atlas_path);
		return !Aeron_FatalErrorRequested();
	}
	static const struct {
		int slot;
		const char* subpath;
		const char* fail_hint;
	} optional_fonts[] = {
		{ 1, "subtitle/font6", "small UI text will render at font8 size" },
		{ 2, "subtitle/font18", "Landru font18 will render at font8 size" },
		{ 3, "subtitle/font12", "Landru font12 will render at font8 size" },
		{ 4, "title/helv20", "title crawl will render at font8 size" },
		{ TIE_SCENE2D_TEXT_RENDERER_FONT_SLOT_COCKPIT_MICRO, "cockpit/micro64",
		  "HUD digits will render in subtitle font" },
		{ TIE_SCENE2D_TEXT_RENDERER_FONT_SLOT_COCKPIT_TINY, "cockpit/tiny64",
		  "HUD text will render in subtitle font" },
	};

	for (size_t i = 0; i < sizeof optional_fonts / sizeof optional_fonts[0]; ++i) {
		char path[1024];
		snprintf(path, sizeof path, "%s/assets/fonts/%s", remaster_dir, optional_fonts[i].subpath);
		if (!TieScene2dTextRenderer_LoadFont(g_remaster.scene2d.text, cmd, (uint8_t)optional_fonts[i].slot,
											 path)) {
			Aeron_LogWarn("tie.assets", "font atlas unavailable at %s; %s", path,
						  optional_fonts[i].fail_hint);
		}
	}
	return !Aeron_FatalErrorRequested();
}

static bool TieRemaster_InitScene2d(AeronCommandBuffer* cmd, const char* remaster_dir,
									const char* frontend_profile_id, int target_width, int target_height,
									uint32_t render_generation) {
	TieRemaster2D* scene = &g_remaster.scene2d;

	scene->target = Aeron_CreateRenderTarget(&(AeronRenderTargetDesc) {
		.width = target_width,
		.height = target_height,
		.format = TIE_REMASTER_2D_RT_FORMAT,
		.sample_count = AERON_SAMPLE_COUNT_1,
	});
	if (!scene->target) {
		Aeron_RequestFatalRendererError("remaster 2D target creation");
		return false;
	}
	scene->target_width = target_width;
	scene->target_height = target_height;
	scene->render_generation = render_generation;
	/* The opening SNAP_ON fade can preserve this target before its first
	 * scene draw. Seed transparent contents so that preservation never
	 * samples a newly allocated texture with undefined contents. */
	AeronRenderPass* initial_clear = Aeron_BeginRenderPass(&(AeronRenderPassDesc) {
		.color_target = scene->target,
		.clear_color = 1,
		.clear_color_rgba = { 0.0f, 0.0f, 0.0f, 0.0f },
		.command_buffer = cmd,
	});
	if (!initial_clear) {
		Aeron_RequestFatalRendererError("remaster 2D target initialization");
		return false;
	}
	Aeron_EndRenderPass(initial_clear);

	if (!TieRemaster_LoadSharedText(cmd, remaster_dir))
		return false;

	scene->cutscene = TieScene2dCutscene_Init(remaster_dir, frontend_profile_id);
	if (!scene->cutscene) {
		if (Aeron_FatalErrorRequested())
			return false;
		Aeron_LogWarn("tie.cutscene", "authored cutscene content unavailable");
	}

	scene->map = TieScene2dMap_Init(cmd, TIE_REMASTER_2D_RT_FORMAT, scene->cutscene, scene->text);
	scene->draw_list =
		AeronDrawList_Create(3 * TIE_MAX_UI_TEXTS * TIE_UI_TEXT_MAX_CHARS + 9 * TIE_MAX_PAINT_CMDS +
							 2 * TIE_MAX_DRAWS_2D + TIE_MAX_TITLE_CRAWL_LINES);
	if (!scene->map || !scene->draw_list) {
		if (!Aeron_FatalErrorRequested())
			Aeron_RequestFatalRendererError("remaster 2D resource creation");
		return false;
	}
	return true;
}

static bool TieRemaster_InitPresentationCursor(void) {
	TiePresentationCursor* cursor = &g_remaster.cursor;
	cursor->renderer = TieScene2dCursor_Init();
	cursor->draw_list = AeronDrawList_Create(8);
	cursor->target = Aeron_CreateRenderTarget(&(AeronRenderTargetDesc) {
		.width = CURSOR_RT_DIM,
		.height = CURSOR_RT_DIM,
		.format = TIE_REMASTER_2D_RT_FORMAT,
		.sample_count = AERON_SAMPLE_COUNT_1,
	});
	if (!cursor->renderer || !cursor->draw_list || !cursor->target) {
		if (!Aeron_FatalErrorRequested())
			Aeron_RequestFatalRendererError("cursor presentation resource creation");
		return false;
	}
	return true;
}

static bool TieRemaster_InitFlight(AeronCommandBuffer* cmd, const char* remaster_dir,
								   const TieRemasterConfig* config) {
	TieRemasterFlight* flight = &g_remaster.flight;
	const TiePresentationLayout* presentation = TiePresentation_Layout();
	if (!presentation)
		return false;
	if (!g_remaster.scene2d.text) {
		g_remaster.scene2d.text = TieScene2dTextRenderer_Init(cmd, TIE_REMASTER_2D_RT_FORMAT, NULL);
		if (!g_remaster.scene2d.text) {
			Aeron_RequestFatalRendererError("flight text renderer initialization");
			return false;
		}
	}
	if (TieFlightAssetSource_IsRemastered(g_remaster.assets)) {
		const TieFlightAssetBundle* catalog = g_remaster.assets->catalog;
		AeronVfs* vfs = g_remaster.assets->vfs;
		const char* prefix = TieFlightAssets_ContentPrefix(catalog);
		const char* micro = TieFlightAssets_CockpitFont(catalog, false);
		const char* tiny = TieFlightAssets_CockpitFont(catalog, true);
		char path[TIE_FLIGHT_ASSET_PATH_MAX + 32];
		if (!micro || !tiny || snprintf(path, sizeof path, "%s/%s", prefix, micro) >= (int)sizeof path ||
			!TieScene2dTextRenderer_LoadFontVfs(g_remaster.scene2d.text, cmd,
												TIE_SCENE2D_TEXT_RENDERER_FONT_SLOT_COCKPIT_MICRO, vfs,
												AERON_VFS_ROOT_ASSET, path, 64u * 1024u * 1024u)) {
			Aeron_RequestFatalError("Flight Asset Error", "required GLB cockpit micro font failed to load");
			return false;
		}
		if (snprintf(path, sizeof path, "%s/%s", prefix, tiny) >= (int)sizeof path ||
			!TieScene2dTextRenderer_LoadFontVfs(g_remaster.scene2d.text, cmd,
												TIE_SCENE2D_TEXT_RENDERER_FONT_SLOT_COCKPIT_TINY, vfs,
												AERON_VFS_ROOT_ASSET, path, 64u * 1024u * 1024u)) {
			Aeron_RequestFatalError("Flight Asset Error", "required GLB cockpit tiny font failed to load");
			return false;
		}
	} else if (g_remaster.flight_rebuild_pending && remaster_dir) {
		static const struct {
			uint8_t slot;
			const char* path;
		} fonts[] = {
			{ TIE_SCENE2D_TEXT_RENDERER_FONT_SLOT_COCKPIT_MICRO, "tie_formats/fonts/cockpit/micro64" },
			{ TIE_SCENE2D_TEXT_RENDERER_FONT_SLOT_COCKPIT_TINY, "tie_formats/fonts/cockpit/tiny64" },
		};
		for (size_t index = 0; index < sizeof fonts / sizeof fonts[0]; ++index) {
			char path[1024];
			snprintf(path, sizeof path, "%s/%s", remaster_dir, fonts[index].path);
			if (!TieScene2dTextRenderer_LoadFont(g_remaster.scene2d.text, cmd, fonts[index].slot, path))
				Aeron_LogWarn("tie.assets", "cockpit font atlas unavailable at %s", path);
		}
	}
	flight->renderer =
		TieFlightRenderer_Init(presentation->render_width, presentation->render_height, TIE_FLIGHT_RT_FORMAT,
							   TIE_FLIGHT_PRESENT_RT_FORMAT, remaster_dir, g_remaster.assets, &config->render,
							   &config->pbr, &config->point_lights);
	if (!flight->renderer) {
		if (!Aeron_FatalErrorRequested())
			Aeron_RequestFatalRendererError("flight renderer initialization");
		return false;
	}

	int target_width = 0;
	int target_height = 0;
	TieFlightRenderer_GetRtDims(flight->renderer, &target_width, &target_height);
	flight->present_target = Aeron_CreateRenderTarget(&(AeronRenderTargetDesc) {
		.width = target_width,
		.height = target_height,
		.format = TIE_FLIGHT_PRESENT_RT_FORMAT,
		.sample_count = AERON_SAMPLE_COUNT_1,
	});
	if (!flight->present_target) {
		Aeron_RequestFatalRendererError("flight presentation target creation");
		return false;
	}
	flight->target_width = target_width;
	flight->target_height = target_height;
	flight->render_generation = presentation->render_generation;

	flight->cockpit = TieCockpitRenderer_Init(cmd, g_remaster.assets, (uint32_t)TIE_FLIGHT_RT_FORMAT,
											  g_remaster.scene2d.text);
	if (!flight->cockpit) {
		if (!Aeron_FatalErrorRequested())
			Aeron_RequestFatalRendererError("cockpit renderer initialization");
		return false;
	}

	TieCockpitRenderer_SetFlightRenderer(flight->cockpit, flight->renderer);
	TieFlightRenderer_SetCockpit(flight->renderer, flight->cockpit);
	return true;
}

static void TieRemaster_ShutdownFlight(void) {
	TieRemasterFlight* flight = &g_remaster.flight;
	if (flight->vr_cockpit)
		TieCockpitRenderer_Shutdown(flight->vr_cockpit);
	if (flight->vr_eye_target)
		Aeron_DestroyRenderTarget(flight->vr_eye_target);
	if (flight->vr_overlay_target)
		Aeron_DestroyRenderTarget(flight->vr_overlay_target);
	if (flight->vr_overlay_depth)
		Aeron_DestroyDepthTarget(flight->vr_overlay_depth);
	flight->vr_cockpit = NULL;
	flight->vr_eye_target = NULL;
	flight->vr_overlay_target = NULL;
	flight->vr_overlay_depth = NULL;
	flight->vr_width = flight->vr_height = 0;
	if (flight->cockpit)
		TieCockpitRenderer_Shutdown(flight->cockpit);
	if (flight->renderer)
		TieFlightRenderer_Shutdown(flight->renderer);
	if (flight->present_target)
		Aeron_DestroyRenderTarget(flight->present_target);
	memset(flight, 0, sizeof *flight);
}

static bool TieRemaster_RebuildFlight(AeronCommandBuffer* cmd) {
	TieRemaster_ShutdownFlight();
	if (!TieRemaster_InitFlight(cmd, g_remaster.config.remaster_dir, &g_remaster.config))
		return false;
	g_remaster.flight_rebuild_pending = false;
	return true;
}

static bool TieRemaster_Scene2dActive(void) {
	const TieRemaster2D* scene = &g_remaster.scene2d;
	return scene->target && scene->cutscene && TieRemasterSnapshot_HasScene2dBundle(scene->cutscene);
}

static bool TieRemaster_FlightActive(const TieSnapshot* snapshot) {
	return g_remaster.flight.renderer && g_remaster.flight.present_target && snapshot &&
		   snapshot->scene_kind == TIE_SCENE_FLIGHT;
}

static bool TieRemaster_PrepareLoadingFlightAssets(const TieSnapshot* snapshot) {
	if (!snapshot || snapshot->scene_kind != TIE_SCENE_FLIGHT_LOADING ||
		snapshot->mission_load_generation == 0) {
		return true;
	}

	const uint32_t generation = snapshot->mission_load_generation;
	if (g_remaster.loading_generation_seen != generation) {
		/* Let the first snapshot reach Present before doing potentially costly
		 * conversion and upload work on the following loading frame. */
		g_remaster.loading_generation_seen = generation;
		return true;
	}
	if (g_remaster.loading_generation_prepared == generation)
		return true;

	AeronCommandBuffer* cmd = Aeron_AcquireCommandBuffer();
	if (!cmd) {
		Aeron_RequestFatalRendererError("flight loading command-buffer acquisition");
		return false;
	}
	Aeron_GpuDebugPush(cmd, "TIE HD mission asset preparation");
	const bool renderer_ready = !g_remaster.flight_rebuild_pending || TieRemaster_RebuildFlight(cmd);
	const bool prepared = renderer_ready && g_remaster.flight.renderer &&
						  TieFlightRenderer_PrepareMissionAssets(g_remaster.flight.renderer, cmd, snapshot);
	Aeron_GpuDebugPop(cmd);
	if (!prepared || Aeron_FatalErrorRequested()) {
		Aeron_CancelCommandBuffer(cmd);
		if (!Aeron_FatalErrorRequested())
			Aeron_RequestFatalRendererError("flight loading asset preparation");
		return false;
	}
	if (!Aeron_SubmitCommandBuffer(cmd)) {
		Aeron_RequestFatalRendererError("flight loading asset submission");
		return false;
	}
	g_remaster.loading_generation_prepared = generation;
	return true;
}

static bool TieRemaster_PreparePendingFlightRenderer(const TieSnapshot* snapshot) {
	if (!g_remaster.flight_rebuild_pending || !snapshot || snapshot->scene_kind == TIE_SCENE_FLIGHT_LOADING)
		return true;
	AeronCommandBuffer* cmd = Aeron_AcquireCommandBuffer();
	if (!cmd) {
		Aeron_RequestFatalRendererError("flight renderer rebuild command-buffer acquisition");
		return false;
	}
	Aeron_GpuDebugPush(cmd, "TIE HD flight renderer rebuild");
	const bool rebuilt = TieRemaster_RebuildFlight(cmd);
	Aeron_GpuDebugPop(cmd);
	if (!rebuilt || Aeron_FatalErrorRequested()) {
		Aeron_CancelCommandBuffer(cmd);
		if (!Aeron_FatalErrorRequested())
			Aeron_RequestFatalRendererError("flight renderer rebuild");
		return false;
	}
	if (!Aeron_SubmitCommandBuffer(cmd)) {
		Aeron_RequestFatalRendererError("flight renderer rebuild submission");
		return false;
	}
	return true;
}

static bool TieRemaster_FlightScreenImplemented(TieFlightScreen screen) {
	switch (screen) {
		case TIE_FLIGHT_SCREEN_NORMAL:
			return true;
		default:
			return false;
	}
}

static bool TieRemaster_FlightRequiresClassicScreen(const TieSnapshot* snapshot) {
	return snapshot && !TieRemaster_FlightScreenImplemented((TieFlightScreen)snapshot->flight_screen);
}

typedef struct TieRemasterMapQuadContext {
	TieScene2dMapRenderer* map;
	const TieSnapshot* snapshot;
} TieRemasterMapQuadContext;

static void TieRemaster_EmitMapQuad(void* userdata, AeronDrawList2D* list, int viewport_width,
									int viewport_height) {
	TieRemasterMapQuadContext* context = (TieRemasterMapQuadContext*)userdata;
	TieScene2dMap_RecordOverlay(context->map, list, viewport_width, viewport_height, context->snapshot);
}

static bool TieRemaster_ResizeScene2dTarget(TieRemaster2D* scene, AeronCommandBuffer* cmd) {
	const TiePresentationLayout* presentation = TiePresentation_Layout();
	if (!presentation || presentation->render_width <= 0 || presentation->render_height <= 0) {
		return false;
	}
	if (scene->render_generation == presentation->render_generation)
		return true;
	if (scene->target_width == presentation->render_width &&
		scene->target_height == presentation->render_height) {
		scene->render_generation = presentation->render_generation;
		return true;
	}

	AeronRenderTarget* new_target = Aeron_CreateRenderTarget(&(AeronRenderTargetDesc) {
		.width = presentation->render_width,
		.height = presentation->render_height,
		.format = TIE_REMASTER_2D_RT_FORMAT,
		.sample_count = AERON_SAMPLE_COUNT_1,
	});
	if (!new_target) {
		Aeron_RequestFatalRendererError("remaster 2D target resize");
		return false;
	}

	const float transparent[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	AeronDrawList_Begin(scene->draw_list, new_target, presentation->render_width, presentation->render_height,
						AERON_DRAWLIST2D_CLEAR, transparent);
	AeronDrawList_AddSprite(scene->draw_list, &(AeronDrawList2DSprite) {
												  .texture = Aeron_RenderTargetGetTexture(scene->target),
												  .src_u0 = 0.0f,
												  .src_v0 = 0.0f,
												  .src_u1 = 1.0f,
												  .src_v1 = 1.0f,
												  .dst_w = (float)presentation->render_width,
												  .dst_h = (float)presentation->render_height,
												  .tint = { 1.0f, 1.0f, 1.0f, 1.0f },
												  .blend = AERON_BLIT2D_BLEND_NONE,
												  .filter = AERON_BLIT2D_FILTER_LINEAR,
											  });
	if (!AeronDrawList_Prepare(scene->draw_list, cmd)) {
		Aeron_DestroyRenderTarget(new_target);
		Aeron_RequestFatalRendererError("remaster 2D resize preparation");
		return false;
	}
	AeronDrawList_Render(scene->draw_list, cmd);

	scene->retired_target = scene->target;
	scene->target = new_target;
	scene->target_width = presentation->render_width;
	scene->target_height = presentation->render_height;
	scene->render_generation = presentation->render_generation;
	return true;
}

static bool TieRemaster_ReconcileScene2dSizeOnly(void) {
	TieRemaster2D* scene = &g_remaster.scene2d;
	const TiePresentationLayout* presentation = TiePresentation_Layout();
	if (!presentation || scene->render_generation == presentation->render_generation) {
		return presentation != NULL;
	}
	AeronCommandBuffer* cmd = Aeron_AcquireCommandBuffer();
	if (!cmd) {
		Aeron_RequestFatalRendererError("remaster 2D resize command-buffer acquisition");
		return false;
	}
	if (scene->retired_target) {
		Aeron_DestroyRenderTarget(scene->retired_target);
		scene->retired_target = NULL;
	}
	if (!TieRemaster_ResizeScene2dTarget(scene, cmd)) {
		Aeron_CancelCommandBuffer(cmd);
		return false;
	}
	if (!Aeron_SubmitCommandBuffer(cmd)) {
		Aeron_RequestFatalRendererError("remaster 2D resize command-buffer submission");
		return false;
	}
	return true;
}

static bool TieRemaster_RenderScene2dTarget(const TieSnapshot* snapshot, bool full_frame, bool just_unfroze) {
	TieRemaster2D* scene = &g_remaster.scene2d;
	TieScene2dActorView views[256];
	int current_cel = 0;
	const char* lfd = NULL;
	const char* film = NULL;
	int source_w, source_h;
	uint8_t source_pixel_aspect;
	TieRemaster_SnapshotLandruSource(snapshot, &source_w, &source_h, &source_pixel_aspect);
	const TieScene2dTextSpace text_space = TieRemaster_LandruTextSpace(source_w, source_h);
	int view_count = TieRemasterSnapshot_BuildActorViews(views, 256, &current_cel, &lfd, &film);

	AeronCommandBuffer* cmd = Aeron_AcquireCommandBuffer();
	if (!cmd) {
		Aeron_RequestFatalRendererError("remaster 2D command-buffer acquisition");
		return false;
	}
	if (scene->retired_target) {
		Aeron_DestroyRenderTarget(scene->retired_target);
		scene->retired_target = NULL;
	}
	if (!TieRemaster_ResizeScene2dTarget(scene, cmd)) {
		Aeron_CancelCommandBuffer(cmd);
		return false;
	}

	TieScene2dCutscene_PrepUploads(scene->cutscene, cmd, lfd, film, current_cel, views, view_count);
	bool prepared = !scene->text || TieScene2dTextRenderer_PrepTitleCrawl(scene->text, cmd, snapshot);
	if (scene->map && snapshot) {
		prepared = TieScene2dMap_Prep(scene->map, cmd, snapshot, views, view_count, lfd, film, current_cel) &&
				   prepared;
	}
	if (!prepared || Aeron_FatalErrorRequested()) {
		Aeron_CancelCommandBuffer(cmd);
		if (!Aeron_FatalErrorRequested())
			Aeron_RequestFatalRendererError("remaster 2D resource preparation");
		return false;
	}

	bool tag_changed = !snapshot || strncmp(g_remaster.last_scene_tag, snapshot->scene_tag,
											sizeof g_remaster.last_scene_tag) != 0;
	if (snapshot) {
		memcpy(g_remaster.last_scene_tag, snapshot->scene_tag, sizeof g_remaster.last_scene_tag);
	}
	bool clear_target = full_frame || tag_changed || just_unfroze;
	bool has_work = snapshot && ((int)snapshot->ui_text_count > 0 || (int)snapshot->paint_cmd_count > 0 ||
								 view_count > 0 || (scene->map && snapshot->map.active) ||
								 snapshot->title_crawl_count > 0);
	if (!clear_target && !has_work) {
		if (!Aeron_SubmitCommandBuffer(cmd)) {
			Aeron_RequestFatalRendererError("remaster 2D upload command-buffer submission");
			return false;
		}
		return true;
	}

	bool opaque_background = lfd && film &&
							 TieScene2dCutscene_BundleMatchesSource(scene->cutscene, lfd, film, source_w,
																	source_h, source_pixel_aspect) &&
							 TieScene2dCutscene_BundleIsOpaque(scene->cutscene, lfd, film);
	const float clear[4] = {
		0.0f,
		0.0f,
		0.0f,
		opaque_background ? 1.0f : 0.0f,
	};
	AeronDrawList_Begin(scene->draw_list, scene->target, scene->target_width, scene->target_height,
						clear_target ? AERON_DRAWLIST2D_CLEAR : AERON_DRAWLIST2D_LOAD, clear);

	if (full_frame) {
		TieScene2dCutscene_RecordActorsInSource(scene->cutscene, scene->draw_list, scene->target_width,
												scene->target_height, source_w, source_h, source_pixel_aspect,
												lfd, film, current_cel, views, view_count);
		if (scene->text && snapshot && snapshot->ui_text_count > 0) {
			TieScene2dTextRenderer_RecordInSpace(scene->text, scene->draw_list, scene->target_width,
												 scene->target_height, &text_space, snapshot->ui_texts,
												 (int)snapshot->ui_text_count, snapshot->palette);
		}
	} else {
		bool map_pending = scene->map && snapshot && snapshot->map.active;
		TieRemasterMapQuadContext map_context = { scene->map, snapshot };
		TieScene2dSnapshotMergeDispatch dispatch = {
			.views = views,
			.view_count = view_count,
			.ui_texts = snapshot ? snapshot->ui_texts : NULL,
			.ui_text_count = snapshot ? (int)snapshot->ui_text_count : 0,
			.paint_cmds = snapshot ? snapshot->paint_cmds : NULL,
			.paint_cmd_count = snapshot ? (int)snapshot->paint_cmd_count : 0,
			.palette = snapshot ? snapshot->palette : NULL,
			.source_w = source_w,
			.source_h = source_h,
			.source_pixel_aspect = source_pixel_aspect,
			.accept_target = TIE_EMIT_TARGET_CUTSCENE,
			.cutscene = scene->cutscene,
			.text_renderer = scene->text,
			.lfd = lfd,
			.film = film,
			.cur_cel = current_cel,
			.map_quad_z = map_pending ? (int)snapshot->map.start_z : 0,
			.emit_map_quad = map_pending ? TieRemaster_EmitMapQuad : NULL,
			.map_quad_userdata = &map_context,
		};
		TieScene2dSnapshotDispatch_Run(scene->draw_list, scene->target_width, scene->target_height,
									   &dispatch);
	}

	if (scene->text && snapshot && snapshot->title_crawl_count > 0) {
		TieScene2dTextRenderer_RecordTitleCrawl(scene->text, scene->draw_list, scene->target_width,
												scene->target_height, snapshot, snapshot->palette);
	}
	if (!AeronDrawList_Prepare(scene->draw_list, cmd)) {
		Aeron_CancelCommandBuffer(cmd);
		Aeron_RequestFatalRendererError("remaster 2D draw-list preparation");
		return false;
	}
	AeronDrawList_Render(scene->draw_list, cmd);

	if (!Aeron_SubmitCommandBuffer(cmd)) {
		Aeron_RequestFatalRendererError("remaster 2D command-buffer submission");
		return false;
	}
	return true;
}

static bool TieRemaster_RenderFlightTarget(const TieSnapshot* snapshot, int32_t delta_us,
										   uint64_t host_time_us, bool paused) {
	TieRemasterFlight* flight = &g_remaster.flight;
	const TiePresentationLayout* presentation = TiePresentation_Layout();
	if (!presentation)
		return false;
	if (flight->render_generation != presentation->render_generation) {
		if (flight->target_width != presentation->render_width ||
			flight->target_height != presentation->render_height) {
			if (!TieFlightRenderer_EnsureOutputSize(flight->renderer, presentation->render_width,
													presentation->render_height)) {
				return false;
			}
			AeronRenderTarget* new_target = Aeron_CreateRenderTarget(&(AeronRenderTargetDesc) {
				.width = presentation->render_width,
				.height = presentation->render_height,
				.format = TIE_FLIGHT_PRESENT_RT_FORMAT,
				.sample_count = AERON_SAMPLE_COUNT_1,
			});
			if (!new_target) {
				if (!Aeron_FatalErrorRequested())
					Aeron_RequestFatalRendererError("flight target resize");
				return false;
			}
			Aeron_DestroyRenderTarget(flight->present_target);
			flight->present_target = new_target;
			flight->target_width = presentation->render_width;
			flight->target_height = presentation->render_height;
		}
		flight->render_generation = presentation->render_generation;
	}
	AeronCommandBuffer* cmd = Aeron_AcquireCommandBuffer();
	if (!cmd) {
		Aeron_RequestFatalRendererError("flight command-buffer acquisition");
		return false;
	}

	bool ok = TieFlightRenderer_PrepareFrame(flight->renderer, cmd, TieSnapshot_Previous(), snapshot,
											 delta_us, host_time_us, paused);
	if (ok && flight->cockpit)
		ok = TieCockpitRenderer_Prepare(flight->cockpit, cmd, snapshot);
	if (Aeron_FatalErrorRequested())
		ok = false;
	if (ok)
		ok = TieFlightRenderer_RenderFrame(flight->renderer, cmd);

	if (ok) {
		AeronRenderPass* pass = Aeron_BeginRenderPass(&(AeronRenderPassDesc) {
			.color_target = flight->present_target,
			.clear_color = 1,
			.clear_color_rgba = { 0.0f, 0.0f, 0.0f, 0.0f },
			.command_buffer = cmd,
		});
		ok = pass != NULL;
		if (pass) {
			static const float tint[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
			TieFlightRenderer_Present(flight->renderer, cmd, pass, tint);
			Aeron_EndRenderPass(pass);
		}
	}
	if (!ok) {
		Aeron_CancelCommandBuffer(cmd);
		if (!Aeron_FatalErrorRequested())
			Aeron_RequestFatalRendererError("flight frame recording");
		return false;
	}
	if (!Aeron_SubmitCommandBuffer(cmd)) {
		Aeron_RequestFatalRendererError("flight command-buffer submission");
		return false;
	}
	return true;
}

static bool TieRemaster_EnsureVrTargets(AeronCommandBuffer* cmd, int width, int height) {
	TieRemasterFlight* flight = &g_remaster.flight;
	if (!flight->vr_cockpit) {
		flight->vr_cockpit = TieCockpitRenderer_Init(cmd, g_remaster.assets,
													 (uint32_t)AERON_TEXTURE_FORMAT_RGBA16_FLOAT,
													 g_remaster.scene2d.text);
		if (!flight->vr_cockpit)
			return false;
		TieCockpitRenderer_SetFlightRenderer(flight->vr_cockpit, flight->renderer);
		Aeron_LogInfo("tie.vr", "stereo flight: overlay cockpit renderer ready");
	}
	if (flight->vr_width == width && flight->vr_height == height && flight->vr_eye_target)
		return true;
	if (flight->vr_eye_target)
		Aeron_DestroyRenderTarget(flight->vr_eye_target);
	if (flight->vr_overlay_target)
		Aeron_DestroyRenderTarget(flight->vr_overlay_target);
	if (flight->vr_overlay_depth)
		Aeron_DestroyDepthTarget(flight->vr_overlay_depth);
	flight->vr_eye_target = Aeron_CreateRenderTarget(&(AeronRenderTargetDesc) {
		.width = width,
		.height = height,
		.format = TIE_FLIGHT_PRESENT_RT_FORMAT,
		.sample_count = AERON_SAMPLE_COUNT_1,
	});
	flight->vr_overlay_target = Aeron_CreateRenderTarget(&(AeronRenderTargetDesc) {
		.width = width,
		.height = height,
		.format = AERON_TEXTURE_FORMAT_RGBA16_FLOAT,
		.sample_count = AERON_SAMPLE_COUNT_1,
	});
	flight->vr_overlay_depth = Aeron_CreateDepthTarget(&(AeronDepthTargetDesc) {
		.width = width,
		.height = height,
		.format = AERON_TEXTURE_FORMAT_D32_FLOAT,
		.sample_count = AERON_SAMPLE_COUNT_1,
	});
	flight->vr_width = width;
	flight->vr_height = height;
	Aeron_LogInfo("tie.vr", "stereo flight targets %dx%d", width, height);
	return flight->vr_eye_target && flight->vr_overlay_target && flight->vr_overlay_depth;
}

/* Tangents of the flat presentation frame's edges as seen from the seat:
 * the game camera's frustum over the modern (flight) rect, extended to the
 * whole frame, so the host can place the cockpit/HUD overlay to line up with
 * the stereo world. */
static void TieRemaster_VrFrameTangents(const TieSnapshot* snapshot, int width, int height, float out[4]) {
	const TiePresentationLayout* presentation = TiePresentation_Layout();
	TieFlightCamera fcam;
	TieRenderMath_BuildCamera(&fcam, &snapshot->camera, width, height);
	const float th = tanf(fcam.h_half_rad);
	const float tv = tanf(fcam.v_half_rad);
	float left = th * (1.0f + fcam.proj_x_offset);
	float right = th * (1.0f - fcam.proj_x_offset);
	float up = tv * (1.0f - fcam.proj_y_offset);
	float down = tv * (1.0f + fcam.proj_y_offset);
	if (presentation && presentation->modern.width > 0 && presentation->modern.height > 0 &&
		presentation->frame.width > 0 && presentation->frame.height > 0) {
		const AeronRectI m = presentation->modern;
		const AeronRectI f = presentation->frame;
		const float kx = (left + right) / (float)m.width;
		const float ky = (up + down) / (float)m.height;
		left += (float)(m.x - f.x) * kx;
		right += (float)((f.x + f.width) - (m.x + m.width)) * kx;
		up += (float)(m.y - f.y) * ky;
		down += (float)((f.y + f.height) - (m.y + m.height)) * ky;
	}
	out[0] = left;
	out[1] = right;
	out[2] = up;
	out[3] = down;
}

/* Quest stereo flight: the 3D world once per head-tracked eye straight into
 * the OpenXR eye images, then the cockpit/HUD alone (transparent elsewhere)
 * into the flat presentation target, which the host shows as an overlay. */
static bool TieRemaster_RenderFlightVr(const TieSnapshot* snapshot, const TieVrStereoFrame* frame,
									   int32_t delta_us, uint64_t host_time_us, bool paused) {
	TieRemasterFlight* flight = &g_remaster.flight;
	const TiePresentationLayout* presentation = TiePresentation_Layout();
	if (!presentation || flight->render_generation != presentation->render_generation)
		return false; /* let the flat path resize first */
	const int width = flight->target_width;
	const int height = flight->target_height;
	AeronCommandBuffer* cmd = Aeron_AcquireCommandBuffer();
	if (!cmd)
		return false;
	bool ok = TieRemaster_EnsureVrTargets(cmd, width, height);
	/* 3D cockpit: X-Wing Alliance's cockpit model for this craft (from the
	 * player's XWA data) or the generated TIE Fighter one in the eyes, and
	 * only the instruments in the overlay. Without a model the craft keeps
	 * its flat cockpit overlay. */
	const TieVrCockpitView* vr_view =
		snapshot->cockpit.classic_w == 640 ? TieVrCockpit_FindView(snapshot->cockpit.view_name) : NULL;
	const bool cockpit3d = TieFlightRenderer_SetVrCockpit(flight->renderer, cmd, vr_view ? vr_view->xwa_model : NULL,
														  vr_view && vr_view->has_builtin);
	if (flight->vr_cockpit)
		TieCockpitRenderer_SetVrInstrumentsOnly(flight->vr_cockpit, cockpit3d);
	for (int eye = 0; ok && eye < 2; ++eye) {
		TieFlightRenderer_SetVrEye(flight->renderer, &frame->eyes[eye], eye);
		ok = TieFlightRenderer_PrepareFrame(flight->renderer, cmd, TieSnapshot_Previous(), snapshot, delta_us,
											host_time_us, paused);
		if (ok && eye == 0) {
			ok = TieCockpitRenderer_Prepare(flight->vr_cockpit, cmd, snapshot);
		}
		if (ok)
			ok = TieFlightRenderer_RenderFrame(flight->renderer, cmd);
		if (ok) {
			AeronRenderPass* pass = Aeron_BeginRenderPass(&(AeronRenderPassDesc) {
				.color_target = flight->vr_eye_target,
				.clear_color = 1,
				.clear_color_rgba = { 0.0f, 0.0f, 0.0f, 1.0f },
				.command_buffer = cmd,
			});
			ok = pass != NULL;
			if (pass) {
				static const float tint[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
				TieFlightRenderer_Present(flight->renderer, cmd, pass, tint);
				Aeron_EndRenderPass(pass);
			}
		}
		if (ok)
			ok = Aeron_BlitTextureToExternal(cmd, Aeron_RenderTargetGetTexture(flight->vr_eye_target),
											 frame->eyes[eye].target, frame->eyes[eye].target_width,
											 frame->eyes[eye].target_height) != 0;
	}
	TieFlightRenderer_SetVrEye(flight->renderer, NULL, 0);

	if (ok) {
		AeronRenderPass* pass = Aeron_BeginRenderPass(&(AeronRenderPassDesc) {
			.color_target = flight->vr_overlay_target,
			.depth_target = flight->vr_overlay_depth,
			.clear_color = 1,
			.clear_color_rgba = { 0.0f, 0.0f, 0.0f, 0.0f },
			.clear_depth = 1,
			.clear_depth_value = 0.0f,
			.command_buffer = cmd,
		});
		ok = pass != NULL;
		if (pass) {
			const AeronRectI full = { 0, 0, width, height };
			Aeron_SetViewport(pass, &full);
			Aeron_SetScissor(pass, &full);
			TieCockpitRenderer_RenderInPass(flight->vr_cockpit, cmd, pass, flight->vr_overlay_target, snapshot);
			Aeron_EndRenderPass(pass);
		}
	}
	if (ok) {
		AeronRenderPass* pass = Aeron_BeginRenderPass(&(AeronRenderPassDesc) {
			.color_target = flight->present_target,
			.clear_color = 1,
			.clear_color_rgba = { 0.0f, 0.0f, 0.0f, 0.0f },
			.command_buffer = cmd,
		});
		ok = pass != NULL;
		if (pass) {
			TieFlightRenderer_PresentOverlay(flight->renderer, cmd, pass,
											 Aeron_RenderTargetGetTexture(flight->vr_overlay_target), width, height);
			Aeron_EndRenderPass(pass);
		}
	}
	if (!ok) {
		Aeron_CancelCommandBuffer(cmd);
		Aeron_LogWarn("tie.vr", "stereo flight frame failed; using the flat view");
		return false;
	}
	if (!Aeron_SubmitCommandBuffer(cmd)) {
		Aeron_RequestFatalRendererError("VR flight command-buffer submission");
		return false;
	}
	float tangents[4];
	TieRemaster_VrFrameTangents(snapshot, width, height, tangents);
	TieRemasterVr_SetRendered(1, tangents);
	if (cockpit3d && presentation->modern.width > 0 && presentation->modern.height > 0) {
		/* Classic 640x480 cockpit pixels -> flat frame pixels. */
		const float map[4] = {
			(float)(presentation->modern.x - presentation->frame.x),
			(float)(presentation->modern.y - presentation->frame.y),
			(float)presentation->modern.width / 640.0f,
			(float)presentation->modern.height / 480.0f,
		};
		const float frame_size[2] = { (float)presentation->frame.width, (float)presentation->frame.height };
		TieRemasterVr_SetCockpit3d(1, map, frame_size, vr_view->view);
	}
	static int logged = -1;
	if (logged != (int)cockpit3d) {
		logged = (int)cockpit3d;
		Aeron_LogInfo("tie.vr", "cockpit view %.10s: %s", snapshot->cockpit.view_name,
					  cockpit3d ? "3D cockpit" : "flat cockpit overlay");
		Aeron_LogInfo("tie.vr", "stereo flight active: frame %d,%d %dx%d modern %d,%d %dx%d, tangents L%.3f R%.3f U%.3f D%.3f",
					  presentation->frame.x, presentation->frame.y, presentation->frame.width,
					  presentation->frame.height, presentation->modern.x, presentation->modern.y,
					  presentation->modern.width, presentation->modern.height, (double)tangents[0],
					  (double)tangents[1], (double)tangents[2], (double)tangents[3]);
	}
	return true;
}

static bool TieRemaster_FlightCameraValid(const TieSnapshot* snapshot) {
	return snapshot && snapshot->camera.fov_h_half_rad > 0.0f && snapshot->camera.fov_v_half_rad > 0.0f;
}

static bool TieRemaster_FlightRenderDue(const TieSnapshot* snapshot) {
	TieRemasterFlight* flight = &g_remaster.flight;
	const TiePresentationLayout* presentation = TiePresentation_Layout();
	const bool camera_valid = TieRemaster_FlightCameraValid(snapshot);
	/* PORT: The bonus task suspends world motion, so flight_frame remains
	 * fixed while the timer and score continue changing. */
	const bool training_bonus_changed =
		(flight->last_training_bonus_active || snapshot->hud.training.bonus_active) &&
		(flight->last_training_bonus_active != snapshot->hud.training.bonus_active ||
		 flight->last_training_timer_min != snapshot->hud.training.timer_min ||
		 flight->last_training_timer_sec != snapshot->hud.training.timer_sec ||
		 flight->last_training_score != snapshot->hud.training.score ||
		 flight->last_training_bonus != snapshot->hud.training.bonus);
	return !flight->rendered || flight->force_render || !presentation ||
		   flight->render_generation != presentation->render_generation ||
		   flight->last_flight_frame != snapshot->flight_frame ||
		   flight->last_mission_generation != snapshot->mission_load_generation ||
		   flight->last_camera_valid != (uint8_t)camera_valid || training_bonus_changed;
}

static void TieRemaster_FlightRenderCommitted(const TieSnapshot* snapshot) {
	TieRemasterFlight* flight = &g_remaster.flight;
	flight->rendered = true;
	flight->force_render = false;
	flight->last_flight_frame = snapshot->flight_frame;
	flight->last_mission_generation = snapshot->mission_load_generation;
	flight->last_camera_valid = (uint8_t)TieRemaster_FlightCameraValid(snapshot);
	flight->last_training_bonus_active = snapshot->hud.training.bonus_active;
	flight->last_training_timer_min = snapshot->hud.training.timer_min;
	flight->last_training_timer_sec = snapshot->hud.training.timer_sec;
	flight->last_training_score = snapshot->hud.training.score;
	flight->last_training_bonus = snapshot->hud.training.bonus;
}

static void TieRemaster_SubmitFlightLayer(float alpha) {
	const TiePresentationLayout* presentation = TiePresentation_Layout();
	if (!presentation)
		return;
	AeronTextureLayerDesc layer = { 0 };
	layer.texture = Aeron_RenderTargetGetTexture(g_remaster.flight.present_target);
	layer.logical_rect = presentation->modern;
	layer.blend_mode = AERON_LAYER_BLEND_PREMULTIPLIED;
	layer.color_space = AERON_COLOR_SPACE_LINEAR_SRGB;
	layer.tint_enabled = 1;
	for (int i = 0; i < 4; ++i)
		layer.tint_rgba[i] = alpha;
	if (g_remaster.view_mode == TIE_REMASTER_VIEW_SPLIT) {
		layer.scissor = presentation->split_scissor;
	}
	if (!Aeron_SubmitTextureLayer(&layer))
		Aeron_RequestFatalRendererError("flight texture layer submission");
}

static void TieRemaster_DrawFlightToSwapchain(AeronCommandBuffer* command_buffer,
											  AeronRenderPass* render_pass, AeronRenderTarget* target,
											  int target_width, int target_height, void* userdata) {
	(void)target;
	(void)target_width;
	(void)target_height;
	TieFlightRenderer_PresentSwapchain((TieFlightRenderer*)userdata, command_buffer, render_pass);
}

static bool TieRemaster_SubmitDirectFlightLayer(void) {
	const TiePresentationLayout* presentation = TiePresentation_Layout();
	if (!presentation ||
		!Aeron_CanRenderDirectToSwapchain(presentation->render_width, presentation->render_height))
		return false;
	const AeronSwapchainRenderLayerDesc layer = {
		.callback = TieRemaster_DrawFlightToSwapchain,
		.userdata = g_remaster.flight.renderer,
		.required_width = presentation->render_width,
		.required_height = presentation->render_height,
		.debug_label = "TIE HD flight direct present",
	};
	if (!Aeron_SubmitSwapchainRenderLayer(&layer)) {
		Aeron_RequestFatalRendererError("direct flight presentation");
		return false;
	}
	return true;
}

static void TieRemaster_SubmitScene2dLayers(const TieSnapshot* snapshot, float alpha) {
	TieRemaster2D* scene = &g_remaster.scene2d;
	const TiePresentationLayout* presentation = TiePresentation_Layout();
	if (!presentation)
		return;
	float hd_factor = 1.0f;
	float fade_r = 0.0f;
	float fade_g = 0.0f;
	float fade_b = 0.0f;
	if (snapshot && snapshot->fade.kind == TIE_FADE_ACTIVE) {
		hd_factor = (float)snapshot->fade.hd_factor / 255.0f;
		fade_r = TieScene2dSrgb_ByteToLinear(snapshot->fade.r);
		fade_g = TieScene2dSrgb_ByteToLinear(snapshot->fade.g);
		fade_b = TieScene2dSrgb_ByteToLinear(snapshot->fade.b);
	}

	AeronTextureLayerDesc layer = { 0 };
	layer.texture = Aeron_RenderTargetGetTexture(scene->target);
	layer.logical_rect = presentation->modern;
	layer.blend_mode = AERON_LAYER_BLEND_PREMULTIPLIED;
	layer.color_space = AERON_COLOR_SPACE_LINEAR_DISPLAY;
	layer.tint_enabled = 1;
	layer.tint_rgba[0] = hd_factor * alpha;
	layer.tint_rgba[1] = hd_factor * alpha;
	layer.tint_rgba[2] = hd_factor * alpha;
	layer.tint_rgba[3] = alpha;
	layer.bias_rgba[0] = fade_r * (1.0f - hd_factor) * alpha;
	layer.bias_rgba[1] = fade_g * (1.0f - hd_factor) * alpha;
	layer.bias_rgba[2] = fade_b * (1.0f - hd_factor) * alpha;
	if (g_remaster.view_mode == TIE_REMASTER_VIEW_SPLIT) {
		layer.scissor = presentation->split_scissor;
	}
	if (!Aeron_SubmitTextureLayer(&layer)) {
		Aeron_RequestFatalRendererError("remaster 2D layer submission");
	}
}

typedef struct TieCursorLayerPlacement {
	float hotspot_x;
	float hotspot_y;
	float scale_x;
	float scale_y;
	float opacity;
	bool scissor_split;
} TieCursorLayerPlacement;

static bool TieRemaster_ResolveCursorLayerPlacement(const TieSnapshot* snapshot, bool scene2d_is_active,
													float scene2d_alpha, TieCursorLayerPlacement* out) {
	const TiePresentationLayout* presentation = TiePresentation_Layout();
	const bool external = TieClassicDisplay_UsesExternalCursor();
	if (!snapshot || !snapshot->cursor.visible || snapshot->cursor.w <= 0 || snapshot->cursor.h <= 0 ||
		!presentation || !out || (!external && (!scene2d_is_active || scene2d_alpha <= 0.0f))) {
		return false;
	}

	const int source_w = snapshot->landru_coord_w ? snapshot->landru_coord_w : CLASSIC_FB_W;
	const int source_h = snapshot->landru_coord_h ? snapshot->landru_coord_h : CLASSIC_FB_H;
	if (source_w <= 0 || source_h <= 0)
		return false;
	const bool tie98_external = external && snapshot->frontend_profile_id == TIE_FRONTEND_PROFILE_TIE98;
	const bool presented_vga =
		external && g_remaster.aspect_correct_legacy_scenes && TieClassicFramebuffer_PresentedVga() != NULL;
	if (presented_vga && (source_w != CLASSIC_FB_W || source_h != CLASSIC_FB_H))
		return false;
	const bool aspect_corrected_vga = presented_vga;

	AeronRectI source_rect = presentation->classic;
	if (tie98_external && !aspect_corrected_vga && source_w == CLASSIC_FB_W && source_h == CLASSIC_FB_H) {
		/* TIE98's VGA compatibility copy doubles the source into the middle
		 * 640x400 rows of its 640x480 render surface. */
		source_rect.y += presentation->classic.height * 40 / 480;
		source_rect.height = presentation->classic.height * 400 / 480;
	}

	float framebuffer_x = 0.0f;
	float framebuffer_y = 0.0f;
	TieInput_CursorFramebufferPosition(&framebuffer_x, &framebuffer_y);
	out->hotspot_x = source_rect.x + framebuffer_x * source_rect.width / (float)source_w;
	out->hotspot_y = source_rect.y + framebuffer_y * source_rect.height / (float)source_h;
	if (external) {
		if (aspect_corrected_vga || !tie98_external) {
			out->scale_x = source_rect.width / (float)source_w;
			out->scale_y = source_rect.height / (float)source_h;
		} else {
			/* The reused Landru artwork is a 16x16 VGA cursor. Double it uniformly
			 * to the 32x32 cursor size of the TIE98 presentation. */
			out->scale_x = presentation->classic.width / 320.0f;
			out->scale_y = presentation->classic.height / 240.0f;
		}
		out->opacity = 1.0f;
		out->scissor_split = false;
	} else {
		out->scale_x = source_rect.width / (float)source_w;
		out->scale_y = source_rect.height / (float)source_h;
		out->opacity = scene2d_alpha;
		out->scissor_split = g_remaster.view_mode == TIE_REMASTER_VIEW_SPLIT;
	}
	return true;
}

static bool TieRemaster_RenderPresentationCursor(const TieSnapshot* snapshot,
												 const TieCursorLayerPlacement* placement) {
	TiePresentationCursor* cursor = &g_remaster.cursor;
	if (cursor->rendered && cursor->scale_x == placement->scale_x && cursor->scale_y == placement->scale_y &&
		cursor->hot_x == snapshot->cursor.hot_x && cursor->hot_y == snapshot->cursor.hot_y &&
		cursor->width == snapshot->cursor.w && cursor->height == snapshot->cursor.h &&
		cursor->kind == snapshot->cursor.kind &&
		memcmp(cursor->palette, snapshot->palette, sizeof cursor->palette) == 0) {
		return true;
	}

	AeronCommandBuffer* cmd = Aeron_AcquireCommandBuffer();
	if (!cmd) {
		Aeron_RequestFatalRendererError("cursor command-buffer acquisition");
		return false;
	}
	int16_t bitmap_width = 0;
	int16_t bitmap_height = 0;
	const uint8_t* bitmap = TieCursorSnapshot_Bitmap(&bitmap_width, &bitmap_height);
	if (!TieScene2dCursor_Prep(cursor->renderer, cmd, bitmap, bitmap_width, bitmap_height,
							   snapshot->palette)) {
		Aeron_CancelCommandBuffer(cmd);
		return false;
	}

	const float clear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	AeronDrawList_Begin(cursor->draw_list, cursor->target, CURSOR_RT_DIM, CURSOR_RT_DIM,
						AERON_DRAWLIST2D_CLEAR, clear);
	TieScene2dCursor_RecordLayer(cursor->renderer, cursor->draw_list, CURSOR_RT_DIM, CURSOR_RT_DIM,
								 CURSOR_RT_DIM / 2.0f, CURSOR_RT_DIM / 2.0f, placement->scale_x,
								 placement->scale_y, &snapshot->cursor);
	if (!AeronDrawList_Prepare(cursor->draw_list, cmd)) {
		Aeron_CancelCommandBuffer(cmd);
		Aeron_RequestFatalRendererError("cursor draw-list preparation");
		return false;
	}
	AeronDrawList_Render(cursor->draw_list, cmd);
	if (!Aeron_SubmitCommandBuffer(cmd)) {
		Aeron_RequestFatalRendererError("cursor command-buffer submission");
		return false;
	}
	cursor->scale_x = placement->scale_x;
	cursor->scale_y = placement->scale_y;
	cursor->hot_x = snapshot->cursor.hot_x;
	cursor->hot_y = snapshot->cursor.hot_y;
	cursor->width = snapshot->cursor.w;
	cursor->height = snapshot->cursor.h;
	cursor->kind = snapshot->cursor.kind;
	memcpy(cursor->palette, snapshot->palette, sizeof cursor->palette);
	cursor->rendered = true;
	return true;
}

static void TieRemaster_SubmitPresentationCursor(const TieSnapshot* snapshot, bool scene2d_is_active,
												 float scene2d_alpha) {
	TieCursorLayerPlacement placement;
	if (!TieRemaster_ResolveCursorLayerPlacement(snapshot, scene2d_is_active, scene2d_alpha, &placement) ||
		!TieRemaster_RenderPresentationCursor(snapshot, &placement)) {
		return;
	}

	AeronTextureLayerDesc cursor = { 0 };
	cursor.texture = Aeron_RenderTargetGetTexture(g_remaster.cursor.target);
	cursor.logical_rect = (AeronRectI) {
		(int)(placement.hotspot_x - CURSOR_RT_DIM / 2.0f),
		(int)(placement.hotspot_y - CURSOR_RT_DIM / 2.0f),
		CURSOR_RT_DIM,
		CURSOR_RT_DIM,
	};
	cursor.blend_mode = AERON_LAYER_BLEND_PREMULTIPLIED;
	cursor.color_space = AERON_COLOR_SPACE_LINEAR_DISPLAY;
	cursor.tint_enabled = 1;
	for (int i = 0; i < 4; ++i)
		cursor.tint_rgba[i] = placement.opacity;
	if (placement.scissor_split)
		cursor.scissor = TiePresentation_Layout()->split_scissor;
	if (!Aeron_SubmitTextureLayer(&cursor))
		Aeron_RequestFatalRendererError("cursor texture layer submission");
}

bool TieRemaster_Init(AeronCommandBuffer* startup_cmd, const TieRemasterConfig* config) {
	if (!config)
		return false;
	if (g_remaster.initialized) {
		Aeron_RequestFatalRendererError("duplicate remaster initialization");
		return false;
	}

	memset(&g_remaster, 0, sizeof g_remaster);
	g_remaster.initialized = true;
	g_remaster.aspect_correct_legacy_scenes = config->aspect_correct_legacy_scenes;
	g_remaster.view_mode = TIE_REMASTER_VIEW_HD;
	g_remaster.video_options = config->video_options;
	g_remaster.video_options_pending = true;
	g_remaster.config = *config;
	Aeron_BlendRampInit(&g_remaster.blend);
	AeronScenePresent_ApplySettings(&config->render.tonemap);

	if (!TieRemaster_ApplyPendingVideoOptions())
		return false;
	if (!startup_cmd) {
		Aeron_RequestFatalRendererError("remaster startup command buffer");
		return false;
	}
	g_remaster.assets = config->flight_source;
	if (!g_remaster.assets) {
		Aeron_RequestFatalError("Flight Asset Error", "selected flight model source is unavailable");
		return false;
	}
	const TiePresentationLayout* presentation = TiePresentation_Layout();
	if (!presentation)
		return false;
	if (!TieRemaster_InitPresentationCursor())
		return false;
	if (config->remaster_dir &&
		!TieRemaster_InitScene2d(startup_cmd, config->remaster_dir, config->frontend_profile_id,
								 presentation->render_width, presentation->render_height,
								 presentation->render_generation))
		return false;
	if (!TieRemaster_InitFlight(startup_cmd, config->remaster_dir, config))
		return false;
	return !Aeron_FatalErrorRequested();
}

void TieRemaster_BeginFrame(const AeronInputSnapshot* input) {
	if (!g_remaster.initialized)
		return;
	if (!TieRemaster_ApplyPendingVideoOptions())
		return;

	const TieSnapshot* snapshot = TieSnapshot_Current();
	bool has_modern_target = TieRemaster_Scene2dActive() || TieRemaster_FlightActive(snapshot);
	if (input && input->key_pressed[AERON_KEY_TAB] && has_modern_target) {
		if (input->has_focus && !TieRemaster_ModifierActiveThisFrame(input)) {
			const TieRemasterViewMode selected =
				g_remaster.pending_view_mode_valid ? g_remaster.pending_view_mode : g_remaster.view_mode;
			const TieRemasterViewMode requested = TieRemaster_NextViewMode(selected);
			if (g_remaster.pending_view_mode_valid) {
				g_remaster.pending_view_mode = requested;
			} else if (g_remaster.view_mode == TIE_REMASTER_VIEW_HD && requested != TIE_REMASTER_VIEW_HD &&
					   TieClassicDisplay_OutputKind() == TIE_CLASSIC_OUTPUT_DX5_SURFACE &&
					   AeronDx5_IsClassicFlightRenderingSuppressed()) {
				g_remaster.pending_view_mode = requested;
				g_remaster.pending_view_mode_valid = true;
				g_remaster.pending_classic_frame_serial = AeronDx5_GetClassicFlightFrameSerial();
				/* Re-enable the recovered pass before this host frame advances. */
				AeronDx5_SetClassicFlightRenderingSuppressed(0);
			} else {
				g_remaster.pending_view_mode_valid = false;
				g_remaster.view_mode = requested;
				g_remaster.flight.force_render = true;
				g_remaster.blend.target = requested == TIE_REMASTER_VIEW_CLASSIC ? 0.0f : 1.0f;
			}
		}
		TieInput_SuppressKey(AERON_KEY_TAB);
	}
}

bool TieRemaster_Frame(const TieSnapshot* snapshot, int32_t delta_us, bool paused) {
	if (!g_remaster.initialized)
		return true;
	if (delta_us > 0)
		g_remaster.host_elapsed_us += (uint32_t)delta_us;
	if (!TieRemaster_PrepareLoadingFlightAssets(snapshot) ||
		!TieRemaster_PreparePendingFlightRenderer(snapshot))
		return false;

	if (g_remaster.pending_view_mode_valid &&
		AeronDx5_GetClassicFlightFrameSerial() != g_remaster.pending_classic_frame_serial) {
		g_remaster.view_mode = g_remaster.pending_view_mode;
		g_remaster.pending_view_mode_valid = false;
		g_remaster.flight.force_render = true;
		g_remaster.blend.target = g_remaster.view_mode == TIE_REMASTER_VIEW_CLASSIC ? 0.0f : 1.0f;
	}

	bool scene2d_is_active = TieRemaster_Scene2dActive();
	bool flight_is_active =
		TieRemaster_FlightActive(snapshot) && !TieRemaster_FlightRequiresClassicScreen(snapshot);
	Aeron_BlendRampAdvance(&g_remaster.blend, delta_us, g_remaster.blend.target);
	float alpha = (scene2d_is_active || flight_is_active) ? g_remaster.blend.alpha : 0.0f;

	if (snapshot) {
		TieInput_UpdateCursor(alpha > 0.0f, snapshot->cursor.x, snapshot->cursor.y);
	}

	bool freeze_overlay = snapshot && snapshot->fade.freeze_overlay;
	bool just_unfroze = g_remaster.overlay_was_frozen && !freeze_overlay;
	g_remaster.overlay_was_frozen = freeze_overlay;

	bool compose_scene2d = scene2d_is_active && alpha > 0.0f;
	if (compose_scene2d && !freeze_overlay) {
		bool full_frame = snapshot && snapshot->redraw_model == TIE_REDRAW_FULL_FRAME;
		compose_scene2d = TieRemaster_RenderScene2dTarget(snapshot, full_frame, just_unfroze);
	} else if (compose_scene2d && freeze_overlay) {
		compose_scene2d = TieRemaster_ReconcileScene2dSizeOnly();
	}

	bool compose_flight = flight_is_active && alpha > 0.0f;
	TieRemasterFlight* flight = &g_remaster.flight;
	if (!compose_flight && flight_is_active && !flight->render_suspended) {
		flight->render_suspended = true;
		TieFlightRenderer_InvalidateHistory(flight->renderer);
	} else if (compose_flight && flight->render_suspended) {
		flight->render_suspended = false;
		flight->force_render = true;
	} else if (!flight_is_active) {
		flight->render_suspended = false;
	}
	/* Quest: head-tracked stereo whenever the HD cockpit view is on screen
	 * (the map and other full-screen flight pages stay on the flat screen). */
	const TieVrStereoFrame* vr_frame = TieRemasterVr_PublishedFrame();
	bool vr_stereo = compose_flight && vr_frame && g_remaster.view_mode == TIE_REMASTER_VIEW_HD &&
					 g_remaster.blend.alpha >= 1.0f && TieRemaster_FlightCameraValid(snapshot) &&
					 snapshot->flight_screen == TIE_FLIGHT_SCREEN_NORMAL;
	TieRemasterVr_SetRendered(0, NULL);
	if (vr_stereo) {
		vr_stereo = TieRemaster_RenderFlightVr(snapshot, vr_frame, delta_us, g_remaster.host_elapsed_us, paused);
		if (vr_stereo) {
			TieRemaster_FlightRenderCommitted(snapshot);
			/* Leaving stereo must redraw the flat frame (it holds the HUD only). */
			flight->force_render = true;
		}
	}
	if (!vr_stereo && compose_flight && TieRemaster_FlightRenderDue(snapshot)) {
		compose_flight =
			TieRemaster_RenderFlightTarget(snapshot, delta_us, g_remaster.host_elapsed_us, paused);
		if (compose_flight)
			TieRemaster_FlightRenderCommitted(snapshot);
	}

	TieCursorLayerPlacement cursor_placement;
	const bool cursor_layer_active =
		TieRemaster_ResolveCursorLayerPlacement(snapshot, scene2d_is_active, alpha, &cursor_placement);
	const TiePresentationLayout* presentation = TiePresentation_Layout();
	const bool direct_flight =
		compose_flight && !vr_stereo && !scene2d_is_active && !cursor_layer_active && presentation &&
		g_remaster.view_mode == TIE_REMASTER_VIEW_HD && g_remaster.blend.alpha >= 1.0f &&
		g_remaster.flight.render_generation == presentation->render_generation &&
		Aeron_CanRenderDirectToSwapchain(presentation->render_width, presentation->render_height);
	if (direct_flight && !Aeron_FatalErrorRequested())
		(void)TieRemaster_SubmitDirectFlightLayer();
	else if (compose_flight && !Aeron_FatalErrorRequested())
		TieRemaster_SubmitFlightLayer(alpha);
	if (compose_scene2d && !Aeron_FatalErrorRequested())
		TieRemaster_SubmitScene2dLayers(snapshot, alpha);
	if (!Aeron_FatalErrorRequested())
		TieRemaster_SubmitPresentationCursor(snapshot, scene2d_is_active, alpha);
	return !Aeron_FatalErrorRequested();
}

bool TieRemaster_ApplyVideoOptions(const TieVideoOptions* options) {
	if (!g_remaster.initialized || !options)
		return false;
	g_remaster.video_options = *options;
	g_remaster.config.video_options = *options;
	g_remaster.config.render.ssao.ssao_quality = options->ssao_quality;
	g_remaster.config.render.shadows.enabled = options->shadows_enabled;
	g_remaster.config.render.shadows.atlas_size = options->shadow_atlas_size;
	g_remaster.config.render.temporal_mode = (TieFlightTemporalMode)options->fsr_mode;
	g_remaster.config.render.temporal_sharpness = options->fsr_sharpness;
	g_remaster.config.render.motion_blur_quality = options->motion_blur_quality;
	g_remaster.config.render.motion_blur_shutter = options->motion_blur_shutter;
	g_remaster.config.render.msaa_samples = options->msaa_samples;
	g_remaster.config.render.starfield_style = options->starfield_style;
	g_remaster.video_options_pending = true;
	g_remaster.flight.force_render = true;
	return true;
}

bool TieRemaster_SuppressesClassicFlight(const TieSnapshot* snapshot) {
	return g_remaster.initialized && snapshot && snapshot->scene_kind == TIE_SCENE_FLIGHT &&
		   !TieRemaster_FlightRequiresClassicScreen(snapshot) && !g_remaster.pending_view_mode_valid &&
		   g_remaster.view_mode == TIE_REMASTER_VIEW_HD && g_remaster.blend.alpha >= 1.0f;
}

bool TieRemaster_GetVideoOptions(TieVideoOptions* out_options) {
	if (!g_remaster.initialized || !out_options)
		return false;
	*out_options = g_remaster.video_options;
	return true;
}

bool TieRemaster_SetAspectCorrectLegacyScenes(bool enabled) {
	if (!g_remaster.initialized)
		return false;
	g_remaster.aspect_correct_legacy_scenes = enabled;
	return true;
}

bool TieRemaster_SetFlightSource(const TieFlightAssetSource* source) {
	if (!g_remaster.initialized)
		return false;
	if (!source) {
		Aeron_LogError("tie.assets", "flight model source for the requested engine is unavailable");
		return false;
	}
	if (source == g_remaster.assets)
		return true;
	g_remaster.assets = source;
	g_remaster.config.flight_source = source;
	g_remaster.flight_rebuild_pending = true;
	g_remaster.loading_generation_seen = 0;
	g_remaster.loading_generation_prepared = 0;
	Aeron_LogInfo("tie.assets", "selected %s for the next flight", source->name);
	return true;
}

void TieRemaster_ReleaseFlightResources(void) {
	if (!g_remaster.initialized)
		return;

	TieFlightRenderer_ReleaseMissionAssets(g_remaster.flight.renderer);
}

void TieRemaster_Shutdown(void) {
	if (!g_remaster.initialized)
		return;

	TieRemaster_ShutdownFlight();
	TiePresentationCursor* cursor = &g_remaster.cursor;
	if (cursor->target)
		Aeron_DestroyRenderTarget(cursor->target);
	AeronDrawList_Destroy(cursor->draw_list);
	if (cursor->renderer)
		TieScene2dCursor_Shutdown(cursor->renderer);

	TieRemaster2D* scene = &g_remaster.scene2d;
	AeronDrawList_Destroy(scene->draw_list);
	if (scene->map)
		TieScene2dMap_Shutdown(scene->map);
	if (scene->text)
		TieScene2dTextRenderer_Shutdown(scene->text);
	if (scene->target)
		Aeron_DestroyRenderTarget(scene->target);
	if (scene->retired_target)
		Aeron_DestroyRenderTarget(scene->retired_target);
	if (scene->cutscene)
		TieScene2dCutscene_Shutdown(scene->cutscene);
	memset(&g_remaster, 0, sizeof g_remaster);
}
