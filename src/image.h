#ifndef DOWNSEE_IMAGE_H
#define DOWNSEE_IMAGE_H

#include <SDL.h>

typedef struct ImageCache ImageCache;

ImageCache*  image_cache_create (SDL_Renderer* r);
void         image_cache_destroy(ImageCache* c);

/* Load (or fetch from cache) the image at `path`. Returns the SDL_Texture
 * and writes native dimensions to w and h. Returns NULL on failure
 * (missing, malformed, unsupported format) — failures are cached too, so
 * we don't retry every frame. */
SDL_Texture* image_cache_get(ImageCache* c, const char* path,
                             int* w, int* h);

#endif
