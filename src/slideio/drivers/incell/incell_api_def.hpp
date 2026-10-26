#pragma once
#if defined(WIN32)
#if defined(SLIDEIO_INCELL_API)
#define SLIDEIO_INCELL_EXPORTS __declspec(dllexport)
#else
#define SLIDEIO_INCELL_EXPORTS __declspec(dllimport)
#endif
#else
#define SLIDEIO_INCELL_EXPORTS
#endif
