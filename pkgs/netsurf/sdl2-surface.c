// sdl2-surface.c: окна NetSurf через SDL2 и Weston
// Автор: Елчинян Арен Артурович, 2026
// Архитектура: x86_64, ABI: musl и libnsfb
// Подсистема: браузер
// Первое изменение: 18.10.2026, последнее изменение: 18.10.2026
// Статус доступа: общедоступный, MIT
#include <stdbool.h>
#include <stdlib.h>
#include <SDL.h>
#include "libnsfb.h"
#include "libnsfb_event.h"
#include "libnsfb_plot.h"
#include "libnsfb_plot_util.h"
#include "nsfb.h"
#include "surface.h"
#include "plot.h"
#include "cursor.h"

struct synapse_surface {
	SDL_Window *window;
	SDL_Surface *pixels;
};

// Привязать программные средства рисования к памяти окна
static int synapse_geometry(nsfb_t *fb, int width, int height, enum nsfb_format_e format) {
	struct synapse_surface *state = fb->surface_priv;
	(void)format;

	if (width <= 0 || height <= 0 || width > 8192 || height > 8192) {
		return -1;
	}

	if (state != NULL) {
		SDL_SetWindowSize(state->window, width, height);
		SDL_Surface *pixels =
		    SDL_CreateRGBSurfaceWithFormat(0, width, height, 32, SDL_PIXELFORMAT_RGB888);
		if (pixels == NULL) {
			return -1;
		}

		SDL_FreeSurface(state->pixels);
		state->pixels = pixels;
		fb->ptr = state->pixels->pixels;
		fb->linelen = state->pixels->pitch;
	}

	fb->width = width;
	fb->height = height;
	fb->format = NSFB_FMT_XRGB8888;
	return select_plotters(fb) ? 0 : -1;
}

// Открыть окно, прямой доступ к экрану остаётся у Weston
static int synapse_initialise(nsfb_t *fb) {
	SDL_SetHint(SDL_HINT_FRAMEBUFFER_ACCELERATION, "0");

	if (fb->surface_priv != NULL || SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
		return -1;
	}

	struct synapse_surface *state = calloc(1, sizeof(*state));

	if (state == NULL) {
		SDL_Quit( );
		return -1;
	}

	state->window = SDL_CreateWindow("NetSurf", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
	                                 fb->width, fb->height, SDL_WINDOW_RESIZABLE);
	if (state->window == NULL) {
		free(state);
		SDL_Quit( );
		return -1;
	}

	fb->surface_priv = state;

	if (synapse_geometry(fb, fb->width, fb->height, NSFB_FMT_XRGB8888) != 0) {
		SDL_DestroyWindow(state->window);
		SDL_FreeSurface(state->pixels);
		free(state);
		fb->surface_priv = NULL;
		SDL_Quit( );
		return -1;
	}

	SDL_ShowCursor(SDL_ENABLE);
	return 0;
}

// Освободить окно после завершения браузера
static int synapse_finalise(nsfb_t *fb) {
	struct synapse_surface *state = fb->surface_priv;

	if (state != NULL) {
		SDL_DestroyWindow(state->window);
		SDL_FreeSurface(state->pixels);
		free(state);
		fb->surface_priv = NULL;
		fb->ptr = NULL;
	}

	SDL_Quit( );
	return 0;
}

// Скрыть программный указатель перед изменением его области
static int synapse_claim(nsfb_t *fb, nsfb_bbox_t *box) {
	if (fb->cursor != NULL && fb->cursor->plotted &&
	    nsfb_plot_bbox_intersect(box, &fb->cursor->loc)) {
		nsfb_cursor_clear(fb, fb->cursor);
	}

	return 0;
}

// Передать изменённое изображение оконному серверу
static int synapse_update(nsfb_t *fb, nsfb_bbox_t *box) {
	struct synapse_surface *state = fb->surface_priv;
	(void)box;

	if (state == NULL) {
		return -1;
	}

	SDL_Surface *window = SDL_GetWindowSurface(state->window);

	if (window == NULL || SDL_BlitSurface(state->pixels, NULL, window, NULL) != 0) {
		return -1;
	}

	return SDL_UpdateWindowSurface(state->window);
}

// Указатель рисуется Weston, libnsfb сохраняет только координаты
static int synapse_cursor(nsfb_t *fb, struct nsfb_cursor_s *cursor) {
	(void)fb;
	(void)cursor;
	return 1;
}

// Перевести клавиши SDL2 в значения libnsfb
static enum nsfb_key_code_e synapse_key(SDL_Keycode key) {
	if (key >= 0 && key < 128) {
		return (enum nsfb_key_code_e)key;
	}

	switch (key) {
		case SDLK_UP:
			return NSFB_KEY_UP;
		case SDLK_DOWN:
			return NSFB_KEY_DOWN;
		case SDLK_LEFT:
			return NSFB_KEY_LEFT;
		case SDLK_RIGHT:
			return NSFB_KEY_RIGHT;
		case SDLK_HOME:
			return NSFB_KEY_HOME;
		case SDLK_END:
			return NSFB_KEY_END;
		case SDLK_PAGEUP:
			return NSFB_KEY_PAGEUP;
		case SDLK_PAGEDOWN:
			return NSFB_KEY_PAGEDOWN;
		case SDLK_INSERT:
			return NSFB_KEY_INSERT;
		case SDLK_LSHIFT:
			return NSFB_KEY_LSHIFT;
		case SDLK_RSHIFT:
			return NSFB_KEY_RSHIFT;
		case SDLK_LCTRL:
			return NSFB_KEY_LCTRL;
		case SDLK_RCTRL:
			return NSFB_KEY_RCTRL;
		case SDLK_LALT:
			return NSFB_KEY_LALT;
		case SDLK_RALT:
			return NSFB_KEY_RALT;
		default:
			return NSFB_KEY_UNKNOWN;
	}
}

// Получить событие окна, сохранив ограниченное ожидание браузерного цикла
static bool synapse_input(nsfb_t *fb, nsfb_event_t *event, int timeout) {
	SDL_Event input;
	(void)fb;

	if (!SDL_WaitEventTimeout(&input, timeout)) {
		event->type = NSFB_EVENT_CONTROL;
		event->value.controlcode = NSFB_CONTROL_TIMEOUT;
		return true;
	}

	event->type = NSFB_EVENT_NONE;

	switch (input.type) {
		case SDL_KEYDOWN:
		case SDL_KEYUP:
			event->type = input.type == SDL_KEYDOWN ? NSFB_EVENT_KEY_DOWN : NSFB_EVENT_KEY_UP;
			event->value.keycode = synapse_key(input.key.keysym.sym);
			break;
		case SDL_MOUSEBUTTONDOWN:
		case SDL_MOUSEBUTTONUP:
			event->type =
			    input.type == SDL_MOUSEBUTTONDOWN ? NSFB_EVENT_KEY_DOWN : NSFB_EVENT_KEY_UP;
			if (input.button.button == SDL_BUTTON_LEFT) {
				event->value.keycode = NSFB_KEY_MOUSE_1;
			} else if (input.button.button == SDL_BUTTON_MIDDLE) {
				event->value.keycode = NSFB_KEY_MOUSE_2;
			} else if (input.button.button == SDL_BUTTON_RIGHT) {
				event->value.keycode = NSFB_KEY_MOUSE_3;
			} else {
				event->type = NSFB_EVENT_NONE;
			}

			break;
		case SDL_MOUSEWHEEL:
			if (input.wheel.y != 0) {
				event->type = NSFB_EVENT_KEY_DOWN;
				event->value.keycode = input.wheel.y > 0 ? NSFB_KEY_MOUSE_4 : NSFB_KEY_MOUSE_5;
			}

			break;
		case SDL_MOUSEMOTION:
			event->type = NSFB_EVENT_MOVE_ABSOLUTE;
			event->value.vector.x = input.motion.x;
			event->value.vector.y = input.motion.y;
			event->value.vector.z = 0;
			break;
		case SDL_QUIT:
			event->type = NSFB_EVENT_CONTROL;
			event->value.controlcode = NSFB_CONTROL_QUIT;
			break;
		case SDL_WINDOWEVENT:
			if (input.window.event == SDL_WINDOWEVENT_CLOSE) {
				event->type = NSFB_EVENT_CONTROL;
				event->value.controlcode = NSFB_CONTROL_QUIT;
			} else if (input.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
				event->type = NSFB_EVENT_RESIZE;
				event->value.resize.w = input.window.data1;
				event->value.resize.h = input.window.data2;
			}

			break;
		default:
			break;
	}

	return true;
}

static const nsfb_surface_rtns_t synapse_routines = { .initialise = synapse_initialise,
	                                                  .finalise = synapse_finalise,
	                                                  .geometry = synapse_geometry,
	                                                  .input = synapse_input,
	                                                  .claim = synapse_claim,
	                                                  .update = synapse_update,
	                                                  .cursor = synapse_cursor };

NSFB_SURFACE_DEF(sdl, NSFB_SURFACE_SDL, &synapse_routines)
