#include <windows.h>

#include <obs-module.h>
#include <util/dstr.h>

#include "cursor-capture.h"

#define do_log(level, format, ...) \
	blog(level, "[duplicator-monitor-capture: '%s'] " format, \
			obs_source_get_name(capture->source), ##__VA_ARGS__)

#define warn(format, ...)  do_log(LOG_WARNING, format, ##__VA_ARGS__)
#define info(format, ...)  do_log(LOG_INFO,    format, ##__VA_ARGS__)
#define debug(format, ...) do_log(LOG_DEBUG,   format, ##__VA_ARGS__)

#define TEXT_MONITOR_CAPTURE obs_module_text("MonitorCapture")
#define TEXT_CAPTURE_CURSOR  obs_module_text("CaptureCursor")
#define TEXT_COMPATIBILITY   obs_module_text("Compatibility")
#define TEXT_MONITOR         obs_module_text("Monitor")
#define TEXT_PRIMARY_MONITOR obs_module_text("PrimaryMonitor")

#define RESET_INTERVAL_SEC 3.0f

struct duplicator_capture {
	obs_source_t                   *source;
	int                            monitor;
	bool                           capture_cursor;

	long                           x;
	long                           y;
	int                            rot;
	uint32_t                       width;
	uint32_t                       height;
	gs_duplicator_t                *duplicator;
	float                          reset_timeout;
	struct cursor_data             cursor_data;
};

/* ------------------------------------------------------------------------- */

static inline void update_settings(struct duplicator_capture *capture,
		obs_data_t *settings)
{
	capture->monitor        = (int)obs_data_get_int(settings, "monitor");
	capture->capture_cursor = obs_data_get_bool(settings, "capture_cursor");

	obs_enter_graphics();

	gs_duplicator_destroy(capture->duplicator);
	capture->duplicator = gs_duplicator_create(capture->monitor);
	capture->width = 0;
	capture->height = 0;
	capture->x = 0;
	capture->y = 0;
	capture->rot = 0;
	capture->reset_timeout = 0.0f;

	obs_leave_graphics();
}

/* ------------------------------------------------------------------------- */

static const char *duplicator_capture_getname(void)
{
	return TEXT_MONITOR_CAPTURE;
}

static void duplicator_capture_destroy(void *data)
{
	struct duplicator_capture *capture = data;

	obs_enter_graphics();

	gs_duplicator_destroy(capture->duplicator);
	cursor_data_free(&capture->cursor_data);

	obs_leave_graphics();

	bfree(capture);
}

static void duplicator_capture_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "monitor", 0);
	obs_data_set_default_bool(settings, "capture_cursor", true);
}

static void duplicator_capture_update(void *data, obs_data_t *settings)
{
	struct duplicator_capture *mc = data;
	update_settings(mc, settings);
}

static void *duplicator_capture_create(obs_data_t *settings,
		obs_source_t *source)
{
	struct duplicator_capture *capture;

	capture = bzalloc(sizeof(struct duplicator_capture));
	capture->source = source;

	update_settings(capture, settings);

	return capture;
}

static void reset_capture_data(struct duplicator_capture *capture)
{
	struct gs_monitor_info monitor_info = {0};
	gs_texture_t *texture = gs_duplicator_get_texture(capture->duplicator);

	gs_get_duplicator_monitor_info(capture->monitor, &monitor_info);
	capture->width = gs_texture_get_width(texture);
	capture->height = gs_texture_get_height(texture);
	capture->x = monitor_info.x;
	capture->y = monitor_info.y;
	capture->rot = monitor_info.rotation_degrees;
}

static void duplicator_capture_tick(void *data, float seconds)
{
	struct duplicator_capture *capture = data;

	if (!obs_source_showing(capture->source))
		return;

	obs_enter_graphics();

	if (!capture->duplicator) {
		capture->reset_timeout += seconds;

		if (capture->reset_timeout >= RESET_INTERVAL_SEC) {
			capture->duplicator =
				gs_duplicator_create(capture->monitor);

			capture->reset_timeout = 0.0f;
		}
	}

	if (!!capture->duplicator) {
		if (capture->capture_cursor)
			cursor_capture(&capture->cursor_data);

		if (!gs_duplicator_update_frame(capture->duplicator)) {
			gs_duplicator_destroy(capture->duplicator);
			capture->duplicator = NULL;
			capture->width = 0;
			capture->height = 0;
			capture->x = 0;
			capture->y = 0;
			capture->rot = 0;
			capture->reset_timeout = 0.0f;

		} else if (capture->width == 0) {
			reset_capture_data(capture);
		}
	}

	obs_leave_graphics();

	UNUSED_PARAMETER(seconds);
}

static uint32_t duplicator_capture_width(void *data)
{
	struct duplicator_capture *capture = data;
	return capture->rot % 180 == 0 ? capture->width : capture->height;
}

static uint32_t duplicator_capture_height(void *data)
{
	struct duplicator_capture *capture = data;
	return capture->rot % 180 == 0 ? capture->height : capture->width;
}

static void draw_cursor(struct duplicator_capture *capture)
{
	cursor_draw(&capture->cursor_data, -capture->x, -capture->y,
		1.0f, 1.0f,
		capture->rot % 180 == 0 ? capture->width : capture->height,
		capture->rot % 180 == 0 ? capture->height : capture->width);
}

static void duplicator_capture_render(void *data, gs_effect_t *effect)
{
	struct duplicator_capture *capture = data;
	gs_texture_t *texture;
	int rot;

	if (!capture->duplicator)
		return;

	texture = gs_duplicator_get_texture(capture->duplicator);
	if (!texture)
		return;

	effect = obs_get_default_effect();

	rot = capture->rot;

	while (gs_effect_loop(effect, "Draw")) {
		gs_enable_blending(false);
		gs_enable_color(true, true, true, false);

		if (rot != 0) {
			float x;
			float y;

			switch (rot) {
			case 90:
				x = (float)capture->height;
				y = 0.0f;
				break;
			case 180:
				x = (float)capture->width;
				y = (float)capture->height;
				break;
			case 270:
				x = 0.0f;
				y = (float)capture->width;
				break;
			}

			gs_matrix_push();
			gs_matrix_translate3f(x, y, 0.0f);
			gs_matrix_rotaa4f(0.0f, 0.0f, 1.0f, RAD((float)rot));
		}

		obs_source_draw(texture, 0, 0, 0, 0, false);

		if (rot != 0)
			gs_matrix_pop();

		gs_enable_blending(true);
		gs_enable_color(true, true, true, true);

		if (capture->capture_cursor)
			draw_cursor(capture);
	}
}

static bool get_monitor_props(obs_property_t *monitor_list, int monitor_idx)
{
	struct dstr monitor_desc = {0};
	struct gs_monitor_info info;

	if (!gs_get_duplicator_monitor_info(monitor_idx, &info))
		return false;

	dstr_catf(&monitor_desc, "%s %d: %ldx%ld @ %ld,%ld",
			TEXT_MONITOR, monitor_idx,
			info.cx, info.cy, info.x, info.y);

	obs_property_list_add_int(monitor_list, monitor_desc.array,
			monitor_idx);

	dstr_free(&monitor_desc);

	return true;
}

static obs_properties_t *duplicator_capture_properties(void *unused)
{
	int monitor_idx = 0;

	UNUSED_PARAMETER(unused);

	obs_properties_t *props = obs_properties_create();

	obs_property_t *monitors = obs_properties_add_list(props,
		"monitor", TEXT_MONITOR,
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);

	obs_property_t *capture_cursor = obs_properties_add_bool(props,
		"capture_cursor", TEXT_CAPTURE_CURSOR);

	obs_enter_graphics();

	while (get_monitor_props(monitors, monitor_idx++));

	obs_leave_graphics();

	return props;
}

struct obs_source_info duplicator_capture_info = {
	.id             = "monitor_capture",
	.type           = OBS_SOURCE_TYPE_INPUT,
	.output_flags   = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW,
	.get_name       = duplicator_capture_getname,
	.create         = duplicator_capture_create,
	.destroy        = duplicator_capture_destroy,
	.video_render   = duplicator_capture_render,
	.video_tick     = duplicator_capture_tick,
	.update         = duplicator_capture_update,
	.get_width      = duplicator_capture_width,
	.get_height     = duplicator_capture_height,
	.get_defaults   = duplicator_capture_defaults,
	.get_properties = duplicator_capture_properties
};