#include "SDL3/SDL.h"
#include "SDL3/SDL_thread.h"

#undef main

#include "main.h"
#include "config.h"
#include "error.h"
#include "system.h"

int main(int argc, char **argv)
{
	/* set default config */
	error_init();
	set_config_defaults();

	/* mark all BIOS as unloaded */
	system_bios = 0;

  return 0;
}
