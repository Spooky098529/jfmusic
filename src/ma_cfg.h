/* miniaudio build options, shared by ma_impl.c and player.c. */
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_ENGINE
#define MA_NO_NODE_GRAPH
#define MA_NO_RESOURCE_MANAGER
#define MA_ENABLE_ONLY_SPECIFIC_BACKENDS
#define MA_ENABLE_WASAPI
#define MA_ENABLE_DSOUND
#define MA_ENABLE_WINMM
#ifdef JF_NULL_AUDIO            /* test builds: no sound card needed */
#define MA_ENABLE_NULL
#endif
#include "../vendor/miniaudio.h"
