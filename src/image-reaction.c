//
// Created by scaled
//
// Based on image-source.c from OBS Studio: https://github.com/obsproject/obs-studio
// Also included some code from Spectralizer plugin: https://github.com/univrsal/spectralizer
//

#include <obs-module.h>
#include <graphics/image-file.h>
#include <util/platform.h>
#include <util/dstr.h>
#include <sys/stat.h>
#include <media-io/audio-math.h>

#define obs_log(level, format, ...) blog(level, "[image-reaction] " format, ##__VA_ARGS__)

enum image_index {
	IMAGE_SILENT,
	IMAGE_TALKING,
	IMAGE_SILENT_BLINK,
	IMAGE_TALKING_BLINK,
	IMAGE_COUNT,
};

static const char *file_keys[IMAGE_COUNT] = {"file1", "file2", "file3", "file4"};

/* Human blink timing */
#define BLINK_MIN_DURATION 0.10f  /* eyelids closed, seconds */
#define BLINK_MAX_DURATION 0.16f
#define DOUBLE_BLINK_CHANCE 0.12f /* roughly 1 in 8 blinks is followed by a quick second blink */
#define DOUBLE_BLINK_MIN_GAP 0.15f
#define DOUBLE_BLINK_MAX_GAP 0.30f
#define BLINK_INTERVAL_SIGMA 0.5f /* spread of the log-normal interval distribution */

struct image_reaction_source {
	obs_source_t *source;
	char source_name[255];

	char *files[IMAGE_COUNT];
	bool persistent;
	bool linear_alpha;
	bool active;

	gs_image_file_ex_t images[IMAGE_COUNT];

	obs_weak_source_t *audio_source;

	bool loud;
	float threshold;
	float smoothness;
	float average;

	uint64_t last_time;
	uint64_t capture_check_time;

	bool animReset1;
	bool animReset2;
	bool loudOld;
	bool animResetTrigger;

	/* Blinking */
	float blink_interval; /* average seconds between blinks */
	float blink_timer;    /* seconds until next state change */
	bool blinking;        /* eyes currently closed */
	bool double_blink_done;
	uint64_t rng_state;
};

/*int MAX(int a, int b) {
	return a > b ? a : b;
}*/

#define MIN(a, b) ((a)<(b) ? (a):(b))
#define MAX(a, b) ((a)>(b) ? (a):(b))

static const char *image_reaction_source_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("ImageReactionSource");
}

static void image_reaction_source_load(struct image_reaction_source *context)
{
	for (int i = 0; i < IMAGE_COUNT; i++) {
		char *file = context->files[i];
		gs_image_file_ex_t *if4 = &context->images[i];

		obs_enter_graphics();
		gs_image_file_ex_free(if4);
		obs_leave_graphics();

		if (file && *file) {
			obs_log(LOG_DEBUG, "loading texture '%s'", file);
			gs_image_file_ex_init(if4, file,
					      context->linear_alpha ? GS_IMAGE_ALPHA_PREMULTIPLY_SRGB
								    : GS_IMAGE_ALPHA_PREMULTIPLY);

			obs_enter_graphics();
			gs_image_file_ex_init_texture(if4);
			obs_leave_graphics();

			if (!if4->loaded)
				obs_log(LOG_WARNING, "failed to load texture '%s'", file);
		}
	}
}

static void image_reaction_source_unload(struct image_reaction_source *context)
{
	obs_enter_graphics();
	for (int i = 0; i < IMAGE_COUNT; i++)
		gs_image_file_ex_free(&context->images[i]);
	obs_leave_graphics();
}

/* xorshift64*: per-source RNG so multiple sources don't blink in sync */
static float random_float(struct image_reaction_source *context)
{
	uint64_t x = context->rng_state;
	x ^= x >> 12;
	x ^= x << 25;
	x ^= x >> 27;
	context->rng_state = x;
	return (float)((x * 0x2545F4914F6CDD1DULL) >> 40) / (float)(1 << 24); /* [0, 1) */
}

static float random_range(struct image_reaction_source *context, float min, float max)
{
	return min + (max - min) * random_float(context);
}

/* Human inter-blink intervals are right-skewed (roughly log-normal):
 * usually a few seconds apart, occasionally much longer. */
static float next_blink_interval(struct image_reaction_source *context)
{
	const float sigma = BLINK_INTERVAL_SIGMA;
	float u1 = MAX(random_float(context), 1e-6f);
	float u2 = random_float(context);
	float z = sqrtf(-2.0f * logf(u1)) * cosf(6.28318531f * u2);
	float interval = context->blink_interval * expf(sigma * z - 0.5f * sigma * sigma);

	return MIN(MAX(interval, 0.3f * context->blink_interval), 3.0f * context->blink_interval);
}

static bool blink_available(struct image_reaction_source *context)
{
	return context->images[IMAGE_SILENT_BLINK].texture || context->images[IMAGE_TALKING_BLINK].texture;
}

static void update_blink(struct image_reaction_source *context, float seconds)
{
	if (!blink_available(context)) {
		context->blinking = false;
		return;
	}

	context->blink_timer -= seconds;
	if (context->blink_timer > 0.0f)
		return;

	if (context->blinking) {
		/* Eyes open again */
		context->blinking = false;
		if (!context->double_blink_done && random_float(context) < DOUBLE_BLINK_CHANCE) {
			context->double_blink_done = true;
			context->blink_timer = random_range(context, DOUBLE_BLINK_MIN_GAP, DOUBLE_BLINK_MAX_GAP);
		} else {
			context->double_blink_done = false;
			context->blink_timer = next_blink_interval(context);
		}
	} else {
		/* Close eyes */
		context->blinking = true;
		context->blink_timer = random_range(context, BLINK_MIN_DURATION, BLINK_MAX_DURATION);
	}
}

static void audio_capture(void *param, obs_source_t *src, const struct audio_data *data, bool muted)
{
	(void)src;
	struct image_reaction_source *context = param;

	if (muted) {
		context->average = 0;
	} else {
		uint32_t samplesCount = data->frames;
		float *samples = (float *)data->data[0];

		float averageLocal = 0.0f;

		for (uint32_t i = 0; i < samplesCount; i++) {
			averageLocal += fabsf(samples[i]) / samplesCount;
		}

		context->average += context->smoothness * (averageLocal - context->average);
	}

	context->loudOld = context->loud;
	context->loud = context->average > context->threshold;

	if (context->loud != context->loudOld)
		context->animResetTrigger = true;
}

static void image_reaction_source_update(void *data, obs_data_t *settings)
{
	struct image_reaction_source *context = data;
	const bool anim_reset_1 = obs_data_get_bool(settings, "anim_reset_1");
	const bool anim_reset_2 = obs_data_get_bool(settings, "anim_reset_2");
	const bool unload = obs_data_get_bool(settings, "unload");
	const bool linear_alpha = obs_data_get_bool(settings, "linear_alpha");
	const float threshold = (float)obs_data_get_double(settings, "threshold");
	const float smoothness = (float)obs_data_get_double(settings, "smoothness");
	const float blink_interval = MAX((float)obs_data_get_double(settings, "blink_interval"), 0.5f);

	for (int i = 0; i < IMAGE_COUNT; i++) {
		bfree(context->files[i]);
		context->files[i] = bstrdup(obs_data_get_string(settings, file_keys[i]));
	}

	if (blink_interval != context->blink_interval) {
		context->blink_interval = blink_interval;
		context->blinking = false;
		context->double_blink_done = false;
		context->blink_timer = next_blink_interval(context);
	}

	context->animReset1 = anim_reset_1;
	context->animReset2 = anim_reset_2;

	context->persistent = !unload;
	context->linear_alpha = linear_alpha;
	context->threshold = db_to_mul(threshold);
	context->smoothness = powf(0.1f, smoothness);

	/* Load the image if the source is persistent or showing */
	if (context->persistent || obs_source_showing(context->source))
		image_reaction_source_load(data);
	else
		image_reaction_source_unload(data);

	const char *cfg_source_name = obs_data_get_string(settings, "audio_source");

	obs_weak_source_t *old = NULL;

	if (cfg_source_name[0] == '\0') {
		if (context->audio_source) {
			old = context->audio_source;
			context->audio_source = NULL;
		}
		context->source_name[0] = '\0';
	} else {
		if (context->source_name[0] == '\0' || strcmp(context->source_name, cfg_source_name) != 0) {
			if (context->audio_source) {
				old = context->audio_source;
				context->audio_source = NULL;
			}
			strcpy(context->source_name, cfg_source_name);
			context->capture_check_time = os_gettime_ns() - 3000000000;
		}
	}

	if (old) {
		obs_source_t *old_source = obs_weak_source_get_source(old);
		if (old_source) {
			obs_log(LOG_INFO, "Removed audio capture from '%s'", obs_source_get_name(old_source));
			obs_source_remove_audio_capture_callback(old_source, audio_capture, context);
			obs_source_release(old_source);
		}
		obs_weak_source_release(old);
	}
}

static void image_reaction_source_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, "unload", false);
	obs_data_set_default_bool(settings, "linear_alpha", false);
	obs_data_set_default_string(settings, "audio_source", "");
	obs_data_set_default_double(settings, "threshold", -40.0);
	obs_data_set_default_double(settings, "smoothness", 1.0);
	obs_data_set_default_double(settings, "blink_interval", 4.0);
}

static void image_reaction_source_show(void *data)
{
	struct image_reaction_source *context = data;

	if (!context->persistent)
		image_reaction_source_load(context);
}

static void image_reaction_source_hide(void *data)
{
	struct image_reaction_source *context = data;

	if (!context->persistent)
		image_reaction_source_unload(context);
}

static void *image_reaction_source_create(obs_data_t *settings, obs_source_t *source)
{
	struct image_reaction_source *context = bzalloc(sizeof(struct image_reaction_source));
	context->source = source;

	context->source_name[0] = '\0';
	context->loud = false;
	context->rng_state = os_gettime_ns() ^ (uint64_t)(uintptr_t)context;
	if (!context->rng_state)
		context->rng_state = 0x9E3779B97F4A7C15ULL;

	image_reaction_source_update(context, settings);
	return context;
}

static void image_reaction_source_destroy(void *data)
{
	struct image_reaction_source *context = data;

	image_reaction_source_unload(context);

	for (int i = 0; i < IMAGE_COUNT; i++)
		bfree(context->files[i]);

	/*if (context->audio_source) {
		//obs_source_t *source = obs_weak_source_get_source(context->audio_source);
		//if (source) {
			obs_log(LOG_INFO, "Removed audio capture from '%s'", obs_source_get_name(context->audio_source));
			obs_source_remove_audio_capture_callback(context->audio_source, audio_capture, context);
			//obs_source_release(source);
		//}
		//obs_weak_source_release(context->audio_source);
	}*/
	if (context->audio_source) {
		obs_source_t *source = obs_weak_source_get_source(context->audio_source);
		if (source) {
			obs_log(LOG_INFO, "Removed audio capture from '%s'", obs_source_get_name(source));
			obs_source_remove_audio_capture_callback(source, audio_capture, context);
			obs_source_release(source);
		}
		obs_weak_source_release(context->audio_source);
	}

	bfree(context);
}

static uint32_t image_reaction_source_getwidth(void *data)
{
	struct image_reaction_source *context = data;
	uint32_t cx = 0;
	for (int i = 0; i < IMAGE_COUNT; i++)
		cx = MAX(cx, context->images[i].cx);
	return cx;
}

static uint32_t image_reaction_source_getheight(void *data)
{
	struct image_reaction_source *context = data;
	uint32_t cy = 0;
	for (int i = 0; i < IMAGE_COUNT; i++)
		cy = MAX(cy, context->images[i].cy);
	return cy;
}

static void image_reaction_source_render(void *data, gs_effect_t *effect)
{
	struct image_reaction_source *context = data;

	const bool previous = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(true);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

	gs_image_file_ex_t *if4 = &context->images[context->loud ? IMAGE_TALKING : IMAGE_SILENT];

	if (context->blinking) {
		gs_image_file_ex_t *blink = &context->images[context->loud ? IMAGE_TALKING_BLINK : IMAGE_SILENT_BLINK];
		/* Fall back to the silent blink image if there's no talking one */
		if (!blink->texture)
			blink = &context->images[IMAGE_SILENT_BLINK];
		if (blink->texture)
			if4 = blink;
	}
	if (if4->texture) {
		gs_eparam_t *const param = gs_effect_get_param_by_name(effect, "image");
		gs_effect_set_texture_srgb(param, if4->texture);

		gs_draw_sprite(if4->texture, 0, if4->cx, if4->cy);
	}
	//context->loud = false;

	gs_blend_state_pop();

	gs_enable_framebuffer_srgb(previous);
}

static void image_reaction_tick(void *data, float seconds)
{
	struct image_reaction_source *context = data;

	update_blink(context, seconds);

	// Update / refresh audio capturing
	char *new_name = NULL;
	if (context->source_name[0] != '\0' && !context->audio_source) {
		uint64_t t = os_gettime_ns();

		if (t - context->capture_check_time > 3000000000) {
			new_name = context->source_name;
			context->capture_check_time = t;
		}
	}

	if (new_name != NULL) {
		obs_source_t *capture = obs_get_source_by_name(new_name);
		obs_weak_source_t *weak_capture = capture ? obs_source_get_weak_source(capture) : NULL;

		if (context->source_name[0] != '\0' && new_name == context->source_name) {
			context->audio_source = weak_capture;
			weak_capture = NULL;
		}

		if (capture) {
			obs_log(LOG_INFO, "Added audio capture to '%s'", obs_source_get_name(capture));
			obs_source_add_audio_capture_callback(capture, audio_capture, context);
			obs_weak_source_release(weak_capture);
			obs_source_release(capture);
		}
	}

	// update GIF's
	uint64_t frame_time = obs_get_video_frame_time();
	if (obs_source_active(context->source)) {
		if (!context->active) {
			for (int i = 0; i < IMAGE_COUNT; i++) {
				if (context->images[i].is_animated_gif)
					context->last_time = frame_time;
			}
			context->active = true;
		}

	} else {
		if (context->active) {
			for (int i = 0; i < IMAGE_COUNT; i++) {
				gs_image_file_ex_t *if4 = &context->images[i];
				if (if4->is_animated_gif) {
					if4->cur_frame = 0;
					if4->cur_loop = 0;
					if4->cur_time = 0;

					obs_enter_graphics();
					gs_image_file_ex_update_texture(if4);
					obs_leave_graphics();
				}
			}

			context->active = false;
		}
	}

	for (int i = 0; i < IMAGE_COUNT; i++) {
		gs_image_file_ex_t *if4 = &context->images[i];
		bool animReset = i == IMAGE_SILENT    ? context->animReset1
				 : i == IMAGE_TALKING ? context->animReset2
						      : false;

		if (context->last_time && if4->is_animated_gif) {
			if (animReset && context->animResetTrigger) {
				if4->cur_frame = 0;
				if4->cur_loop = 0;
				if4->cur_time = 0;

				obs_enter_graphics();
				gs_image_file_ex_update_texture(if4);
				obs_leave_graphics();
			} else {
				uint64_t elapsed = frame_time - context->last_time;
				bool updated = gs_image_file_ex_tick(if4, elapsed);

				if (updated) {
					obs_enter_graphics();
					gs_image_file_ex_update_texture(if4);
					obs_leave_graphics();
				}
			}
		}
	}
	context->animResetTrigger = false;

	context->last_time = frame_time;
}

static const char *image_filter = "All formats (*.bmp *.tga *.png *.jpeg *.jpg *.gif *.psd *.webp);;"
				  "BMP Files (*.bmp);;"
				  "Targa Files (*.tga);;"
				  "PNG Files (*.png);;"
				  "JPEG Files (*.jpeg *.jpg);;"
				  "GIF Files (*.gif);;"
				  "PSD Files (*.psd);;"
				  "WebP Files (*.webp);;"
				  "All Files (*.*)";

static bool add_source(void *param, obs_source_t *src)
{
	obs_property_t *list = param;

	uint32_t caps = obs_source_get_output_flags(src);

	if ((caps & OBS_SOURCE_AUDIO) == 0)
		return true;
	const char *name = obs_source_get_name(src);
	obs_property_list_add_string(list, name, name);
	return true;
}

static bool source_changed(obs_properties_t *props, obs_property_t *prop, obs_data_t *data)
{
	(void)props;
	(void)prop;
	obs_data_get_string(data, "audio_source");
	return true;
}

static obs_properties_t *image_reaction_source_properties(void *data)
{
	struct image_reaction_source *s = data;
	struct dstr path = {0};

	obs_properties_t *props = obs_properties_create();

	if (s && s->files[IMAGE_SILENT] && *s->files[IMAGE_SILENT]) {
		const char *slash;

		dstr_copy(&path, s->files[IMAGE_SILENT]);
		dstr_replace(&path, "\\", "/");
		slash = strrchr(path.array, '/');
		if (slash)
			dstr_resize(&path, slash - path.array + 1);
	}

	obs_properties_add_path(props, "file1", obs_module_text("Reaction1"), OBS_PATH_FILE, image_filter, path.array);
	obs_properties_add_bool(props, "anim_reset_1", obs_module_text("AnimReset1"));
	obs_properties_add_path(props, "file2", obs_module_text("Reaction2"), OBS_PATH_FILE, image_filter, path.array);
	obs_properties_add_bool(props, "anim_reset_2", obs_module_text("AnimReset2"));
	obs_properties_add_path(props, "file3", obs_module_text("BlinkSilent"), OBS_PATH_FILE, image_filter, path.array);
	obs_properties_add_path(props, "file4", obs_module_text("BlinkTalking"), OBS_PATH_FILE, image_filter,
				path.array);
	obs_property_t *bi = obs_properties_add_float_slider(props, "blink_interval", obs_module_text("BlinkInterval"),
							     1.0, 10.0, 0.1);
	obs_property_float_set_suffix(bi, " s");
	dstr_free(&path);

	obs_properties_add_bool(props, "unload", obs_module_text("UnloadWhenNotShowing"));
	obs_properties_add_bool(props, "linear_alpha", obs_module_text("LinearAlpha"));
	obs_property_t *sources_list = obs_properties_add_list(props, "audio_source", obs_module_text("AudioSource"),
							       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(sources_list, "", "");

	obs_property_t *p =
		obs_properties_add_float_slider(props, "threshold", obs_module_text("Threshold"), -60.0, 0.0, 0.1);
	obs_property_float_set_suffix(p, " dB");

	obs_properties_add_float_slider(props, "smoothness", obs_module_text("Smoothness"), 0.0, 5.0, 0.1);

	//obs_property_set_modified_callback(src, source_changed);
	obs_enum_sources(add_source, sources_list);

	return props;
}

uint64_t image_reaction_source_get_memory_usage(void *data)
{
	struct image_reaction_source *s = data;
	uint64_t total = 0;
	for (int i = 0; i < IMAGE_COUNT; i++)
		total += s->images[i].mem_usage;
	return total;
}

static void missing_file_callback(void *src, const char *new_path, void *data)
{
	struct image_reaction_source *s = src;

	obs_source_t *source = s->source;
	obs_data_t *settings = obs_source_get_settings(source);
	obs_data_set_string(settings, "file", new_path);
	obs_source_update(source, settings);
	obs_data_release(settings);

	UNUSED_PARAMETER(data);
}

static struct obs_source_info image_reaction_source_info = {
	.id = "image_reaction_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB,
	.get_name = image_reaction_source_get_name,
	.create = image_reaction_source_create,
	.destroy = image_reaction_source_destroy,
	.update = image_reaction_source_update,
	.get_defaults = image_reaction_source_defaults,
	.show = image_reaction_source_show,
	.hide = image_reaction_source_hide,
	.get_width = image_reaction_source_getwidth,
	.get_height = image_reaction_source_getheight,
	.video_render = image_reaction_source_render,
	.video_tick = image_reaction_tick,
	.get_properties = image_reaction_source_properties,
	.icon_type = OBS_ICON_TYPE_IMAGE,
};

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("image-reaction", "en-US")
MODULE_EXPORT const char *obs_module_description(void)
{
	return "Image reaction source";
}

extern struct obs_source_info slideshow_info;

bool obs_module_load(void)
{
	obs_register_source(&image_reaction_source_info);
	return true;
}
