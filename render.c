#include <cairo/cairo.h>
#include <pango/pangocairo.h>

#include "mako.h"
#include "notification.h"
#include "render.h"

static void set_cairo_source_u32(cairo_t *cairo, uint32_t color) {
	cairo_set_source_rgba(cairo,
		(color >> (3*8) & 0xFF) / 255.0,
		(color >> (2*8) & 0xFF) / 255.0,
		(color >> (1*8) & 0xFF) / 255.0,
		(color >> (0*8) & 0xFF) / 255.0);
}

int render_notification(cairo_t *cairo, struct mako_state *state, const char *text, int offset_y) {
	struct mako_config *config = &state->config;

	int border_size = 2 * config->border_size;
	int padding_size = 2 * config->padding;

	PangoLayout *layout = pango_cairo_create_layout(cairo);
	pango_layout_set_width(layout,
			(config->width - border_size - padding_size) * PANGO_SCALE);
	pango_layout_set_height(layout,
			(config->height - border_size - padding_size) * PANGO_SCALE);
	pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
	pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
	PangoFontDescription *desc =
		pango_font_description_from_string(config->font);
	pango_layout_set_font_description(layout, desc);
	pango_font_description_free(desc);

	if (config->markup) {
		PangoAttrList *attrs = NULL;
		char *buf = NULL;
		GError *error = NULL;
		if (!pango_parse_markup(text, -1, 0, &attrs, &buf, NULL, &error)) {
			fprintf(stderr, "cannot parse pango markup: %s\n",
					error->message);
			g_error_free(error);
			g_object_unref(layout);
			return 0;
		}
		pango_layout_set_markup(layout, buf, -1);
		pango_layout_set_attributes(layout, attrs);
		pango_attr_list_unref(attrs);
		free(buf);
	} else {
		pango_layout_set_text(layout, text, -1);
	}

	int text_height = 0;
	pango_layout_get_pixel_size(layout, NULL, &text_height);

	int notif_height = border_size + padding_size + text_height;

	// Render border
	set_cairo_source_u32(cairo, config->colors.border);
	cairo_set_line_width(cairo, border_size);
	cairo_rectangle(cairo, 0, offset_y, state->width, notif_height);
	cairo_stroke(cairo);

	// Render background
	set_cairo_source_u32(cairo, config->colors.background);
	cairo_set_line_width(cairo, 0);
	cairo_rectangle(cairo,
			config->border_size, offset_y + config->border_size,
			state->width - border_size, notif_height - border_size);
	cairo_fill(cairo);

	// Render text
	set_cairo_source_u32(cairo, config->colors.text);
	cairo_move_to(cairo, config->border_size + config->padding,
			offset_y + config->border_size + config->padding);
	pango_cairo_update_layout(cairo, layout);
	pango_cairo_show_layout(cairo, layout);

	g_object_unref(layout);

	return notif_height;
}

int render(struct mako_state *state, struct pool_buffer *buffer) {
	struct mako_config *config = &state->config;
	cairo_t *cairo = buffer->cairo;

	if (wl_list_empty(&state->notifications)) {
		return 0;
	}

	// Clear
	set_cairo_source_u32(cairo, 0x00000000);
	cairo_paint(cairo);


	int inner_margin = config->margin.top;
	if (config->margin.bottom > config->margin.top) {
		inner_margin = config->margin.bottom;
	}

	int notif_width = state->width;

	size_t i = 0;
	int height = 0;
	struct mako_notification *notif;
	wl_list_for_each(notif, &state->notifications, link) {

		size_t text_len = format_notification(notif, config->format, NULL);
		char *text = malloc(text_len + 1);
		if (text == NULL) {
			break;
		}
		format_notification(notif, config->format, text);

		int notif_y = height;
		if (i > 0) {
			notif_y += inner_margin;
		}

		int notif_height = render_notification(cairo, state, text, notif_y);
		height = notif_y +  notif_height;

		// Update hotspot
		notif->hotspot.x = 0;
		notif->hotspot.y = notif_y;
		notif->hotspot.width = notif_width;
		notif->hotspot.height = notif_height;

		++i;
		if (config->max_visible >= 0 &&
				i >= (size_t)config->max_visible) {
			break;
		}
	}

	if (wl_list_length(&state->notifications) > config->max_visible) {
		int hidden = wl_list_length(&state->notifications) - config->max_visible;
		int hidden_ln = snprintf(NULL, 0, "[%d]", hidden);

		char *hidden_text;
		hidden_text = malloc(hidden_ln + 1);
		snprintf(hidden_text, hidden_ln + 1, "[%d]", hidden);


		int hidden_height = render_notification(cairo, state, hidden_text, height);
		height += hidden_height;	
	}

	return height;
}
