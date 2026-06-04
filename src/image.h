#ifndef DESCRY_IMAGE_H
#define DESCRY_IMAGE_H

#include <SDL.h>

typedef struct ImageCache ImageCache;

/* Load status reported by image_cache_get via its `status` out-param. */
enum {
    IMG_READY   = 0,   /* texture is ready to draw */
    IMG_LOADING = 1,   /* remote URL still downloading (no texture yet) */
    IMG_FAILED  = 2,   /* missing / malformed / unsupported / fetch failed */
};

ImageCache*  image_cache_create (SDL_Renderer* r);
void         image_cache_destroy(ImageCache* c);

/* Load (or fetch from cache) the image at `src`, which may be a local file
 * path OR an http(s):// URL. On success returns the SDL_Texture and writes
 * native dimensions to w and h.
 *
 * Local files (png/jpg/svg) decode synchronously, exactly as before.
 *
 * Remote URLs are downloaded on a background thread: the first call returns
 * NULL with *status = IMG_LOADING and kicks off the fetch; once the bytes
 * land (an SDL user event wakes the main loop) a later call decodes them on
 * the calling thread and returns the texture. Downloads are cached on disk
 * so reloads are instant. Failures are cached too, so we don't retry every
 * frame. `status` may be NULL if the caller doesn't care. */
SDL_Texture* image_cache_get(ImageCache* c, const char* src,
                             int* w, int* h, int* status);

#endif
